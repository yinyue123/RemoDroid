/* What the X server delivers from its touch screen, for tests/e2e_remote_browser.py: one JSON line per
 * XI2 touch event on the root window, in root pixels, and one per core button press (there should be
 * none while touch is the input: a finger turned into a mouse is the bug this catches).
 *
 *   {"ev":"ready","w":880,"h":1808}
 *   {"ev":"touch","t":"begin","id":3,"x":440,"y":1266}      t: begin | update | end
 *   {"ev":"button","b":1,"x":440,"y":1266}
 *
 *   touchprobe        DISPLAY set */
#include <stdio.h>

#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>

int main(void)
{
    Display *d = XOpenDisplay(NULL);
    int op, ev, er, maj = 2, min = 2;
    if (!d || !XQueryExtension(d, "XInputExtension", &op, &ev, &er) || XIQueryVersion(d, &maj, &min) != Success) {
        fprintf(stderr, "touchprobe: no X display with XI 2.2\n");
        return 2;
    }
    Window root = DefaultRootWindow(d);
    unsigned char mask[XIMaskLen(XI_LASTEVENT)] = { 0 };
    XISetMask(mask, XI_TouchBegin);
    XISetMask(mask, XI_TouchUpdate);
    XISetMask(mask, XI_TouchEnd);
    XISetMask(mask, XI_ButtonPress);
    XIEventMask em = { XIAllMasterDevices, sizeof mask, mask };
    XISelectEvents(d, root, &em, 1);
    XSync(d, False);
    printf("{\"ev\":\"ready\",\"w\":%d,\"h\":%d}\n", DisplayWidth(d, DefaultScreen(d)), DisplayHeight(d, DefaultScreen(d)));
    fflush(stdout);
    for (;;) {
        XEvent e;
        XNextEvent(d, &e);
        if (e.type != GenericEvent || e.xcookie.extension != op || !XGetEventData(d, &e.xcookie))
            continue;
        XIDeviceEvent *de = e.xcookie.data;
        const char *t = de->evtype == XI_TouchBegin ? "begin" : de->evtype == XI_TouchUpdate ? "update"
                      : de->evtype == XI_TouchEnd   ? "end"   : NULL;
        if (t)
            printf("{\"ev\":\"touch\",\"t\":\"%s\",\"id\":%d,\"x\":%.0f,\"y\":%.0f}\n", t, de->detail, de->root_x,
                   de->root_y);
        else if (de->evtype == XI_ButtonPress)
            printf("{\"ev\":\"button\",\"b\":%d,\"x\":%.0f,\"y\":%.0f}\n", de->detail, de->root_x, de->root_y);
        fflush(stdout);
        XFreeEventData(d, &e.xcookie);
    }
}
