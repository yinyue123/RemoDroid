/* X11 CLIPBOARD (UTF-8 text). Owning a selection means answering other
 * programs' requests, so a thread with its own connection and window does the
 * X side; the agent thread asks it through a mutex. XFixes reports owner
 * changes, which is what dsk_clip_seq counts. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xfixes.h>

#include "desk.h"
#include "x11.h"

static struct {
    int started, ok;
    rd_mutex_t mu;
    rd_cond_t cv;
    rd_thread_t th;
    int wake[2];
    Display *d;
    Window w;
    Atom clipboard, utf8, targets, prop;
    int fixes_ev;
    volatile unsigned long seq;
    char *mine;           /* text we own, served to others */
    int want_get, got_done;
    char *got;            /* answer to a get */
    char *to_set;
    int set_pending, set_done, set_ok;
} C;

static void serve(XSelectionRequestEvent *r)
{
    XSelectionEvent ev = { .type = SelectionNotify, .display = r->display, .requestor = r->requestor,
                           .selection = r->selection, .target = r->target, .property = None, .time = r->time };
    rd_mutex_lock(&C.mu);
    if (C.mine && r->target == C.targets) {
        Atom t[] = { C.targets, C.utf8, XA_STRING };
        XChangeProperty(C.d, r->requestor, r->property, XA_ATOM, 32, PropModeReplace, (unsigned char *)t, 3);
        ev.property = r->property;
    } else if (C.mine && (r->target == C.utf8 || r->target == XA_STRING)) {
        XChangeProperty(C.d, r->requestor, r->property, r->target, 8, PropModeReplace, (unsigned char *)C.mine,
                        (int)strlen(C.mine));
        ev.property = r->property;
    }
    rd_mutex_unlock(&C.mu);
    XSendEvent(C.d, r->requestor, False, 0, (XEvent *)&ev);
    XFlush(C.d);
}

static char *read_prop(void)
{
    Atom type;
    int fmt;
    unsigned long n, left;
    unsigned char *data = NULL;
    if (XGetWindowProperty(C.d, C.w, C.prop, 0, 4 * 1024 * 1024 / 4, True, AnyPropertyType, &type, &fmt, &n, &left,
                           &data) != Success || !data)
        return NULL;
    char *s = NULL;
    if (fmt == 8 && type != XInternAtom(C.d, "INCR", False)) { /* INCR (huge transfers) is not supported */
        s = malloc(n + 1);
        memcpy(s, data, n);
        s[n] = 0;
    }
    XFree(data);
    return s;
}

static void *thread_main(void *arg)
{
    int conv_pending = 0;
    uint64_t conv_started = 0;
    for (;;) {
        rd_mutex_lock(&C.mu);
        if (C.set_pending) {
            free(C.mine);
            C.mine = C.to_set;
            C.to_set = NULL;
            C.set_pending = 0;
            XSetSelectionOwner(C.d, C.clipboard, C.w, CurrentTime);
            C.set_ok = XGetSelectionOwner(C.d, C.clipboard) == C.w;
            C.set_done = 1;
            rd_cond_signal(&C.cv);
        }
        if (C.want_get && !conv_pending) {
            C.want_get = 0;
            if (C.mine && XGetSelectionOwner(C.d, C.clipboard) == C.w) { /* our own text */
                C.got = rd_strdup(C.mine);
                C.got_done = 1;
                rd_cond_signal(&C.cv);
            } else {
                XConvertSelection(C.d, C.clipboard, C.utf8, C.prop, C.w, CurrentTime);
                conv_pending = 1;
                conv_started = rd_mono_ms();
            }
        }
        rd_mutex_unlock(&C.mu);
        XFlush(C.d);
        while (XPending(C.d)) {
            XEvent e;
            XNextEvent(C.d, &e);
            if (e.type == SelectionRequest) {
                serve(&e.xselectionrequest);
            } else if (e.type == SelectionClear) {
                rd_mutex_lock(&C.mu);
                free(C.mine);
                C.mine = NULL;
                rd_mutex_unlock(&C.mu);
            } else if (e.type == SelectionNotify && conv_pending) {
                char *s = e.xselection.property != None ? read_prop() : NULL;
                rd_mutex_lock(&C.mu);
                C.got = s;
                C.got_done = 1;
                conv_pending = 0;
                rd_cond_signal(&C.cv);
                rd_mutex_unlock(&C.mu);
            } else if (C.fixes_ev && e.type == C.fixes_ev + XFixesSelectionNotify) {
                C.seq++;
            }
        }
        if (conv_pending && rd_mono_ms() - conv_started > 1000) { /* the owner never answered */
            rd_mutex_lock(&C.mu);
            C.got = NULL;
            C.got_done = 1;
            conv_pending = 0;
            rd_cond_signal(&C.cv);
            rd_mutex_unlock(&C.mu);
        }
        fd_set rf;
        FD_ZERO(&rf);
        int xfd = ConnectionNumber(C.d);
        FD_SET(xfd, &rf);
        FD_SET(C.wake[0], &rf);
        struct timeval tv = { 0, 200000 };
        if (select((xfd > C.wake[0] ? xfd : C.wake[0]) + 1, &rf, NULL, NULL, &tv) > 0 && FD_ISSET(C.wake[0], &rf)) {
            char b[16];
            if (read(C.wake[0], b, sizeof b) < 0 && errno != EAGAIN) {}
        }
    }
    return NULL;
}

static int start(void)
{
    if (C.started)
        return C.ok;
    C.started = 1;
    rd_mutex_init(&C.mu);
    rd_cond_init(&C.cv);
    C.d = x11_open();
    if (!C.d || pipe(C.wake) < 0)
        return 0;
    rd_sock_nonblock(C.wake[0]);
    C.w = XCreateSimpleWindow(C.d, DefaultRootWindow(C.d), -10, -10, 1, 1, 0, 0, 0);
    C.clipboard = XInternAtom(C.d, "CLIPBOARD", False);
    C.utf8 = XInternAtom(C.d, "UTF8_STRING", False);
    C.targets = XInternAtom(C.d, "TARGETS", False);
    C.prop = XInternAtom(C.d, "RD_CLIP", False);
    int ev, er;
    if (XFixesQueryExtension(C.d, &ev, &er)) {
        C.fixes_ev = ev;
        XFixesSelectSelectionInput(C.d, C.w, C.clipboard, XFixesSetSelectionOwnerNotifyMask);
    }
    XFlush(C.d);
    C.ok = rd_thread_start(&C.th, thread_main, NULL) == 0;
    return C.ok;
}

static void poke(void)
{
    if (write(C.wake[1], "x", 1) < 0) {}
}

unsigned long dsk_clip_seq(void)
{
    start();
    return C.seq;
}

/* waits for the clipboard thread (the agent thread holds C.mu only while waiting) */
static void wait_for(int *flag, int ms)
{
    uint64_t end = rd_mono_ms() + (uint64_t)ms;
    while (!*flag && rd_mono_ms() < end) {
        rd_mutex_unlock(&C.mu);
        rd_sleep_ms(5);
        rd_mutex_lock(&C.mu);
    }
}

char *dsk_clip_get(void)
{
    if (!start())
        return NULL;
    rd_mutex_lock(&C.mu);
    C.got_done = 0;
    C.got = NULL;
    C.want_get = 1;
    poke();
    wait_for(&C.got_done, 1500);
    char *s = C.got_done ? C.got : NULL;
    C.got = NULL;
    rd_mutex_unlock(&C.mu);
    return s;
}

int dsk_clip_set(const char *utf8)
{
    if (!start())
        return -1;
    rd_mutex_lock(&C.mu);
    free(C.to_set);
    C.to_set = rd_strdup(utf8);
    C.set_pending = 1;
    C.set_done = 0;
    poke();
    wait_for(&C.set_done, 1000);
    int ok = C.set_done && C.set_ok;
    rd_mutex_unlock(&C.mu);
    return ok ? 0 : -1;
}
