/* The agent → RemoDroidInput helper pipe (desktop/win/inputsvc.c).
 *
 * Windows of elevated programs ignore input from the agent, which runs as the signed-in user
 * (UIPI). With the RemoDroidInput service installed, a helper runs as SYSTEM in each signed-in
 * session; the agent sends it its mouse and keyboard events, and the helper injects them, which
 * elevated windows accept. Without the service the agent injects them itself (input.c). */
#ifndef RD_INPUTPIPE_H
#define RD_INPUTPIPE_H

#include <stdint.h>

#define RI_PIPE_FMT L"\\\\.\\pipe\\RemoDroidInput-%lu" /* + the session id */
#define RI_SERVICE L"RemoDroidInput"
#define RI_EXE L"remodroid-input.exe"
#define RI_TEXT_MAX 65536

enum {
    RI_MOVE = 1, /* a, b: the pixel on the virtual desktop */
    RI_BUTTON,   /* a: button (0 left … 4 forward), b: down */
    RI_SCROLL,   /* x, y: notches (desk.h's dsk_mouse_scroll) */
    RI_KEY,      /* a: USB HID usage, b: down */
    RI_TEXT,     /* a: the UTF-8 bytes that follow */
};

typedef struct {
    int32_t op, a, b, pad;
    double x, y;
} ri_msg_t; /* 32 bytes, the same on both sides of the pipe (one machine) */

#endif
