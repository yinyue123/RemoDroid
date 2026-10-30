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

/* The lock screen. A locked session shows it on the secure desktop, which the agent can neither capture
 * nor type into; the helper can. While the session is locked, and only then, it injects there, and on
 * this second pipe answers each request with a picture of it (else "not locked": the agent's own capture
 * is the one to use). An unlocked session's secure desktop (a UAC prompt) stays out of reach. */
#define RS_PIPE_FMT L"\\\\.\\pipe\\RemoDroidScreen-%lu" /* + the session id */
#define RS_MAX_PIXELS (8192 * 8192)

typedef struct {
    int32_t x, y, w, h; /* this part of the virtual desktop, in pixels */
} rs_req_t;

typedef struct {
    int32_t locked, w, h, pad; /* locked: w × h BGRA pixels follow */
} rs_reply_t;

/* Whether Windows reports this process's session as locked (input.c). */
int ri_session_locked(void);

#endif
