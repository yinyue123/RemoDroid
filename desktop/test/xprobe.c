/* Test helper for tests/e2e_desktop.py: a full-screen X window painted in four
 * colours (red, green / blue, yellow quadrants), with the input focus, that
 * prints every button, wheel and key event as a JSON line; and a clipboard
 * client driven by stdin:
 *   setclip TEXT    own CLIPBOARD with TEXT
 *   getclip         print {"ev":"clip","text":…}
 *   cc -o xprobe desktop/test/xprobe.c -lX11 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

static Display *d;
static Window w;
static int W, H;
static GC gc;
static Atom CLIP, UTF8, TARGETS, PROP;
static char *mine;

static void paint(void)
{
    unsigned long cols[4] = { 0xff0000, 0x00ff00, 0x0000ff, 0xffff00 };
    for (int i = 0; i < 4; i++) {
        XSetForeground(d, gc, cols[i]);
        XFillRectangle(d, w, gc, (i & 1) * W / 2, (i >> 1) * H / 2, (unsigned)W / 2, (unsigned)H / 2);
    }
    XFlush(d);
}

static void json_str(const char *s)
{
    putchar('"');
    for (; *s; s++) {
        if (*s == '"' || *s == '\\')
            printf("\\%c", *s);
        else if ((unsigned char)*s < 0x20)
            printf("\\u%04x", *s);
        else
            putchar(*s);
    }
    putchar('"');
}

static void serve(XSelectionRequestEvent *r)
{
    XSelectionEvent ev = { .type = SelectionNotify, .display = d, .requestor = r->requestor, .selection = r->selection,
                           .target = r->target, .property = None, .time = r->time };
    if (mine && r->target == TARGETS) {
        Atom t[] = { TARGETS, UTF8 };
        XChangeProperty(d, r->requestor, r->property, XA_ATOM, 32, PropModeReplace, (unsigned char *)t, 2);
        ev.property = r->property;
    } else if (mine && (r->target == UTF8 || r->target == XA_STRING)) {
        XChangeProperty(d, r->requestor, r->property, r->target, 8, PropModeReplace, (unsigned char *)mine,
                        (int)strlen(mine));
        ev.property = r->property;
    }
    XSendEvent(d, r->requestor, False, 0, (XEvent *)&ev);
    XFlush(d);
}

static void command(char *line)
{
    line[strcspn(line, "\r\n")] = 0;
    if (!strncmp(line, "setclip ", 8)) {
        free(mine);
        mine = strdup(line + 8);
        XSetSelectionOwner(d, CLIP, w, CurrentTime);
        XFlush(d);
        printf("{\"ev\":\"setclip\",\"ok\":%s}\n", XGetSelectionOwner(d, CLIP) == w ? "true" : "false");
    } else if (!strcmp(line, "getclip")) {
        XConvertSelection(d, CLIP, UTF8, PROP, w, CurrentTime);
        XFlush(d);
    }
    fflush(stdout);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    d = XOpenDisplay(NULL);
    if (!d)
        return 1;
    int scr = DefaultScreen(d);
    W = DisplayWidth(d, scr);
    H = DisplayHeight(d, scr);
    XSetWindowAttributes a = { .override_redirect = True, .background_pixel = 0 };
    w = XCreateWindow(d, RootWindow(d, scr), 0, 0, (unsigned)W, (unsigned)H, 0, CopyFromParent, InputOutput,
                      CopyFromParent, CWOverrideRedirect | CWBackPixel, &a);
    XSelectInput(d, w, ExposureMask | ButtonPressMask | ButtonReleaseMask | KeyPressMask | KeyReleaseMask);
    gc = XCreateGC(d, w, 0, NULL);
    CLIP = XInternAtom(d, "CLIPBOARD", False);
    UTF8 = XInternAtom(d, "UTF8_STRING", False);
    TARGETS = XInternAtom(d, "TARGETS", False);
    PROP = XInternAtom(d, "XPROBE", False);
    XMapRaised(d, w);
    XSync(d, False);
    XSetInputFocus(d, w, RevertToParent, CurrentTime);
    paint();
    printf("{\"ev\":\"ready\",\"w\":%d,\"h\":%d}\n", W, H);
    char line[4096];
    for (;;) {
        while (XPending(d)) {
            XEvent e;
            XNextEvent(d, &e);
            if (e.type == Expose) {
                paint();
            } else if (e.type == MappingNotify) { /* as toolkits do: the agent maps characters onto spare keys */
                XRefreshKeyboardMapping(&e.xmapping);
            } else if (e.type == ButtonPress || e.type == ButtonRelease) {
                printf("{\"ev\":\"button\",\"b\":%u,\"down\":%d,\"x\":%d,\"y\":%d}\n", e.xbutton.button,
                       e.type == ButtonPress, e.xbutton.x_root, e.xbutton.y_root);
            } else if (e.type == KeyPress || e.type == KeyRelease) {
                char buf[32] = "";
                KeySym ks = 0;
                int n = XLookupString(&e.xkey, buf, sizeof buf - 1, &ks, NULL);
                buf[n > 0 ? n : 0] = 0;
                const char *name = XKeysymToString(ks);
                printf("{\"ev\":\"key\",\"down\":%d,\"keycode\":%u,\"keysym\":", e.type == KeyPress, e.xkey.keycode);
                json_str(name ? name : "");
                printf(",\"state\":%u}\n", e.xkey.state);
            } else if (e.type == SelectionRequest) {
                serve(&e.xselectionrequest);
            } else if (e.type == SelectionNotify) {
                Atom type;
                int fmt;
                unsigned long nitems, left;
                unsigned char *data = NULL;
                printf("{\"ev\":\"clip\",\"text\":");
                if (e.xselection.property != None &&
                    XGetWindowProperty(d, w, PROP, 0, 65536, True, AnyPropertyType, &type, &fmt, &nitems, &left, &data) ==
                        Success && data) {
                    json_str((char *)data);
                    XFree(data);
                } else {
                    printf("null");
                }
                printf("}\n");
            }
        }
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(0, &rf);
        FD_SET(ConnectionNumber(d), &rf);
        struct timeval tv = { 0, 100000 };
        if (select(ConnectionNumber(d) + 1, &rf, NULL, NULL, &tv) > 0 && FD_ISSET(0, &rf)) {
            if (!fgets(line, sizeof line, stdin))
                return 0;
            command(line);
        }
    }
}
