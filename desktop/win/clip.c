/* Windows clipboard (Unicode text). Owning the clipboard means answering
 * messages other programs send synchronously (WM_DESTROYCLIPBOARD…), so the
 * owner window lives on its own thread with a message loop; the agent thread
 * asks it with SendMessageTimeout and never blocks other programs. */
#include "plat.h"

#include <stdlib.h>
#include <string.h>

#include "desk.h"

#define WM_CLIP_GET (WM_APP + 1)
#define WM_CLIP_SET (WM_APP + 2)

static struct {
    INIT_ONCE once;
    HWND hwnd;
    HANDLE ready;
} C = { INIT_ONCE_STATIC_INIT };

static int open_clipboard(HWND owner)
{
    for (int i = 0; i < 20; i++) { /* another program may hold it for a moment */
        if (OpenClipboard(owner))
            return 1;
        Sleep(10);
    }
    return 0;
}

static char *get_text(void)
{
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !open_clipboard(C.hwnd))
        return NULL;
    char *out = NULL;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    const wchar_t *w = h ? GlobalLock(h) : NULL;
    if (w) {
        int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
        out = malloc(n > 0 ? (size_t)n : 1);
        if (n <= 0 || !WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, NULL, NULL))
            out[0] = 0;
        GlobalUnlock(h);
        char *d = out; /* CRLF → LF */
        for (const char *s = out; *s; s++)
            if (!(*s == '\r' && s[1] == '\n'))
                *d++ = *s;
        *d = 0;
    }
    CloseClipboard();
    return out;
}

static int set_text(const char *utf8)
{
    size_t lf = 0;
    for (const char *s = utf8; *s; s++)
        lf += *s == '\n';
    char *crlf = malloc(strlen(utf8) + lf + 1), *d = crlf; /* LF → CRLF */
    for (const char *s = utf8; *s; s++) {
        if (*s == '\n' && (s == utf8 || s[-1] != '\r'))
            *d++ = '\r';
        *d++ = *s;
    }
    *d = 0;
    int n = MultiByteToWideChar(CP_UTF8, 0, crlf, -1, NULL, 0);
    HGLOBAL g = n > 0 ? GlobalAlloc(GMEM_MOVEABLE, sizeof(wchar_t) * (size_t)n) : NULL;
    int ok = 0;
    if (g) {
        MultiByteToWideChar(CP_UTF8, 0, crlf, -1, GlobalLock(g), n);
        GlobalUnlock(g);
        if (open_clipboard(C.hwnd)) {
            EmptyClipboard();
            ok = SetClipboardData(CF_UNICODETEXT, g) != NULL;
            CloseClipboard();
        }
        if (!ok)
            GlobalFree(g);
    }
    free(crlf);
    return ok ? 0 : -1;
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_CLIP_GET) {
        *(char **)lp = get_text();
        return 1;
    }
    if (msg == WM_CLIP_SET)
        return set_text((const char *)lp) == 0 ? 1 : 2;
    return DefWindowProcW(h, msg, wp, lp);
}

static DWORD WINAPI thread_main(LPVOID arg)
{
    WNDCLASSW wc = { .lpfnWndProc = wndproc, .hInstance = GetModuleHandleW(NULL), .lpszClassName = L"RemoDroidClip" };
    RegisterClassW(&wc);
    C.hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
    SetEvent(C.ready);
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}

static BOOL CALLBACK start(PINIT_ONCE o, PVOID p, PVOID *ctx)
{
    C.ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE th = CreateThread(NULL, 0, thread_main, NULL, 0, NULL);
    if (th) {
        WaitForSingleObject(C.ready, 5000);
        CloseHandle(th);
    }
    return TRUE;
}

static HWND owner(void)
{
    InitOnceExecuteOnce(&C.once, start, NULL, NULL);
    return C.hwnd;
}

unsigned long dsk_clip_seq(void) { return GetClipboardSequenceNumber(); }

char *dsk_clip_get(void)
{
    HWND h = owner();
    char *s = NULL;
    DWORD_PTR r = 0;
    if (!h || !SendMessageTimeoutW(h, WM_CLIP_GET, 0, (LPARAM)&s, SMTO_ABORTIFHUNG, 2000, &r))
        return NULL;
    return s;
}

int dsk_clip_set(const char *utf8)
{
    HWND h = owner();
    DWORD_PTR r = 0;
    if (!h || !SendMessageTimeoutW(h, WM_CLIP_SET, 0, (LPARAM)utf8, SMTO_ABORTIFHUNG, 2000, &r))
        return -1;
    return r == 1 ? 0 : -1;
}
