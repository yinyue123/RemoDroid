/* Physical keys: USB HID usage (what the viewer sends, from KeyboardEvent.code)
 * → each OS's own key number. Physical, not characters: the remote computer's
 * keyboard layout decides what a key types, as with a keyboard plugged into it.
 * The viewer's table (web/app.js, KEYS) must name the same codes; tests check. */
#include <stddef.h>

#include "desk.h"

#define NO 0xFFFF

static const keymap_t KEYS[] = {
    /* hid   code              win     mac   evdev */
    { 0x04, "KeyA",           0x1E,   0x00, 30 },
    { 0x05, "KeyB",           0x30,   0x0B, 48 },
    { 0x06, "KeyC",           0x2E,   0x08, 46 },
    { 0x07, "KeyD",           0x20,   0x02, 32 },
    { 0x08, "KeyE",           0x12,   0x0E, 18 },
    { 0x09, "KeyF",           0x21,   0x03, 33 },
    { 0x0A, "KeyG",           0x22,   0x05, 34 },
    { 0x0B, "KeyH",           0x23,   0x04, 35 },
    { 0x0C, "KeyI",           0x17,   0x22, 23 },
    { 0x0D, "KeyJ",           0x24,   0x26, 36 },
    { 0x0E, "KeyK",           0x25,   0x28, 37 },
    { 0x0F, "KeyL",           0x26,   0x25, 38 },
    { 0x10, "KeyM",           0x32,   0x2E, 50 },
    { 0x11, "KeyN",           0x31,   0x2D, 49 },
    { 0x12, "KeyO",           0x18,   0x1F, 24 },
    { 0x13, "KeyP",           0x19,   0x23, 25 },
    { 0x14, "KeyQ",           0x10,   0x0C, 16 },
    { 0x15, "KeyR",           0x13,   0x0F, 19 },
    { 0x16, "KeyS",           0x1F,   0x01, 31 },
    { 0x17, "KeyT",           0x14,   0x11, 20 },
    { 0x18, "KeyU",           0x16,   0x20, 22 },
    { 0x19, "KeyV",           0x2F,   0x09, 47 },
    { 0x1A, "KeyW",           0x11,   0x0D, 17 },
    { 0x1B, "KeyX",           0x2D,   0x07, 45 },
    { 0x1C, "KeyY",           0x15,   0x10, 21 },
    { 0x1D, "KeyZ",           0x2C,   0x06, 44 },
    { 0x1E, "Digit1",         0x02,   0x12, 2 },
    { 0x1F, "Digit2",         0x03,   0x13, 3 },
    { 0x20, "Digit3",         0x04,   0x14, 4 },
    { 0x21, "Digit4",         0x05,   0x15, 5 },
    { 0x22, "Digit5",         0x06,   0x17, 6 },
    { 0x23, "Digit6",         0x07,   0x16, 7 },
    { 0x24, "Digit7",         0x08,   0x1A, 8 },
    { 0x25, "Digit8",         0x09,   0x1C, 9 },
    { 0x26, "Digit9",         0x0A,   0x19, 10 },
    { 0x27, "Digit0",         0x0B,   0x1D, 11 },
    { 0x28, "Enter",          0x1C,   0x24, 28 },
    { 0x29, "Escape",         0x01,   0x35, 1 },
    { 0x2A, "Backspace",      0x0E,   0x33, 14 },
    { 0x2B, "Tab",            0x0F,   0x30, 15 },
    { 0x2C, "Space",          0x39,   0x31, 57 },
    { 0x2D, "Minus",          0x0C,   0x1B, 12 },
    { 0x2E, "Equal",          0x0D,   0x18, 13 },
    { 0x2F, "BracketLeft",    0x1A,   0x21, 26 },
    { 0x30, "BracketRight",   0x1B,   0x1E, 27 },
    { 0x31, "Backslash",      0x2B,   0x2A, 43 },
    { 0x33, "Semicolon",      0x27,   0x29, 39 },
    { 0x34, "Quote",          0x28,   0x27, 40 },
    { 0x35, "Backquote",      0x29,   0x32, 41 },
    { 0x36, "Comma",          0x33,   0x2B, 51 },
    { 0x37, "Period",         0x34,   0x2F, 52 },
    { 0x38, "Slash",          0x35,   0x2C, 53 },
    { 0x39, "CapsLock",       0x3A,   0x39, 58 },
    { 0x3A, "F1",             0x3B,   0x7A, 59 },
    { 0x3B, "F2",             0x3C,   0x78, 60 },
    { 0x3C, "F3",             0x3D,   0x63, 61 },
    { 0x3D, "F4",             0x3E,   0x76, 62 },
    { 0x3E, "F5",             0x3F,   0x60, 63 },
    { 0x3F, "F6",             0x40,   0x61, 64 },
    { 0x40, "F7",             0x41,   0x62, 65 },
    { 0x41, "F8",             0x42,   0x64, 66 },
    { 0x42, "F9",             0x43,   0x65, 67 },
    { 0x43, "F10",            0x44,   0x6D, 68 },
    { 0x44, "F11",            0x57,   0x67, 87 },
    { 0x45, "F12",            0x58,   0x6F, 88 },
    { 0x46, "PrintScreen",    0xE037, 0x69, 99 },  /* mac: F13 */
    { 0x47, "ScrollLock",     0x46,   0x6B, 70 },  /* mac: F14 */
    { 0x48, "Pause",          0x45,   0x71, 119 }, /* win: sent as VK_PAUSE; mac: F15 */
    { 0x49, "Insert",         0xE052, 0x72, 110 }, /* mac: Help */
    { 0x4A, "Home",           0xE047, 0x73, 102 },
    { 0x4B, "PageUp",         0xE049, 0x74, 104 },
    { 0x4C, "Delete",         0xE053, 0x75, 111 },
    { 0x4D, "End",            0xE04F, 0x77, 107 },
    { 0x4E, "PageDown",       0xE051, 0x79, 109 },
    { 0x4F, "ArrowRight",     0xE04D, 0x7C, 106 },
    { 0x50, "ArrowLeft",      0xE04B, 0x7B, 105 },
    { 0x51, "ArrowDown",      0xE050, 0x7D, 108 },
    { 0x52, "ArrowUp",        0xE048, 0x7E, 103 },
    { 0x53, "NumLock",        0xE045, 0x47, 69 },  /* mac: keypad Clear */
    { 0x54, "NumpadDivide",   0xE035, 0x4B, 98 },
    { 0x55, "NumpadMultiply", 0x37,   0x43, 55 },
    { 0x56, "NumpadSubtract", 0x4A,   0x4E, 74 },
    { 0x57, "NumpadAdd",      0x4E,   0x45, 78 },
    { 0x58, "NumpadEnter",    0xE01C, 0x4C, 96 },
    { 0x59, "Numpad1",        0x4F,   0x53, 79 },
    { 0x5A, "Numpad2",        0x50,   0x54, 80 },
    { 0x5B, "Numpad3",        0x51,   0x55, 81 },
    { 0x5C, "Numpad4",        0x4B,   0x56, 75 },
    { 0x5D, "Numpad5",        0x4C,   0x57, 76 },
    { 0x5E, "Numpad6",        0x4D,   0x58, 77 },
    { 0x5F, "Numpad7",        0x47,   0x59, 71 },
    { 0x60, "Numpad8",        0x48,   0x5B, 72 },
    { 0x61, "Numpad9",        0x49,   0x5C, 73 },
    { 0x62, "Numpad0",        0x52,   0x52, 82 },
    { 0x63, "NumpadDecimal",  0x53,   0x41, 83 },
    { 0x64, "IntlBackslash",  0x56,   0x0A, 86 },
    { 0x65, "ContextMenu",    0xE05D, 0x6E, 127 },
    { 0x67, "NumpadEqual",    0x59,   0x51, 117 },
    { 0x68, "F13",            0x64,   0x69, 183 },
    { 0x69, "F14",            0x65,   0x6B, 184 },
    { 0x6A, "F15",            0x66,   0x71, 185 },
    { 0x6B, "F16",            0x67,   0x6A, 186 },
    { 0x6C, "F17",            0x68,   0x40, 187 },
    { 0x6D, "F18",            0x69,   0x4F, 188 },
    { 0x6E, "F19",            0x6A,   0x50, 189 },
    { 0x6F, "F20",            0x6B,   0x5A, 190 },
    { 0x7F, "AudioVolumeMute", 0xE020, 0x4A, 113 },
    { 0x80, "AudioVolumeUp",  0xE030, 0x48, 115 },
    { 0x81, "AudioVolumeDown", 0xE02E, 0x49, 114 },
    { 0x85, "NumpadComma",    0x7E,   0x5F, 121 },
    { 0x87, "IntlRo",         0x73,   0x5E, 89 },
    { 0x88, "KanaMode",       0x70,   0x68, 93 },
    { 0x89, "IntlYen",        0x7D,   0x5D, 124 },
    { 0x8A, "Convert",        0x79,   NO,   92 },
    { 0x8B, "NonConvert",     0x7B,   NO,   94 },
    { 0x90, "Lang1",          0,      0x68, 122 },
    { 0x91, "Lang2",          0,      0x66, 123 },
    { 0xE0, "ControlLeft",    0x1D,   0x3B, 29 },
    { 0xE1, "ShiftLeft",      0x2A,   0x38, 42 },
    { 0xE2, "AltLeft",        0x38,   0x3A, 56 },
    { 0xE3, "MetaLeft",       0xE05B, 0x37, 125 },
    { 0xE4, "ControlRight",   0xE01D, 0x3E, 97 },
    { 0xE5, "ShiftRight",     0x36,   0x3C, 54 },
    { 0xE6, "AltRight",       0xE038, 0x3D, 100 },
    { 0xE7, "MetaRight",      0xE05C, 0x36, 126 },
};

const keymap_t *keymap_find(int hid)
{
    for (size_t i = 0; i < RD_ARRAY_LEN(KEYS); i++)
        if (KEYS[i].hid == hid)
            return &KEYS[i];
    return NULL;
}

const keymap_t *keymap_all(size_t *n)
{
    *n = RD_ARRAY_LEN(KEYS);
    return KEYS;
}

/* The phone's "key" ctrl message uses Android key codes; the ones a computer has. */
int keymap_android_to_hid(int k)
{
    if (k >= 29 && k <= 54) /* KEYCODE_A..Z */
        return 0x04 + (k - 29);
    if (k == 7) /* KEYCODE_0 */
        return 0x27;
    if (k >= 8 && k <= 16) /* KEYCODE_1..9 */
        return 0x1E + (k - 8);
    if (k >= 131 && k <= 142) /* KEYCODE_F1..F12 */
        return 0x3A + (k - 131);
    static const struct { int a, h; } M[] = {
        { 66, 0x28 },  { 67, 0x2A },  { 112, 0x4C }, { 61, 0x2B },  { 111, 0x29 }, { 62, 0x2C },
        { 19, 0x52 },  { 20, 0x51 },  { 21, 0x50 },  { 22, 0x4F },  { 122, 0x4A }, { 123, 0x4D },
        { 92, 0x4B },  { 93, 0x4E },  { 124, 0x49 }, { 113, 0xE0 }, { 114, 0xE4 }, { 59, 0xE1 },
        { 60, 0xE5 },  { 57, 0xE2 },  { 58, 0xE6 },  { 117, 0xE3 }, { 118, 0xE7 }, { 24, 0x80 },
        { 25, 0x81 },  { 164, 0x7F }, { 69, 0x2D },  { 70, 0x2E },  { 71, 0x2F },  { 72, 0x30 },
        { 73, 0x31 },  { 74, 0x33 },  { 75, 0x34 },  { 68, 0x35 },  { 55, 0x36 },  { 56, 0x37 },
        { 76, 0x38 },  { 115, 0x39 }, { 160, 0x58 },
    };
    for (size_t i = 0; i < RD_ARRAY_LEN(M); i++)
        if (M[i].a == k)
            return M[i].h;
    return 0;
}
