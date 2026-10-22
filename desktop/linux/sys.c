/* Linux (X11) backend: system information, screens, folders, notifications,
 * display power and locking. Wayland sessions need the xdg-desktop-portal
 * path (not yet): the agent says so instead of streaming a black picture. */
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/dpms.h>

#include "desk.h"
#include "x11.h"

extern char **environ;

const char *dsk_platform(void) { return "linux"; }

void dsk_os_name(char *out, size_t cap)
{
    char name[128] = "Linux";
    FILE *f = fopen("/etc/os-release", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            if (!strncmp(line, "PRETTY_NAME=", 12)) {
                char *v = line + 12;
                v[strcspn(v, "\r\n")] = 0;
                if (*v == '"') {
                    v++;
                    char *q = strrchr(v, '"');
                    if (q)
                        *q = 0;
                }
                snprintf(name, sizeof name, "%s", v);
                break;
            }
        }
        fclose(f);
    }
    const char *st = getenv("XDG_SESSION_TYPE");
    snprintf(out, cap, "%s (%s)", name, st && *st ? st : "x11");
}

static int wayland_session(void)
{
    const char *t = getenv("XDG_SESSION_TYPE");
    return (t && !strcmp(t, "wayland")) || (getenv("WAYLAND_DISPLAY") && *getenv("WAYLAND_DISPLAY"));
}

int dsk_init(char *problem, size_t cap)
{
    XInitThreads(); /* capture, input and clipboard each have their own connection and thread */
    signal(SIGCHLD, SIG_DFL);
    return dsk_problem(problem, cap) > 0 ? -1 : 0;
}

int dsk_problem(char *out, size_t cap)
{
    static char last[200];
    static uint64_t checked;
    uint64_t now = rd_mono_ms();
    if (!checked || now - checked > 5000) { /* asked every second; an X connection each time is wasteful */
        checked = now;
        last[0] = 0;
        Display *d;
        if (getenv("RD_ALLOW_WAYLAND") == NULL && wayland_session())
            snprintf(last, sizeof last,
                     "this is a Wayland session; choose \"GNOME on Xorg\" (or another X11 session) at login");
        else if (!(d = x11_open()))
            snprintf(last, sizeof last, "no X11 display (DISPLAY=%s)", getenv("DISPLAY") ? getenv("DISPLAY") : "unset");
        else
            XCloseDisplay(d);
    }
    snprintf(out, cap, "%s", last);
    return last[0] ? 1 : 0;
}

Display *x11_open(void) { return XOpenDisplay(NULL); }

int dsk_screens(dsk_screen_t *out, int max)
{
    Display *d = x11_open();
    if (!d)
        return 0;
    Window root = DefaultRootWindow(d);
    int n = 0, rr_ev, rr_err;
    if (XRRQueryExtension(d, &rr_ev, &rr_err)) {
        int nm = 0;
        XRRMonitorInfo *m = XRRGetMonitors(d, root, True, &nm);
        for (int i = 0; m && i < nm && n < max; i++) {
            dsk_screen_t *s = &out[n];
            memset(s, 0, sizeof *s);
            s->index = n;
            s->x = m[i].x;
            s->y = m[i].y;
            s->w = s->pw = m[i].width;
            s->h = s->ph = m[i].height;
            s->primary = m[i].primary;
            char *name = m[i].name ? XGetAtomName(d, m[i].name) : NULL;
            snprintf(s->name, sizeof s->name, "%s", name ? name : "screen");
            if (name)
                XFree(name);
            n++;
        }
        if (m)
            XRRFreeMonitors(m);
    }
    if (!n && max > 0) { /* no RandR (Xvfb without it, old servers): the whole root window */
        dsk_screen_t *s = &out[0];
        memset(s, 0, sizeof *s);
        s->w = s->pw = DisplayWidth(d, DefaultScreen(d));
        s->h = s->ph = DisplayHeight(d, DefaultScreen(d));
        s->primary = 1;
        snprintf(s->name, sizeof s->name, "screen");
        n = 1;
    }
    if (n && !out[0].primary) {
        int any = 0;
        for (int i = 0; i < n; i++)
            any |= out[i].primary;
        if (!any)
            out[0].primary = 1;
    }
    XCloseDisplay(d);
    return n;
}

/* ---------------- folders ---------------- */

const char *dsk_config_dir(void)
{
    static char dir[512];
    if (!dir[0]) {
        const char *x = getenv("XDG_CONFIG_HOME");
        if (x && *x == '/')
            snprintf(dir, sizeof dir, "%s/remodroid", x);
        else
            snprintf(dir, sizeof dir, "%s/.config/remodroid", rd_home_dir());
        rd_mkdirs(dir, 0700);
    }
    return dir;
}

/* XDG_DOWNLOAD_DIR from user-dirs.dirs ("$HOME/Downloads"), else ~/Downloads */
const char *dsk_downloads_dir(void)
{
    static char dir[512];
    if (dir[0])
        return dir;
    snprintf(dir, sizeof dir, "%s/Downloads", rd_home_dir());
    char path[600];
    const char *x = getenv("XDG_CONFIG_HOME");
    if (x && *x == '/')
        snprintf(path, sizeof path, "%s/user-dirs.dirs", x);
    else
        snprintf(path, sizeof path, "%s/.config/user-dirs.dirs", rd_home_dir());
    FILE *f = fopen(path, "r");
    if (!f)
        return dir;
    char line[600];
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "XDG_DOWNLOAD_DIR=\"", 18))
            continue;
        char *v = line + 18, *q = strchr(v, '"');
        if (q)
            *q = 0;
        if (!strncmp(v, "$HOME", 5))
            snprintf(dir, sizeof dir, "%s%s", rd_home_dir(), v + 5);
        else if (*v == '/')
            snprintf(dir, sizeof dir, "%s", v);
        break;
    }
    fclose(f);
    return dir;
}

/* ---------------- helpers that run desktop tools ---------------- */

/* Runs argv without a shell, output discarded. wait: its exit code, else the pid (reaped by reap()). */
static pid_t pending[8];

static void reap(void)
{
    for (size_t i = 0; i < RD_ARRAY_LEN(pending); i++) {
        int st;
        if (pending[i] > 0 && waitpid(pending[i], &st, WNOHANG) != 0)
            pending[i] = 0;
    }
}

static int run(char *const argv[], int wait)
{
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid;
    int r = posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (r)
        return -1;
    int st = 0;
    if (!wait) { /* notifications must not block the agent */
        reap();
        for (size_t i = 0; i < RD_ARRAY_LEN(pending); i++)
            if (!pending[i]) {
                pending[i] = pid;
                return 0;
            }
        waitpid(pid, &st, 0);
        return 0;
    }
    if (waitpid(pid, &st, 0) < 0)
        return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

void dsk_notify(const char *title, const char *text)
{
    char *argv[] = { "notify-send", "-a", "RemoDroid", "-i", "remodroid", (char *)title, (char *)text, NULL };
    run(argv, 0);
}

int dsk_screen_power(int on)
{
    Display *d = x11_open();
    if (!d)
        return -1;
    int ev, err, ok = -1;
    if (DPMSQueryExtension(d, &ev, &err) && DPMSCapable(d)) {
        DPMSEnable(d);
        ok = DPMSForceLevel(d, on ? DPMSModeOn : DPMSModeOff) ? 0 : -1;
        XFlush(d);
    }
    XCloseDisplay(d);
    return ok;
}

int dsk_lock(void)
{
    char *a[] = { "loginctl", "lock-session", NULL };
    if (run(a, 1) == 0)
        return 0;
    char *b[] = { "xdg-screensaver", "lock", NULL };
    return run(b, 1) == 0 ? 0 : -1;
}

/* keep the X screensaver from blanking while the viewer only watches */
void dsk_session(int active)
{
    Display *d = x11_open();
    if (!d)
        return;
    XResetScreenSaver(d);
    XCloseDisplay(d);
}
