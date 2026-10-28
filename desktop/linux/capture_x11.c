/* X11 screen capture: MIT-SHM for the pixels (XGetImage without it), XDamage
 * to wait for changes instead of polling, and the pointer drawn in from
 * XFixes (X doesn't include it in captures). One connection per capture, used
 * only by the video thread. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/select.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>

#include "desk.h"
#include "x11.h"

struct dsk_capture {
    Display *d;
    Window root;
    int x, y, w, h;
    XImage *img;
    XShmSegmentInfo shm;
    int use_shm;
    int damage_ev;
    Damage damage;
    int fixes;
    int dirty;           /* damage seen since the last grab */
    int last_cx, last_cy;
    unsigned long last_cursor;
    uint8_t *out;        /* the frame with the pointer drawn in */
};

static int x_error;
static int on_x_error(Display *d, XErrorEvent *e)
{
    x_error = e->error_code;
    return 0;
}

/* The segment is made 0600, so an X server running as another user can't attach it — root too,
 * inside a container that drops CAP_IPC_OWNER (the remote browser's Xorg on hqnode) — and the
 * capture falls back to XGetImage: the whole screen through the socket for every frame. Make the
 * server's user the segment's owner: we keep access as its creator, and nobody else gets any.
 * Only over a local socket, where the peer's user is known. */
static void shm_share_with_server(Display *d, int shmid)
{
    struct sockaddr_storage sa;
    socklen_t salen = sizeof sa;
    struct ucred cr;
    socklen_t crlen = sizeof cr;
    struct shmid_ds ds;
    if (getsockname(ConnectionNumber(d), (struct sockaddr *)&sa, &salen) < 0 || sa.ss_family != AF_UNIX)
        return;
    if (getsockopt(ConnectionNumber(d), SOL_SOCKET, SO_PEERCRED, &cr, &crlen) < 0 || cr.uid == geteuid())
        return;
    if (shmctl(shmid, IPC_STAT, &ds) < 0)
        return;
    ds.shm_perm.uid = cr.uid;
    if (shmctl(shmid, IPC_SET, &ds) < 0)
        LOGW("capture: can't share the MIT-SHM segment with the X server's user %u: %s", (unsigned)cr.uid,
             strerror(errno));
}

dsk_capture_t *dsk_capture_open(const dsk_screen_t *s, char *err, size_t cap)
{
    dsk_capture_t *c = calloc(1, sizeof *c);
    c->d = x11_open();
    if (!c->d) {
        snprintf(err, cap, "cannot open the X display");
        free(c);
        return NULL;
    }
    XSetErrorHandler(on_x_error);
    c->root = DefaultRootWindow(c->d);
    c->x = s->x;
    c->y = s->y;
    c->w = s->pw;
    c->h = s->ph;
    int scr = DefaultScreen(c->d);
    Visual *vis = DefaultVisual(c->d, scr);
    int depth = DefaultDepth(c->d, scr);
    if (depth < 24 || vis->red_mask != 0xff0000 || vis->blue_mask != 0xff) {
        snprintf(err, cap, "unsupported X visual (depth %d); a 24-bit TrueColor screen is needed", depth);
        XCloseDisplay(c->d);
        free(c);
        return NULL;
    }
    if (XShmQueryExtension(c->d)) {
        c->img = XShmCreateImage(c->d, vis, (unsigned)depth, ZPixmap, NULL, &c->shm, (unsigned)c->w, (unsigned)c->h);
        if (c->img) {
            c->shm.shmid = shmget(IPC_PRIVATE, (size_t)c->img->bytes_per_line * c->h, IPC_CREAT | 0600);
            c->shm.shmaddr = c->img->data = c->shm.shmid >= 0 ? shmat(c->shm.shmid, NULL, 0) : (char *)-1;
            c->shm.readOnly = False;
            x_error = 0;
            if (c->shm.shmaddr != (char *)-1)
                shm_share_with_server(c->d, c->shm.shmid);
            if (c->shm.shmaddr != (char *)-1 && XShmAttach(c->d, &c->shm)) {
                XSync(c->d, False);
                c->use_shm = !x_error; /* fails over the network / in containers without shared IPC */
            }
            if (c->shm.shmid >= 0)
                shmctl(c->shm.shmid, IPC_RMID, NULL); /* freed when both sides detach */
            if (!c->use_shm) {
                if (c->shm.shmaddr != (char *)-1)
                    shmdt(c->shm.shmaddr);
                c->img->data = NULL;
                XDestroyImage(c->img);
                c->img = NULL;
            }
        }
    }
    int ev, er;
    if (XDamageQueryExtension(c->d, &ev, &er)) {
        c->damage_ev = ev;
        c->damage = XDamageCreate(c->d, c->root, XDamageReportNonEmpty);
    }
    c->fixes = XFixesQueryExtension(c->d, &ev, &er);
    c->dirty = 1;
    c->out = malloc((size_t)c->w * c->h * 4);
    LOGI("capture: X11 %dx%d at %d,%d (%s%s)", c->w, c->h, c->x, c->y, c->use_shm ? "MIT-SHM" : "XGetImage",
         c->damage ? ", XDamage" : "");
    return c;
}

/* Collects damage events; returns 1 if the captured area changed. */
static int drain(dsk_capture_t *c)
{
    int changed = 0;
    while (XPending(c->d)) {
        XEvent e;
        XNextEvent(c->d, &e);
        if (c->damage && e.type == c->damage_ev + XDamageNotify) {
            XDamageNotifyEvent *de = (XDamageNotifyEvent *)&e;
            if (de->area.x < c->x + c->w && de->area.x + de->area.width > c->x && de->area.y < c->y + c->h &&
                de->area.y + de->area.height > c->y)
                changed = 1;
            /* NonEmpty reports once until the damage is cleared */
            XDamageSubtract(c->d, c->damage, None, None);
        }
    }
    return changed;
}

static int wait_fd(int fd, int ms)
{
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(fd, &rf);
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    return select(fd + 1, &rf, NULL, NULL, &tv);
}

/* Draws the pointer (premultiplied ARGB from XFixes) into the frame. Not on a touch screen ([input] touch):
 * a phone shows no pointer, and the one the fingers drag along would sit on whatever was last tapped. */
static int draw_cursor(dsk_capture_t *c, uint8_t *dst, int stride)
{
    if (!c->fixes || dsk_touch_points() > 0)
        return 0;
    XFixesCursorImage *ci = XFixesGetCursorImage(c->d);
    if (!ci)
        return 0;
    int moved = ci->x != c->last_cx || ci->y != c->last_cy || ci->cursor_serial != c->last_cursor;
    c->last_cx = ci->x;
    c->last_cy = ci->y;
    c->last_cursor = ci->cursor_serial;
    int ox = ci->x - ci->xhot - c->x, oy = ci->y - ci->yhot - c->y;
    for (int j = 0; j < ci->height; j++) {
        int y = oy + j;
        if (y < 0 || y >= c->h)
            continue;
        for (int i = 0; i < ci->width; i++) {
            int x = ox + i;
            if (x < 0 || x >= c->w)
                continue;
            unsigned long p = ci->pixels[j * ci->width + i]; /* 32 bits in an unsigned long */
            unsigned a = (p >> 24) & 255;
            if (!a)
                continue;
            uint8_t *q = dst + (size_t)y * stride + x * 4;
            q[0] = (uint8_t)((p & 255) + q[0] * (255 - a) / 255);
            q[1] = (uint8_t)(((p >> 8) & 255) + q[1] * (255 - a) / 255);
            q[2] = (uint8_t)(((p >> 16) & 255) + q[2] * (255 - a) / 255);
        }
    }
    XFree(ci);
    return moved;
}

int dsk_capture_next(dsk_capture_t *c, dsk_frame_t *f, int timeout_ms)
{
    uint64_t t0 = rd_mono_ms();
    int changed = drain(c) || c->dirty || !c->damage;
    while (!changed && (int)(rd_mono_ms() - t0) < timeout_ms) {
        int left = timeout_ms - (int)(rd_mono_ms() - t0);
        /* the pointer moves without damage: look at it every 16 ms */
        if (wait_fd(ConnectionNumber(c->d), left < 16 ? left : 16) < 0 && errno != EINTR)
            return -1;
        changed = drain(c);
        if (!changed && c->fixes && dsk_touch_points() <= 0) {
            Window r, ch;
            int rx, ry, wx, wy;
            unsigned mask;
            if (XQueryPointer(c->d, c->root, &r, &ch, &rx, &ry, &wx, &wy, &mask) &&
                (rx != c->last_cx || ry != c->last_cy))
                changed = 1;
        }
    }
    if (!changed)
        return 0;
    c->dirty = 0;
    x_error = 0;
    XImage *img = c->img;
    if (c->use_shm) {
        if (!XShmGetImage(c->d, c->root, img, c->x, c->y, AllPlanes))
            return -1;
    } else {
        img = XGetImage(c->d, c->root, c->x, c->y, (unsigned)c->w, (unsigned)c->h, AllPlanes, ZPixmap);
        if (!img)
            return -1;
    }
    XSync(c->d, False);
    if (x_error) {
        if (img != c->img)
            XDestroyImage(img);
        return -1; /* the screen changed size: reopen */
    }
    for (int j = 0; j < c->h; j++)
        memcpy(c->out + (size_t)j * c->w * 4, img->data + (size_t)j * img->bytes_per_line, (size_t)c->w * 4);
    if (img != c->img)
        XDestroyImage(img);
    draw_cursor(c, c->out, c->w * 4);
    f->bgra = c->out;
    f->w = c->w;
    f->h = c->h;
    f->stride = c->w * 4;
    return 1;
}

void dsk_capture_close(dsk_capture_t *c)
{
    if (!c)
        return;
    if (c->damage)
        XDamageDestroy(c->d, c->damage);
    if (c->use_shm) {
        XShmDetach(c->d, &c->shm);
        shmdt(c->shm.shmaddr);
    }
    if (c->img) {
        c->img->data = NULL;
        XDestroyImage(c->img);
    }
    XCloseDisplay(c->d);
    free(c->out);
    free(c);
}
