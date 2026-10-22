/* The binary "input" channel on a computer (multi-platform.md §8.2):
 *   0x01 touch   (14 bytes)  pointer 0 acts as the left mouse button (older viewers, touch screens)
 *   0x02 scroll  (13 bytes)  x, y, h, v in 1/256 notches
 *   0x03 mouse   (12 bytes)  action, button, buttons, x, y
 *   0x04 key     (10 bytes)  action, HID usage, modifier state
 *   0x05 reset   (5 bytes)   release everything
 *   0x06 gamepad (20 bytes)  index, buttons, lx, ly, rx, ry, lt, rt: one pad's whole state
 * Coordinates are 0..65535 across the video frame, i.e. the captured screen.
 * Everything held down is tracked so a lost viewer can't leave a key stuck. */
#include <string.h>

#include "desk.h"

static uint8_t keys_down[256]; /* by HID usage */
static uint8_t buttons_down;   /* bit per button */
static int events;
static int8_t pad_on[DSK_MAX_PADS]; /* 1 open, -1 couldn't be created (not retried this session) */

int input_events(void) { return events; }

static const dsk_screen_t *screen(void)
{
    if (D.screen < 0 || D.screen >= D.nscreens)
        return NULL;
    return &D.screens[D.screen];
}

static void move(uint16_t x, uint16_t y)
{
    const dsk_screen_t *s = screen();
    if (s)
        dsk_mouse_move(s, x / 65535.0, y / 65535.0);
}

static void button(int b, int down)
{
    if (b < 0 || b > 4)
        return;
    if (down)
        buttons_down |= (uint8_t)(1u << b);
    else if (!(buttons_down & (1u << b)))
        return; /* never pressed by us */
    else
        buttons_down &= (uint8_t)~(1u << b);
    dsk_mouse_button(b, down);
}

static void key(int hid, int down)
{
    if (hid <= 0 || hid > 255 || !keymap_find(hid))
        return;
    if (!down && !keys_down[hid])
        return;
    if (down && keys_down[hid] && hid >= 0xE0)
        return; /* modifiers don't auto-repeat */
    keys_down[hid] = (uint8_t)down;
    dsk_key(hid, down);
}

/* Virtual gamepads appear on a pad's first state and stay plugged in until the session ends. */
static void pad(int i, const dsk_pad_t *p)
{
    if (i < 0 || i >= DSK_MAX_PADS)
        return;
    if (!pad_on[i])
        pad_on[i] = dsk_pad_open(i) == 0 ? 1 : -1;
    if (pad_on[i] > 0)
        dsk_pad_update(i, p);
}

void input_release_all(void)
{
    for (int b = 0; b < 5; b++)
        if (buttons_down & (1u << b))
            button(b, 0);
    for (int h = 0; h < 256; h++)
        if (keys_down[h])
            key(h, 0);
    static const dsk_pad_t rest;
    for (int i = 0; i < DSK_MAX_PADS; i++)
        if (pad_on[i] > 0)
            dsk_pad_update(i, &rest);
}

void input_pads_close(void)
{
    for (int i = 0; i < DSK_MAX_PADS; i++) {
        if (pad_on[i] > 0)
            dsk_pad_close(i);
        pad_on[i] = 0;
    }
}

/* The viewer says which modifiers are down: release any we still hold that it lost track of
 * (e.g. Alt released while another window had focus). */
static void sync_mods(int mods, int except)
{
    for (int i = 0; i < 8; i++) {
        int h = 0xE0 + i;
        if (h != except && keys_down[h] && !(mods & (1 << i)))
            key(h, 0);
    }
}

void input_handle(const uint8_t *d, size_t n)
{
    if (!n || D.view_only)
        return;
    events++;
    switch (d[0]) {
    case 0x01: /* touch: action, pointer, tool, x, y, pressure, ts */
        if (n >= 14 && d[2] == 0) {
            move(rd_get_be16(d + 4), rd_get_be16(d + 6));
            if (d[1] == 0)
                button(0, 1);
            else if (d[1] == 1 || d[1] == 3)
                button(0, 0);
        }
        break;
    case 0x02: /* scroll: x, y, h, v (signed, 1/256 notch) */
        if (n >= 13) {
            move(rd_get_be16(d + 1), rd_get_be16(d + 3));
            dsk_mouse_scroll((int16_t)rd_get_be16(d + 5) / 256.0, (int16_t)rd_get_be16(d + 7) / 256.0);
        }
        break;
    case 0x03: /* mouse: action, button, buttons, x, y, ts */
        if (n >= 12) {
            move(rd_get_be16(d + 4), rd_get_be16(d + 6));
            if (d[1] == 1 || d[1] == 2)
                button(d[2], d[1] == 1);
            /* buttons the viewer no longer holds (released outside the picture) */
            for (int b = 0; b < 5; b++)
                if ((buttons_down & (1u << b)) && !(d[3] & (1u << b)) && !(d[1] == 1 && d[2] == b))
                    button(b, 0);
        }
        break;
    case 0x04: /* key: action, usage, mods, ts */
        if (n >= 10) {
            int hid = rd_get_be16(d + 2);
            sync_mods(rd_get_be16(d + 4), hid);
            key(hid, d[1] == 0);
        }
        break;
    case 0x05:
        input_release_all();
        break;
    case 0x06: /* gamepad: index, buttons, lx, ly, rx, ry, lt, rt, ts */
        if (n >= 20) {
            dsk_pad_t p = { rd_get_be32(d + 2), (int16_t)rd_get_be16(d + 6), (int16_t)rd_get_be16(d + 8),
                            (int16_t)rd_get_be16(d + 10), (int16_t)rd_get_be16(d + 12), d[14], d[15] };
            pad(d[1], &p);
        }
        break;
    }
}
