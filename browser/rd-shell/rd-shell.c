/* rd-shell: the remote browser's controller bar, its window manager and the CloakBrowser supervisor,
 * in one small X client (remote-browser-plan.md, "Controller bar").
 *
 *   - The bar: a strip 44 CSS px tall across the top of the screen (88 px at 2x, 44 pt on the iPhone):
 *     back, forward, reload, the page's title (tap it to type a URL), a star that saves the page to the
 *     favourites (or takes it out), and a menu with Home, New tab, the tabs (listed only when there is
 *     more than one), Favourites, History, the user, New identity, Restart and Stop/Start, then the
 *     container's CPU, memory and disk. Taps arrive as the touch screen's emulated pointer events.
 *   - The pages: Users, History (with Clear history & data, Clear cookies) and Favourites (each with a
 *     close button that deletes it), drawn over the browser below the bar. A drag scrolls them.
 *   - The users (Alice … Frank by default): each has a profile of their own (cookies, site data), a
 *     fingerprint seed, the tabs they had open, a history and favourites, in <data>/<name>. Switching
 *     closes the browser completely and starts it again as the other user, with that user's tabs.
 *   - The window manager: the only one on the display. Every browser window is placed below the bar
 *     whatever it asks for, a fullscreen request (kiosk) included, and there are no frames.
 *   - The supervisor: starts the browser with the settings in /data/browser.conf, restarts it when it
 *     dies, and gives the user a new fingerprint seed and an empty profile on New identity. With no proxy
 *     set it refuses to start it, since the page would see this machine's own address.
 *
 * Back, forward and reload are keys through XTest (Alt+Left, Alt+Right, F5), as a person would press them.
 * The tabs go through DevTools on a pipe (--remote-debugging-pipe: no port a page could scan), and only
 * through browser-level calls: the Target calls to list, switch to, open and close a tab, Storage.clearCookies
 * and Browser.close. Nothing attaches to a page and no Runtime call is made, so a page has nothing to
 * notice. The history is rd-shell's own, from the tabs' addresses as DevTools reports them.
 *
 *   rd-shell [-c /data/browser.conf]          DISPLAY from the environment
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xatom.h>
#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

#include "cJSON.h"

#define LOG(...) (fprintf(stderr, "rd-shell: " __VA_ARGS__), fputc('\n', stderr))
#define MAX_CLIENTS 64

/* ---------------- settings: /data/browser.conf (key = value, # comments) ---------------- */

static struct {
    char path[PATH_MAX];
    char key[64][48], val[64][512];
    int n;
} C = { "/data/browser.conf" };

static void conf_load(void)
{
    C.n = 0;
    FILE *f = fopen(C.path, "r");
    if (!f) {
        LOG("%s: %s (using defaults)", C.path, strerror(errno));
        return;
    }
    char line[700];
    while (fgets(line, sizeof line, f) && C.n < 64) {
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        char *eq = strchr(p, '=');
        if (*p == '#' || !eq)
            continue;
        char *k = p, *v = eq + 1, *e = eq;
        while (e > k && (e[-1] == ' ' || e[-1] == '\t'))
            e--;
        *e = 0;
        while (*v == ' ' || *v == '\t')
            v++;
        size_t n = strlen(v);
        while (n && (v[n - 1] == '\n' || v[n - 1] == '\r' || v[n - 1] == ' ' || v[n - 1] == '\t'))
            v[--n] = 0;
        snprintf(C.key[C.n], sizeof C.key[0], "%s", k);
        snprintf(C.val[C.n], sizeof C.val[0], "%s", v);
        C.n++;
    }
    fclose(f);
}

static const char *conf(const char *k, const char *def)
{
    for (int i = C.n - 1; i >= 0; i--)
        if (!strcmp(C.key[i], k))
            return C.val[i];
    return def;
}

/* ---------------- the users: <data>/<name>/{profile/, seed, tabs, history, favourites} ---------------- */

#define MAX_USERS 8
#define URL_MAX 2048
#define MAX_TABS_OPEN 8 /* a user's tabs opened again: more would crowd the OECT's memory */

static struct {
    char name[MAX_USERS][32];
    int n, cur;
    char dir[PATH_MAX / 2]; /* <data> */
} U;

static const char *user(void) { return U.name[U.cur]; }

/* The setting for the user in use: "key.Name = …" before "key = …" (a proxy, timezone or language each). */
static const char *uconf(const char *k, const char *def)
{
    char uk[96];
    snprintf(uk, sizeof uk, "%s.%s", k, user());
    const char *v = conf(uk, NULL);
    return v ? v : conf(k, def);
}

static void upath(char *out, size_t cap, const char *file)
{
    snprintf(out, cap, "%s/%s/%s", U.dir, user(), file);
}

static void read_line(const char *path, char *out, size_t cap)
{
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    if (fgets(out, (int)cap, f))
        out[strcspn(out, "\r\n")] = 0;
    fclose(f);
}

/* A file is written whole beside itself, then renamed over the old one: a power cut leaves one or the other. */
static FILE *save_begin(const char *path, char *tmp, size_t cap)
{
    snprintf(tmp, cap, "%s.new", path);
    FILE *f = fopen(tmp, "w");
    if (!f)
        LOG("can't write %s: %s", tmp, strerror(errno));
    return f;
}

static void save_end(FILE *f, const char *tmp, const char *path)
{
    if (!f)
        return;
    int bad = fflush(f) != 0 || fsync(fileno(f)) != 0;
    if (fclose(f) || bad || rename(tmp, path)) {
        LOG("can't replace %s: %s", path, strerror(errno));
        unlink(tmp);
    }
}

static void write_line(const char *path, const char *s)
{
    char tmp[PATH_MAX + 8];
    FILE *f = save_begin(path, tmp, sizeof tmp);
    if (f)
        fprintf(f, "%s\n", s);
    save_end(f, tmp, path);
}

/* tabs and newlines would break the files' lines */
static void one_line(char *s)
{
    for (; *s; s++)
        if (*s == '\t' || *s == '\n' || *s == '\r')
            *s = ' ';
}

static int is_web(const char *url)
{
    return !strncmp(url, "http://", 7) || !strncmp(url, "https://", 8);
}

static int rm_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    if (remove(path))
        LOG("rm %s: %s", path, strerror(errno));
    return 0;
}

static void rm_tree(const char *path)
{
    if (strlen(path) > 1) /* never "/" */
        nftw(path, rm_entry, 16, FTW_DEPTH | FTW_PHYS | FTW_MOUNT);
}

/* users = Alice Bob …: the names are directory names, so letters, digits, - and _ only */
static void users_load(void)
{
    snprintf(U.dir, sizeof U.dir, "%s", conf("data", "/data/users"));
    char list[512], cur[64], p[PATH_MAX];
    snprintf(list, sizeof list, "%s", conf("users", "Alice Bob Charlie David Eve Frank"));
    U.n = U.cur = 0;
    for (char *save = NULL, *t = strtok_r(list, " ,", &save); t && U.n < MAX_USERS; t = strtok_r(NULL, " ,", &save)) {
        if (strlen(t) >= sizeof U.name[0] ||
            strspn(t, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") != strlen(t)) {
            LOG("users: \"%s\" left out (letters, digits, - and _ only)", t);
            continue;
        }
        snprintf(U.name[U.n++], sizeof U.name[0], "%s", t);
    }
    if (!U.n)
        snprintf(U.name[U.n++], sizeof U.name[0], "Default");
    if (mkdir(U.dir, 0700) && errno != EEXIST)
        LOG("mkdir %s: %s", U.dir, strerror(errno));
    snprintf(p, sizeof p, "%s/current", U.dir); /* the one in use when rd-shell last ran */
    read_line(p, cur, sizeof cur);
    for (int i = 0; i < U.n; i++)
        if (!strcmp(U.name[i], cur))
            U.cur = i;
}

/* ---------------- state ---------------- */

static Display *dpy;
static Window root, bar, menu, check;
static int W, H, S, B; /* screen size, scale, bar height */
static XftFont *font[2], *font_s[2]; /* the main size, the small one */
static XftDraw *bar_draw, *menu_draw;
static XftColor c_text, c_dim, c_ok, c_bad, c_star;
static unsigned long px_bg, px_field, px_line, px_press;
static XIM xim;
static XIC xic;

enum { A_WM_STATE, A_WM_PROTOCOLS, A_WM_DELETE, A_NET_SUPPORTED, A_NET_CHECK, A_NET_NAME, A_NET_STATE, A_NET_FULLSCREEN,
       A_NET_ACTIVE, A_NET_WORKAREA, A_NET_CLIENTS, A_NET_DESKTOPS, A_NET_DESKTOP, A_NET_GEOMETRY, A_NET_VIEWPORT,
       A_NET_TYPE, A_NET_TYPE_DOCK, A_NET_STRUT, A_UTF8, A_COUNT };
static Atom A[A_COUNT];
static const char *ATOM_NAMES[A_COUNT] = {
    "WM_STATE", "WM_PROTOCOLS", "WM_DELETE_WINDOW", "_NET_SUPPORTED", "_NET_SUPPORTING_WM_CHECK", "_NET_WM_NAME",
    "_NET_WM_STATE", "_NET_WM_STATE_FULLSCREEN", "_NET_ACTIVE_WINDOW", "_NET_WORKAREA", "_NET_CLIENT_LIST",
    "_NET_NUMBER_OF_DESKTOPS", "_NET_CURRENT_DESKTOP", "_NET_DESKTOP_GEOMETRY", "_NET_DESKTOP_VIEWPORT",
    "_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_DOCK", "_NET_WM_STRUT_PARTIAL", "UTF8_STRING",
};

static struct {
    Window w;
    int transient;
} clients[MAX_CLIENTS];
static int nclients;
static Window active;

static struct {
    pid_t pid;
    int want;        /* the browser should be running */
    char seed[40];   /* the user's fingerprint */
    int fails;       /* quick deaths in a row: the restart waits longer */
    time_t started;
    double restart_at;
    int stopping;    /* Browser.close sent: the tabs closing now are not the user closing them */
    int restore_n;   /* the user's tabs being opened: how many, and which goes in front once they are */
    char front[URL_MAX];
    double restore_until;
    char restored[MAX_TABS_OPEN][URL_MAX]; /* their addresses: opening them again isn't a visit */
    int nrestored;
} BR;

static char title[512];
static char message[200];   /* shown instead of the title: why the browser isn't running */
static char note[120];      /* shown instead of the title for a moment: what a tap did */
static double note_until;
static int editing;
static char edit[1024];
static int menu_open;

static struct {
    int fd;           /* a connect in progress */
    int ok;           /* -1 not known yet, 0 down, 1 up */
    double next, deadline;
} PX = { -1, -1, 0, 0 };

static volatile sig_atomic_t quit;
static int sigpipe[2];

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void draw_bar(void);
static void draw_menu(void);
static void menu_layout(void);
static void panel_refresh(void);

static void show_note(const char *s)
{
    snprintf(note, sizeof note, "%s", s);
    note_until = now_s() + 2.5;
    draw_bar();
}

/* ---------------- the user's history and favourites: time \t url \t title, one per line ---------------- */

#define MAX_HIST 1000
#define MAX_FAVS 200

static struct hist {
    time_t t;
    char url[URL_MAX], title[200];
} HI[MAX_HIST];         /* oldest first */
static int nhist, hist_dirty;
static double hist_save_at;

static struct {
    char url[URL_MAX], title[200];
} FV[MAX_FAVS];         /* in the order they were added */
static int nfavs;

static void hist_push(time_t t, const char *url, const char *ttl)
{
    if (nhist == MAX_HIST) { /* full: the oldest tenth goes */
        memmove(HI, HI + MAX_HIST / 10, sizeof HI[0] * (MAX_HIST - MAX_HIST / 10));
        nhist -= MAX_HIST / 10;
    }
    HI[nhist].t = t;
    snprintf(HI[nhist].url, sizeof HI[0].url, "%s", url);
    snprintf(HI[nhist].title, sizeof HI[0].title, "%s", ttl);
    one_line(HI[nhist].title);
    nhist++;
}

static void hist_changed(void)
{
    if (!hist_dirty)
        hist_save_at = now_s() + 3; /* a page loading changes it several times: one write for them */
    hist_dirty = 1;
    panel_refresh();
}

/* A tab went to url. The title comes later (hist_title): what DevTools says now may be the last page's. */
static void hist_add(const char *url)
{
    if (!is_web(url) || strlen(url) >= URL_MAX || strchr(url, '\t') || strchr(url, '\n'))
        return;
    if (nhist && !strcmp(HI[nhist - 1].url, url)) /* the same page again: a reload */
        return;
    hist_push(time(NULL), url, "");
    hist_changed();
}

/* A page's title for the history. Not one that is only its address, which Chromium shows while it loads. */
static void hist_title(const char *url, const char *ttl)
{
    const char *bare = url ? strstr(url, "://") : NULL;
    if (!url || !*url || !ttl || !*ttl || !strcmp(url, ttl) || (bare && !strcmp(bare + 3, ttl)))
        return;
    for (int i = nhist - 1; i >= 0 && i >= nhist - 50; i--)
        if (!strcmp(HI[i].url, url)) {
            if (strcmp(HI[i].title, ttl)) {
                snprintf(HI[i].title, sizeof HI[i].title, "%s", ttl);
                one_line(HI[i].title);
                hist_changed();
            }
            return;
        }
}

static void hist_save(void)
{
    char p[PATH_MAX], tmp[PATH_MAX + 8];
    hist_dirty = 0;
    upath(p, sizeof p, "history");
    FILE *f = save_begin(p, tmp, sizeof tmp);
    for (int i = 0; f && i < nhist; i++)
        fprintf(f, "%lld\t%s\t%s\n", (long long)HI[i].t, HI[i].url, HI[i].title);
    save_end(f, tmp, p);
}

static void favs_save(void)
{
    char p[PATH_MAX], tmp[PATH_MAX + 8];
    upath(p, sizeof p, "favourites");
    FILE *f = save_begin(p, tmp, sizeof tmp);
    for (int i = 0; f && i < nfavs; i++)
        fprintf(f, "%s\t%s\n", FV[i].url, FV[i].title);
    save_end(f, tmp, p);
}

/* history and favourites of the user in use, from their files */
static void lists_load(void)
{
    static char line[URL_MAX + 400];
    char p[PATH_MAX];
    nhist = nfavs = hist_dirty = 0;
    upath(p, sizeof p, "history");
    FILE *f = fopen(p, "r");
    while (f && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *u = strchr(line, '\t'), *t = u ? strchr(u + 1, '\t') : NULL;
        if (!t)
            continue;
        *u++ = 0;
        *t++ = 0;
        hist_push((time_t)atoll(line), u, t);
    }
    if (f)
        fclose(f);
    upath(p, sizeof p, "favourites");
    f = fopen(p, "r");
    while (f && fgets(line, sizeof line, f) && nfavs < MAX_FAVS) {
        line[strcspn(line, "\r\n")] = 0;
        char *t = strchr(line, '\t');
        if (t)
            *t++ = 0;
        if (!*line)
            continue;
        snprintf(FV[nfavs].url, sizeof FV[0].url, "%s", line);
        snprintf(FV[nfavs].title, sizeof FV[0].title, "%s", t ? t : "");
        nfavs++;
    }
    if (f)
        fclose(f);
}

static int fav_find(const char *url)
{
    for (int i = 0; url && i < nfavs; i++)
        if (!strcmp(FV[i].url, url))
            return i;
    return -1;
}

static void fav_delete(int i)
{
    if (i < 0 || i >= nfavs)
        return;
    memmove(&FV[i], &FV[i + 1], sizeof FV[0] * (size_t)(nfavs - i - 1));
    nfavs--;
    favs_save();
}

/* ---------------- the browser ---------------- */

static void random_seed(char *out, size_t cap)
{
    uint32_t r[2] = { (uint32_t)time(NULL), (uint32_t)getpid() };
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        if (read(fd, r, sizeof r) != (ssize_t)sizeof r)
            r[1] ^= (uint32_t)clock();
        close(fd);
    }
    snprintf(out, cap, "%u", r[0] ^ (r[1] << 1)); /* CloakBrowser takes a number */
}

/* host and port out of socks5://[user:pass@]host:port (or http://…) */
static int proxy_hostport(char *host, size_t hcap, char *port, size_t pcap)
{
    const char *p = uconf("proxy", "");
    const char *s = strstr(p, "://");
    s = s ? s + 3 : p;
    const char *at = strrchr(s, '@');
    if (at)
        s = at + 1;
    const char *end = s + strcspn(s, "/");
    const char *colon = NULL;
    if (*s == '[') { /* [v6]:port */
        const char *rb = memchr(s, ']', (size_t)(end - s));
        if (!rb)
            return -1;
        snprintf(host, hcap, "%.*s", (int)(rb - s - 1), s + 1);
        colon = rb[1] == ':' ? rb + 1 : NULL;
    } else {
        for (const char *q = s; q < end; q++)
            if (*q == ':')
                colon = q;
        snprintf(host, hcap, "%.*s", (int)((colon ? colon : end) - s), s);
    }
    if (!colon || !*host)
        return -1;
    snprintf(port, pcap, "%.*s", (int)(end - colon - 1), colon + 1);
    return 0;
}

/* urls: the tabs to open (none: the home page) */
static char **browser_argv(const char *const *urls, int nurls, int devtools)
{
    static char args[40 + MAX_TABS_OPEN][URL_MAX];
    static char *argv[48 + MAX_TABS_OPEN];
    int n = 0;
#define ARG(...) snprintf(args[n], sizeof args[n], __VA_ARGS__), argv[n] = args[n], n++
    ARG("%s", conf("browser", "/opt/cloakbrowser/chrome"));
    ARG("--fingerprint-platform=%s", uconf("platform", "windows"));
    ARG("--fingerprint=%s", BR.seed);
    const char *proxy = uconf("proxy", "");
    if (*proxy) {
        char host[256], port[16];
        ARG("--proxy-server=%s", proxy);
        if (!proxy_hostport(host, sizeof host, port, sizeof port)) /* no lookups but the proxy's own name */
            ARG("--host-resolver-rules=MAP * ~NOTFOUND , EXCLUDE %s", host);
        ARG("--force-webrtc-ip-handling-policy=disable_non_proxied_udp");
        ARG("--fingerprint-webrtc-ip=%s", uconf("webrtc_ip", "auto")); /* the proxy's exit address, not ours */
    }
    ARG("--kiosk");
    ARG("--force-device-scale-factor=%d", S);
    ARG("--touch-events=enabled");
    char profile[PATH_MAX];
    upath(profile, sizeof profile, "profile");
    ARG("--user-data-dir=%s", profile);
    ARG("--disk-cache-dir=%s", conf("cache", "/tmp/rd-cache")); /* tmpfs: emptied when the user changes */
    ARG("--disable-dev-shm-usage");
    /* No GPU here, but a page without WebGL looks like a bot: SwiftShader draws it on the CPU.
     * webgl = 0 turns the GPU process off instead (cheaper on the OECT's cores). */
    if (strcmp(conf("webgl", "1"), "0"))
        ARG("--enable-unsafe-swiftshader");
    else
        ARG("--disable-gpu");
    ARG("--renderer-process-limit=%s", conf("renderer_process_limit", "2"));
    ARG("--disk-cache-size=52428800");
    ARG("--no-first-run");
    ARG("--no-default-browser-check");
    if (devtools)
        ARG("--remote-debugging-pipe"); /* fds 3 and 4: see spawn */
    const char *lang = uconf("lang", ""), *tz = uconf("timezone", "");
    if (*lang) {
        ARG("--lang=%s", lang);
        ARG("--fingerprint-locale=%s", lang);
    }
    if (*tz)
        ARG("--fingerprint-timezone=%s", tz); /* CloakBrowser's own; TZ (spawn) is for a plain Chromium */
    /* extra flags, space-separated */
    static char extra[512];
    snprintf(extra, sizeof extra, "%s", conf("args", ""));
    for (char *save = NULL, *t = strtok_r(extra, " ", &save); t && n < 44; t = strtok_r(NULL, " ", &save))
        ARG("%s", t);
    for (int i = 0; i < nurls && i < MAX_TABS_OPEN; i++)
        ARG("%s", urls[i]);
    if (!nurls)
        ARG("%s", uconf("home", "about:blank"));
#undef ARG
    argv[n] = NULL;
    return argv;
}

/* With to/from >= 0 the child gets them as fds 3 (what it reads) and 4 (what it writes), DevTools' pipe. */
static pid_t spawn(char **argv, int to, int from)
{
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        if (to >= 0) { /* out of the way first: either may already be 3 or 4 */
            int a = fcntl(to, F_DUPFD_CLOEXEC, 10), b = fcntl(from, F_DUPFD_CLOEXEC, 10);
            if (a < 0 || b < 0 || dup2(a, 3) < 0 || dup2(b, 4) < 0)
                _exit(127);
        }
        const char *tz = uconf("timezone", ""), *lang = uconf("lang", "");
        if (*tz)
            setenv("TZ", tz, 1);
        if (*lang) {
            setenv("LANGUAGE", lang, 1);
            char l[64];
            snprintf(l, sizeof l, "%s.UTF-8", lang);
            for (char *c = l; *c && *c != '.'; c++)
                if (*c == '-')
                    *c = '_';
            setenv("LANG", l, 1);
        }
        execv(argv[0], argv);
        fprintf(stderr, "rd-shell: exec %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    if (pid < 0)
        LOG("fork: %s", strerror(errno));
    return pid;
}

/* ---------------- the tabs: DevTools' Target calls over the pipe ---------------- */

#define MAX_TABS 16

static struct {
    int wfd, rfd;       /* to the browser, from it; -1 when it isn't running */
    char buf[1 << 16];  /* what came so far: messages end with a NUL */
    size_t n;
    int id;             /* the last request's */
    int create_id;      /* the createTarget whose answer closes `replace` (one tab) */
    int list_id;        /* the getTargets asked for when the menu opens: the titles as they are now */
    char replace[40];
    struct {
        char id[40], title[200], url[URL_MAX];
    } tab[MAX_TABS];
    int ntabs;          /* page tabs, in the order they opened; -1: not known (no DevTools) */
    char cur[40];       /* the one in front, as far as can be told */
    int dirty;          /* the user's tabs file is behind: written at save_at */
    double save_at;
} T = { .wfd = -1, .rfd = -1, .ntabs = -1 };

static int cdp_send(const char *method, cJSON *params)
{
    if (T.wfd < 0) {
        cJSON_Delete(params);
        return -1;
    }
    cJSON *m = cJSON_CreateObject();
    cJSON_AddNumberToObject(m, "id", ++T.id);
    cJSON_AddStringToObject(m, "method", method);
    if (params)
        cJSON_AddItemToObject(m, "params", params);
    char *s = cJSON_PrintUnformatted(m);
    cJSON_Delete(m);
    size_t len = s ? strlen(s) + 1 : 0, off = 0; /* the NUL ends the message */
    while (off < len) {
        ssize_t w = write(T.wfd, s + off, len - off);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            break;
        off += (size_t)w;
    }
    free(s);
    return len && off == len ? T.id : -1;
}

static cJSON *target(const char *key, const char *val)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, key, val);
    return p;
}

static int tab_find(const char *id)
{
    for (int i = 0; i < T.ntabs; i++)
        if (!strcmp(T.tab[i].id, id))
            return i;
    return -1;
}

static const char *cur_url(void)
{
    int i = T.cur[0] ? tab_find(T.cur) : -1;
    return i >= 0 && T.tab[i].url[0] ? T.tab[i].url : NULL;
}

static void tabs_changed(void)
{
    if (menu_open) {
        menu_layout();
        draw_menu();
    }
    if (!BR.stopping && T.ntabs > 0) {
        if (!T.dirty)
            T.save_at = now_s() + 2;
        T.dirty = 1;
    }
    draw_bar(); /* the star: is the page in front a favourite */
}

/* The user's open tabs, one URL a line, the one in front marked "* ": opened again on the next start. */
static void tabs_save(void)
{
    char p[PATH_MAX], tmp[PATH_MAX + 8];
    T.dirty = 0;
    if (T.ntabs <= 0) /* not known: what the file says stays */
        return;
    upath(p, sizeof p, "tabs");
    FILE *f = save_begin(p, tmp, sizeof tmp);
    for (int i = 0; f && i < T.ntabs; i++)
        if (T.tab[i].url[0] && strncmp(T.tab[i].url, "devtools:", 9) && !strchr(T.tab[i].url, '\n'))
            fprintf(f, "%s%s\n", strcmp(T.tab[i].id, T.cur) ? "" : "* ", T.tab[i].url);
    save_end(f, tmp, p);
}

static void tab_select(int i);

/* Target.targetCreated / targetInfoChanged: a page tab, new, retitled or gone to another address. A new one is
 * in front: tabs a page opens (target=_blank, window.open) and the ones opened from the bar all come up in
 * front. Each new address goes into the history. */
static void tab_info(const cJSON *info, int created)
{
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(info, "type"));
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(info, "targetId"));
    const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(info, "title"));
    const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(info, "url"));
    if (!type || strcmp(type, "page") || !id)
        return;
    int i = tab_find(id);
    if (i < 0) {
        if (T.ntabs >= MAX_TABS)
            return;
        i = T.ntabs++;
        snprintf(T.tab[i].id, sizeof T.tab[i].id, "%s", id);
        T.tab[i].url[0] = 0;
    }
    snprintf(T.tab[i].title, sizeof T.tab[i].title, "%s", t && *t ? t : url ? url : "");
    if (url && strcmp(T.tab[i].url, url)) {
        int again = 0; /* a tab of the user's opened again at the start */
        for (int k = 0; !is_web(T.tab[i].url) && k < BR.nrestored && now_s() < BR.restore_until; k++)
            again |= !strcmp(BR.restored[k], url);
        snprintf(T.tab[i].url, sizeof T.tab[i].url, "%s", url);
        if (!again)
            hist_add(url);
    } else if (url) {
        hist_title(url, t);
    }
    if (created)
        snprintf(T.cur, sizeof T.cur, "%s", id);
    if (BR.restore_n && T.ntabs >= BR.restore_n) /* the user's tabs are all open: the one they left in front */
        for (int k = 0; k < T.ntabs; k++)
            if (!strcmp(T.tab[k].url, BR.front)) {
                BR.restore_n = 0;
                tab_select(k);
                break;
            }
    tabs_changed();
}

static void tab_gone(const char *id)
{
    int i = id ? tab_find(id) : -1;
    if (i < 0)
        return;
    memmove(&T.tab[i], &T.tab[i + 1], sizeof T.tab[0] * (size_t)(T.ntabs - i - 1));
    T.ntabs--;
    if (!strcmp(T.cur, id)) /* the browser picks the next one; the window title says which (tab_match) */
        snprintf(T.cur, sizeof T.cur, "%s", T.ntabs ? T.tab[T.ntabs - 1].id : "");
    tabs_changed();
}

/* The window's title is the front tab's ("Title - Chromium"): when the tab thought to be in front doesn't
 * match it and another does, that one is (a tab closed, or Ctrl+Tab from the iPhone's keyboard). */
static void tab_match(const char *wtitle)
{
    int best = -1;
    for (int i = 0; i < T.ntabs; i++) {
        size_t l = strlen(T.tab[i].title);
        if (!l || strncmp(wtitle, T.tab[i].title, l) || (wtitle[l] && strncmp(wtitle + l, " - ", 3)))
            continue;
        if (!strcmp(T.tab[i].id, T.cur))
            return;
        best = i;
    }
    if (best >= 0) {
        snprintf(T.cur, sizeof T.cur, "%s", T.tab[best].id);
        tabs_changed();
    }
}

static void cdp_message(char *msg)
{
    cJSON *m = cJSON_Parse(msg);
    if (!m)
        return;
    const char *method = cJSON_GetStringValue(cJSON_GetObjectItem(m, "method"));
    cJSON *params = cJSON_GetObjectItem(m, "params");
    if (method && !strcmp(method, "Target.targetCreated"))
        tab_info(cJSON_GetObjectItem(params, "targetInfo"), 1);
    else if (method && !strcmp(method, "Target.targetInfoChanged"))
        tab_info(cJSON_GetObjectItem(params, "targetInfo"), 0);
    else if (method && !strcmp(method, "Target.targetDestroyed"))
        tab_gone(cJSON_GetStringValue(cJSON_GetObjectItem(params, "targetId")));
    else if (!method && T.list_id && cJSON_GetNumberValue(cJSON_GetObjectItem(m, "id")) == T.list_id) {
        T.list_id = 0; /* titles only: which tabs exist is the events' business */
        const cJSON *info;
        cJSON_ArrayForEach(info, cJSON_GetObjectItem(cJSON_GetObjectItem(m, "result"), "targetInfos"))
        {
            const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(info, "targetId"));
            if (id && tab_find(id) >= 0)
                tab_info(info, 0);
        }
    } else if (!method && T.create_id && cJSON_GetNumberValue(cJSON_GetObjectItem(m, "id")) == T.create_id) {
        T.create_id = 0; /* the new tab exists (or failed to): only then is the old one closed */
        if (cJSON_GetObjectItem(m, "result") && T.replace[0] && tab_find(T.replace) >= 0)
            cdp_send("Target.closeTarget", target("targetId", T.replace));
        T.replace[0] = 0;
    } else if (!method && cJSON_GetObjectItem(m, "error")) {
        char *e = cJSON_PrintUnformatted(cJSON_GetObjectItem(m, "error"));
        LOG("devtools: %s", e ? e : "error");
        free(e);
    }
    cJSON_Delete(m);
}

static void cdp_close(void)
{
    if (T.dirty && !BR.stopping) /* the browser died: its tabs as they were last */
        tabs_save();
    T.dirty = 0;
    if (T.wfd >= 0)
        close(T.wfd);
    if (T.rfd >= 0)
        close(T.rfd);
    T.wfd = T.rfd = -1;
    T.n = 0;
    T.ntabs = -1;
    T.cur[0] = T.replace[0] = 0;
    T.create_id = T.list_id = 0;
    tabs_changed();
}

static void cdp_read(void)
{
    for (;;) {
        ssize_t r = read(T.rfd, T.buf + T.n, sizeof T.buf - 1 - T.n);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0 && errno == EAGAIN)
            break;
        if (r <= 0) { /* the browser went */
            cdp_close();
            return;
        }
        T.n += (size_t)r;
        T.buf[T.n] = 0;
        char *p = T.buf, *end;
        while ((end = memchr(p, 0, (size_t)(T.buf + T.n - p))) && end < T.buf + T.n) {
            cdp_message(p);
            p = end + 1;
        }
        T.n -= (size_t)(p - T.buf);
        memmove(T.buf, p, T.n);
        if (T.n >= sizeof T.buf - 1) { /* one message too big for the buffer: none of ours is */
            LOG("devtools: message over %zu bytes, dropped", sizeof T.buf);
            T.n = 0;
        }
    }
}

static void tab_select(int i)
{
    if (i < 0 || i >= T.ntabs)
        return;
    snprintf(T.cur, sizeof T.cur, "%s", T.tab[i].id);
    cdp_send("Target.activateTarget", target("targetId", T.tab[i].id));
}

static void tab_close_current(void)
{
    if (T.ntabs > 1 && T.cur[0]) /* never the last one: that would end the browser */
        cdp_send("Target.closeTarget", target("targetId", T.cur));
}

static void browser_start(void)
{
    if (BR.pid > 0)
        return;
    BR.want = 1;
    message[0] = 0;
    if (!*uconf("proxy", "") && strcmp(conf("allow_direct", "0"), "1")) {
        snprintf(message, sizeof message, "No proxy set in %s", C.path);
        LOG("%s: not starting the browser (allow_direct = 1 to start it anyway)", message);
        draw_bar();
        return;
    }
    /* the user's tabs from last time; after quick deaths in a row only the home page, in case one of them
     * is what kills the browser */
    char (*urls)[URL_MAX] = BR.restored;
    const char *u[MAX_TABS_OPEN];
    char tabs[PATH_MAX], line[URL_MAX + 4];
    int nurls = 0;
    BR.front[0] = 0;
    upath(tabs, sizeof tabs, "tabs");
    FILE *f = BR.fails < 3 ? fopen(tabs, "r") : NULL;
    while (f && nurls < MAX_TABS_OPEN && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        const char *url = !strncmp(line, "* ", 2) ? line + 2 : line;
        if (!*url)
            continue;
        if (url != line)
            snprintf(BR.front, sizeof BR.front, "%s", url);
        snprintf(urls[nurls], sizeof urls[0], "%s", url);
        u[nurls] = urls[nurls];
        nurls++;
    }
    if (f)
        fclose(f);
    BR.nrestored = nurls;
    BR.restore_n = nurls > 1 && BR.front[0] ? nurls : 0;
    BR.restore_until = now_s() + 15;
    int to[2] = { -1, -1 }, from[2] = { -1, -1 };
    int devtools = !pipe2(to, O_CLOEXEC) && !pipe2(from, O_CLOEXEC);
    BR.pid = spawn(browser_argv(u, nurls, devtools), devtools ? to[0] : -1, devtools ? from[1] : -1);
    if (to[0] >= 0)
        close(to[0]);
    if (from[1] >= 0)
        close(from[1]);
    if (devtools && BR.pid > 0) {
        T.wfd = to[1];
        T.rfd = from[0];
        fcntl(T.rfd, F_SETFL, O_NONBLOCK);
        T.ntabs = 0;
        cJSON *p = cJSON_CreateObject();
        cJSON_AddBoolToObject(p, "discover", 1);
        cdp_send("Target.setDiscoverTargets", p); /* every tab there is, then each change */
    } else {
        if (to[1] >= 0)
            close(to[1]);
        if (from[0] >= 0)
            close(from[0]);
    }
    BR.started = time(NULL);
    LOG("browser started for %s (pid %d, seed %s, %d tab%s)", user(), (int)BR.pid, BR.seed, nurls ? nurls : 1,
        nurls > 1 ? "s" : "");
    draw_bar();
}

/* Waits up to ms for the browser to exit, without reaping it: until it is reaped its pid can't be reused,
 * so the SIGKILL to its process group can't reach anyone else's. */
static int browser_wait(int ms)
{
    for (int i = 0; i < ms / 50; i++) {
        siginfo_t si = { 0 };
        if (!waitid(P_PID, (id_t)BR.pid, &si, WEXITED | WNOHANG | WNOWAIT) && si.si_pid == BR.pid)
            return 1;
        usleep(50000);
    }
    return 0;
}

/* Browser.close first: Chromium writes its cookies and closes the profile as on a normal quit. A signal if
 * that doesn't do, and at the end SIGKILL for whatever is left of its process group (renderers, the zygote):
 * nothing of the user keeps running. */
static void browser_stop(void)
{
    BR.want = 0;
    BR.restart_at = 0;
    if (BR.pid <= 0)
        return;
    tabs_save(); /* before Browser.close closes them one by one */
    BR.stopping = 1;
    int exited = (cdp_send("Browser.close", NULL) > 0 && browser_wait(8000)) ||
                 (kill(-BR.pid, SIGTERM) == 0 && browser_wait(5000));
    kill(-BR.pid, SIGKILL);
    waitpid(BR.pid, NULL, 0);
    BR.pid = 0;
    cdp_close();
    BR.stopping = 0;
    BR.restore_n = 0;
    LOG("browser stopped%s", exited ? "" : " (killed)");
}

/* the user's fingerprint seed: made the first time and kept in their directory */
static void seed_load(int fresh)
{
    char p[PATH_MAX];
    upath(p, sizeof p, "seed");
    if (!fresh)
        read_line(p, BR.seed, sizeof BR.seed);
    if (!BR.seed[0] || fresh || strspn(BR.seed, "0123456789") != strlen(BR.seed)) {
        random_seed(BR.seed, sizeof BR.seed);
        write_line(p, BR.seed);
    }
}

/* Makes U.cur the user in use: their directory, fingerprint, history and favourites. The disk cache is
 * shared by everyone, so it is emptied: one user's cache must not show up in another's pages. */
static void user_open(void)
{
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/%s", U.dir, user());
    if (mkdir(p, 0700) && errno != EEXIST)
        LOG("mkdir %s: %s", p, strerror(errno));
    BR.seed[0] = 0;
    seed_load(0);
    lists_load();
    rm_tree(conf("cache", "/tmp/rd-cache"));
    snprintf(p, sizeof p, "%s/current", U.dir);
    write_line(p, user());
    const char *tz = uconf("timezone", ""); /* the history's times in the user's zone */
    if (*tz)
        setenv("TZ", tz, 1);
    else
        unsetenv("TZ");
    tzset();
    PX.ok = -1; /* the user may have a proxy of their own */
    PX.next = 0;
    LOG("user %s: %d in the history, %d favourites", user(), nhist, nfavs);
}

static void user_switch(int i)
{
    if (i < 0 || i >= U.n || i == U.cur)
        return;
    browser_stop();
    if (hist_dirty)
        hist_save();
    U.cur = i;
    user_open();
    title[0] = 0;
    BR.fails = 0;
    browser_start();
    char s[80];
    snprintf(s, sizeof s, "Now %s", user());
    show_note(s);
}

/* the user's profile, the cache and the user's tabs: the next start is an empty browser */
static void profile_wipe(void)
{
    char p[PATH_MAX];
    upath(p, sizeof p, "profile");
    rm_tree(p);
    rm_tree(conf("cache", "/tmp/rd-cache"));
    upath(p, sizeof p, "tabs");
    unlink(p);
}

/* New identity: a new fingerprint and an empty profile for the user; the history and favourites are
 * rd-shell's own, never seen by a page, and stay. */
static void new_identity(void)
{
    browser_stop();
    profile_wipe();
    seed_load(1);
    LOG("new identity for %s: seed %s, empty profile", user(), BR.seed);
    BR.fails = 0;
    browser_start();
}

/* Clear history & data: the history, and everything a site left (cookies, storage, cache); the fingerprint
 * and the favourites stay. */
static void clear_all(void)
{
    browser_stop();
    profile_wipe();
    nhist = 0;
    hist_save();
    LOG("%s: history and site data cleared", user());
    BR.fails = 0;
    browser_start();
    show_note("History and data cleared");
}

/* Clear cookies: through DevTools while the browser runs (its tabs stay); without it, the cookie files go
 * with the browser stopped. */
static void clear_cookies(void)
{
    if (T.wfd < 0 || cdp_send("Storage.clearCookies", NULL) < 0) {
        char p[PATH_MAX];
        int was = BR.pid > 0;
        browser_stop();
        upath(p, sizeof p, "profile/Default/Cookies");
        unlink(p);
        upath(p, sizeof p, "profile/Default/Cookies-journal");
        unlink(p);
        if (was)
            browser_start();
    }
    LOG("%s: cookies cleared", user());
    show_note("Cookies cleared");
}

/* Kiosk has no address bar to type into, so a URL opens in a new tab (Target.createTarget), and with one_tab
 * the tab it replaces is closed once the new one exists: closing the last tab would end the browser.
 *
 * Without DevTools it goes through a second copy of the browser (its process singleton), which opens it in
 * a new tab. Once the title changes, the new tab is in front and there are two: the first is closed. */
static struct {
    double until; /* waiting for the new tab's title, until then */
    char before[512];
} OPEN;

/* replace: with one_tab, the tab in front gives way to the new one (a typed URL, Home); New tab keeps it */
static void browser_open(const char *url, int replace)
{
    if (BR.pid <= 0) {
        browser_start();
        return;
    }
    int one = replace && strcmp(conf("one_tab", "1"), "0") != 0;
    if (T.wfd >= 0) {
        int id = cdp_send("Target.createTarget", target("url", url));
        if (id > 0 && one && T.cur[0]) {
            T.create_id = id;
            snprintf(T.replace, sizeof T.replace, "%s", T.cur);
        }
        return;
    }
    if (spawn(browser_argv(&url, 1, 0), -1, -1) > 0 && one) {
        OPEN.until = now_s() + 5;
        snprintf(OPEN.before, sizeof OPEN.before, "%s", title);
    }
}

static void reap(void)
{
    int st;
    pid_t p;
    siginfo_t si = { 0 };
    /* the browser died: what is left of its process group (renderers, the zygote) goes too, before its
     * profile is used again; it isn't reaped yet, so its pid can't be someone else's */
    if (BR.pid > 0 && !waitid(P_PID, (id_t)BR.pid, &si, WEXITED | WNOHANG | WNOWAIT) && si.si_pid == BR.pid)
        kill(-BR.pid, SIGKILL);
    while ((p = waitpid(-1, &st, WNOHANG)) > 0) {
        if (p != BR.pid)
            continue; /* a URL handed over */
        BR.pid = 0;
        cdp_close();
        LOG("browser exited (%s %d)", WIFSIGNALED(st) ? "signal" : "status",
            WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st));
        if (!BR.want)
            continue;
        BR.fails = time(NULL) - BR.started < 30 ? BR.fails + 1 : 0;
        int wait = BR.fails ? (1 << (BR.fails < 5 ? BR.fails : 5)) : 1; /* 1, 2, 4 … 32 s */
        BR.restart_at = now_s() + wait;
        snprintf(message, sizeof message, "The browser stopped; starting again in %d s", wait);
        draw_bar();
    }
}

/* ---------------- the proxy, every 10 s: does it accept a connection ---------------- */

static void probe_start(void)
{
    PX.next = now_s() + 10;
    char host[256], port[16];
    if (!*uconf("proxy", "") || proxy_hostport(host, sizeof host, port, sizeof port))
        return;
    struct addrinfo hints = { .ai_socktype = SOCK_STREAM }, *ai = NULL;
    if (getaddrinfo(host, port, &hints, &ai) || !ai) {
        PX.ok = 0;
        return;
    }
    PX.fd = socket(ai->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (PX.fd >= 0 && connect(PX.fd, ai->ai_addr, ai->ai_addrlen) < 0 && errno != EINPROGRESS) {
        close(PX.fd);
        PX.fd = -1;
        PX.ok = 0;
    }
    freeaddrinfo(ai);
    PX.deadline = now_s() + 3;
}

static void probe_done(int ok)
{
    if (PX.fd >= 0)
        close(PX.fd);
    PX.fd = -1;
    if (PX.ok != ok) {
        PX.ok = ok;
        LOG("proxy %s", ok ? "reachable" : "unreachable");
        draw_bar();
        if (menu_open)
            draw_menu();
    }
}

/* ---------------- drawing ---------------- */

/* fs: a pair, the main font and the CJK one for what it lacks (font, or font_s: the small size) */
static XftFont *font_for(XftFont **fs, FcChar32 c)
{
    if (fs[0] && XftCharExists(dpy, fs[0], c))
        return fs[0];
    if (fs[1] && XftCharExists(dpy, fs[1], c))
        return fs[1];
    return fs[0];
}

static int utf8_next(const char *s, FcChar32 *c)
{
    int n = FcUtf8ToUcs4((const FcChar8 *)s, c, (int)strlen(s));
    return n > 0 ? n : 1;
}

/* width of s in pixels, or draws it at x (baseline y) when d is given; stops at maxw with an ellipsis */
static int text_in(XftFont **fs, XftDraw *d, XftColor *col, int x, int y, const char *s, int maxw)
{
    int w = 0;
    XGlyphInfo gi;
    XftTextExtentsUtf8(dpy, fs[0], (const FcChar8 *)"…", (int)strlen("…"), &gi);
    int ell = gi.xOff;
    for (const char *p = s; *p;) {
        FcChar32 c;
        int n = utf8_next(p, &c);
        XftFont *f = font_for(fs, c);
        XftTextExtentsUtf8(dpy, f, (const FcChar8 *)p, n, &gi);
        if (maxw > 0 && w + gi.xOff > maxw - (p[n] ? ell : 0)) {
            if (d)
                XftDrawStringUtf8(d, col, fs[0], x + w, y, (const FcChar8 *)"…", (int)strlen("…"));
            return w + ell;
        }
        if (d)
            XftDrawStringUtf8(d, col, f, x + w, y, (const FcChar8 *)p, n);
        w += gi.xOff;
        p += n;
    }
    return w;
}

static int text(XftDraw *d, XftColor *col, int x, int y, const char *s, int maxw)
{
    return text_in(font, d, col, x, y, s, maxw);
}

static void icon_triangle(Drawable dr, GC gc, int cx, int cy, int r, int dir)
{
    XPoint p[3] = { { (short)(cx + dir * r / 2), (short)cy }, /* the tip, pointing dir */
                    { (short)(cx - dir * r / 2), (short)(cy - r) },
                    { (short)(cx - dir * r / 2), (short)(cy + r) } };
    XFillPolygon(dpy, dr, gc, p, 3, Convex, CoordModeOrigin);
}

/* five points around (cx, cy): filled for a favourite, an outline otherwise */
static void icon_star(Drawable dr, GC gc, int cx, int cy, int r, int filled)
{
    XPoint p[11];
    for (int i = 0; i < 10; i++) {
        double a = -M_PI / 2 + i * M_PI / 5, rr = i % 2 ? r * 0.45 : r;
        p[i].x = (short)lround(cx + rr * cos(a));
        p[i].y = (short)lround(cy + rr * sin(a));
    }
    p[10] = p[0];
    if (filled)
        XFillPolygon(dpy, dr, gc, p, 10, Nonconvex, CoordModeOrigin);
    else
        XDrawLines(dpy, dr, gc, p, 11, CoordModeOrigin);
}

/* The pages drawn over the browser, below the bar. Drawn into a pixmap first: a drag redraws the whole
 * page for every move, and the video would show it half drawn. */
enum { P_NONE, P_USERS, P_HISTORY, P_FAVS };
static const char *PAGE_NAME[] = { "", "Users", "History", "Favourites" };

static struct {
    int kind;
    Window w;
    Pixmap pm;
    XftDraw *d;
    int scroll, total;            /* the list's offset at the top, its height */
    int down, down_y, down_scroll, moved; /* a finger on it: where it started, and has it dragged */
    int confirm;                  /* a Clear row tapped once: a second tap within 4 s does it */
    double confirm_until;
    int utabs[MAX_USERS], ufavs[MAX_USERS];
} P;

/* The page's own title: the window's, without the browser's name after it. */
static void page_title(char *out, size_t cap)
{
    static const char *names[] = { "Chromium", "Google Chrome", "Chrome", "CloakBrowser" };
    snprintf(out, cap, "%s", title);
    char *last = NULL;
    for (char *p = strstr(out, " - "); p; p = strstr(p + 1, " - "))
        last = p;
    for (size_t i = 0; last && i < sizeof names / sizeof names[0]; i++)
        if (!strcmp(last + 3, names[i]))
            *last = 0;
}

static void draw_bar(void)
{
    if (!bar)
        return;
    GC gc = DefaultGC(dpy, DefaultScreen(dpy));
    XSetForeground(dpy, gc, px_bg);
    XFillRectangle(dpy, bar, gc, 0, 0, (unsigned)W, (unsigned)B);
    XSetForeground(dpy, gc, px_line);
    XFillRectangle(dpy, bar, gc, 0, B - S, (unsigned)W, (unsigned)S);
    int r = B / 6, cy = B / 2, on = BR.pid > 0, page = P.kind != P_NONE;
    XSetForeground(dpy, gc, on || page ? c_text.pixel : c_dim.pixel);
    icon_triangle(bar, gc, B / 2, cy, r, -1);       /* back; with a page open, it closes the page */
    XSetForeground(dpy, gc, on && !page ? c_text.pixel : c_dim.pixel);
    icon_triangle(bar, gc, B + B / 2, cy, r, 1);    /* forward */
    XSetLineAttributes(dpy, gc, (unsigned)(S * 2), LineSolid, CapRound, JoinRound);
    XDrawArc(dpy, bar, gc, 2 * B + B / 2 - r, cy - r, (unsigned)(2 * r), (unsigned)(2 * r), 90 * 64, 270 * 64);
    XPoint head[3] = { { (short)(2 * B + B / 2), (short)(cy - r - r / 2) },
                       { (short)(2 * B + B / 2 + r / 2 + S), (short)(cy - r) },
                       { (short)(2 * B + B / 2), (short)(cy - r + r / 2) } };
    XFillPolygon(dpy, bar, gc, head, 3, Convex, CoordModeOrigin); /* reload */
    const char *url = cur_url();
    int fav = url && fav_find(url) >= 0;
    XSetForeground(dpy, gc, fav ? c_star.pixel : on && !page && url && is_web(url) ? c_text.pixel : c_dim.pixel);
    icon_star(bar, gc, W - B - B / 2, cy, B / 5, fav);
    XSetLineAttributes(dpy, gc, 0, LineSolid, CapButt, JoinMiter);
    XSetForeground(dpy, gc, c_text.pixel);
    for (int i = -1; i <= 1; i++) /* the menu */
        XFillArc(dpy, bar, gc, W - B / 2 + i * r - S * 2, cy - S * 2, (unsigned)(S * 4), (unsigned)(S * 4), 0, 360 * 64);

    /* the field: the title, or what is being typed */
    int fx = 3 * B + S * 2, fw = W - 5 * B - S * 4, fh = B - S * 14, fy = (B - fh) / 2;
    XSetForeground(dpy, gc, editing ? px_press : px_field);
    XFillRectangle(dpy, bar, gc, fx, fy, (unsigned)fw, (unsigned)fh);
    int dot = S * 4, base = cy + (font[0]->ascent - font[0]->descent) / 2, tx = fx + S * 6;
    int maxw = fw - S * 12 - dot * 2;
    if (editing) {
        int w = text(NULL, NULL, 0, 0, edit, 0);
        int shift = w > maxw ? w - maxw : 0; /* keep the end of what is typed in sight */
        XRectangle clip = { (short)tx, (short)fy, (unsigned short)maxw, (unsigned short)fh };
        XftDrawSetClipRectangles(bar_draw, 0, 0, &clip, 1);
        if (*edit)
            text(bar_draw, &c_text, tx - shift, base, edit, 0);
        else
            text(bar_draw, &c_dim, tx, base, "Search or type a URL", maxw);
        XftDrawSetClip(bar_draw, NULL);
        XSetForeground(dpy, gc, c_text.pixel);
        XFillRectangle(dpy, bar, gc, tx + (*edit ? w - shift : 0) + S, fy + S * 6, (unsigned)S, (unsigned)(fh - S * 12));
    } else if (note[0]) {
        text(bar_draw, &c_ok, tx, base, note, maxw);
    } else if (page) {
        char s[80];
        snprintf(s, sizeof s, "%s · %s", PAGE_NAME[P.kind], user());
        text(bar_draw, &c_text, tx, base, P.kind == P_USERS ? PAGE_NAME[P.kind] : s, maxw);
    } else if (message[0]) {
        text(bar_draw, &c_bad, tx, base, message, maxw);
    } else {
        char pt[sizeof title];
        page_title(pt, sizeof pt);
        text(bar_draw, on ? &c_text : &c_dim, tx, base, pt[0] ? pt : on ? "Loading…" : "Stopped — tap ⋯ to start", maxw);
    }
    /* the proxy: green when it answers, red when it doesn't */
    XftColor *pc = PX.ok > 0 ? &c_ok : PX.ok == 0 ? &c_bad : &c_dim;
    XSetForeground(dpy, gc, pc->pixel);
    XFillArc(dpy, bar, gc, fx + fw - S * 6 - dot * 2, cy - dot, (unsigned)(dot * 2), (unsigned)(dot * 2), 0, 360 * 64);
    XFlush(dpy);
}

/* ---------------- the container's CPU, memory and disk, in the menu ---------------- */

static struct {
    double next;
    unsigned long long busy, total; /* the last CPU reading: time used, time there was, in one unit */
    int cpu;                        /* percent; -1 until there are two readings */
    unsigned long long mem, mem_max, disk, disk_max;
} ST = { .cpu = -1 };

/* A number from a cgroup or /proc file: the first in it, or the one after key. 0 when there is none, as for
 * a limit of "max". */
static int read_num(const char *path, const char *key, unsigned long long *v)
{
    char line[256];
    FILE *f = fopen(path, "r");
    int ok = 0;
    size_t kl = key ? strlen(key) : 0;
    while (f && !ok && fgets(line, sizeof line, f))
        if (!key || (!strncmp(line, key, kl) && (line[kl] == ' ' || line[kl] == '\t')))
            ok = sscanf(line + kl, " %llu", v) == 1;
    if (f)
        fclose(f);
    return ok;
}

/* In the container, its own cgroup: CPU time against what its share of the cores could do, and memory
 * without the page cache it can give back (as `docker stats` counts). Outside one, the whole machine's.
 * The disk is the one the users' profiles are on. */
static void stats_sample(void)
{
    unsigned long long v, busy = 0, total = 0, avail = 0;
    if (read_num("/sys/fs/cgroup/cpu.stat", "usage_usec", &v)) {
        double cores = (double)sysconf(_SC_NPROCESSORS_ONLN);
        char q[32];
        unsigned long long per = 0;
        FILE *f = fopen("/sys/fs/cgroup/cpu.max", "r");
        if (f && fscanf(f, "%31s %llu", q, &per) == 2 && strcmp(q, "max") && per)
            cores = strtod(q, NULL) / (double)per;
        if (f)
            fclose(f);
        busy = v;
        total = (unsigned long long)(now_s() * 1e6 * cores);
    } else {
        unsigned long long c[8] = { 0 };
        FILE *f = fopen("/proc/stat", "r");
        if (f && fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &c[0], &c[1], &c[2], &c[3], &c[4], &c[5],
                        &c[6], &c[7]) >= 5) {
            for (int i = 0; i < 8; i++)
                total += c[i];
            busy = total - c[3] - c[4]; /* all but idle and iowait */
        }
        if (f)
            fclose(f);
    }
    if (ST.total && total > ST.total && busy >= ST.busy) {
        int pc = (int)(100.0 * (double)(busy - ST.busy) / (double)(total - ST.total) + 0.5);
        ST.cpu = pc > 100 ? 100 : pc;
    }
    ST.busy = busy;
    ST.total = total;

    if (read_num("/sys/fs/cgroup/memory.current", NULL, &v)) {
        unsigned long long inactive = 0;
        read_num("/sys/fs/cgroup/memory.stat", "inactive_file", &inactive);
        ST.mem = v > inactive ? v - inactive : v;
        if (!read_num("/sys/fs/cgroup/memory.max", NULL, &ST.mem_max) && read_num("/proc/meminfo", "MemTotal:", &v))
            ST.mem_max = v * 1024;
    } else if (read_num("/proc/meminfo", "MemTotal:", &v) && read_num("/proc/meminfo", "MemAvailable:", &avail)) {
        ST.mem_max = v * 1024;
        ST.mem = (v - avail) * 1024;
    }
    struct statvfs fs;
    if (!statvfs(U.dir, &fs)) {
        ST.disk_max = (unsigned long long)fs.f_blocks * fs.f_frsize;
        ST.disk = (unsigned long long)(fs.f_blocks - fs.f_bfree) * fs.f_frsize;
    }
}

static void size_str(unsigned long long v, char *out, size_t cap)
{
    if (v < 1024ULL * 1024 * 1024)
        snprintf(out, cap, "%llu MB", v >> 20);
    else
        snprintf(out, cap, "%.1f GB", (double)v / (1024.0 * 1024 * 1024));
}

/* The menu's rows. The tabs are listed only when there is more than one, with Close tab under them; one tab
 * shows none of it. Without DevTools the count isn't known, and the blind keys stand in (Ctrl+Tab, Ctrl+W).
 * The last three only show the container's CPU, memory and disk. */
enum { M_HOME, M_NEW_TAB, M_TAB, M_CLOSE_TAB, M_NEXT_TAB, M_FAVS, M_HISTORY, M_USER, M_NEW_ID, M_RESTART,
       M_STARTSTOP, M_PROXY, M_CPU, M_MEM, M_DISK };

static struct {
    int kind, tab;
} items[MAX_TABS + 16];
static int nitems;

static int menu_w(void) { return W * 3 / 5; }

static void menu_layout(void)
{
    nitems = 0;
#define ITEM(k, t) (items[nitems].kind = (k), items[nitems].tab = (t), nitems++)
    ITEM(M_HOME, -1);
    if (BR.pid > 0)
        ITEM(M_NEW_TAB, -1);
    if (T.ntabs > 1) {
        int room = (H - B) / B - 13; /* the rows that fit on the screen beside the others */
        for (int i = 0; i < T.ntabs && i < room; i++)
            ITEM(M_TAB, i);
        ITEM(M_CLOSE_TAB, -1);
    } else if (T.ntabs < 0 && BR.pid > 0) {
        ITEM(M_NEXT_TAB, -1);
        ITEM(M_CLOSE_TAB, -1);
    }
    ITEM(M_FAVS, -1);
    ITEM(M_HISTORY, -1);
    ITEM(M_USER, -1);
    ITEM(M_NEW_ID, -1);
    ITEM(M_RESTART, -1);
    ITEM(M_STARTSTOP, -1);
    ITEM(M_PROXY, -1);
    ITEM(M_CPU, -1);
    ITEM(M_MEM, -1);
    ITEM(M_DISK, -1);
#undef ITEM
    XResizeWindow(dpy, menu, (unsigned)menu_w(), (unsigned)(B * nitems));
}

static void menu_label(int i, char *out, size_t cap)
{
    switch (items[i].kind) {
    case M_HOME: snprintf(out, cap, "Home"); break;
    case M_NEW_TAB: snprintf(out, cap, "New tab"); break;
    case M_TAB: snprintf(out, cap, "%s", T.tab[items[i].tab].title[0] ? T.tab[items[i].tab].title : "New tab"); break;
    case M_NEXT_TAB: snprintf(out, cap, "Next tab"); break;
    case M_CLOSE_TAB: snprintf(out, cap, "Close tab"); break;
    case M_FAVS: snprintf(out, cap, "Favourites"); break;
    case M_HISTORY: snprintf(out, cap, "History"); break;
    case M_USER: snprintf(out, cap, "User: %s", user()); break;
    case M_NEW_ID: snprintf(out, cap, "New identity"); break;
    case M_RESTART: snprintf(out, cap, "Restart browser"); break;
    case M_STARTSTOP: snprintf(out, cap, BR.pid > 0 ? "Stop browser" : "Start browser"); break;
    case M_PROXY:
        snprintf(out, cap, "Proxy: %s", !*uconf("proxy", "") ? "none set" : PX.ok > 0 ? "ok" : PX.ok == 0 ? "down" : "…");
        break;
    case M_CPU: snprintf(out, cap, "CPU"); break;
    case M_MEM: snprintf(out, cap, "Memory"); break;
    case M_DISK: snprintf(out, cap, "Disk"); break;
    }
}

/* a gauge row: the name, the value at the right, and a bar under them, amber past 70 %, red past 90 % */
static void draw_gauge(int i, const char *name, const char *value, double frac)
{
    GC gc = DefaultGC(dpy, DefaultScreen(dpy));
    int mw = menu_w(), x = S * 12, y = i * B, gw = mw - 2 * x, gy = y + B - S * 12;
    int base = y + B / 2 - S * 4 + (font[0]->ascent - font[0]->descent) / 2;
    text(menu_draw, &c_dim, x, base, name, gw / 2);
    int vw = text_in(font_s, NULL, NULL, 0, 0, value, 0);
    text_in(font_s, menu_draw, &c_text, mw - x - vw, base, value, 0);
    XSetForeground(dpy, gc, px_line);
    XFillRectangle(dpy, menu, gc, x, gy, (unsigned)gw, (unsigned)(S * 3));
    if (frac > 0) {
        XSetForeground(dpy, gc, frac < 0.7 ? c_ok.pixel : frac < 0.9 ? c_star.pixel : c_bad.pixel);
        XFillRectangle(dpy, menu, gc, x, gy, (unsigned)(gw * (frac < 1 ? frac : 1)), (unsigned)(S * 3));
    }
}

static void draw_menu(void)
{
    if (!menu_open)
        return;
    GC gc = DefaultGC(dpy, DefaultScreen(dpy));
    int mw = menu_w();
    XSetForeground(dpy, gc, px_field);
    XFillRectangle(dpy, menu, gc, 0, 0, (unsigned)mw, (unsigned)(B * nitems));
    for (int i = 0; i < nitems; i++) {
        char l[220], v[64], a[24], b[24];
        menu_label(i, l, sizeof l);
        int kind = items[i].kind, tab = kind == M_TAB, front = tab && !strcmp(T.tab[items[i].tab].id, T.cur);
        if (front) { /* the tab in front */
            XSetForeground(dpy, gc, px_press);
            XFillRectangle(dpy, menu, gc, 0, i * B, (unsigned)mw, (unsigned)B);
            XSetForeground(dpy, gc, c_ok.pixel);
            XFillRectangle(dpy, menu, gc, 0, i * B, (unsigned)(S * 3), (unsigned)B);
        }
        if (kind == M_CPU) {
            snprintf(v, sizeof v, ST.cpu < 0 ? "…" : "%d %%", ST.cpu);
            draw_gauge(i, l, v, ST.cpu / 100.0);
        } else if (kind == M_MEM || kind == M_DISK) {
            unsigned long long used = kind == M_MEM ? ST.mem : ST.disk, max = kind == M_MEM ? ST.mem_max : ST.disk_max;
            size_str(used, a, sizeof a);
            size_str(max, b, sizeof b);
            snprintf(v, sizeof v, max ? "%s of %s" : "…", a, b);
            draw_gauge(i, l, v, max ? (double)used / (double)max : 0);
        } else {
            int base = i * B + B / 2 + (font[0]->ascent - font[0]->descent) / 2, x = tab ? S * 24 : S * 12;
            text(menu_draw, kind == M_PROXY ? &c_dim : &c_text, x, base, l, mw - x - S * 12);
        }
        int after_front = i && items[i - 1].kind == M_TAB && !strcmp(T.tab[items[i - 1].tab].id, T.cur);
        if (i && !front && !after_front) { /* no line against the highlighted row */
            XSetForeground(dpy, gc, px_line);
            XFillRectangle(dpy, menu, gc, S * 12, i * B, (unsigned)(mw - S * 12), (unsigned)(kind == M_CPU ? S * 3 : S));
        }
    }
    XFlush(dpy);
}

static void menu_show(int on)
{
    if (on == menu_open)
        return;
    menu_open = on;
    if (on) {
        /* the events carry a tab's title only when it navigates, not when the page sets it */
        if (T.ntabs > 1)
            T.list_id = cdp_send("Target.getTargets", NULL);
        stats_sample(); /* the CPU needs a second reading: in a second, then every two */
        ST.next = now_s() + 1;
        menu_layout();
        XMapRaised(dpy, menu);
        draw_menu();
        /* a tap anywhere else closes it: the grab sends it here */
        XGrabPointer(dpy, menu, True, ButtonPressMask | ButtonReleaseMask, GrabModeAsync, GrabModeAsync, None, None,
                     CurrentTime);
    } else {
        XUngrabPointer(dpy, CurrentTime);
        XUnmapWindow(dpy, menu);
    }
    XFlush(dpy);
}

/* ---------------- the pages: Users, History, Favourites ---------------- */

enum { R_HEAD, R_CLEAR_ALL, R_CLEAR_COOKIES, R_USER, R_HIST, R_FAV, R_EMPTY };

static struct {
    int kind, ref, y, h; /* ref: the user, history entry or favourite; y: from the top of the list */
} rows[MAX_HIST * 2 + 8];
static int nrows;

static void panel_clamp(void)
{
    int max_scroll = P.total - (H - B);
    if (P.scroll > max_scroll)
        P.scroll = max_scroll;
    if (P.scroll < 0)
        P.scroll = 0;
}

static void panel_layout(void)
{
    int y = 0, tall = B + B / 3, day = -1;
    nrows = 0;
#define ROW(k, r, hh) (rows[nrows].kind = (k), rows[nrows].ref = (r), rows[nrows].y = y, rows[nrows].h = (hh), \
                       y += (hh), nrows++)
    switch (P.kind) {
    case P_USERS:
        for (int i = 0; i < U.n; i++)
            ROW(R_USER, i, tall);
        break;
    case P_HISTORY: /* newest first, under a heading for each day */
        ROW(R_CLEAR_ALL, 0, B);
        ROW(R_CLEAR_COOKIES, 0, B);
        if (!nhist)
            ROW(R_EMPTY, 0, B);
        for (int i = nhist - 1; i >= 0; i--) {
            struct tm tm;
            localtime_r(&HI[i].t, &tm);
            if (tm.tm_year * 400 + tm.tm_yday != day) {
                day = tm.tm_year * 400 + tm.tm_yday;
                ROW(R_HEAD, i, B * 2 / 3);
            }
            ROW(R_HIST, i, tall);
        }
        break;
    case P_FAVS: /* the newest first */
        if (!nfavs)
            ROW(R_EMPTY, 0, B);
        for (int i = nfavs - 1; i >= 0; i--)
            ROW(R_FAV, i, tall);
        break;
    }
#undef ROW
    P.total = y;
    panel_clamp();
}

static void day_name(time_t t, char *out, size_t cap)
{
    struct tm a, b, c;
    time_t now = time(NULL), yesterday = now - 86400;
    localtime_r(&t, &a);
    localtime_r(&now, &b);
    localtime_r(&yesterday, &c);
    if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday)
        snprintf(out, cap, "Today");
    else if (a.tm_year == c.tm_year && a.tm_yday == c.tm_yday)
        snprintf(out, cap, "Yesterday");
    else
        strftime(out, cap, a.tm_year == b.tm_year ? "%A, %e %B" : "%A, %e %B %Y", &a);
}

static const char *no_scheme(const char *url)
{
    const char *u = strstr(url, "://");
    return u ? u + 3 : url;
}

static void panel_draw(void)
{
    if (!P.kind)
        return;
    GC gc = DefaultGC(dpy, DefaultScreen(dpy));
    int ph = H - B, x = S * 12, tw = W - 2 * x;
    int confirm = P.confirm && now_s() < P.confirm_until ? P.confirm : 0;
    XSetForeground(dpy, gc, px_bg);
    XFillRectangle(dpy, P.pm, gc, 0, 0, (unsigned)W, (unsigned)ph);
    for (int i = 0; i < nrows; i++) {
        int y = rows[i].y - P.scroll, h = rows[i].h, ref = rows[i].ref, kind = rows[i].kind;
        if (y + h <= 0 || y >= ph)
            continue;
        int mid = y + h / 2;
        int one = mid + (font[0]->ascent - font[0]->descent) / 2; /* a row of one line: its baseline */
        int top = mid - S * 2, low = mid + font_s[0]->ascent + S * 3; /* of two lines */
        char s[300], hm[16];
        struct tm tm;
        switch (kind) {
        case R_HEAD:
            day_name(HI[ref].t, s, sizeof s);
            text_in(font_s, P.d, &c_dim, x, y + h - S * 8, s, tw);
            break;
        case R_CLEAR_ALL:
            text(P.d, &c_bad, x, one, confirm == kind ? "Tap again: history, cookies, site data, cache" : "Clear history & data", tw);
            break;
        case R_CLEAR_COOKIES:
            text(P.d, &c_bad, x, one, confirm == kind ? "Tap again: every site's cookies" : "Clear cookies", tw);
            break;
        case R_EMPTY:
            text(P.d, &c_dim, x, one, P.kind == P_FAVS ? "None yet: the star on the bar adds the page" : "No history yet", tw);
            break;
        case R_USER:
            if (ref == U.cur) {
                XSetForeground(dpy, gc, px_press);
                XFillRectangle(dpy, P.pm, gc, 0, y, (unsigned)W, (unsigned)h);
                XSetForeground(dpy, gc, c_ok.pixel);
                XFillRectangle(dpy, P.pm, gc, 0, y, (unsigned)(S * 3), (unsigned)h);
            }
            text(P.d, &c_text, x, top, U.name[ref], tw);
            snprintf(s, sizeof s, "%s%d tab%s · %d favourite%s", ref == U.cur ? "In use · " : "", P.utabs[ref],
                     P.utabs[ref] == 1 ? "" : "s", P.ufavs[ref], P.ufavs[ref] == 1 ? "" : "s");
            text_in(font_s, P.d, &c_dim, x, low, s, tw);
            break;
        case R_HIST:
            localtime_r(&HI[ref].t, &tm);
            strftime(hm, sizeof hm, "%H:%M", &tm);
            text(P.d, &c_text, x, top, HI[ref].title[0] ? HI[ref].title : no_scheme(HI[ref].url), tw);
            snprintf(s, sizeof s, "%s · %s", hm, no_scheme(HI[ref].url));
            text_in(font_s, P.d, &c_dim, x, low, s, tw);
            break;
        case R_FAV: { /* and a close button at the right, which deletes it */
            int cx = W - B / 2, r = B / 9;
            text(P.d, &c_text, x, top, FV[ref].title[0] ? FV[ref].title : no_scheme(FV[ref].url), tw - B);
            text_in(font_s, P.d, &c_dim, x, low, no_scheme(FV[ref].url), tw - B);
            XSetForeground(dpy, gc, c_dim.pixel);
            XSetLineAttributes(dpy, gc, (unsigned)(S * 2), LineSolid, CapRound, JoinRound);
            XDrawLine(dpy, P.pm, gc, cx - r, mid - r, cx + r, mid + r);
            XDrawLine(dpy, P.pm, gc, cx - r, mid + r, cx + r, mid - r);
            XSetLineAttributes(dpy, gc, 0, LineSolid, CapButt, JoinMiter);
            break;
        }
        }
        if (kind != R_HEAD && i + 1 < nrows && rows[i + 1].kind != R_HEAD) { /* a line under it */
            XSetForeground(dpy, gc, px_line);
            XFillRectangle(dpy, P.pm, gc, x, y + h - S, (unsigned)(W - x), (unsigned)S);
        }
    }
    XCopyArea(dpy, P.pm, P.w, gc, 0, 0, (unsigned)W, (unsigned)ph, 0, 0);
    XFlush(dpy);
}

static int count_lines(const char *path)
{
    FILE *f = fopen(path, "r");
    int n = 0, c, last = '\n';
    while (f && (c = getc(f)) != EOF) {
        n += c == '\n';
        last = c;
    }
    if (f)
        fclose(f);
    return n + (last != '\n');
}

static void panel_show(int kind)
{
    if (kind == P_USERS) /* each user's tabs and favourites, as their files say */
        for (int i = 0; i < U.n; i++) {
            char p[PATH_MAX];
            snprintf(p, sizeof p, "%s/%s/tabs", U.dir, U.name[i]);
            P.utabs[i] = i == U.cur && T.ntabs >= 0 ? T.ntabs : count_lines(p);
            snprintf(p, sizeof p, "%s/%s/favourites", U.dir, U.name[i]);
            P.ufavs[i] = i == U.cur ? nfavs : count_lines(p);
        }
    P.kind = kind;
    P.scroll = P.confirm = P.down = 0;
    if (kind) {
        panel_layout();
        XMapRaised(dpy, P.w);
        if (menu_open)
            XRaiseWindow(dpy, menu);
        panel_draw();
    } else {
        XUnmapWindow(dpy, P.w);
    }
    draw_bar();
}

static void panel_refresh(void)
{
    if (P.kind) {
        panel_layout();
        panel_draw();
    }
}

static void panel_tap(int x, int y)
{
    int i = 0;
    while (i < nrows && y >= rows[i].y + rows[i].h)
        i++;
    if (i >= nrows || y < rows[i].y)
        return;
    int kind = rows[i].kind, ref = rows[i].ref;
    char url[URL_MAX];
    switch (kind) {
    case R_CLEAR_ALL:
    case R_CLEAR_COOKIES:
        if (P.confirm == kind && now_s() < P.confirm_until) {
            P.confirm = 0;
            if (kind == R_CLEAR_ALL) {
                clear_all();
                panel_refresh();
            } else {
                clear_cookies();
                panel_draw();
            }
        } else {
            P.confirm = kind;
            P.confirm_until = now_s() + 4;
            panel_draw();
        }
        break;
    case R_USER:
        panel_show(P_NONE);
        user_switch(ref);
        break;
    case R_HIST:
        snprintf(url, sizeof url, "%s", HI[ref].url);
        panel_show(P_NONE);
        browser_open(url, 1);
        break;
    case R_FAV:
        if (x >= W - B) {
            fav_delete(ref);
            panel_refresh();
            draw_bar();
        } else {
            snprintf(url, sizeof url, "%s", FV[ref].url);
            panel_show(P_NONE);
            browser_open(url, 1);
        }
        break;
    }
}

/* The star: the page in front into the favourites, or out of them. */
static void fav_toggle(void)
{
    const char *url = cur_url();
    int i = fav_find(url);
    if (i >= 0) {
        fav_delete(i);
        show_note("Removed from favourites");
        return;
    }
    if (!url || !is_web(url) || strlen(url) >= URL_MAX || strchr(url, '\t')) {
        show_note(T.wfd < 0 ? "Favourites need the browser's tab list" : "Only a web page can be a favourite");
        return;
    }
    if (nfavs == MAX_FAVS) {
        show_note("The favourites are full");
        return;
    }
    snprintf(FV[nfavs].url, sizeof FV[0].url, "%s", url);
    page_title(FV[nfavs].title, sizeof FV[0].title);
    one_line(FV[nfavs].title);
    nfavs++;
    favs_save();
    show_note("Added to favourites");
}

/* ---------------- the window manager ---------------- */

static int find(Window w)
{
    for (int i = 0; i < nclients; i++)
        if (clients[i].w == w)
            return i;
    return -1;
}

static void publish_clients(void)
{
    Window l[MAX_CLIENTS];
    for (int i = 0; i < nclients; i++)
        l[i] = clients[i].w;
    XChangeProperty(dpy, root, A[A_NET_CLIENTS], XA_WINDOW, 32, PropModeReplace, (unsigned char *)l, nclients);
}

static void read_title(void)
{
    title[0] = 0;
    if (!active)
        return;
    Atom type;
    int fmt;
    unsigned long n, after;
    unsigned char *p = NULL;
    if (XGetWindowProperty(dpy, active, A[A_NET_NAME], 0, 1024, False, A[A_UTF8], &type, &fmt, &n, &after, &p) ==
            Success && p && n) {
        snprintf(title, sizeof title, "%s", (char *)p);
    } else {
        XTextProperty tp;
        if (XGetWMName(dpy, active, &tp) && tp.value) {
            snprintf(title, sizeof title, "%s", (char *)tp.value);
            XFree(tp.value);
        }
    }
    if (p)
        XFree(p);
}

static void focus(Window w)
{
    active = w;
    if (w) {
        XRaiseWindow(dpy, w);
        XSetInputFocus(dpy, w, RevertToPointerRoot, CurrentTime);
    }
    XChangeProperty(dpy, root, A[A_NET_ACTIVE], XA_WINDOW, 32, PropModeReplace, (unsigned char *)&w, w ? 1 : 0);
    if (P.kind)
        XRaiseWindow(dpy, P.w);
    if (menu_open)
        XRaiseWindow(dpy, menu);
    read_title();
    draw_bar();
}

/* Top-level windows fill the area below the bar; a transient (a dialog) keeps its size, centred in it. */
static void place(Window w, int transient)
{
    int ah = H - B;
    if (!transient) {
        XMoveResizeWindow(dpy, w, 0, B, (unsigned)W, (unsigned)ah);
    } else {
        XWindowAttributes a;
        if (!XGetWindowAttributes(dpy, w, &a))
            return;
        int ww = a.width < W ? a.width : W, wh = a.height < ah ? a.height : ah;
        XMoveResizeWindow(dpy, w, (W - ww) / 2, B + (ah - wh) / 2, (unsigned)ww, (unsigned)wh);
    }
    XSetWindowBorderWidth(dpy, w, 0);
}

static void manage(Window w)
{
    XWindowAttributes a;
    if (!XGetWindowAttributes(dpy, w, &a) || a.override_redirect || find(w) >= 0 || w == bar || w == menu || w == P.w)
        return;
    if (nclients == MAX_CLIENTS) {
        XMapWindow(dpy, w);
        return;
    }
    Window tf = None;
    int transient = XGetTransientForHint(dpy, w, &tf) && tf;
    clients[nclients].w = w;
    clients[nclients].transient = transient;
    nclients++;
    XSelectInput(dpy, w, PropertyChangeMask | StructureNotifyMask);
    place(w, transient);
    long state[2] = { NormalState, None };
    XChangeProperty(dpy, w, A[A_WM_STATE], A[A_WM_STATE], 32, PropModeReplace, (unsigned char *)state, 2);
    XMapWindow(dpy, w);
    publish_clients();
    focus(w);
}

static void unmanage(Window w)
{
    int i = find(w);
    if (i < 0)
        return;
    clients[i] = clients[--nclients];
    publish_clients();
    if (active == w)
        focus(nclients ? clients[nclients - 1].w : None);
}

/* _NET_WM_STATE: kiosk asks for fullscreen; it gets the state and keeps the area below the bar */
static void net_state(Window w, long action, Atom a1, Atom a2)
{
    if (a1 != A[A_NET_FULLSCREEN] && a2 != A[A_NET_FULLSCREEN])
        return;
    Atom type;
    int fmt;
    unsigned long n = 0, after;
    unsigned char *p = NULL;
    int on = 0;
    if (XGetWindowProperty(dpy, w, A[A_NET_STATE], 0, 32, False, XA_ATOM, &type, &fmt, &n, &after, &p) == Success && p)
        for (unsigned long i = 0; i < n; i++)
            on |= ((Atom *)p)[i] == A[A_NET_FULLSCREEN];
    if (p)
        XFree(p);
    on = action == 1 ? 1 : action == 0 ? 0 : !on;
    XChangeProperty(dpy, w, A[A_NET_STATE], XA_ATOM, 32, PropModeReplace, (unsigned char *)&A[A_NET_FULLSCREEN],
                    on ? 1 : 0);
    int i = find(w);
    place(w, i >= 0 && clients[i].transient);
}

static void ewmh_setup(void)
{
    check = XCreateSimpleWindow(dpy, root, -1, -1, 1, 1, 0, 0, 0);
    XChangeProperty(dpy, root, A[A_NET_CHECK], XA_WINDOW, 32, PropModeReplace, (unsigned char *)&check, 1);
    XChangeProperty(dpy, check, A[A_NET_CHECK], XA_WINDOW, 32, PropModeReplace, (unsigned char *)&check, 1);
    XChangeProperty(dpy, check, A[A_NET_NAME], A[A_UTF8], 8, PropModeReplace, (unsigned char *)"rd-shell", 8);
    Atom sup[] = { A[A_NET_SUPPORTED], A[A_NET_CHECK], A[A_NET_NAME], A[A_NET_STATE], A[A_NET_FULLSCREEN],
                   A[A_NET_ACTIVE], A[A_NET_WORKAREA], A[A_NET_CLIENTS], A[A_NET_DESKTOPS], A[A_NET_DESKTOP],
                   A[A_NET_GEOMETRY], A[A_NET_VIEWPORT] };
    XChangeProperty(dpy, root, A[A_NET_SUPPORTED], XA_ATOM, 32, PropModeReplace, (unsigned char *)sup,
                    (int)(sizeof sup / sizeof sup[0]));
    long one = 1, zero[2] = { 0, 0 }, geom[2] = { W, H }, area[4] = { 0, B, W, H - B };
    XChangeProperty(dpy, root, A[A_NET_DESKTOPS], XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&one, 1);
    XChangeProperty(dpy, root, A[A_NET_DESKTOP], XA_CARDINAL, 32, PropModeReplace, (unsigned char *)zero, 1);
    XChangeProperty(dpy, root, A[A_NET_GEOMETRY], XA_CARDINAL, 32, PropModeReplace, (unsigned char *)geom, 2);
    XChangeProperty(dpy, root, A[A_NET_VIEWPORT], XA_CARDINAL, 32, PropModeReplace, (unsigned char *)zero, 2);
    XChangeProperty(dpy, root, A[A_NET_WORKAREA], XA_CARDINAL, 32, PropModeReplace, (unsigned char *)area, 4);
}

static int another_wm;
static int on_wm_error(Display *d, XErrorEvent *e)
{
    if (e->error_code == BadAccess)
        another_wm = 1;
    return 0;
}
static int on_error(Display *d, XErrorEvent *e) { return 0; } /* windows vanish under us; that's fine */

/* ---------------- input: the bar's taps, typing a URL ---------------- */

static void send_keys(const KeySym *mods, int nmods, KeySym key)
{
    if (!active)
        return;
    XSetInputFocus(dpy, active, RevertToPointerRoot, CurrentTime);
    for (int i = 0; i < nmods; i++)
        XTestFakeKeyEvent(dpy, XKeysymToKeycode(dpy, mods[i]), True, CurrentTime);
    KeyCode k = XKeysymToKeycode(dpy, key);
    XTestFakeKeyEvent(dpy, k, True, CurrentTime);
    XTestFakeKeyEvent(dpy, k, False, CurrentTime);
    for (int i = nmods - 1; i >= 0; i--)
        XTestFakeKeyEvent(dpy, XKeysymToKeycode(dpy, mods[i]), False, CurrentTime);
    XFlush(dpy);
}

static void edit_begin(void)
{
    editing = 1;
    edit[0] = 0;
    XSetInputFocus(dpy, bar, RevertToPointerRoot, CurrentTime);
    if (xic)
        XSetICFocus(xic);
    draw_bar();
}

static void edit_end(int go)
{
    if (!editing)
        return;
    editing = 0;
    if (xic)
        XUnsetICFocus(xic);
    if (go && *edit) {
        char url[1400];
        const char *e = edit;
        while (*e == ' ')
            e++;
        int has_scheme = strstr(e, "://") || !strncmp(e, "about:", 6) || !strncmp(e, "data:", 5);
        int looks_like_host = !strchr(e, ' ') && (strchr(e, '.') || !strncmp(e, "localhost", 9));
        /* an address of its own (localhost, 192.168.…) is plain http; a name gets https */
        int local = !strncmp(e, "localhost", 9) || (*e >= '0' && *e <= '9' && strspn(e, "0123456789.:") >= 7);
        if (has_scheme)
            snprintf(url, sizeof url, "%s", e);
        else if (looks_like_host)
            snprintf(url, sizeof url, "%s://%s", local ? "http" : "https", e);
        else { /* a search: the words go into the template, URL-encoded */
            char q[1200];
            size_t n = 0;
            for (const unsigned char *c = (const unsigned char *)e; *c && n + 4 < sizeof q; c++)
                if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || strchr("-_.~", *c))
                    q[n++] = (char)*c;
                else if (*c == ' ')
                    q[n++] = '+';
                else
                    n += (size_t)snprintf(q + n, sizeof q - n, "%%%02X", *c);
            q[n] = 0;
            const char *t = conf("search", "https://duckduckgo.com/?q=%s");
            const char *pct = strstr(t, "%s");
            if (pct)
                snprintf(url, sizeof url, "%.*s%s%s", (int)(pct - t), t, q, pct + 2);
            else
                snprintf(url, sizeof url, "%s%s", t, q);
        }
        LOG("open %s", url);
        browser_open(url, 1);
    }
    focus(active);
}

static void edit_key(XKeyEvent *ev)
{
    char buf[64] = "";
    KeySym ks = NoSymbol;
    Status st;
    int n = xic ? Xutf8LookupString(xic, ev, buf, sizeof buf - 1, &ks, &st)
                : XLookupString(ev, buf, sizeof buf - 1, &ks, NULL);
    if (ks == XK_Return || ks == XK_KP_Enter) {
        edit_end(1);
        return;
    }
    if (ks == XK_Escape) {
        edit_end(0);
        return;
    }
    if (ks == XK_BackSpace) {
        size_t l = strlen(edit);
        while (l && (edit[l - 1] & 0xC0) == 0x80) /* one whole UTF-8 character */
            l--;
        if (l)
            l--;
        edit[l] = 0;
    } else if (n > 0 && (unsigned char)buf[0] >= 0x20 && strlen(edit) + (size_t)n < sizeof edit - 1) {
        buf[n] = 0;
        strcat(edit, buf);
    }
    draw_bar();
}

static void menu_action(int i)
{
    KeySym ctrl = XK_Control_L;
    int kind = items[i].kind, tab = items[i].tab;
    if (kind == M_CPU || kind == M_MEM || kind == M_DISK) /* nothing to do: the menu stays */
        return;
    menu_show(0);
    if (kind == M_FAVS || kind == M_HISTORY || kind == M_USER) {
        panel_show(kind == M_FAVS ? P_FAVS : kind == M_HISTORY ? P_HISTORY : P_USERS);
        return;
    }
    panel_show(P_NONE);
    switch (kind) {
    case M_HOME: browser_open(uconf("home", "about:blank"), 1); break;
    case M_NEW_TAB: browser_open(uconf("home", "about:blank"), 0); break;
    case M_TAB: tab_select(tab); break;
    case M_NEXT_TAB: send_keys(&ctrl, 1, XK_Tab); break;
    case M_CLOSE_TAB:
        if (T.wfd >= 0)
            tab_close_current();
        else
            send_keys(&ctrl, 1, XK_w); /* the last one ends the browser: it restarts */
        break;
    case M_NEW_ID: new_identity(); break;
    case M_RESTART:
        browser_stop();
        BR.fails = 0;
        browser_start();
        break;
    case M_STARTSTOP:
        if (BR.pid > 0) {
            browser_stop();
            title[0] = 0;
            draw_bar();
        } else {
            BR.fails = 0;
            browser_start();
        }
        break;
    case M_PROXY: probe_start(); break;
    }
}

static void bar_tap(int x)
{
    KeySym alt = XK_Alt_L;
    if (editing && (x < 3 * B || x >= W - 2 * B))
        edit_end(0);
    if (P.kind && x < W - B) { /* a page is open: back closes it, and so does the field, to type a URL */
        panel_show(P_NONE);
        if (x >= 3 * B && x < W - 2 * B && BR.pid > 0)
            edit_begin();
        return;
    }
    if (x < B)
        send_keys(&alt, 1, XK_Left);
    else if (x < 2 * B)
        send_keys(&alt, 1, XK_Right);
    else if (x < 3 * B)
        send_keys(NULL, 0, XK_F5);
    else if (x >= W - B)
        menu_show(!menu_open);
    else if (x >= W - 2 * B)
        fav_toggle();
    else if (!editing && BR.pid > 0)
        edit_begin();
}

/* ---------------- main ---------------- */

static void on_signal(int s)
{
    if (s == SIGCHLD) {
        int e = errno;
        if (write(sigpipe[1], "c", 1) < 0) {
        }
        errno = e;
    } else {
        quit = 1;
        if (write(sigpipe[1], "q", 1) < 0) {
        }
    }
}

static XftColor color(const char *name)
{
    XftColor c;
    XftColorAllocName(dpy, DefaultVisual(dpy, DefaultScreen(dpy)), DefaultColormap(dpy, DefaultScreen(dpy)), name, &c);
    return c;
}

int main(int argc, char **argv)
{
    for (int o; (o = getopt(argc, argv, "c:")) != -1;)
        if (o == 'c')
            snprintf(C.path, sizeof C.path, "%s", optarg);
        else {
            fprintf(stderr, "usage: rd-shell [-c /data/browser.conf]\n");
            return 2;
        }
    setlocale(LC_ALL, "");
    conf_load();
    if (!(dpy = XOpenDisplay(NULL))) {
        LOG("no X display (DISPLAY=%s)", getenv("DISPLAY") ? getenv("DISPLAY") : "unset");
        return 1;
    }
    int scr = DefaultScreen(dpy);
    root = RootWindow(dpy, scr);
    W = DisplayWidth(dpy, scr);
    H = DisplayHeight(dpy, scr);
    const char *screen = conf("screen", "light");
    S = atoi(conf("scale", !strcmp(screen, "native") ? "3" : "2"));
    if (S < 1 || S > 4)
        S = 2;
    B = 44 * S;
    XInternAtoms(dpy, (char **)ATOM_NAMES, A_COUNT, False, A);

    XSetErrorHandler(on_wm_error);
    XSelectInput(dpy, root, SubstructureRedirectMask | SubstructureNotifyMask | PropertyChangeMask);
    XSync(dpy, False);
    if (another_wm) {
        LOG("another window manager is running on %s", DisplayString(dpy));
        return 1;
    }
    XSetErrorHandler(on_error);
    ewmh_setup();

    char fn[96];
    snprintf(fn, sizeof fn, "%s:pixelsize=%d", conf("font", "Noto Sans"), 15 * S);
    font[0] = XftFontOpenName(dpy, scr, fn);
    snprintf(fn, sizeof fn, "Noto Sans CJK SC:pixelsize=%d", 15 * S);
    font[1] = XftFontOpenName(dpy, scr, fn);
    snprintf(fn, sizeof fn, "%s:pixelsize=%d", conf("font", "Noto Sans"), 12 * S);
    font_s[0] = XftFontOpenName(dpy, scr, fn);
    snprintf(fn, sizeof fn, "Noto Sans CJK SC:pixelsize=%d", 12 * S);
    font_s[1] = XftFontOpenName(dpy, scr, fn);
    if (!font[0] || !font_s[0]) {
        LOG("no font");
        return 1;
    }
    c_text = color("#f2f2f7");
    c_dim = color("#8e8e93");
    c_ok = color("#30d158");
    c_bad = color("#ff6961");
    c_star = color("#ffd60a");
    px_bg = color("#1c1c1e").pixel;
    px_field = color("#2c2c2e").pixel;
    px_line = color("#38383a").pixel;
    px_press = color("#3a3a3c").pixel;

    XSetWindowAttributes wa = { .override_redirect = True, .background_pixel = px_bg,
                                .event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | KeyPressMask };
    bar = XCreateWindow(dpy, root, 0, 0, (unsigned)W, (unsigned)B, 0, CopyFromParent, InputOutput, CopyFromParent,
                        CWOverrideRedirect | CWBackPixel | CWEventMask, &wa);
    XChangeProperty(dpy, bar, A[A_NET_TYPE], XA_ATOM, 32, PropModeReplace, (unsigned char *)&A[A_NET_TYPE_DOCK], 1);
    long strut[12] = { 0, 0, B, 0, 0, 0, 0, 0, 0, W - 1, 0, 0 };
    XChangeProperty(dpy, bar, A[A_NET_STRUT], XA_CARDINAL, 32, PropModeReplace, (unsigned char *)strut, 12);
    XStoreName(dpy, bar, "rd-shell");
    menu = XCreateWindow(dpy, root, W - menu_w(), B, (unsigned)menu_w(), (unsigned)B, 0, CopyFromParent,
                         InputOutput, CopyFromParent, CWOverrideRedirect | CWBackPixel | CWEventMask, &wa);
    wa.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | Button1MotionMask;
    P.w = XCreateWindow(dpy, root, 0, B, (unsigned)W, (unsigned)(H - B), 0, CopyFromParent, InputOutput,
                        CopyFromParent, CWOverrideRedirect | CWBackPixel | CWEventMask, &wa);
    P.pm = XCreatePixmap(dpy, P.w, (unsigned)W, (unsigned)(H - B), (unsigned)DefaultDepth(dpy, scr));
    P.d = XftDrawCreate(dpy, P.pm, DefaultVisual(dpy, scr), DefaultColormap(dpy, scr));
    bar_draw = XftDrawCreate(dpy, bar, DefaultVisual(dpy, scr), DefaultColormap(dpy, scr));
    menu_draw = XftDrawCreate(dpy, menu, DefaultVisual(dpy, scr), DefaultColormap(dpy, scr));
    if ((xim = XOpenIM(dpy, NULL, NULL, NULL)))
        xic = XCreateIC(xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow, bar, XNFocusWindow,
                        bar, NULL);
    XMapRaised(dpy, bar);

    /* windows that were already there (a restart of rd-shell) */
    Window r, p, *kids = NULL;
    unsigned nk = 0;
    if (XQueryTree(dpy, root, &r, &p, &kids, &nk)) {
        for (unsigned i = 0; i < nk; i++) {
            XWindowAttributes a;
            if (XGetWindowAttributes(dpy, kids[i], &a) && a.map_state == IsViewable)
                manage(kids[i]);
        }
        if (kids)
            XFree(kids);
    }

    if (pipe2(sigpipe, O_CLOEXEC | O_NONBLOCK) < 0)
        return 1;
    struct sigaction sa = { .sa_handler = on_signal, .sa_flags = SA_RESTART | SA_NOCLDSTOP };
    sigaction(SIGCHLD, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    users_load();
    user_open();
    draw_bar();
    if (strcmp(conf("autostart", "1"), "0"))
        browser_start();
    probe_start();

    while (!quit) {
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (XFilterEvent(&ev, None))
                continue;
            switch (ev.type) {
            case MapRequest: manage(ev.xmaprequest.window); break;
            case ConfigureRequest: {
                XConfigureRequestEvent *c = &ev.xconfigurerequest;
                int i = find(c->window);
                if (i >= 0) { /* managed: it stays where it was put, and is told so */
                    place(c->window, clients[i].transient);
                    XWindowAttributes a;
                    if (XGetWindowAttributes(dpy, c->window, &a)) {
                        XConfigureEvent ce = { .type = ConfigureNotify, .display = dpy, .event = c->window,
                                               .window = c->window, .x = a.x, .y = a.y, .width = a.width,
                                               .height = a.height, .border_width = 0, .above = None };
                        XSendEvent(dpy, c->window, False, StructureNotifyMask, (XEvent *)&ce);
                    }
                } else {
                    XWindowChanges wc = { c->x, c->y, c->width, c->height, c->border_width, c->above, c->detail };
                    XConfigureWindow(dpy, c->window, (unsigned)c->value_mask, &wc);
                }
                break;
            }
            case UnmapNotify:
                if (ev.xunmap.event == root || find(ev.xunmap.window) >= 0)
                    unmanage(ev.xunmap.window);
                break;
            case DestroyNotify: unmanage(ev.xdestroywindow.window); break;
            case ClientMessage:
                if (ev.xclient.message_type == A[A_NET_STATE])
                    net_state(ev.xclient.window, ev.xclient.data.l[0], (Atom)ev.xclient.data.l[1],
                              (Atom)ev.xclient.data.l[2]);
                else if (ev.xclient.message_type == A[A_NET_ACTIVE] && find(ev.xclient.window) >= 0)
                    focus(ev.xclient.window);
                break;
            case PropertyNotify:
                if (ev.xproperty.window == active &&
                    (ev.xproperty.atom == A[A_NET_NAME] || ev.xproperty.atom == XA_WM_NAME)) {
                    read_title();
                    tab_match(title);
                    char pt[200];
                    page_title(pt, sizeof pt);
                    hist_title(cur_url(), pt);
                    if (OPEN.until && strcmp(title, OPEN.before)) { /* the new tab is in front */
                        KeySym ctrl = XK_Control_L;
                        OPEN.until = 0;
                        send_keys(&ctrl, 1, XK_1); /* the old one … */
                        send_keys(&ctrl, 1, XK_w); /* … closed */
                    }
                    if (!editing)
                        draw_bar();
                }
                break;
            case Expose:
                if (ev.xexpose.count)
                    break;
                if (ev.xexpose.window == menu)
                    draw_menu();
                else if (ev.xexpose.window == P.w)
                    panel_draw();
                else
                    draw_bar();
                break;
            case ButtonPress: /* a finger on a page: a tap, or the start of a drag */
                if (ev.xbutton.window == P.w && ev.xbutton.button == Button1 && !menu_open) {
                    P.down = 1;
                    P.down_y = ev.xbutton.y;
                    P.down_scroll = P.scroll;
                    P.moved = 0;
                }
                break;
            case MotionNotify:
                if (ev.xmotion.window == P.w && P.down) {
                    while (XCheckTypedWindowEvent(dpy, P.w, MotionNotify, &ev)) { /* only the latest */
                    }
                    int dy = ev.xmotion.y - P.down_y;
                    if (dy > S * 6 || dy < -S * 6)
                        P.moved = 1;
                    if (P.moved) {
                        P.scroll = P.down_scroll - dy;
                        panel_clamp();
                        panel_draw();
                    }
                }
                break;
            case ButtonRelease:
                if (ev.xbutton.button != Button1)
                    break;
                if (menu_open) {
                    int x = ev.xbutton.x_root - (W - menu_w()), y = ev.xbutton.y_root - B;
                    if (x >= 0 && x < menu_w() && y >= 0 && y < B * nitems)
                        menu_action(y / B);
                    else if (ev.xbutton.y_root < B && ev.xbutton.x_root >= W - B)
                        menu_show(0); /* the ⋯ again */
                    else
                        menu_show(0);
                } else if (ev.xbutton.window == bar) {
                    bar_tap(ev.xbutton.x);
                } else if (ev.xbutton.window == P.w && P.down) {
                    P.down = 0;
                    if (!P.moved)
                        panel_tap(ev.xbutton.x, ev.xbutton.y + P.scroll);
                }
                break;
            case KeyPress:
                if (editing)
                    edit_key(&ev.xkey);
                break;
            }
        }

        double t = now_s(), next = t + 60;
        if (OPEN.until && t >= OPEN.until)
            OPEN.until = 0; /* no new tab came: leave the tabs as they are */
        if (BR.restart_at && t >= BR.restart_at) {
            BR.restart_at = 0;
            browser_start();
        }
        if (PX.fd < 0 && t >= PX.next)
            probe_start();
        if (PX.fd >= 0 && t >= PX.deadline)
            probe_done(0);
        if (hist_dirty && t >= hist_save_at)
            hist_save();
        if (BR.restore_n && t >= BR.restore_until)
            BR.restore_n = 0; /* the tab to put in front never showed up */
        if (T.dirty && !BR.restore_n && t >= T.save_at)
            tabs_save();
        if (note[0] && t >= note_until) {
            note[0] = 0;
            draw_bar();
        }
        if (P.confirm && t >= P.confirm_until) {
            P.confirm = 0;
            panel_draw();
        }
        if (menu_open && t >= ST.next) {
            stats_sample();
            ST.next = t + 2;
            draw_menu();
        }
        if (hist_dirty && hist_save_at < next)
            next = hist_save_at;
        if (T.dirty && T.save_at < next)
            next = T.save_at;
        if (BR.restore_n && BR.restore_until < next)
            next = BR.restore_until;
        if (note[0] && note_until < next)
            next = note_until;
        if (P.confirm && P.confirm_until < next)
            next = P.confirm_until;
        if (menu_open && ST.next < next)
            next = ST.next;
        if (BR.restart_at && BR.restart_at < next)
            next = BR.restart_at;
        if (PX.fd < 0 && PX.next < next)
            next = PX.next;
        if (PX.fd >= 0 && PX.deadline < next)
            next = PX.deadline;

        struct pollfd pf[4] = { { ConnectionNumber(dpy), POLLIN, 0 }, { sigpipe[0], POLLIN, 0 }, { T.rfd, POLLIN, 0 },
                                { PX.fd, POLLOUT, 0 } };
        int ms = (int)((next - now_s()) * 1000) + 1;
        if (poll(pf, PX.fd >= 0 ? 4 : 3, ms < 0 ? 0 : ms) < 0 && errno != EINTR)
            break;
        if (T.rfd >= 0 && pf[2].revents)
            cdp_read();
        if (pf[1].revents) {
            char c[16];
            while (read(sigpipe[0], c, sizeof c) > 0) {
            }
            reap();
        }
        if (PX.fd >= 0 && pf[3].revents) {
            int err = 0;
            socklen_t el = sizeof err;
            getsockopt(PX.fd, SOL_SOCKET, SO_ERROR, &err, &el);
            probe_done(err == 0);
        }
    }
    browser_stop();
    if (hist_dirty)
        hist_save();
    XCloseDisplay(dpy);
    return 0;
}
