/* rd-shell: the remote browser's controller bar, its window manager and the CloakBrowser supervisor,
 * in one small X client (remote-browser-plan.md, "Controller bar").
 *
 *   - The bar: a strip 44 CSS px tall across the top of the screen (88 px at 2x, 44 pt on the iPhone):
 *     back, forward, reload, the page's title (tap it to type a URL), and a menu with Home, New tab, the tabs
 *     (listed only when there is more than one), New identity, Restart and Stop/Start. Taps arrive as
 *     the touch screen's emulated pointer events.
 *   - The window manager: the only one on the display. Every browser window is placed below the bar
 *     whatever it asks for, a fullscreen request (kiosk) included, and there are no frames.
 *   - The supervisor: starts the browser with the settings in /data/browser.conf, restarts it when it
 *     dies, and gives it a new fingerprint seed and an empty profile on New identity. With no proxy set
 *     it refuses to start it, since the page would see this machine's own address.
 *
 * Back, forward and reload are keys through XTest (Alt+Left, Alt+Right, F5), as a person would press them.
 * The tabs go through DevTools on a pipe (--remote-debugging-pipe: no port a page could scan), and only
 * through the browser-level Target calls: list, switch to, open and close a tab. Nothing attaches to a
 * page and no Runtime call is made, so a page has nothing to notice.
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
    char key[64][32], val[64][512];
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

/* Writes key = value into the file: the existing line replaced, or one appended. */
static void conf_store(const char *k, const char *v)
{
    char tmp[PATH_MAX + 8], line[700];
    snprintf(tmp, sizeof tmp, "%s.new", C.path);
    FILE *in = fopen(C.path, "r"), *out = fopen(tmp, "w");
    if (!out) {
        LOG("can't write %s: %s", tmp, strerror(errno));
        if (in)
            fclose(in);
        return;
    }
    int done = 0;
    while (in && fgets(line, sizeof line, in)) {
        const char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        size_t kl = strlen(k);
        if (!done && !strncmp(p, k, kl) && (p[kl] == ' ' || p[kl] == '\t' || p[kl] == '=')) {
            fprintf(out, "%s = %s\n", k, v);
            done = 1;
        } else {
            fputs(line, out);
        }
    }
    if (!done)
        fprintf(out, "%s = %s\n", k, v);
    if (in)
        fclose(in);
    struct stat st;
    if (!stat(C.path, &st))
        fchmod(fileno(out), st.st_mode & 07777);
    if (fclose(out) || rename(tmp, C.path))
        LOG("can't replace %s: %s", C.path, strerror(errno));
}

/* ---------------- state ---------------- */

static Display *dpy;
static Window root, bar, menu, check;
static int W, H, S, B; /* screen size, scale, bar height */
static XftFont *font[2];
static XftDraw *bar_draw, *menu_draw;
static XftColor c_text, c_dim, c_ok, c_bad;
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
    char seed[40];
    int fails;       /* quick deaths in a row: the restart waits longer */
    time_t started;
    double restart_at;
} BR;

static char title[512];
static char message[200];   /* shown instead of the title: why the browser isn't running */
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
    const char *p = conf("proxy", "");
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

static char **browser_argv(const char *url, int devtools)
{
    static char args[40][600];
    static char *argv[48];
    int n = 0;
#define ARG(...) snprintf(args[n], sizeof args[n], __VA_ARGS__), argv[n] = args[n], n++
    ARG("%s", conf("browser", "/opt/cloakbrowser/chrome"));
    ARG("--fingerprint-platform=%s", conf("platform", "windows"));
    ARG("--fingerprint=%s", BR.seed);
    const char *proxy = conf("proxy", "");
    if (*proxy) {
        char host[256], port[16];
        ARG("--proxy-server=%s", proxy);
        if (!proxy_hostport(host, sizeof host, port, sizeof port)) /* no lookups but the proxy's own name */
            ARG("--host-resolver-rules=MAP * ~NOTFOUND , EXCLUDE %s", host);
        ARG("--force-webrtc-ip-handling-policy=disable_non_proxied_udp");
        ARG("--fingerprint-webrtc-ip=%s", conf("webrtc_ip", "auto")); /* the proxy's exit address, not ours */
    }
    ARG("--kiosk");
    ARG("--force-device-scale-factor=%d", S);
    ARG("--touch-events=enabled");
    ARG("--user-data-dir=%s", conf("profile", "/tmp/profile"));
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
    const char *lang = conf("lang", ""), *tz = conf("timezone", "");
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
    if (url && *url)
        ARG("%s", url);
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
        const char *tz = conf("timezone", ""), *lang = conf("lang", "");
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
        char id[40], title[200];
    } tab[MAX_TABS];
    int ntabs;          /* page tabs, in the order they opened; -1: not known (no DevTools) */
    char cur[40];       /* the one in front, as far as can be told */
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

static void tabs_changed(void)
{
    if (menu_open) {
        menu_layout();
        draw_menu();
    }
}

/* Target.targetCreated / targetInfoChanged: a page tab, new or retitled. A new one is in front: tabs a
 * page opens (target=_blank, window.open) and the ones opened from the bar all come up in front. */
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
    }
    snprintf(T.tab[i].title, sizeof T.tab[i].title, "%s", t && *t ? t : url ? url : "");
    if (created)
        snprintf(T.cur, sizeof T.cur, "%s", id);
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
    if (!*conf("proxy", "") && strcmp(conf("allow_direct", "0"), "1")) {
        snprintf(message, sizeof message, "No proxy set in %s", C.path);
        LOG("%s: not starting the browser (allow_direct = 1 to start it anyway)", message);
        draw_bar();
        return;
    }
    if (!BR.seed[0]) {
        const char *s = conf("seed", "");
        if (*s)
            snprintf(BR.seed, sizeof BR.seed, "%s", s);
        else
            random_seed(BR.seed, sizeof BR.seed);
    }
    int to[2] = { -1, -1 }, from[2] = { -1, -1 };
    int devtools = !pipe2(to, O_CLOEXEC) && !pipe2(from, O_CLOEXEC);
    BR.pid = spawn(browser_argv(conf("home", "about:blank"), devtools), devtools ? to[0] : -1, devtools ? from[1] : -1);
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
    LOG("browser started (pid %d, seed %s)", (int)BR.pid, BR.seed);
    draw_bar();
}

static void browser_stop(void)
{
    BR.want = 0;
    BR.restart_at = 0;
    if (BR.pid <= 0)
        return;
    kill(-BR.pid, SIGTERM);
    for (int i = 0; i < 50; i++) { /* up to 5 s for it to save nothing and leave */
        if (waitpid(BR.pid, NULL, WNOHANG) == BR.pid) {
            BR.pid = 0;
            break;
        }
        usleep(100000);
    }
    if (BR.pid > 0) {
        kill(-BR.pid, SIGKILL);
        waitpid(BR.pid, NULL, 0);
        BR.pid = 0;
    }
    cdp_close();
    LOG("browser stopped");
}

static int rm_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    if (remove(path))
        LOG("rm %s: %s", path, strerror(errno));
    return 0;
}

static void new_identity(void)
{
    browser_stop();
    const char *profile = conf("profile", "/tmp/profile");
    if (strlen(profile) > 1) /* never "/" */
        nftw(profile, rm_entry, 16, FTW_DEPTH | FTW_PHYS | FTW_MOUNT);
    random_seed(BR.seed, sizeof BR.seed);
    if (!strcmp(conf("seed_mode", "session"), "sticky")) {
        conf_store("seed", BR.seed);
        conf_load();
    }
    LOG("new identity: seed %s, empty profile", BR.seed);
    BR.fails = 0;
    browser_start();
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
    if (spawn(browser_argv(url, 0), -1, -1) > 0 && one) {
        OPEN.until = now_s() + 5;
        snprintf(OPEN.before, sizeof OPEN.before, "%s", title);
    }
}

static void reap(void)
{
    int st;
    pid_t p;
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
    if (!*conf("proxy", "") || proxy_hostport(host, sizeof host, port, sizeof port))
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

static XftFont *font_for(FcChar32 c)
{
    if (font[0] && XftCharExists(dpy, font[0], c))
        return font[0];
    if (font[1] && XftCharExists(dpy, font[1], c))
        return font[1];
    return font[0];
}

static int utf8_next(const char *s, FcChar32 *c)
{
    int n = FcUtf8ToUcs4((const FcChar8 *)s, c, (int)strlen(s));
    return n > 0 ? n : 1;
}

/* width of s in pixels, or draws it at x (baseline y) when d is given; stops at maxw with an ellipsis */
static int text(XftDraw *d, XftColor *col, int x, int y, const char *s, int maxw)
{
    int w = 0;
    XGlyphInfo gi;
    XftTextExtentsUtf8(dpy, font[0], (const FcChar8 *)"…", (int)strlen("…"), &gi);
    int ell = gi.xOff;
    for (const char *p = s; *p;) {
        FcChar32 c;
        int n = utf8_next(p, &c);
        XftFont *f = font_for(c);
        XftTextExtentsUtf8(dpy, f, (const FcChar8 *)p, n, &gi);
        if (maxw > 0 && w + gi.xOff > maxw - (p[n] ? ell : 0)) {
            if (d)
                XftDrawStringUtf8(d, col, font[0], x + w, y, (const FcChar8 *)"…", (int)strlen("…"));
            return w + ell;
        }
        if (d)
            XftDrawStringUtf8(d, col, f, x + w, y, (const FcChar8 *)p, n);
        w += gi.xOff;
        p += n;
    }
    return w;
}

static void icon_triangle(Drawable dr, GC gc, int cx, int cy, int r, int dir)
{
    XPoint p[3] = { { (short)(cx + dir * r / 2), (short)cy }, /* the tip, pointing dir */
                    { (short)(cx - dir * r / 2), (short)(cy - r) },
                    { (short)(cx - dir * r / 2), (short)(cy + r) } };
    XFillPolygon(dpy, dr, gc, p, 3, Convex, CoordModeOrigin);
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
    int r = B / 6, cy = B / 2, on = BR.pid > 0;
    XSetForeground(dpy, gc, on ? c_text.pixel : c_dim.pixel);
    icon_triangle(bar, gc, B / 2, cy, r, -1);       /* back */
    icon_triangle(bar, gc, B + B / 2, cy, r, 1);    /* forward */
    XSetLineAttributes(dpy, gc, (unsigned)(S * 2), LineSolid, CapRound, JoinRound);
    XDrawArc(dpy, bar, gc, 2 * B + B / 2 - r, cy - r, (unsigned)(2 * r), (unsigned)(2 * r), 90 * 64, 270 * 64);
    XPoint head[3] = { { (short)(2 * B + B / 2), (short)(cy - r - r / 2) },
                       { (short)(2 * B + B / 2 + r / 2 + S), (short)(cy - r) },
                       { (short)(2 * B + B / 2), (short)(cy - r + r / 2) } };
    XFillPolygon(dpy, bar, gc, head, 3, Convex, CoordModeOrigin); /* reload */
    XSetLineAttributes(dpy, gc, 0, LineSolid, CapButt, JoinMiter);
    XSetForeground(dpy, gc, c_text.pixel);
    for (int i = -1; i <= 1; i++) /* the menu */
        XFillArc(dpy, bar, gc, W - B / 2 + i * r - S * 2, cy - S * 2, (unsigned)(S * 4), (unsigned)(S * 4), 0, 360 * 64);

    /* the field: the title, or what is being typed */
    int fx = 3 * B + S * 2, fw = W - 4 * B - S * 4, fh = B - S * 14, fy = (B - fh) / 2;
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
    } else if (message[0]) {
        text(bar_draw, &c_bad, tx, base, message, maxw);
    } else {
        text(bar_draw, on ? &c_text : &c_dim, tx, base, title[0] ? title : on ? "Loading…" : "Stopped — tap ⋯ to start", maxw);
    }
    /* the proxy: green when it answers, red when it doesn't */
    XftColor *pc = PX.ok > 0 ? &c_ok : PX.ok == 0 ? &c_bad : &c_dim;
    XSetForeground(dpy, gc, pc->pixel);
    XFillArc(dpy, bar, gc, fx + fw - S * 6 - dot * 2, cy - dot, (unsigned)(dot * 2), (unsigned)(dot * 2), 0, 360 * 64);
    XFlush(dpy);
}

/* The menu's rows. The tabs are listed only when there is more than one, with Close tab under them; one tab
 * shows none of it. Without DevTools the count isn't known, and the blind keys stand in (Ctrl+Tab, Ctrl+W). */
enum { M_HOME, M_NEW_TAB, M_TAB, M_CLOSE_TAB, M_NEXT_TAB, M_NEW_ID, M_RESTART, M_STARTSTOP, M_PROXY };

static struct {
    int kind, tab;
} items[MAX_TABS + 8];
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
        int room = (H - B) / B - 7; /* the rows that fit on the screen beside the others */
        for (int i = 0; i < T.ntabs && i < room; i++)
            ITEM(M_TAB, i);
        ITEM(M_CLOSE_TAB, -1);
    } else if (T.ntabs < 0 && BR.pid > 0) {
        ITEM(M_NEXT_TAB, -1);
        ITEM(M_CLOSE_TAB, -1);
    }
    ITEM(M_NEW_ID, -1);
    ITEM(M_RESTART, -1);
    ITEM(M_STARTSTOP, -1);
    ITEM(M_PROXY, -1);
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
    case M_NEW_ID: snprintf(out, cap, "New identity"); break;
    case M_RESTART: snprintf(out, cap, "Restart browser"); break;
    case M_STARTSTOP: snprintf(out, cap, BR.pid > 0 ? "Stop browser" : "Start browser"); break;
    case M_PROXY:
        snprintf(out, cap, "Proxy: %s", !*conf("proxy", "") ? "none set" : PX.ok > 0 ? "ok" : PX.ok == 0 ? "down" : "…");
        break;
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
        char l[220];
        menu_label(i, l, sizeof l);
        int tab = items[i].kind == M_TAB, front = tab && !strcmp(T.tab[items[i].tab].id, T.cur);
        if (front) { /* the tab in front */
            XSetForeground(dpy, gc, px_press);
            XFillRectangle(dpy, menu, gc, 0, i * B, (unsigned)mw, (unsigned)B);
            XSetForeground(dpy, gc, c_ok.pixel);
            XFillRectangle(dpy, menu, gc, 0, i * B, (unsigned)(S * 3), (unsigned)B);
        }
        int base = i * B + B / 2 + (font[0]->ascent - font[0]->descent) / 2, x = tab ? S * 24 : S * 12;
        text(menu_draw, items[i].kind == M_PROXY ? &c_dim : &c_text, x, base, l, mw - x - S * 12);
        int after_front = i && items[i - 1].kind == M_TAB && !strcmp(T.tab[items[i - 1].tab].id, T.cur);
        if (i && !front && !after_front) { /* no line against the highlighted row */
            XSetForeground(dpy, gc, px_line);
            XFillRectangle(dpy, menu, gc, S * 12, i * B, (unsigned)(mw - S * 12), (unsigned)S);
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
    if (!XGetWindowAttributes(dpy, w, &a) || a.override_redirect || find(w) >= 0 || w == bar || w == menu)
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
    menu_show(0);
    switch (kind) {
    case M_HOME: browser_open(conf("home", "about:blank"), 1); break;
    case M_NEW_TAB: browser_open(conf("home", "about:blank"), 0); break;
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
    if (editing && (x < 3 * B || x >= W - B))
        edit_end(0);
    if (x < B)
        send_keys(&alt, 1, XK_Left);
    else if (x < 2 * B)
        send_keys(&alt, 1, XK_Right);
    else if (x < 3 * B)
        send_keys(NULL, 0, XK_F5);
    else if (x >= W - B)
        menu_show(!menu_open);
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
    if (!font[0]) {
        LOG("no font");
        return 1;
    }
    c_text = color("#f2f2f7");
    c_dim = color("#8e8e93");
    c_ok = color("#30d158");
    c_bad = color("#ff6961");
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
                if (!ev.xexpose.count)
                    ev.xexpose.window == menu ? draw_menu() : draw_bar();
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
    XCloseDisplay(dpy);
    return 0;
}
