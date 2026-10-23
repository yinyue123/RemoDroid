/* browser/agent/touch_inputtest.c against a real X server: a two-finger pinch sent with dsk_touch must
 * come back as XI2 touch events from a direct-touch device, where the fingers were, and still after the
 * backend connects a second time. Run by
 * touch_test.sh, which starts Xorg with the dummy screen and the inputtest touch device.
 *
 *   touch_test SOCKET        DISPLAY set to that Xorg */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>

#include "desk.h"
#include "touch_inputtest.h"
#include "x11.h"

#if __has_include(<xorg/xf86-input-inputtest-protocol.h>)
#include <xorg/xf86-input-inputtest-protocol.h>
_Static_assert(sizeof(it_touch_t) == sizeof(xf86ITEventTouch), "touch event layout");
_Static_assert(sizeof(it_client_version_t) == sizeof(xf86ITEventClientVersion), "version layout");
_Static_assert(sizeof(it_server_version_t) == sizeof(xf86ITResponseServerVersion), "response layout");
_Static_assert(offsetof(it_touch_t, valuators) == offsetof(xf86ITEventTouch, valuators), "valuators offset");
#define LAYOUT "checked against the X server's header"
#else
#define LAYOUT "not checked (no xf86-input-inputtest-protocol.h)"
#endif

/* what touch_inputtest.c needs from the rest of the agent */
int rd_log_level = RD_LOG_INFO;
void rd_log(int level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
Display *x11_open(void) { return XOpenDisplay(NULL); }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } else printf("ok: " __VA_ARGS__), printf("\n"); } while (0)

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: touch_test SOCKET (with DISPLAY set)\n");
        return 2;
    }
    Display *d = XOpenDisplay(NULL);
    if (!d) {
        fprintf(stderr, "no X display\n");
        return 2;
    }
    if (!strcmp(argv[1], "--probe")) /* touch_test.sh: is the X server up yet */
        return 0;
    int op, ev, er, maj = 2, min = 2;
    if (!XQueryExtension(d, "XInputExtension", &op, &ev, &er) || XIQueryVersion(d, &maj, &min) != Success) {
        fprintf(stderr, "no XI 2.2\n");
        return 2;
    }
    printf("layout: %s\n", LAYOUT);

    /* the device: direct touch, 5 fingers */
    int ndev, direct = 0, touches = 0;
    XIDeviceInfo *info = XIQueryDevice(d, XIAllDevices, &ndev);
    for (int i = 0; i < ndev; i++)
        for (int c = 0; c < info[i].num_classes; c++)
            if (info[i].classes[c]->type == XITouchClass) {
                XITouchClassInfo *t = (XITouchClassInfo *)info[i].classes[c];
                direct |= t->mode == XIDirectTouch;
                touches = t->num_touches;
            }
    XIFreeDeviceInfo(info);
    CHECK(direct && touches == 5, "a direct-touch device with 5 touches (%d, %d)", direct, touches);

    Window root = DefaultRootWindow(d);
    int W = DisplayWidth(d, DefaultScreen(d)), H = DisplayHeight(d, DefaultScreen(d));
    unsigned char mask[XIMaskLen(XI_LASTEVENT)] = { 0 };
    XISetMask(mask, XI_TouchBegin);
    XISetMask(mask, XI_TouchUpdate);
    XISetMask(mask, XI_TouchEnd);
    XIEventMask em = { XIAllMasterDevices, sizeof mask, mask };
    XISelectEvents(d, root, &em, 1);
    XSync(d, False);

    char spec[300], err[200] = "";
    snprintf(spec, sizeof spec, "inputtest:%s", argv[1]);
    CHECK(dsk_touch_open(spec, 5, err, sizeof err) == 0 && dsk_touch_points() == 5, "open %s", err);
    dsk_screen_t s = { .w = W, .h = H, .pw = W, .ph = H };
    /* a pinch: fingers 0 and 1 move apart, then lift; finger 2 goes down and is lifted by a reset */
    dsk_touch(&s, 0, 0, 0.40, 0.50);
    dsk_touch(&s, 1, 0, 0.60, 0.50);
    for (int k = 1; k <= 5; k++) {
        dsk_touch(&s, 0, 2, 0.40 - k * 0.04, 0.50);
        dsk_touch(&s, 1, 2, 0.60 + k * 0.04, 0.50);
    }
    dsk_touch(&s, 0, 1, 0.20, 0.50);
    dsk_touch(&s, 1, 3, 0.80, 0.50);
    dsk_touch(&s, 2, 0, 0.50, 0.90);
    dsk_touch(&s, 7, 2, 0.50, 0.50); /* a move of a finger never put down: ignored */
    dsk_touch_reset();

    int begins = 0, updates = 0, ends = 0, ids[16] = { 0 }, nid = 0;
    double first_x = -1, first_y = -1, last0_x = -1;
    for (int idle = 0; idle < 20;) {
        if (!XPending(d)) {
            usleep(50000);
            idle++;
            continue;
        }
        XEvent e;
        XNextEvent(d, &e);
        if (e.type != GenericEvent || e.xcookie.extension != op || !XGetEventData(d, &e.xcookie))
            continue;
        XIDeviceEvent *de = e.xcookie.data;
        if (de->evtype == XI_TouchBegin) {
            if (begins++ == 0)
                first_x = de->root_x, first_y = de->root_y;
            if (nid < 16)
                ids[nid++] = de->detail;
        } else if (de->evtype == XI_TouchUpdate) {
            updates++;
            if (de->detail == ids[0])
                last0_x = de->root_x;
        } else if (de->evtype == XI_TouchEnd) {
            ends++;
        }
        XFreeEventData(d, &e.xcookie);
    }
    CHECK(begins == 3 && ends == 3, "3 touches began and ended (%d, %d)", begins, ends);
    CHECK(updates >= 10, "the pinch moved (%d updates)", updates);
    CHECK(nid == 3 && ids[0] != ids[1] && ids[1] != ids[2], "each finger is its own touch");
    CHECK(abs((int)first_x - (int)(0.40 * (W - 1))) <= 2 && abs((int)first_y - (int)(0.50 * (H - 1))) <= 2,
          "the first finger landed at %.0f,%.0f (want %d,%d)", first_x, first_y, (int)(0.40 * (W - 1)),
          (int)(0.50 * (H - 1)));
    CHECK(abs((int)last0_x - (int)(0.20 * (W - 1))) <= 2, "and moved to x %.0f (want %d)", last0_x,
          (int)(0.20 * (W - 1)));

    /* a second connection, as after the agent restarts: the driver accepts none once the first has closed,
     * until the device is switched off and on, which the backend does when it isn't answered */
    CHECK(dsk_touch_open(spec, 5, err, sizeof err) == 0, "open again %s", err); /* closes the first */
    usleep(200000);
    dsk_touch(&s, 0, 0, 0.70, 0.30);
    dsk_touch(&s, 0, 1, 0.70, 0.30);
    int again = 0;
    double ax = -1, ay = -1;
    for (int idle = 0; idle < 30 && !again;) {
        if (!XPending(d)) {
            usleep(50000);
            idle++;
            continue;
        }
        XEvent e;
        XNextEvent(d, &e);
        if (e.type != GenericEvent || e.xcookie.extension != op || !XGetEventData(d, &e.xcookie))
            continue;
        XIDeviceEvent *de = e.xcookie.data;
        if (de->evtype == XI_TouchBegin)
            again = 1, ax = de->root_x, ay = de->root_y;
        XFreeEventData(d, &e.xcookie);
    }
    CHECK(again && abs((int)ax - (int)(0.70 * (W - 1))) <= 2 && abs((int)ay - (int)(0.30 * (H - 1))) <= 2,
          "a second connection's finger arrives too (%.0f,%.0f)", ax, ay);
    printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
