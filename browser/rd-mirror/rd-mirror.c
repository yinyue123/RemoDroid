/* rd-mirror: Page mode for the remote browser (browser/dom-mirror.md). It sends the viewer the page's
 * elements instead of a picture of them, and brings the viewer's operations back to the real page.
 *
 *   viewer ⇄ remodroid-desktopd ⇄ rd-mirror ⇄ rd-shell ⇄ the browser's DevTools pipe
 *            ("dom" channel)      (this)      (relay)
 *
 * While a viewer is in Page mode it attaches to the tab in front (rd-shell says which), puts recorder.js
 * into an isolated world of the page and waits on its rd.next(): a snapshot, then each batch of
 * differences, which go to the viewers deflated. It answers their requests for a picture, a font or a
 * stylesheet from what the page itself loaded (Page.getResourceContent), never by fetching. Only these
 * DevTools calls are made, and no Runtime.enable: Target.attachToTarget / detachFromTarget, Page.enable,
 * Page.createIsolatedWorld, Runtime.evaluate (in that world), Page.getResourceContent,
 * Page.handleJavaScriptDialog (with Page enabled the browser shows no alert of its own: the viewer does).
 *
 * Towards rd-shell (its socket, mirror_socket in browser.conf): JSON messages ending with a NUL; ours carry
 * ids from 1 000 000 000 and pass through it to the browser, {"rd":…} ones are for rd-shell itself.
 *
 * Towards remodroid-desktopd ([mirror] socket in desktop.ini), one connection per viewer in Page mode; a
 * frame is a kind byte, a 4-byte big-endian length and the body:
 *   to it    'J'  JSON (an array of messages), raw deflate           → the viewer
 *            'R'  a resource: u16 address length, the address, u8 type length, the MIME type, the bytes
 *            'T'  an "input" channel message (a touch) for the agent itself to carry out; not for the viewer
 *   from it  'v'  JSON from the viewer: {k:"hello"|"res.get"|"op"|…}, or {rd:…} for rd-shell's bar
 *
 *   rd-mirror [-s /run/rd-shell/sock] [-l /run/rd-mirror/sock] [-v]
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "cJSON.h"
#include "picture.h"
#include "recorder.h" /* recorder.js as RECORDER[], made by the build */

#define LOG(...) (fprintf(stderr, "rd-mirror: " __VA_ARGS__), fputc('\n', stderr))
#define DBG(...) (verbose ? (void)(fprintf(stderr, "rd-mirror: " __VA_ARGS__), fputc('\n', stderr)) : (void)0)
static int verbose; /* -v: every DevTools event and answer, by name */
#define FIRST_ID 1000000000
#define MAX_VIEWERS 4
#define QUEUE_MAX ((size_t)8 << 20)  /* a viewer this far behind is let go: it shows the video instead */
#define MSG_MAX ((size_t)96 << 20)

typedef struct {
    char *d;
    size_t n, cap, seen;
} buf_t;

static int buf_room(buf_t *b, size_t more)
{
    if (b->n + more <= b->cap)
        return 0;
    size_t cap = b->cap ? b->cap : 1 << 14;
    while (cap < b->n + more)
        cap *= 2;
    char *d = cap <= MSG_MAX ? realloc(b->d, cap) : NULL;
    if (!d)
        return -1;
    b->d = d;
    b->cap = cap;
    return 0;
}

static int buf_add(buf_t *b, const void *d, size_t n)
{
    if (buf_room(b, n))
        return -1;
    memcpy(b->d + b->n, d, n);
    b->n += n;
    return 0;
}

static void buf_eat(buf_t *b, size_t n)
{
    b->n -= n;
    memmove(b->d, b->d + n, b->n);
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---------------- the viewers (remodroid-desktopd's connections) ---------------- */

static struct viewer {
    int fd;
    buf_t in, out;
    int hello; /* it said it is in Page mode: it gets the page */
    int serial; /* which connection this slot holds: a picture made for an earlier one is dropped */
    int max_w;  /* the widest it can show a picture, in its own pixels (hello's "pw"); 0: not said */
} V[MAX_VIEWERS];
static int listen_fd = -1;

static int viewers(void)
{
    int n = 0;
    for (int i = 0; i < MAX_VIEWERS; i++)
        n += V[i].fd >= 0 && V[i].hello;
    return n;
}

static void viewer_drop(struct viewer *v, const char *why)
{
    if (v->fd < 0)
        return;
    LOG("viewer %d: %s", (int)(v - V), why);
    close(v->fd);
    v->fd = -1;
    v->in.n = v->out.n = 0;
    v->hello = 0;
}

static void viewer_flush(struct viewer *v)
{
    size_t off = 0;
    while (v->fd >= 0 && off < v->out.n) {
        ssize_t w = send(v->fd, v->out.d + off, v->out.n - off, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        if (w <= 0) {
            viewer_drop(v, "gone");
            return;
        }
        off += (size_t)w;
    }
    if (v->fd >= 0)
        buf_eat(&v->out, off);
}

/* one frame to one viewer (v) or to all in Page mode (NULL) */
static void frame(struct viewer *one, char kind, const void *a, size_t an, const void *b, size_t bn)
{
    uint8_t h[5] = { (uint8_t)kind, (uint8_t)((an + bn) >> 24), (uint8_t)((an + bn) >> 16), (uint8_t)((an + bn) >> 8),
                     (uint8_t)(an + bn) };
    for (struct viewer *v = V; v < V + MAX_VIEWERS; v++) {
        if (v->fd < 0 || (one ? v != one : !v->hello))
            continue;
        if (v->out.n + an + bn > QUEUE_MAX || buf_add(&v->out, h, 5) || buf_add(&v->out, a, an) || (bn && buf_add(&v->out, b, bn))) {
            viewer_drop(v, "too far behind");
            continue;
        }
        viewer_flush(v);
    }
}

/* JSON to the viewer(s), raw deflate: a page's snapshot shrinks to about a seventh */
static void send_json(struct viewer *one, const char *s, size_t n)
{
    static buf_t z;
    z_stream zs = { 0 };
    if (deflateInit2(&zs, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return;
    z.n = 0;
    if (!buf_room(&z, deflateBound(&zs, n))) {
        zs.next_in = (Bytef *)s;
        zs.avail_in = (uInt)n;
        zs.next_out = (Bytef *)z.d;
        zs.avail_out = (uInt)z.cap;
        if (deflate(&zs, Z_FINISH) == Z_STREAM_END) {
            DBG("to the viewer%s: %.40s… %zu bytes, %lu deflated", one ? "" : "s", s, n, (unsigned long)zs.total_out);
            frame(one, 'J', z.d, zs.total_out, NULL, 0);
        }
    }
    deflateEnd(&zs);
}

static void send_obj(struct viewer *one, cJSON *o) /* one message, as the array of one the viewer expects */
{
    cJSON *a = cJSON_CreateArray();
    cJSON_AddItemToArray(a, o);
    char *s = cJSON_PrintUnformatted(a);
    cJSON_Delete(a);
    if (s)
        send_json(one, s, strlen(s));
    free(s);
}

/* ---------------- rd-shell and, through it, the browser ---------------- */

static struct {
    const char *path;
    int fd;
    buf_t in, out;
    double retry_at;
    char *bar;          /* rd-shell's last {"rd":"bar",…}, for a viewer that joins */
    int running;
    char front[48];     /* the tab in front */
    double page[4], screen[2]; /* where the page is on the X screen, in pixels */
} SH = { .path = "/run/rd-shell/sock", .fd = -1 };

enum { P_NONE, P_ATTACH, P_WORLD, P_INJECT, P_POLL, P_RES, P_OP, P_QUIET };
static struct pending {
    int id, kind, gen;
    char *arg; /* P_RES: the address asked for */
    int viewer;
} PD[128];
static int next_id = FIRST_ID;

static struct {
    char tab[48], sid[64]; /* the tab attached to, and the DevTools session */
    int ctx;               /* the isolated world; 0: none in this document yet */
    int gen;               /* counts documents and restarts: an answer for an older one is dropped */
    int polling;
    double inject_at;      /* when to put the recorder in (again); 0: not waiting */
    double poll_at;        /* when the poll was sent: it answers within 20 s whatever happens */
    double vw, vh;         /* the page's window in CSS pixels, from its last snapshot */
    char url[2048];        /* the document the tab says it shows now (Page.frameNavigated); "": not told */
    int wrong;             /* how often the world turned out to be another document's */
} M;

static void shell_close(void)
{
    if (SH.fd >= 0)
        close(SH.fd);
    SH.fd = -1;
    SH.in.n = SH.in.seen = SH.out.n = 0;
    SH.running = 0;
    SH.front[0] = 0;
    memset(&M, 0, sizeof M);
    for (struct pending *p = PD; p < PD + 128; p++) {
        free(p->arg);
        memset(p, 0, sizeof *p);
    }
    SH.retry_at = now_s() + 1;
}

static void shell_flush(void)
{
    size_t off = 0;
    while (SH.fd >= 0 && off < SH.out.n) {
        ssize_t w = send(SH.fd, SH.out.d + off, SH.out.n - off, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        if (w <= 0) {
            LOG("rd-shell went");
            shell_close();
            return;
        }
        off += (size_t)w;
    }
    if (SH.fd >= 0)
        buf_eat(&SH.out, off);
}

static void shell_send(const char *s)
{
    if (SH.fd < 0 || buf_add(&SH.out, s, strlen(s) + 1))
        return;
    shell_flush();
}

/* a DevTools call; params is taken. The answer comes to answer() with this kind */
static int call(int kind, const char *method, cJSON *params, int session, const char *arg, int viewer)
{
    struct pending *p = PD;
    while (p < PD + 128 && p->id)
        p++;
    if (SH.fd < 0 || p == PD + 128) {
        cJSON_Delete(params);
        return 0;
    }
    cJSON *m = cJSON_CreateObject();
    cJSON_AddNumberToObject(m, "id", ++next_id);
    cJSON_AddStringToObject(m, "method", method);
    cJSON_AddItemToObject(m, "params", params ? params : cJSON_CreateObject());
    if (session)
        cJSON_AddStringToObject(m, "sessionId", M.sid);
    char *s = cJSON_PrintUnformatted(m);
    cJSON_Delete(m);
    if (!s)
        return 0;
    p->id = next_id;
    p->kind = kind;
    p->gen = M.gen;
    p->arg = arg ? strdup(arg) : NULL;
    p->viewer = viewer;
    shell_send(s);
    free(s);
    return next_id;
}

static void evaluate(int kind, const char *expr, int wait, const char *arg, int viewer)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddNumberToObject(p, "contextId", M.ctx);
    cJSON_AddStringToObject(p, "expression", expr);
    cJSON_AddBoolToObject(p, "awaitPromise", wait);
    cJSON_AddBoolToObject(p, "returnByValue", 1);
    call(kind, "Runtime.evaluate", p, 1, arg, viewer);
}

static void detach(void)
{
    if (M.sid[0]) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "sessionId", M.sid);
        call(P_QUIET, "Target.detachFromTarget", p, 0, NULL, 0);
        LOG("left tab %s", M.tab);
    }
    int gen = M.gen + 1;
    memset(&M, 0, sizeof M);
    M.gen = gen;
}

/* The recorder goes into a new isolated world of the document that is there now. */
static void inject(void)
{
    M.inject_at = 0;
    M.ctx = 0;
    M.polling = 0;
    M.gen++;
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "frameId", M.tab); /* a page's main frame has its target's id */
    cJSON_AddStringToObject(p, "worldName", "rd");
    call(P_WORLD, "Page.createIsolatedWorld", p, 1, NULL, 0);
}

static void poll_page(void)
{
    if (!M.ctx || M.polling)
        return;
    M.polling = 1;
    M.poll_at = now_s();
    evaluate(P_POLL, "rd.next()", 1, NULL, 0);
}

/* Attached to the tab in front while someone is watching in Page mode, and to nothing otherwise. */
static void follow(void)
{
    int want = viewers() > 0 && SH.running && SH.front[0];
    if (M.tab[0] && (!want || strcmp(M.tab, SH.front)))
        detach();
    if (want && !M.tab[0]) {
        snprintf(M.tab, sizeof M.tab, "%s", SH.front);
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "targetId", M.tab);
        cJSON_AddBoolToObject(p, "flatten", 1);
        call(P_ATTACH, "Target.attachToTarget", p, 0, NULL, 0);
    }
}

static const char *sniff(const unsigned char *d, size_t n, int text)
{
    if (n >= 8 && !memcmp(d, "\x89PNG", 4)) return "image/png";
    if (n >= 3 && !memcmp(d, "\xff\xd8\xff", 3)) return "image/jpeg";
    if (n >= 6 && !memcmp(d, "GIF8", 4)) return "image/gif";
    if (n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4)) return "image/webp";
    if (n >= 12 && !memcmp(d + 4, "ftypavif", 8)) return "image/avif";
    if (n >= 4 && !memcmp(d, "wOF2", 4)) return "font/woff2";
    if (n >= 4 && !memcmp(d, "wOFF", 4)) return "font/woff";
    if (n >= 4 && (!memcmp(d, "\0\1\0\0", 4) || !memcmp(d, "true", 4))) return "font/ttf";
    if (n >= 4 && !memcmp(d, "OTTO", 4)) return "font/otf";
    if (n >= 4 && !memcmp(d, "\0\0\1\0", 4)) return "image/x-icon";
    /* SVG is text, but Chrome hands a picture over in base64 whatever it is: look for it either way. Without
     * the right type the viewer's browser won't draw it (a logo shown as a broken picture, 2026-11-10). */
    size_t look = n < 2048 ? n : 2048;
    if (memmem(d, look, "<svg", 4) || memmem(d, look, "<SVG", 4)) return "image/svg+xml";
    if (text)
        return "text/css";
    return "application/octet-stream";
}

static size_t unbase64(char *s)
{
    static int8_t T[256];
    if (!T['B']) {
        memset(T, -1, sizeof T);
        for (int i = 0; i < 64; i++)
            T[(unsigned char)"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[i]] = (int8_t)i;
    }
    unsigned acc = 0, bits = 0;
    char *o = s;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (T[*p] < 0)
            continue;
        acc = acc << 6 | (unsigned)T[*p];
        if ((bits += 6) >= 8)
            *o++ = (char)(acc >> (bits -= 8));
    }
    return (size_t)(o - s);
}

/* ---------------- pictures: made smaller on a thread of their own, so a big one holds nothing up ---------------- */

static struct job {
    struct job *next;
    int viewer, serial, max_w;
    char *url;
    uint8_t *d;
    size_t n;
    const char *mime;
} *jobs_in, *jobs_in_tail, *jobs_out, *jobs_out_tail;
static pthread_mutex_t jobs_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t jobs_cv = PTHREAD_COND_INITIALIZER;
static int jobs_pipe[2] = { -1, -1 };
static size_t saved_in, saved_out; /* -v: what the pictures were, and what was sent */

static void *picture_thread(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&jobs_mu);
        while (!jobs_in)
            pthread_cond_wait(&jobs_cv, &jobs_mu);
        struct job *j = jobs_in;
        jobs_in = j->next;
        pthread_mutex_unlock(&jobs_mu);
        size_t wn = 0;
        uint8_t *w = picture_smaller(j->d, j->n, j->max_w, &wn);
        __atomic_add_fetch(&saved_in, j->n, __ATOMIC_RELAXED);
        if (w) {
            free(j->d);
            j->d = w;
            j->n = wn;
            j->mime = "image/webp";
        }
        __atomic_add_fetch(&saved_out, j->n, __ATOMIC_RELAXED);
        j->next = NULL;
        pthread_mutex_lock(&jobs_mu);
        if (jobs_out)
            jobs_out_tail->next = j;
        else
            jobs_out = j;
        jobs_out_tail = j;
        pthread_mutex_unlock(&jobs_mu);
        while (write(jobs_pipe[1], "", 1) < 0 && errno == EINTR) {
        }
    }
    return NULL;
}

static void resource_out(int viewer, const char *url, const char *mime, const void *d, size_t n)
{
    static buf_t h;
    size_t ul = strlen(url);
    uint8_t l[2] = { (uint8_t)(ul >> 8), (uint8_t)ul }, ml = (uint8_t)strlen(mime);
    h.n = 0;
    if (ul < 65536 && !buf_add(&h, l, 2) && !buf_add(&h, url, ul) && !buf_add(&h, &ml, 1) && !buf_add(&h, mime, ml))
        frame(&V[viewer], 'R', h.d, h.n, d, n);
}

/* the pictures the thread has finished, to their viewers */
static void pictures_done(void)
{
    char c[64];
    while (read(jobs_pipe[0], c, sizeof c) > 0) {
    }
    pthread_mutex_lock(&jobs_mu);
    struct job *j = jobs_out;
    jobs_out = NULL;
    pthread_mutex_unlock(&jobs_mu);
    while (j) {
        struct job *next = j->next;
        if (V[j->viewer].fd >= 0 && V[j->viewer].serial == j->serial)
            resource_out(j->viewer, j->url, j->mime, j->d, j->n);
        free(j->url);
        free(j->d);
        free(j);
        j = next;
    }
    DBG("pictures so far: %zu bytes as the site sent them, %zu sent", saved_in, saved_out);
}

static void res_none(int viewer, const char *u)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "k", "res.none");
    cJSON_AddStringToObject(o, "u", u);
    send_obj(&V[viewer], o);
}

/* touches for the agent to make: at once, or a little later (a tap's lift) */
static struct later {
    double at;
    uint8_t m[14];
} LATER[32];

static void touch_now(const uint8_t *m)
{
    for (struct viewer *v = V; v < V + MAX_VIEWERS; v++)
        if (v->fd >= 0 && v->hello) { /* one agent carries it out: they all drive the same screen */
            frame(v, 'T', m, 14, NULL, 0);
            break;
        }
}

static double touches_due(double t)
{
    double next = 0;
    for (struct later *l = LATER; l < LATER + 32; l++) {
        if (l->at && l->at <= t) {
            touch_now(l->m);
            l->at = 0;
        } else if (l->at && (!next || l->at < next))
            next = l->at;
    }
    return next;
}

/* the fingers the recorder asks for: [[action, pointer, x, y, delay ms]…], x and y 0…1 across the page's window */
static void touches(const cJSON *list)
{
    const cJSON *t;
    if (SH.screen[0] <= 0 || SH.screen[1] <= 0)
        return;
    cJSON_ArrayForEach(t, list)
    {
        double a = cJSON_GetNumberValue(cJSON_GetArrayItem(t, 0)), id = cJSON_GetNumberValue(cJSON_GetArrayItem(t, 1));
        double x = (SH.page[0] + cJSON_GetNumberValue(cJSON_GetArrayItem(t, 2)) * SH.page[2]) / SH.screen[0];
        double y = (SH.page[1] + cJSON_GetNumberValue(cJSON_GetArrayItem(t, 3)) * SH.page[3]) / SH.screen[1];
        if (!(x >= 0 && x <= 1 && y >= 0 && y <= 1)) /* NaN too */
            continue;
        unsigned X = (unsigned)(x * 65535 + 0.5), Y = (unsigned)(y * 65535 + 0.5);
        uint8_t m[14] = { 0x01, (uint8_t)a, (uint8_t)id, 0, (uint8_t)(X >> 8), (uint8_t)X, (uint8_t)(Y >> 8), (uint8_t)Y, 0xff, 0xff };
        double delay = cJSON_GetNumberValue(cJSON_GetArrayItem(t, 4));
        struct later *l = LATER;
        while (l < LATER + 32 && l->at)
            l++;
        if (delay > 0 && delay <= 2000 && l < LATER + 32) {
            l->at = now_s() + delay / 1000;
            memcpy(l->m, m, sizeof m);
        } else
            touch_now(m);
    }
}

/* the answer to one of our DevTools calls */
static void answer(struct pending *p, const cJSON *m)
{
    const cJSON *res = cJSON_GetObjectItem(m, "result"), *err = cJSON_GetObjectItem(m, "error");
    const char *emsg = err ? cJSON_GetStringValue(cJSON_GetObjectItem(err, "message")) : NULL;
    const cJSON *thrown = cJSON_GetObjectItem(res, "exceptionDetails");
    const cJSON *val = cJSON_GetObjectItem(cJSON_GetObjectItem(res, "result"), "value");
    int stale = p->gen != M.gen;
    switch (p->kind) {
    case P_ATTACH:
        if (stale)
            break;
        if (!cJSON_GetStringValue(cJSON_GetObjectItem(res, "sessionId"))) {
            LOG("attach to %s: %s", M.tab, emsg ? emsg : "no session");
            M.tab[0] = 0; /* tried again when rd-shell next says what is in front */
            break;
        }
        snprintf(M.sid, sizeof M.sid, "%s", cJSON_GetStringValue(cJSON_GetObjectItem(res, "sessionId")));
        LOG("in tab %s", M.tab);
        call(P_QUIET, "Page.enable", NULL, 1, NULL, 0);
        inject();
        break;
    case P_WORLD:
        if (stale)
            break;
        M.ctx = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(res, "executionContextId"));
        if (M.ctx <= 0) {
            M.ctx = 0;
            M.inject_at = now_s() + 0.3; /* between two documents */
            break;
        }
        {
            static char *src;
            if (!src && asprintf(&src, "%s\n;location.href", RECORDER) < 0)
                src = NULL;
            if (src)
                evaluate(P_INJECT, src, 0, NULL, 0);
        }
        break;
    case P_INJECT: {
        if (stale)
            break;
        if (err || thrown) {
            LOG("the recorder did not start: %s", emsg ? emsg : "it threw");
            M.ctx = 0;
            M.inject_at = now_s() + 0.5;
            break;
        }
        /* The world must be the document the tab says it shows. Around a navigation (and a page coming back
         * from the back/forward cache) it could still be the one being left: again, then. */
        size_t ul = strcspn(M.url, "#"); /* the two are compared without their #fragment */
        if (M.url[0] && cJSON_IsString(val) && M.wrong < 20 &&
            (strcspn(val->valuestring, "#") != ul || strncmp(val->valuestring, M.url, ul))) {
            M.wrong++;
            M.ctx = 0;
            M.inject_at = now_s() + 0.15;
            break;
        }
        M.wrong = 0;
        evaluate(P_QUIET, "rd.start()", 0, NULL, 0);
        poll_page();
        break;
    }
    case P_POLL:
        if (stale)
            break;
        M.polling = 0;
        if (err || thrown || !cJSON_IsString(val)) { /* the document went (a navigation), and the world with it */
            M.ctx = 0;
            if (!M.inject_at)
                M.inject_at = now_s() + 0.1;
            break;
        }
        if (strcmp(val->valuestring, "[]")) {
            const char *s = val->valuestring;
            if (strstr(s, "{\"k\":\"err\"")) {
                LOG("recorder: %.300s", strstr(s, "{\"k\":\"err\""));
                evaluate(P_QUIET, "rd.start()", 0, NULL, 0);
            } else {
                const char *w;
                if (!strncmp(s, "[{\"k\":\"doc\"", 11) && (w = strstr(s, ",\"w\":")) && w - s < 4096) {
                    M.vw = strtod(w + 5, NULL); /* the page's window: what the touches are scaled by */
                    if ((w = strstr(w, ",\"h\":")))
                        M.vh = strtod(w + 5, NULL);
                }
                send_json(NULL, s, strlen(s));
            }
        }
        poll_page();
        break;
    case P_RES: {
        const cJSON *c = cJSON_GetObjectItem(res, "content");
        if (p->viewer < 0 || p->viewer >= MAX_VIEWERS || V[p->viewer].fd < 0)
            break;
        if (stale || !cJSON_IsString(c)) {
            res_none(p->viewer, p->arg);
            break;
        }
        int b64 = cJSON_IsTrue(cJSON_GetObjectItem(res, "base64Encoded"));
        size_t n = b64 ? unbase64(c->valuestring) : strlen(c->valuestring);
        const char *mime = sniff((const unsigned char *)c->valuestring, n, !b64);
        struct job *j = NULL;
        /* a JPEG, a PNG or a WebP worth making smaller goes to the thread; everything else as it is, now */
        if (jobs_pipe[0] >= 0 && n >= (20u << 10) && (!strcmp(mime, "image/jpeg") || !strcmp(mime, "image/png") || !strcmp(mime, "image/webp")) &&
            (j = calloc(1, sizeof *j)) && (j->d = malloc(n)) && (j->url = strdup(p->arg))) {
            memcpy(j->d, c->valuestring, n);
            j->n = n;
            j->mime = mime;
            j->viewer = p->viewer;
            j->serial = V[p->viewer].serial;
            j->max_w = V[p->viewer].max_w;
            pthread_mutex_lock(&jobs_mu);
            if (jobs_in)
                jobs_in_tail->next = j;
            else
                jobs_in = j;
            jobs_in_tail = j;
            pthread_cond_signal(&jobs_cv);
            pthread_mutex_unlock(&jobs_mu);
            break;
        }
        if (j) {
            free(j->d);
            free(j);
        }
        resource_out(p->viewer, p->arg, mime, c->valuestring, n);
        break;
    }
    case P_OP: /* the recorder's answer to an operation: {t: touches for the agent, r: a reply for the viewer} */
        if (stale || !cJSON_IsString(val))
            break;
        {
            cJSON *o = cJSON_Parse(val->valuestring);
            touches(cJSON_GetObjectItem(o, "t"));
            cJSON *r = cJSON_DetachItemFromObject(o, "r");
            if (r && p->viewer >= 0 && p->viewer < MAX_VIEWERS && V[p->viewer].fd >= 0)
                send_obj(&V[p->viewer], r);
            else
                cJSON_Delete(r);
            cJSON_Delete(o);
        }
        break;
    }
}

static void bar_to(struct viewer *one)
{
    if (!SH.bar)
        return;
    cJSON *o = cJSON_Parse(SH.bar);
    if (!o)
        return;
    cJSON_DeleteItemFromObject(o, "rd");
    cJSON_AddStringToObject(o, "k", "bar");
    send_obj(one, o);
}

static void shell_message(char *s, size_t len)
{
    /* megabytes of a page are not parsed twice: only the envelope is needed to route them */
    cJSON *m = cJSON_ParseWithLength(s, len);
    if (!m)
        return;
    const char *rd = cJSON_GetStringValue(cJSON_GetObjectItem(m, "rd"));
    const char *method = cJSON_GetStringValue(cJSON_GetObjectItem(m, "method"));
    const cJSON *id = cJSON_GetObjectItem(m, "id");
    if (rd && !strcmp(rd, "bar")) {
        free(SH.bar);
        SH.bar = strdup(s);
        SH.running = cJSON_IsTrue(cJSON_GetObjectItem(m, "running"));
        const char *f = cJSON_GetStringValue(cJSON_GetObjectItem(m, "front"));
        snprintf(SH.front, sizeof SH.front, "%s", f ? f : "");
        for (int i = 0; i < 4; i++)
            SH.page[i] = cJSON_GetNumberValue(cJSON_GetArrayItem(cJSON_GetObjectItem(m, "page"), i));
        for (int i = 0; i < 2; i++)
            SH.screen[i] = cJSON_GetNumberValue(cJSON_GetArrayItem(cJSON_GetObjectItem(m, "screen"), i));
        bar_to(NULL);
        follow();
    } else if (rd && !strcmp(rd, "list")) {
        cJSON *o = cJSON_Duplicate(m, 1);
        cJSON_DeleteItemFromObject(o, "rd");
        cJSON_AddStringToObject(o, "k", "list");
        send_obj(NULL, o);
    } else if (rd && !strcmp(rd, "gone")) { /* the browser closed: nothing to detach from */
        int gen = M.gen + 1;
        memset(&M, 0, sizeof M);
        M.gen = gen;
    } else if (cJSON_IsNumber(id)) {
        for (struct pending *p = PD; p < PD + 128; p++)
            if (p->id == (int)id->valuedouble) {
                DBG("answer %d kind %d gen %d/%d%s (%zu bytes)", p->id, p->kind, p->gen, M.gen,
                    cJSON_GetObjectItem(m, "error") ? " ERROR" : "", len);
                answer(p, m);
                free(p->arg);
                memset(p, 0, sizeof *p);
                break;
            }
    } else if (method && M.sid[0]) {
        const char *sid = cJSON_GetStringValue(cJSON_GetObjectItem(m, "sessionId"));
        const cJSON *params = cJSON_GetObjectItem(m, "params");
        DBG("event %s", method);
        if (!sid || strcmp(sid, M.sid)) {
        } else if (!strcmp(method, "Page.frameNavigated")) {
            const cJSON *f = cJSON_GetObjectItem(params, "frame");
            const char *fid = cJSON_GetStringValue(cJSON_GetObjectItem(f, "id"));
            if (fid && !strcmp(fid, M.tab) && !cJSON_GetObjectItem(f, "parentId")) {
                const char *u = cJSON_GetStringValue(cJSON_GetObjectItem(f, "url"));
                snprintf(M.url, sizeof M.url, "%s", u && strlen(u) < sizeof M.url ? u : "");
                M.wrong = 0;
                inject(); /* a new document: the recorder goes in as it begins, and the viewer sees it build up */
            }
        } else if (!strcmp(method, "Page.javascriptDialogOpening")) { /* alert, confirm, prompt: the page waits for the viewer */
            const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(params, "type"));
            if (type && !strcmp(type, "beforeunload")) { /* "Leave site?": yes, the person asked to go */
                cJSON *p = cJSON_CreateObject();
                cJSON_AddBoolToObject(p, "accept", 1);
                call(P_QUIET, "Page.handleJavaScriptDialog", p, 1, NULL, 0);
            } else {
                cJSON *o = cJSON_Duplicate(params, 1);
                cJSON_AddStringToObject(o, "k", "dialog");
                send_obj(NULL, o);
            }
        } else if (!strcmp(method, "Page.javascriptDialogClosed")) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "k", "dialog.closed");
            send_obj(NULL, o);
        } else if (!strcmp(method, "Page.navigatedWithinDocument")) { /* pushState, #fragment: the same document */
            const char *fid = cJSON_GetStringValue(cJSON_GetObjectItem(params, "frameId"));
            const char *u = cJSON_GetStringValue(cJSON_GetObjectItem(params, "url"));
            if (fid && !strcmp(fid, M.tab))
                snprintf(M.url, sizeof M.url, "%s", u && strlen(u) < sizeof M.url ? u : "");
        } else if (!strcmp(method, "Target.detachedFromTarget") || !strcmp(method, "Inspector.detached")) {
            int gen = M.gen + 1;
            memset(&M, 0, sizeof M);
            M.gen = gen;
            follow();
        }
    }
    cJSON_Delete(m);
}

static void shell_read(void)
{
    for (;;) {
        if (buf_room(&SH.in, 1 << 16)) {
            LOG("a message from rd-shell over %zu bytes", SH.in.cap);
            shell_close();
            return;
        }
        ssize_t r = recv(SH.fd, SH.in.d + SH.in.n, SH.in.cap - SH.in.n, 0);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        if (r <= 0) {
            LOG("rd-shell went");
            shell_close();
            return;
        }
        SH.in.n += (size_t)r;
        char *p = SH.in.d, *end;
        while ((end = memchr(p + SH.in.seen, 0, (size_t)(SH.in.d + SH.in.n - p) - SH.in.seen))) {
            shell_message(p, (size_t)(end - p));
            p = end + 1;
            SH.in.seen = 0;
            if (SH.fd < 0)
                return;
        }
        buf_eat(&SH.in, (size_t)(p - SH.in.d));
        SH.in.seen = SH.in.n;
    }
}

static void shell_connect(void)
{
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", SH.path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a)) {
        if (fd >= 0)
            close(fd);
        SH.retry_at = now_s() + 1;
        return;
    }
    fcntl(fd, F_SETFL, O_NONBLOCK);
    SH.fd = fd;
    LOG("with rd-shell at %s", SH.path);
}

/* ---------------- what a viewer sends ---------------- */

static void viewer_message(struct viewer *v, const char *s, size_t len)
{
    cJSON *m = cJSON_ParseWithLength(s, len);
    const char *k = cJSON_GetStringValue(cJSON_GetObjectItem(m, "k"));
    if (cJSON_GetObjectItem(m, "rd")) { /* the bar's commands: rd-shell's business */
        char *t = cJSON_PrintUnformatted(m);
        if (t)
            shell_send(t);
        free(t);
    } else if (!k) {
    } else if (!strcmp(k, "hello")) { /* Page mode is on here: the bar, then the page from its beginning */
        v->hello = 1;
        v->max_w = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(m, "pw")); /* NaN → 0 on every platform we build for */
        if (v->max_w < 0 || v->max_w > 16384)
            v->max_w = 0;
        bar_to(v);
        if (M.ctx)
            evaluate(P_QUIET, "rd.start()", 0, NULL, 0); /* everyone gets the snapshot again; the poll brings it */
        follow();
    } else if (!strcmp(k, "dialog.answer") && M.sid[0]) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddBoolToObject(p, "accept", cJSON_IsTrue(cJSON_GetObjectItem(m, "accept")));
        if (cJSON_GetStringValue(cJSON_GetObjectItem(m, "text")))
            cJSON_AddStringToObject(p, "promptText", cJSON_GetStringValue(cJSON_GetObjectItem(m, "text")));
        call(P_QUIET, "Page.handleJavaScriptDialog", p, 1, NULL, 0);
    } else if (!strcmp(k, "res.get")) {
        const char *u = cJSON_GetStringValue(cJSON_GetObjectItem(m, "u"));
        if (u && M.sid[0] && strlen(u) < 8192) {
            cJSON *p = cJSON_CreateObject();
            cJSON_AddStringToObject(p, "frameId", M.tab);
            cJSON_AddStringToObject(p, "url", u);
            if (!call(P_RES, "Page.getResourceContent", p, 1, u, (int)(v - V)))
                res_none((int)(v - V), u);
        } else if (u && strlen(u) < 8192)
            res_none((int)(v - V), u);
    } else if (!strcmp(k, "op") && M.ctx) { /* a tap, where the viewer is looking, a value: the recorder's business */
        char *arg = cJSON_PrintUnformatted(m), *expr = NULL;
        cJSON *str = arg ? cJSON_CreateStringReference(arg) : NULL;
        char *quoted = str ? cJSON_PrintUnformatted(str) : NULL; /* the JSON, as a JavaScript string */
        cJSON_Delete(str);
        if (quoted && asprintf(&expr, "rd.op(%s)", quoted) > 0)
            evaluate(P_OP, expr, 0, NULL, (int)(v - V));
        free(expr);
        free(quoted);
        free(arg);
    }
    cJSON_Delete(m);
}

static void viewer_read(struct viewer *v)
{
    for (;;) {
        if (buf_room(&v->in, 1 << 12)) {
            viewer_drop(v, "a frame too long");
            return;
        }
        ssize_t r = recv(v->fd, v->in.d + v->in.n, v->in.cap - v->in.n, 0);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        if (r <= 0) {
            viewer_drop(v, "gone");
            return;
        }
        v->in.n += (size_t)r;
        size_t off = 0;
        while (v->in.n - off >= 5) {
            const uint8_t *h = (const uint8_t *)v->in.d + off;
            size_t n = (size_t)h[1] << 24 | (size_t)h[2] << 16 | (size_t)h[3] << 8 | h[4];
            if (n > ((size_t)1 << 20)) {
                viewer_drop(v, "a frame too long");
                return;
            }
            if (v->in.n - off - 5 < n)
                break;
            if (h[0] == 'v')
                viewer_message(v, (const char *)h + 5, n);
            off += 5 + n;
            if (v->fd < 0)
                return;
        }
        buf_eat(&v->in, off);
    }
}

int main(int argc, char **argv)
{
    const char *lpath = "/run/rd-mirror/sock";
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-s") && i + 1 < argc)
            SH.path = argv[++i];
        else if (!strcmp(argv[i], "-l") && i + 1 < argc)
            lpath = argv[++i];
        else if (!strcmp(argv[i], "-v"))
            verbose = 1;
        else {
            fprintf(stderr, "usage: rd-mirror [-s rd-shell's socket] [-l the socket to listen on] [-v]\n");
            return 2;
        }
    signal(SIGPIPE, SIG_IGN);
    for (int i = 0; i < MAX_VIEWERS; i++)
        V[i].fd = -1;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", lpath);
    unlink(lpath);
    listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    /* the agent is another user: the socket is open to this machine's users, the directory is the unit's */
    if (listen_fd < 0 || bind(listen_fd, (struct sockaddr *)&a, sizeof a) || chmod(lpath, 0666) || listen(listen_fd, 4)) {
        LOG("%s: %s", lpath, strerror(errno));
        return 1;
    }
    pthread_t th;
    if (pipe2(jobs_pipe, O_NONBLOCK | O_CLOEXEC) || pthread_create(&th, NULL, picture_thread, NULL)) {
        LOG("no thread for the pictures: they go as the site sent them");
        jobs_pipe[0] = jobs_pipe[1] = -1;
    }
    LOG("listening at %s", lpath);
    for (;;) {
        double t = now_s();
        if (SH.fd < 0 && t >= SH.retry_at)
            shell_connect();
        if (M.inject_at && t >= M.inject_at && M.sid[0])
            inject();
        if (M.polling && t - M.poll_at > 40) { /* it answers within 20 s: the page is stuck, or gone */
            M.polling = 0;
            inject();
        }
        struct pollfd pf[3 + MAX_VIEWERS] = { { listen_fd, POLLIN, 0 }, { SH.fd, (short)(POLLIN | (SH.out.n ? POLLOUT : 0)), 0 } };
        for (int i = 0; i < MAX_VIEWERS; i++)
            pf[2 + i] = (struct pollfd){ V[i].fd, (short)(POLLIN | (V[i].out.n ? POLLOUT : 0)), 0 };
        pf[2 + MAX_VIEWERS] = (struct pollfd){ jobs_pipe[0], POLLIN, 0 };
        int ms = 1000;
        double due = touches_due(t);
        if (due && (due - t) * 1000 < ms)
            ms = (int)((due - t) * 1000) + 1;
        if (M.inject_at && (M.inject_at - t) * 1000 < ms)
            ms = (int)((M.inject_at - t) * 1000) + 1;
        if (poll(pf, 3 + MAX_VIEWERS, ms < 0 ? 0 : ms) < 0 && errno != EINTR)
            return 1;
        if (pf[2 + MAX_VIEWERS].revents)
            pictures_done();
        if (pf[0].revents) {
            int fd = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC), i = 0;
            while (i < MAX_VIEWERS && V[i].fd >= 0)
                i++;
            if (fd >= 0 && i < MAX_VIEWERS) {
                static int serial;
                V[i].fd = fd;
                V[i].serial = ++serial;
                V[i].max_w = 0;
                LOG("viewer %d: here", i);
            } else if (fd >= 0)
                close(fd);
        }
        if (SH.fd >= 0 && pf[1].revents) {
            if (pf[1].revents & POLLOUT)
                shell_flush();
            if (SH.fd >= 0 && (pf[1].revents & (POLLIN | POLLHUP | POLLERR)))
                shell_read();
        }
        for (int i = 0; i < MAX_VIEWERS; i++) {
            if (V[i].fd < 0 || !pf[2 + i].revents || pf[2 + i].fd != V[i].fd)
                continue;
            if (pf[2 + i].revents & POLLOUT)
                viewer_flush(&V[i]);
            if (V[i].fd >= 0 && (pf[2 + i].revents & (POLLIN | POLLHUP | POLLERR)))
                viewer_read(&V[i]);
        }
        follow(); /* the last viewer left: nothing stays attached to the page */
    }
}
