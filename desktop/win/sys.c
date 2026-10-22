/* Windows backend: system information, monitors, folders, display power,
 * locking and keeping the display awake. The process is per-monitor DPI aware,
 * so monitor rectangles are physical pixels in virtual-desktop coordinates. */
#include "plat.h"

#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "desk.h"
#include "win.h"

const char *dsk_platform(void) { return "windows"; }

void win_to_utf8(const wchar_t *w, char *out, size_t cap)
{
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL) && cap)
        out[0] = 0;
}

wchar_t *win_from_utf8(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = malloc(sizeof(wchar_t) * (size_t)(n > 0 ? n : 1));
    if (n <= 0 || !MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n))
        w[0] = 0;
    return w;
}

void dsk_os_name(char *out, size_t cap)
{
    typedef LONG(WINAPI * rtl_get_version_t)(OSVERSIONINFOW *);
    OSVERSIONINFOW v = { .dwOSVersionInfoSize = sizeof v };
    rtl_get_version_t f = (rtl_get_version_t)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (!f || f(&v) != 0) {
        snprintf(out, cap, "Windows");
        return;
    }
    /* Windows 11 still reports 10.0; the build number tells them apart */
    snprintf(out, cap, "Windows %s (build %lu)", v.dwMajorVersion == 10 && v.dwBuildNumber >= 22000 ? "11"
                                                 : v.dwMajorVersion == 10                         ? "10"
                                                                                                  : "",
             v.dwBuildNumber);
}

int dsk_init(char *problem, size_t cap)
{
    /* physical pixels everywhere (Windows 10 1703+; older: system DPI aware) */
    typedef BOOL(WINAPI * set_ctx_t)(HANDLE);
    set_ctx_t f = (set_ctx_t)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    if (!f || !f((HANDLE)-4 /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */))
        SetProcessDPIAware();
    return dsk_problem(problem, cap) > 0 ? -1 : 0;
}

int dsk_problem(char *out, size_t cap)
{
    DWORD sess = 0;
    if (ProcessIdToSessionId(GetCurrentProcessId(), &sess) && sess == 0) {
        snprintf(out, cap, "RemoDroid runs as a service (session 0) and can't see the desktop; start it from the "
                           "user's session");
        return 1;
    }
    out[0] = 0;
    return 0;
}

typedef struct {
    dsk_screen_t *out;
    int max, n;
} enum_t;

static BOOL CALLBACK on_monitor(HMONITOR m, HDC dc, LPRECT r, LPARAM lp)
{
    enum_t *e = (enum_t *)lp;
    if (e->n >= e->max)
        return FALSE;
    MONITORINFOEXW mi = { .cbSize = sizeof mi };
    if (!GetMonitorInfoW(m, (MONITORINFO *)&mi))
        return TRUE;
    dsk_screen_t *s = &e->out[e->n];
    memset(s, 0, sizeof *s);
    s->index = e->n;
    s->x = mi.rcMonitor.left;
    s->y = mi.rcMonitor.top;
    s->w = s->pw = mi.rcMonitor.right - mi.rcMonitor.left;
    s->h = s->ph = mi.rcMonitor.bottom - mi.rcMonitor.top;
    s->primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    win_to_utf8(mi.szDevice, s->name, sizeof s->name); /* "\\.\DISPLAY1" */
    e->n++;
    return TRUE;
}

int dsk_screens(dsk_screen_t *out, int max)
{
    enum_t e = { out, max, 0 };
    EnumDisplayMonitors(NULL, NULL, on_monitor, (LPARAM)&e);
    return e.n;
}

/* ---------------- folders ---------------- */

static const GUID FOLDER_ROAMING = { 0x3EB685DB, 0x65F9, 0x4CF6, { 0xA0, 0x3A, 0xE3, 0xEF, 0x65, 0x72, 0x9F, 0x3D } };
static const GUID FOLDER_DOWNLOADS = { 0x374DE290, 0x123F, 0x4565, { 0x91, 0x64, 0x39, 0xC4, 0x92, 0x5E, 0x46, 0x7B } };

static void known_folder(const GUID *id, const char *env_fallback, char *out, size_t cap)
{
    PWSTR w = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, NULL, &w)) && w) {
        win_to_utf8(w, out, cap);
        CoTaskMemFree(w);
        return;
    }
    const char *e = getenv(env_fallback);
    snprintf(out, cap, "%s", e && *e ? e : rd_home_dir());
}

const char *dsk_config_dir(void)
{
    static char dir[MAX_PATH * 3];
    if (!dir[0]) {
        char base[MAX_PATH * 3];
        known_folder(&FOLDER_ROAMING, "APPDATA", base, sizeof base);
        snprintf(dir, sizeof dir, "%s\\RemoDroid", base);
        rd_mkdirs(dir, 0700);
    }
    return dir;
}

const char *dsk_downloads_dir(void)
{
    static char dir[MAX_PATH * 3];
    if (!dir[0]) {
        known_folder(&FOLDER_DOWNLOADS, "USERPROFILE", dir, sizeof dir);
        if (!strcmp(dir, rd_home_dir()))
            snprintf(dir + strlen(dir), sizeof dir - strlen(dir), "\\Downloads");
    }
    return dir;
}

/* ---------------- system ---------------- */

void dsk_notify(const char *title, const char *text) { /* the app shows them (rd_agent_cb_t.on_notify) */ }

int dsk_screen_power(int on)
{
    if (on) { /* any input wakes the displays */
        INPUT in = { .type = INPUT_MOUSE };
        in.mi.dwFlags = MOUSEEVENTF_MOVE;
        SendInput(1, &in, sizeof in);
        SetThreadExecutionState(ES_DISPLAY_REQUIRED);
        return 0;
    }
    return PostMessageW(HWND_BROADCAST, WM_SYSCOMMAND, SC_MONITORPOWER, 2) ? 0 : -1;
}

void dsk_session(int active)
{
    /* the agent thread lives as long as the agent, so the state sticks */
    SetThreadExecutionState(active ? ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED : ES_CONTINUOUS);
}

int dsk_lock(void) { return LockWorkStation() ? 0 : -1; }

/* gamepads: a virtual Xbox pad on Windows needs the ViGEmBus driver; not yet */
int dsk_pad_supported(char *why, size_t cap)
{
    snprintf(why, cap, "gamepads aren't supported on Windows yet");
    return 0;
}
int dsk_pad_open(int index) { return -1; }
void dsk_pad_update(int index, const dsk_pad_t *p) {}
void dsk_pad_close(int index) {}
