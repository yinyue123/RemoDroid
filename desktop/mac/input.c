/* macOS input with Quartz events (needs the Accessibility permission; without it
 * macOS drops them silently). Moves become drags while a button is held,
 * repeated clicks carry a click count (double-click), and the modifier keys
 * held are set on every event, so ⇧-click and ⌘-drag work. */
#include <ApplicationServices/ApplicationServices.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "desk.h"

static CGEventSourceRef src;
static CGPoint pos;
static int held;                 /* buttons, bit per button */
static CGEventFlags flags;       /* modifiers held */
static int clicks, last_button = -1;
static uint64_t last_click_ms;
static CGPoint last_click_pos;
static double acc_x, acc_y;

static CGEventSourceRef source(void)
{
    if (!src)
        src = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
    return src;
}

static void post(CGEventRef e)
{
    if (!e)
        return;
    CGEventSetFlags(e, flags);
    CGEventPost(kCGHIDEventTap, e);
    CFRelease(e);
}

static CGMouseButton cg_button(int b) { return b == 0 ? kCGMouseButtonLeft : b == 2 ? kCGMouseButtonRight : (CGMouseButton)(b == 1 ? 2 : b); }

void dsk_mouse_move(const dsk_screen_t *s, double nx, double ny)
{
    pos = CGPointMake(s->x + nx * s->w, s->y + ny * s->h);
    CGEventType t = held & 1 ? kCGEventLeftMouseDragged : held & 4 ? kCGEventRightMouseDragged
                  : held    ? kCGEventOtherMouseDragged : kCGEventMouseMoved;
    int b = held & 1 ? 0 : held & 4 ? 2 : held & 2 ? 1 : held & 8 ? 3 : held & 16 ? 4 : 0;
    post(CGEventCreateMouseEvent(source(), t, pos, cg_button(b)));
}

void dsk_mouse_button(int b, int down)
{
    if (b < 0 || b > 4)
        return;
    CGEventType t = b == 0 ? (down ? kCGEventLeftMouseDown : kCGEventLeftMouseUp)
                  : b == 2 ? (down ? kCGEventRightMouseDown : kCGEventRightMouseUp)
                           : (down ? kCGEventOtherMouseDown : kCGEventOtherMouseUp);
    if (down) {
        uint64_t now = rd_mono_ms();
        int near = fabs(pos.x - last_click_pos.x) < 4 && fabs(pos.y - last_click_pos.y) < 4;
        clicks = b == last_button && near && now - last_click_ms < 500 ? clicks + 1 : 1;
        last_click_ms = now;
        last_click_pos = pos;
        last_button = b;
        held |= 1 << b;
    } else {
        held &= ~(1 << b);
    }
    CGEventRef e = CGEventCreateMouseEvent(source(), t, pos, cg_button(b));
    if (e) {
        CGEventSetIntegerValueField(e, kCGMouseEventClickState, clicks);
        if (b >= 1 && b != 2)
            CGEventSetIntegerValueField(e, kCGMouseEventButtonNumber, b == 1 ? 2 : b);
    }
    post(e);
}

void dsk_mouse_scroll(double dx, double dy)
{
    /* pixels: smooth on trackpad-style input, about 40 px per wheel notch */
    acc_y += dy * 40;
    acc_x += dx * 40;
    int32_t py = (int32_t)acc_y, px = (int32_t)acc_x;
    acc_y -= py;
    acc_x -= px;
    if (py || px)
        post(CGEventCreateScrollWheelEvent2(source(), kCGScrollEventUnitPixel, 2, py, px, 0));
}

static CGEventFlags flag_of(int hid)
{
    switch (hid) {
    case 0xE0: case 0xE4: return kCGEventFlagMaskControl;
    case 0xE1: case 0xE5: return kCGEventFlagMaskShift;
    case 0xE2: case 0xE6: return kCGEventFlagMaskAlternate;
    case 0xE3: case 0xE7: return kCGEventFlagMaskCommand;
    default: return 0;
    }
}

void dsk_key(int hid, int down)
{
    const keymap_t *k = keymap_find(hid);
    if (!k || k->mac == 0xFFFF)
        return;
    CGEventFlags f = flag_of(hid);
    if (hid == 0x39 && down) /* Caps Lock toggles */
        flags ^= kCGEventFlagMaskAlphaShift;
    else if (f)
        flags = down ? flags | f : flags & ~f;
    post(CGEventCreateKeyboardEvent(source(), (CGKeyCode)k->mac, down ? true : false));
}

void dsk_text(const char *utf8)
{
    CFStringRef s = CFStringCreateWithCString(NULL, utf8, kCFStringEncodingUTF8);
    if (!s)
        return;
    CFIndex n = CFStringGetLength(s);
    UniChar buf[20];
    CGEventFlags saved = flags;
    flags = 0; /* typed text must not pick up held modifiers */
    for (CFIndex i = 0; i < n;) {
        CFIndex k = n - i > 20 ? 20 : n - i; /* at most 20 UTF-16 units per event */
        if (k == 20 && CFStringGetCharacterAtIndex(s, i + k - 1) >= 0xD800 && CFStringGetCharacterAtIndex(s, i + k - 1) < 0xDC00)
            k--; /* don't split a surrogate pair */
        CFStringGetCharacters(s, CFRangeMake(i, k), buf);
        for (int down = 1; down >= 0; down--) {
            CGEventRef e = CGEventCreateKeyboardEvent(source(), 0, down ? true : false);
            if (e)
                CGEventKeyboardSetUnicodeString(e, (UniCharCount)k, buf);
            post(e);
        }
        i += k;
    }
    flags = saved;
    CFRelease(s);
}
