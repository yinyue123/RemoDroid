/* File control (ctrl file.*) and data (the "file" channel: u32 xfer | u32 seq |
 * payload) on a computer; a port of android/java/…/Files.java. Paths are kept
 * inside the user's home folder ("~" works); an upload aimed elsewhere (the
 * viewer's phone-style default) lands in [files] upload_dir. Runs on the agent
 * thread; pulls are paced by the data channel's send buffer. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

#include <mbedtls/sha256.h>

#include "desk.h"
#include "msg.h"
#include "rd_rtc.h"

#define CHUNK (16 * 1024)
#define MAX_XFERS 16
#define SEND_LIMIT (512 * 1024) /* stop pulling while the channel holds this much */

typedef struct {
    int xfer, used, pull;
    FILE *f;
    char tmp[1024], dest[1024];
    long long size, got;
    char sha[65];
    mbedtls_sha256_context md;
    uint32_t next_seq;
    /* pull: a chunk the channel didn't take yet */
    uint8_t *pending;
    size_t pending_len;
} xfer_t;

static xfer_t X[MAX_XFERS];
static ev_timer pump_timer;
static int pump_inited;

/* ---------------- paths ---------------- */

#ifdef _WIN32
#define stat_t struct _stat64
#define STAT(p, s) _stat64(p, s)
#define IS_DIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#define IS_REG(m) (((m) & _S_IFMT) == _S_IFREG)
#else
#define stat_t struct stat
#define STAT(p, s) stat(p, s)
#define IS_DIR(m) S_ISDIR(m)
#define IS_REG(m) S_ISREG(m)
#endif

static int is_sep(char c) { return c == '/' || c == '\\'; }

/* "~/x", "~", absolute → absolute with native separators, "." and ".." resolved. */
static int normalize(const char *in, char *out, size_t cap)
{
    char tmp[1024];
    if (in[0] == '~' && (!in[1] || is_sep(in[1])))
        snprintf(tmp, sizeof tmp, "%s%s", rd_home_dir(), in + 1);
    else
        snprintf(tmp, sizeof tmp, "%s", in);
#ifdef _WIN32
    if (!_fullpath(out, tmp, cap))
        return -1;
    for (char *p = out; *p; p++)
        if (*p == '/')
            *p = '\\';
    size_t n = strlen(out);
    while (n > 3 && out[n - 1] == '\\')
        out[--n] = 0;
    return 0;
#else
    if (tmp[0] != '/')
        return -1;
    /* resolve . and .. lexically (the target may not exist yet) */
    char *parts[128];
    int np = 0;
    char *save = NULL;
    for (char *t = strtok_r(tmp, "/", &save); t; t = strtok_r(NULL, "/", &save)) {
        if (!strcmp(t, "."))
            continue;
        if (!strcmp(t, "..")) {
            if (np)
                np--;
            continue;
        }
        if (np == (int)RD_ARRAY_LEN(parts))
            return -1;
        parts[np++] = t;
    }
    size_t o = 0;
    out[0] = 0;
    for (int i = 0; i < np; i++) {
        int k = snprintf(out + o, cap - o, "/%s", parts[i]);
        if (k < 0 || (size_t)k >= cap - o)
            return -1;
        o += (size_t)k;
    }
    if (!np)
        snprintf(out, cap, "/");
    /* follow symlinks of what exists, so a link can't point out of home */
    char real[1024];
    if (realpath(out, real))
        snprintf(out, cap, "%s", real);
    return 0;
#endif
}

static int inside(const char *path, const char *root)
{
    size_t n = strlen(root);
#ifdef _WIN32
    if (_strnicmp(path, root, n))
#else
    if (strncmp(path, root, n))
#endif
        return 0;
    return path[n] == 0 || is_sep(path[n]);
}

/* A path the viewer may touch: under the home folder. */
static int allowed(const char *in, char *out, size_t cap)
{
    char home[1024];
    if (normalize(rd_home_dir(), home, sizeof home) < 0 || normalize(in, out, cap) < 0)
        return 0;
    return inside(out, home);
}

static const char *base_name(const char *p)
{
    const char *b = p;
    for (const char *s = p; *s; s++)
        if (is_sep(*s))
            b = s + 1;
    return b;
}

static void upload_dir(char *out, size_t cap)
{
    const char *d = desk_conf("files", "upload_dir", "");
    char def[1024];
    snprintf(def, sizeof def, "%s%cRemoDroid", dsk_downloads_dir(), RD_PATH_SEP);
    if (!*d || normalize(d, out, cap) < 0)
        snprintf(out, cap, "%s", def);
}

static int file_exists(const char *p)
{
    stat_t st;
    return STAT(p, &st) == 0;
}

static void parent_of(const char *p, char *out, size_t cap)
{
    snprintf(out, cap, "%s", p);
    char *b = (char *)base_name(out);
    if (b > out)
        b[-1] = 0;
}

/* "a.jpg" → "a (1).jpg", the first free one. */
static void unique(char *path, size_t cap)
{
    if (!file_exists(path))
        return;
    char dir[1024], name[512], base[512], ext[64] = "";
    parent_of(path, dir, sizeof dir);
    snprintf(name, sizeof name, "%s", base_name(path));
    char *dot = strrchr(name, '.');
    if (dot && dot != name) {
        snprintf(ext, sizeof ext, "%s", dot);
        *dot = 0;
    }
    snprintf(base, sizeof base, "%s", name);
    for (int i = 1; i < 10000; i++) {
        snprintf(path, cap, "%s%c%s (%d)%s", dir, RD_PATH_SEP, base, i, ext);
        if (!file_exists(path))
            return;
    }
}

static int replace_file(const char *from, const char *to)
{
#ifdef _WIN32
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
#else
    return rename(from, to);
#endif
}

static int remove_path(const char *p)
{
    stat_t st;
    if (STAT(p, &st) < 0)
        return -1;
#ifdef _WIN32
    return IS_DIR(st.st_mode) ? _rmdir(p) : _unlink(p);
#else
    return IS_DIR(st.st_mode) ? rmdir(p) : unlink(p);
#endif
}

/* ---------------- transfers ---------------- */

static xfer_t *find(int xfer)
{
    for (int i = 0; i < MAX_XFERS; i++)
        if (X[i].used && X[i].xfer == xfer)
            return &X[i];
    return NULL;
}

static xfer_t *alloc_xfer(int xfer)
{
    for (int i = 0; i < MAX_XFERS; i++)
        if (!X[i].used) {
            memset(&X[i], 0, sizeof X[i]);
            X[i].used = 1;
            X[i].xfer = xfer;
            return &X[i];
        }
    return NULL;
}

static void close_xfer(xfer_t *x, int drop_tmp)
{
    if (x->f)
        fclose(x->f);
    x->f = NULL;
    if (!x->pull) {
        mbedtls_sha256_free(&x->md);
        if (drop_tmp && x->tmp[0])
            remove(x->tmp);
    }
    free(x->pending);
    memset(x, 0, sizeof *x);
}

static int sha256_file(const char *path, char out[65])
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    mbedtls_sha256_context md;
    mbedtls_sha256_init(&md);
    mbedtls_sha256_starts(&md, 0);
    uint8_t buf[65536], h[32];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        mbedtls_sha256_update(&md, buf, n);
    fclose(f);
    mbedtls_sha256_finish(&md, h);
    mbedtls_sha256_free(&md);
    rd_hex(h, 32, out);
    return 0;
}

static void send_ctrl(const char *t, int xfer, const char *k, long long v)
{
    cJSON *m = msg_new(t, 0, NULL);
    cJSON_AddNumberToObject(msg_d(m), "xfer", xfer);
    if (k)
        cJSON_AddNumberToObject(msg_d(m), k, (double)v);
    session_send(m);
}

static void on_pump(struct ev_loop *loop, ev_timer *w, int revents)
{
    files_pump();
}

static void start_pump(void)
{
    if (!pump_inited) {
        ev_timer_init(&pump_timer, on_pump, 0.005, 0.005);
        pump_inited = 1;
    }
    if (!ev_is_active(&pump_timer))
        ev_timer_start(D.loop, &pump_timer);
}

void files_pump(void)
{
    rtc_t *r = rd_rtc_stack();
    int active = 0;
    for (int i = 0; i < MAX_XFERS; i++) {
        xfer_t *x = &X[i];
        if (!x->used || !x->pull)
            continue;
        active = 1;
        for (int k = 0; k < 64 && r && rtc_dc_buffered(r) < SEND_LIMIT; k++) {
            if (!x->pending) {
                x->pending = malloc(8 + CHUNK);
                size_t n = fread(x->pending + 8, 1, CHUNK, x->f);
                if (n == 0) {
                    free(x->pending);
                    x->pending = NULL;
                    int xfer = x->xfer;
                    close_xfer(x, 0);
                    send_ctrl("file.done", xfer, NULL, 0);
                    break;
                }
                rd_put_be32(x->pending, (uint32_t)x->xfer);
                rd_put_be32(x->pending + 4, x->next_seq++);
                x->pending_len = 8 + n;
            }
            if (rtc_dc_send(r, "file", 1, x->pending, x->pending_len) < 0)
                break; /* buffer full: again on the next tick */
            free(x->pending);
            x->pending = NULL;
        }
    }
    if (active)
        start_pump();
    else if (pump_inited && ev_is_active(&pump_timer))
        ev_timer_stop(D.loop, &pump_timer);
}

void files_chunk(const uint8_t *d, size_t n)
{
    if (n < 8)
        return;
    xfer_t *x = find((int)rd_get_be32(d));
    if (!x || x->pull || rd_get_be32(d + 4) != x->next_seq)
        return;
    size_t len = n - 8;
    if (fwrite(d + 8, 1, len, x->f) != len) {
        LOGW("files: write to %s failed", x->tmp);
        close_xfer(x, 1);
        return;
    }
    mbedtls_sha256_update(&x->md, d + 8, len);
    long long before = x->got;
    x->got += (long long)len;
    x->next_seq++;
    if (x->got / (256 * 1024) != before / (256 * 1024))
        send_ctrl("file.progress", x->xfer, "bytes", x->got);
}

void files_cancel_all(void)
{
    for (int i = 0; i < MAX_XFERS; i++)
        if (X[i].used)
            close_xfer(&X[i], 1);
    files_pump();
}

#define FAIL(c, ...)                                                                                                   \
    do {                                                                                                               \
        snprintf(code, codecap, "%s", c);                                                                              \
        snprintf(err, errcap, __VA_ARGS__);                                                                            \
        return NULL;                                                                                                   \
    } while (0)

static cJSON *list_dir(const char *path, char *code, size_t codecap, char *err, size_t errcap)
{
    cJSON *arr = cJSON_CreateArray();
#ifdef _WIN32
    char pat[1100];
    snprintf(pat, sizeof pat, "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        cJSON_Delete(arr);
        FAIL("not_found", "no such folder");
    }
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
            continue;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", fd.cFileName);
        cJSON_AddNumberToObject(e, "size", (double)((unsigned long long)fd.nFileSizeHigh << 32 | fd.nFileSizeLow));
        unsigned long long ft = (unsigned long long)fd.ftLastWriteTime.dwHighDateTime << 32 | fd.ftLastWriteTime.dwLowDateTime;
        cJSON_AddNumberToObject(e, "mtime", (double)(ft / 10000 - 11644473600000ULL));
        cJSON_AddBoolToObject(e, "dir", (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
        cJSON_AddItemToArray(arr, e);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *dir = opendir(path);
    if (!dir) {
        cJSON_Delete(arr);
        FAIL("not_found", "no such folder");
    }
    struct dirent *de;
    while ((de = readdir(dir))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        char full[2048];
        snprintf(full, sizeof full, "%s/%s", path, de->d_name);
        struct stat st;
        if (stat(full, &st) < 0)
            continue;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", de->d_name);
        cJSON_AddNumberToObject(e, "size", (double)st.st_size);
        cJSON_AddNumberToObject(e, "mtime", (double)st.st_mtime * 1000);
        cJSON_AddBoolToObject(e, "dir", S_ISDIR(st.st_mode));
        cJSON_AddItemToArray(arr, e);
    }
    closedir(dir);
#endif
    cJSON *r = cJSON_CreateObject();
    cJSON_AddItemToObject(r, "entries", arr);
    cJSON_AddStringToObject(r, "path", path);
    return r;
}

/* Returns the ack payload, or NULL with code/err set. */
cJSON *files_control(const char *t, cJSON *d, char *code, size_t codecap, char *err, size_t errcap)
{
    const char *in = msg_str(d, "path", "");
    char path[1024] = "";
    if (*in && !allowed(in, path, sizeof path) && strcmp(t, "file.push"))
        FAIL("forbidden", "the path must be inside your home folder (%s)", rd_home_dir());
    if (!strcmp(t, "file.list")) {
        if (!*in)
            normalize(rd_home_dir(), path, sizeof path);
        return list_dir(path, code, codecap, err, errcap);
    }
    if (!strcmp(t, "file.stat")) {
        stat_t st;
        if (STAT(path, &st) < 0)
            FAIL("not_found", "no such file");
        cJSON *r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, "size", (double)st.st_size);
        cJSON_AddNumberToObject(r, "mtime", (double)st.st_mtime * 1000);
        cJSON_AddBoolToObject(r, "dir", IS_DIR(st.st_mode));
        return r;
    }
    if (!strcmp(t, "file.delete")) {
        if (remove_path(path) < 0)
            FAIL("io_error", "cannot delete");
        return cJSON_CreateObject();
    }
    if (!strcmp(t, "file.mkdir")) {
        if (rd_mkdirs(path, 0755) < 0)
            FAIL("io_error", "cannot create");
        return cJSON_CreateObject();
    }
    if (!strcmp(t, "file.push")) {
        int xfer = (int)msg_num(d, "xfer", 0);
        long long size = (long long)msg_num(d, "size", 0);
        long long max_mb = desk_conf_int("files", "max_mb", 2048);
        if (size > max_mb * 1024 * 1024)
            FAIL("too_large", "over the %lld MB limit", max_mb);
        if (!*in || !path[0] || !allowed(in, path, sizeof path)) {
            /* a phone path or nothing: the upload folder, same file name */
            char dir[1024];
            upload_dir(dir, sizeof dir);
            snprintf(path, sizeof path, "%s%c%s", dir, RD_PATH_SEP, *in ? base_name(in) : "upload");
        }
        if (!*base_name(path))
            FAIL("bad_request", "no file name");
        if (msg_bool(d, "overwrite", desk_conf_bool("files", "overwrite", 0)) == 0)
            unique(path, sizeof path);
        char parent[1024];
        parent_of(path, parent, sizeof parent);
        rd_mkdirs(parent, 0755);
        xfer_t *x = alloc_xfer(xfer);
        if (!x)
            FAIL("busy", "too many transfers");
        snprintf(x->dest, sizeof x->dest, "%s", path);
        snprintf(x->tmp, sizeof x->tmp, "%s.part", path);
        x->f = fopen(x->tmp, "wb");
        if (!x->f) {
            x->tmp[0] = 0;
            close_xfer(x, 0);
            FAIL("io_error", "cannot write %s", path);
        }
        x->size = size;
        snprintf(x->sha, sizeof x->sha, "%s", msg_str(d, "sha256", ""));
        mbedtls_sha256_init(&x->md);
        mbedtls_sha256_starts(&x->md, 0);
        cJSON *r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, "xfer", xfer);
        cJSON_AddStringToObject(r, "path", path);
        return r;
    }
    if (!strcmp(t, "file.done")) {
        int xfer = (int)msg_num(d, "xfer", 0);
        xfer_t *x = find(xfer);
        if (!x || x->pull)
            FAIL("not_found", "unknown transfer");
        fclose(x->f);
        x->f = NULL;
        uint8_t h[32];
        char got[65];
        mbedtls_sha256_finish(&x->md, h);
        rd_hex(h, 32, got);
        if (x->got != x->size || (x->sha[0] && strcmp(x->sha, got))) {
            close_xfer(x, 1);
            FAIL("io_error", "size or checksum mismatch");
        }
        if (replace_file(x->tmp, x->dest) < 0) {
            close_xfer(x, 1);
            FAIL("io_error", "cannot move the file into place");
        }
        LOGI("files: received %s (%lld bytes)", x->dest, x->size);
        cJSON *r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, "xfer", xfer);
        cJSON_AddStringToObject(r, "path", x->dest);
        cJSON_AddBoolToObject(r, "verified", 1);
        x->tmp[0] = 0;
        close_xfer(x, 0);
        return r;
    }
    if (!strcmp(t, "file.cancel")) {
        xfer_t *x = find((int)msg_num(d, "xfer", 0));
        if (x)
            close_xfer(x, 1);
        return cJSON_CreateObject();
    }
    if (!strcmp(t, "file.pull")) {
        int xfer = (int)msg_num(d, "xfer", 0);
        stat_t st;
        if (STAT(path, &st) < 0 || !IS_REG(st.st_mode))
            FAIL("not_found", "no such file");
        xfer_t *x = alloc_xfer(xfer);
        if (!x)
            FAIL("busy", "too many transfers");
        x->pull = 1;
        x->f = fopen(path, "rb");
        if (!x->f) {
            close_xfer(x, 0);
            FAIL("io_error", "cannot read %s", path);
        }
        char sha[65] = "";
        if (st.st_size <= 64 * 1024 * 1024) /* larger ones would stall the agent thread */
            sha256_file(path, sha);
        cJSON *r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, "xfer", xfer);
        cJSON_AddNumberToObject(r, "size", (double)st.st_size);
        cJSON_AddStringToObject(r, "sha256", sha);
        start_pump(); /* first chunk on the next tick: after the viewer has the ack */
        return r;
    }
    FAIL("unsupported", "%s", t);
}
