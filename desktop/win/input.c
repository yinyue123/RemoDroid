/* Windows input with SendInput: absolute moves across the virtual desktop,
 * keys as scan codes (physical keys, the computer's layout applies), text as
 * KEYEVENTF_UNICODE. Windows of elevated programs ignore input from the agent
 * (UIPI), so when the RemoDroidInput service is installed the events go to its
 * helper instead, which injects them as SYSTEM (inputpipe.h). The helper builds
 * this file with RD_INPUT_HELPER: the same injection, driven by ri_apply(). */
#include "plat.h"

#include <aclapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "desk.h"
#include "inputpipe.h"

static double acc_x, acc_y; /* scroll remainders, in WHEEL_DELTA units */

static void send1(INPUT *in) { SendInput(1, in, sizeof *in); }

static void local_move(int px, int py)
{
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw < 2 || vh < 2)
        return;
    INPUT in = { .type = INPUT_MOUSE };
    /* 0..65535 spans the virtual desktop; the rounding lands on the intended pixel */
    in.mi.dx = (LONG)(((px - vx) * 65536.0 + 32768) / vw);
    in.mi.dy = (LONG)(((py - vy) * 65536.0 + 32768) / vh);
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    send1(&in);
}

static void local_button(int b, int down)
{
    static const DWORD dn[] = { MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_XDOWN,
                                MOUSEEVENTF_XDOWN };
    static const DWORD up[] = { MOUSEEVENTF_LEFTUP, MOUSEEVENTF_MIDDLEUP, MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_XUP,
                                MOUSEEVENTF_XUP };
    if (b < 0 || b > 4)
        return;
    INPUT in = { .type = INPUT_MOUSE };
    in.mi.dwFlags = down ? dn[b] : up[b];
    if (b == 3)
        in.mi.mouseData = XBUTTON1;
    else if (b == 4)
        in.mi.mouseData = XBUTTON2;
    send1(&in);
}

static void local_scroll(double dx, double dy)
{
    acc_y += dy * WHEEL_DELTA;
    acc_x += -dx * WHEEL_DELTA; /* HWHEEL: positive = right; ours: positive = left */
    int vy = (int)acc_y, vx = (int)acc_x;
    acc_y -= vy;
    acc_x -= vx;
    INPUT in = { .type = INPUT_MOUSE };
    if (vy) {
        in.mi.dwFlags = MOUSEEVENTF_WHEEL;
        in.mi.mouseData = (DWORD)vy;
        send1(&in);
    }
    if (vx) {
        in.mi.dwFlags = MOUSEEVENTF_HWHEEL;
        in.mi.mouseData = (DWORD)vx;
        send1(&in);
    }
}

static void local_key(int hid, int down)
{
    const keymap_t *k = keymap_find(hid);
    if (!k)
        return;
    INPUT in = { .type = INPUT_KEYBOARD };
    if (hid == 0x48 || hid == 0x90 || hid == 0x91) { /* Pause, Lang1 (Hangul), Lang2 (Hanja): virtual keys */
        in.ki.wVk = hid == 0x48 ? VK_PAUSE : hid == 0x90 ? VK_HANGUL : VK_HANJA;
        in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
        send1(&in);
        return;
    }
    if (!k->win)
        return;
    in.ki.wScan = (WORD)(k->win & 0xFF);
    in.ki.dwFlags = KEYEVENTF_SCANCODE | ((k->win & 0xFF00) == 0xE000 ? KEYEVENTF_EXTENDEDKEY : 0) |
                    (down ? 0 : KEYEVENTF_KEYUP);
    send1(&in);
}

static void local_text(const char *utf8)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (n <= 1)
        return;
    wchar_t *w = malloc(sizeof(wchar_t) * (size_t)n);
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, n);
    for (int i = 0; w[i]; i++) {
        INPUT in[2] = { { .type = INPUT_KEYBOARD }, { .type = INPUT_KEYBOARD } };
        if (w[i] == '\n' || w[i] == '\r') { /* Enter as the key: text boxes treat a typed CR differently */
            if (w[i] == '\r' && w[i + 1] == '\n')
                continue;
            in[0].ki.wVk = in[1].ki.wVk = VK_RETURN;
            in[1].ki.dwFlags = KEYEVENTF_KEYUP;
        } else {
            in[0].ki.wScan = in[1].ki.wScan = w[i]; /* surrogate halves go one by one, as Windows expects */
            in[0].ki.dwFlags = KEYEVENTF_UNICODE;
            in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        }
        SendInput(2, in, sizeof in[0]);
    }
    free(w);
}

#ifdef RD_INPUT_HELPER

void ri_apply(const ri_msg_t *m, const char *text)
{
    switch (m->op) {
    case RI_MOVE: local_move(m->a, m->b); break;
    case RI_BUTTON: local_button(m->a, m->b); break;
    case RI_SCROLL: local_scroll(m->x, m->y); break;
    case RI_KEY: local_key(m->a, m->b); break;
    case RI_TEXT: if (text) local_text(text); break;
    }
}

#else

static HANDLE pipe_h = INVALID_HANDLE_VALUE;
static ULONGLONG pipe_retry; /* when to look for the helper again */
static int pipe_said;

/* The helper's pipe, if it is there and SYSTEM made it (anyone else could make one by that
 * name to read what the viewer types). */
static int pipe_open(void)
{
    if (pipe_h != INVALID_HANDLE_VALUE)
        return 1;
    ULONGLONG now = GetTickCount64();
    if (now < pipe_retry)
        return 0;
    pipe_retry = now + 3000;
    DWORD sid = 0;
    wchar_t name[64];
    ProcessIdToSessionId(GetCurrentProcessId(), &sid);
    swprintf(name, 64, RI_PIPE_FMT, (unsigned long)sid);
    HANDLE h = CreateFileW(name, GENERIC_WRITE | READ_CONTROL, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    PSID owner = NULL;
    PSECURITY_DESCRIPTOR sd = NULL;
    int ok = GetSecurityInfo(h, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &owner, NULL, NULL, NULL, &sd) ==
                 ERROR_SUCCESS && owner && IsWellKnownSid(owner, WinLocalSystemSid);
    LocalFree(sd);
    if (!ok) {
        CloseHandle(h);
        if (!pipe_said++)
            LOGW("input: %ls is not the RemoDroidInput service's; ignoring it", name);
        return 0;
    }
    pipe_h = h;
    LOGI("input: through the RemoDroidInput service (windows of elevated programs included)");
    return 1;
}

/* Sends one event to the helper; 0 if there is none (inject it here instead). */
static int forward(int op, int a, int b, double x, double y, const char *text)
{
    if (!pipe_open())
        return 0;
    ri_msg_t m = { .op = op, .a = a, .b = b, .x = x, .y = y };
    DWORD n;
    if (WriteFile(pipe_h, &m, sizeof m, &n, NULL) && n == sizeof m &&
        (!text || (WriteFile(pipe_h, text, (DWORD)m.a, &n, NULL) && n == (DWORD)m.a)))
        return 1;
    LOGW("input: the RemoDroidInput service went away; injecting directly");
    CloseHandle(pipe_h);
    pipe_h = INVALID_HANDLE_VALUE;
    return 0;
}

void dsk_mouse_move(const dsk_screen_t *s, double nx, double ny)
{
    int px = s->x + (int)(nx * (s->w - 1) + 0.5), py = s->y + (int)(ny * (s->h - 1) + 0.5);
    if (!forward(RI_MOVE, px, py, 0, 0, NULL))
        local_move(px, py);
}

void dsk_mouse_button(int b, int down)
{
    if (!forward(RI_BUTTON, b, down, 0, 0, NULL))
        local_button(b, down);
}

void dsk_mouse_scroll(double dx, double dy)
{
    if (!forward(RI_SCROLL, 0, 0, dx, dy, NULL))
        local_scroll(dx, dy);
}

void dsk_key(int hid, int down)
{
    if (!forward(RI_KEY, hid, down, 0, 0, NULL))
        local_key(hid, down);
}

void dsk_text(const char *utf8)
{
    size_t n = strlen(utf8);
    if (n && n < RI_TEXT_MAX && forward(RI_TEXT, (int)n, 0, 0, 0, utf8))
        return;
    local_text(utf8);
}

#endif
