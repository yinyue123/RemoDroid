/* X11 input with XTest, on the agent thread's own connection. Keys are
 * physical (X keycode = Linux evdev code + 8); text that no key types is sent by
 * mapping its keysym onto a spare keycode (what xdotool does). */
#include <stdlib.h>
#include <string.h>

#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

#include "desk.h"
#include "x11.h"

static Display *dpy;
static double acc_x, acc_y; /* fractional scroll notches */

static Display *conn(void)
{
    if (!dpy) {
        dpy = x11_open();
        int ev, er, maj, min;
        if (dpy && !XTestQueryExtension(dpy, &ev, &er, &maj, &min)) {
            LOGW("input: the X server has no XTEST extension; mouse and keyboard won't work");
            XCloseDisplay(dpy);
            dpy = NULL;
        }
    }
    return dpy;
}

void dsk_mouse_move(const dsk_screen_t *s, double nx, double ny)
{
    Display *d = conn();
    if (!d)
        return;
    int x = s->x + (int)(nx * (s->w - 1) + 0.5), y = s->y + (int)(ny * (s->h - 1) + 0.5);
    XTestFakeMotionEvent(d, -1, x, y, CurrentTime);
    XFlush(d);
}

void dsk_mouse_button(int b, int down)
{
    static const unsigned map[] = { 1, 2, 3, 8, 9 }; /* left, middle, right, back, forward */
    Display *d = conn();
    if (!d || b < 0 || b > 4)
        return;
    XTestFakeButtonEvent(d, map[b], down ? True : False, CurrentTime);
    XFlush(d);
}

static void click(Display *d, unsigned b, int n)
{
    for (int i = 0; i < n; i++) {
        XTestFakeButtonEvent(d, b, True, CurrentTime);
        XTestFakeButtonEvent(d, b, False, CurrentTime);
    }
}

void dsk_mouse_scroll(double dx, double dy)
{
    Display *d = conn();
    if (!d)
        return;
    acc_y += dy;
    acc_x += dx;
    int ny = (int)acc_y, nx = (int)acc_x;
    acc_y -= ny;
    acc_x -= nx;
    click(d, ny > 0 ? 4 : 5, abs(ny)); /* 4 = up */
    click(d, nx > 0 ? 6 : 7, abs(nx)); /* 6 = left */
    XFlush(d);
}

void dsk_key(int hid, int down)
{
    const keymap_t *k = keymap_find(hid);
    Display *d = conn();
    if (!d || !k || !k->evdev)
        return;
    XTestFakeKeyEvent(d, (unsigned)k->evdev + 8, down ? True : False, CurrentTime);
    XFlush(d);
}

/* ---------------- text ---------------- */

static unsigned next_cp(const unsigned char **p)
{
    const unsigned char *s = *p;
    unsigned c = *s++;
    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (n)
        c &= 0x3F >> n;
    while (n-- && (*s & 0xC0) == 0x80)
        c = c << 6 | (*s++ & 0x3F);
    *p = s;
    return c;
}

static KeySym keysym_of(unsigned cp)
{
    if (cp == '\n' || cp == '\r')
        return XK_Return;
    if (cp == '\t')
        return XK_Tab;
    if (cp < 0x100)
        return cp; /* Latin-1 keysyms are their code points */
    return 0x01000000 | cp;
}

/* Keycodes with no keysyms, borrowed for characters no key types. A borrowed key keeps its
 * character: programs look keysyms up when they handle the event, so mapping it back right
 * after the key press would hand them an empty key (the race xdotool works around by
 * waiting). The pool is reused round-robin, so each mapping lives on for a while. */
#define SPARES 24
static KeyCode spare[SPARES];
static KeySym spare_sym[SPARES];
static int nspare, next_spare;

static void find_spares(Display *d)
{
    int min, max, per;
    XDisplayKeycodes(d, &min, &max);
    KeySym *map = XGetKeyboardMapping(d, (KeyCode)min, max - min + 1, &per);
    for (int k = max; k >= min && nspare < SPARES; k--) {
        int empty = 1;
        for (int j = 0; j < per; j++)
            if (map[(k - min) * per + j] != NoSymbol)
                empty = 0;
        if (empty)
            spare[nspare++] = (KeyCode)k;
    }
    XFree(map);
    if (!nspare)
        LOGW("input: no free keycodes; characters without a key can't be typed");
}

static KeyCode borrow(Display *d, KeySym ks)
{
    if (!nspare)
        find_spares(d);
    for (int i = 0; i < nspare; i++)
        if (spare_sym[i] == ks)
            return spare[i];
    if (!nspare)
        return 0;
    int i = next_spare;
    next_spare = (next_spare + 1) % nspare;
    KeySym syms[2] = { ks, ks };
    XChangeKeyboardMapping(d, spare[i], 2, syms, 1);
    spare_sym[i] = ks;
    XSync(d, False);
    rd_sleep_ms(15); /* let programs see the MappingNotify first */
    return spare[i];
}

static void tap(Display *d, KeyCode kc, int shift)
{
    KeyCode sh = XKeysymToKeycode(d, XK_Shift_L);
    if (shift)
        XTestFakeKeyEvent(d, sh, True, CurrentTime);
    XTestFakeKeyEvent(d, kc, True, CurrentTime);
    XTestFakeKeyEvent(d, kc, False, CurrentTime);
    if (shift)
        XTestFakeKeyEvent(d, sh, False, CurrentTime);
}

void dsk_text(const char *utf8)
{
    Display *d = conn();
    if (!d)
        return;
    for (const unsigned char *p = (const unsigned char *)utf8; *p;) {
        unsigned cp = next_cp(&p);
        KeySym ks = keysym_of(cp);
        KeyCode kc = XKeysymToKeycode(d, ks);
        if (kc && XkbKeycodeToKeysym(d, kc, 0, 0) == ks)
            tap(d, kc, 0);
        else if (kc && XkbKeycodeToKeysym(d, kc, 0, 1) == ks)
            tap(d, kc, 1);
        else if ((kc = borrow(d, ks)))
            tap(d, kc, 0);
        XSync(d, False);
    }
    XFlush(d);
}
