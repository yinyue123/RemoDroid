/* A real touch screen on Linux: the X server's inputtest driver (xf86-input-inputtest, shipped in
 * xserver-xorg-core), configured as a "Touch" device. It reads events from a Unix socket and gives
 * clients an XI2 direct-touch device, which is what Chromium takes for a touchscreen. Nothing is made
 * in the kernel, so this works in a container whose Xorg is its own (browser/hqnode/rd-xorg-*.conf):
 *
 *   [input]
 *   touch = inputtest:/run/rd-touch.sock
 *   touch_points = 5            the device's TouchCount: navigator.maxTouchPoints
 *
 * The socket is connected on the first finger down and again after Xorg restarts. Wire format: the
 * driver's xf86-input-inputtest-protocol.h, version 1.1, repeated here so the agent builds without
 * the X server's headers (browser/test/touch_test.c checks the layout against the real one). */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "desk.h"
#include "touch_inputtest.h"
#include "x11.h"

#define MAX_FINGERS 10

static struct {
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    int points;
    int fd;
    int root_w, root_h;
    uint32_t next_id;
    uint32_t tid[MAX_FINGERS]; /* the driver's touch id for each of the viewer's pointer ids */
    uint8_t on[MAX_FINGERS];
} T = { .fd = -1 };

static void forget(void)
{
    if (T.fd >= 0)
        close(T.fd);
    T.fd = -1;
    memset(T.on, 0, sizeof T.on); /* a new X server has no touches in progress */
}

static int put(const void *m, size_t n)
{
    const uint8_t *p = m;
    while (n) {
        ssize_t w = send(T.fd, p, n, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0) {
            LOGW("touch: %s: %s", T.path, w < 0 ? strerror(errno) : "closed");
            forget();
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int connect_x(void)
{
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", T.path);
    T.fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (T.fd < 0 || connect(T.fd, (struct sockaddr *)&a, sizeof a) < 0) {
        LOGW("touch: connect %s: %s", T.path, strerror(errno));
        forget();
        return -1;
    }
    /* the agent thread must not hang on a stuck X server: a finger lost is better */
    struct timeval tv = { 0, 200000 };
    setsockopt(T.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(T.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    it_client_version_t v = { { sizeof v, IT_EVENT_CLIENT_VERSION }, IT_VERSION_MAJOR, IT_VERSION_MINOR };
    it_server_version_t r;
    if (put(&v, sizeof v) < 0)
        return -1;
    if (recv(T.fd, &r, sizeof r, MSG_WAITALL) != (ssize_t)sizeof r || r.header.type != IT_RESPONSE_SERVER_VERSION) {
        LOGW("touch: %s didn't answer as an inputtest device", T.path);
        forget();
        return -1;
    }
    /* the device's 0..65535 spans the whole root window */
    Display *d = x11_open();
    if (d) {
        T.root_w = DisplayWidth(d, DefaultScreen(d));
        T.root_h = DisplayHeight(d, DefaultScreen(d));
        XCloseDisplay(d);
    }
    LOGI("touch: inputtest %u.%u at %s, %d fingers, root %dx%d", r.major, r.minor, T.path, T.points, T.root_w,
         T.root_h);
    return 0;
}

static int send_touch(int i, uint32_t type, double rx, double ry)
{
    it_touch_t e;
    memset(&e, 0, sizeof e);
    e.header.length = sizeof e;
    e.header.type = IT_EVENT_TOUCH;
    e.touchid = T.tid[i];
    e.touch_type = type;
    e.valuators.mask[0] = 3; /* x and y */
    e.valuators.valuators[0] = rx * 65535.0;
    e.valuators.valuators[1] = ry * 65535.0;
    return put(&e, sizeof e);
}

int dsk_touch_open(const char *spec, int points, char *err, size_t cap)
{
    dsk_touch_reset();
    forget();
    T.points = 0;
    if (!spec || !*spec)
        return 0;
    if (strncmp(spec, "inputtest:", 10) || !spec[10]) {
        snprintf(err, cap, "unknown touch backend (want inputtest:/path/to/socket)");
        return -1;
    }
    if (strlen(spec + 10) >= sizeof T.path) {
        snprintf(err, cap, "socket path too long");
        return -1;
    }
    snprintf(T.path, sizeof T.path, "%s", spec + 10);
    T.points = points < 1 ? 1 : points > MAX_FINGERS ? MAX_FINGERS : points;
    return 0;
}

int dsk_touch_points(void) { return T.points; }

void dsk_touch(const dsk_screen_t *s, int id, int action, double nx, double ny)
{
    if (!T.points || id < 0 || id >= MAX_FINGERS)
        return;
    if (action != 0 && !T.on[id])
        return; /* a finger we never put down (or lost with the X server) */
    if (T.fd < 0 && connect_x() < 0)
        return;
    int rw = T.root_w > 1 ? T.root_w : s->x + s->w, rh = T.root_h > 1 ? T.root_h : s->y + s->h;
    double rx = (s->x + nx * (s->w - 1)) / (rw - 1), ry = (s->y + ny * (s->h - 1)) / (rh - 1);
    if (action == 0) {
        if (T.on[id]) /* down again without an up: end the old touch first */
            send_touch(id, IT_TOUCH_END, rx, ry);
        if (T.fd < 0 && connect_x() < 0)
            return;
        T.tid[id] = ++T.next_id;
        if (send_touch(id, IT_TOUCH_BEGIN, rx, ry) == 0)
            T.on[id] = 1;
    } else if (action == 2) {
        send_touch(id, IT_TOUCH_UPDATE, rx, ry);
    } else { /* up, cancel */
        T.on[id] = 0;
        send_touch(id, IT_TOUCH_END, rx, ry);
    }
}

void dsk_touch_reset(void)
{
    for (int i = 0; i < MAX_FINGERS; i++)
        if (T.on[i] && T.fd >= 0) {
            T.on[i] = 0;
            it_touch_t e;
            memset(&e, 0, sizeof e); /* no valuators: the touch ends where it was */
            e.header.length = sizeof e;
            e.header.type = IT_EVENT_TOUCH;
            e.touchid = T.tid[i];
            e.touch_type = IT_TOUCH_END;
            put(&e, sizeof e);
        }
    memset(T.on, 0, sizeof T.on);
}
