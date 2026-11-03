/* The panel in a window of its own on Windows: "Open window" in the tray menu shows the panel (device list,
 * viewer) in WebView2 — the Edge engine that ships with Windows 11 and comes with Edge on Windows 10 — instead
 * of the default browser. Without the WebView2 Runtime, rd_web_show() fails and the caller opens the browser.
 *
 * The runtime is found the way WebView2Loader.dll finds it: the EdgeUpdate ClientState key of the stable
 * channel names its folder, whose EmbeddedBrowserWebView.dll creates the environment. The profile (the sign-in
 * cookie among it) lives in %LOCALAPPDATA%\RemoDroid\WebView2. Links to other sites open in the default
 * browser; WebView2 handles downloads, file pickers and dialogs itself. The window menu (Alt+Space) has
 * Reload and Open in browser. Closing the window closes the page, and any viewer session in it. */
#ifndef UNICODE
#define UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <wctype.h>

#include "web.h"
#include "wv2.h"

#define MIN_RUNTIME 86 /* every interface method used here is in ICoreWebView2 1.0 */
#define SC_RELOAD 0x0010
#define SC_BROWSER 0x0020

static const wchar_t WEB_CLASS[] = L"RemoDroidPanel";
static const wchar_t STABLE[] = L"{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";

static HINSTANCE g_inst;
static HWND g_hwnd;
static ICoreWebView2Environment *g_env;
static ICoreWebView2Controller *g_ctl;
static ICoreWebView2 *g_view;
static wchar_t g_url[2048]; /* the panel's address */
static int g_creating, g_fullscreen;
static WINDOWPLACEMENT g_place = { sizeof g_place };

static void open_browser(const wchar_t *url) { ShellExecuteW(NULL, L"open", url, NULL, NULL, SW_SHOWNORMAL); }

/* "host[:port]" of an http(s) URL, lowercased; 0 if it is not http(s) */
static int host_of(const wchar_t *url, wchar_t *out, size_t cap)
{
    const wchar_t *p;
    if (!_wcsnicmp(url, L"https://", 8))
        p = url + 8;
    else if (!_wcsnicmp(url, L"http://", 7))
        p = url + 7;
    else
        return 0;
    size_t n = wcscspn(p, L"/?#");
    if (n >= cap)
        n = cap - 1;
    for (size_t i = 0; i < n; i++)
        out[i] = towlower(p[i]);
    out[n] = 0;
    return 1;
}

static int same_panel(const wchar_t *url)
{
    wchar_t a[256], b[256];
    return host_of(url, a, 256) && host_of(g_url, b, 256) && !wcscmp(a, b);
}

/* ---------------- finding the runtime ---------------- */

/* The folder of the installed WebView2 Runtime (e.g. …\EdgeWebView\Application\131.0.2903.70), if new enough. */
static int runtime_dir(wchar_t *dir, DWORD cap)
{
    static const HKEY roots[] = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER }; /* machine-wide, or per user */
    wchar_t sub[120];
    swprintf(sub, 120, L"SOFTWARE\\Microsoft\\EdgeUpdate\\ClientState\\%ls", STABLE);
    for (int r = 0; r < 2; r++) {
        HKEY k;
        if (RegOpenKeyExW(roots[r], sub, 0, KEY_READ | KEY_WOW64_32KEY, &k) != ERROR_SUCCESS) /* EdgeUpdate is 32-bit */
            continue;
        DWORD n = cap * sizeof(wchar_t);
        LSTATUS e = RegGetValueW(k, NULL, L"EBWebView", RRF_RT_REG_SZ, NULL, dir, &n);
        RegCloseKey(k);
        if (e != ERROR_SUCCESS || !dir[0])
            continue;
        size_t len = wcslen(dir);
        while (len && (dir[len - 1] == L'\\' || dir[len - 1] == L'/'))
            dir[--len] = 0;
        const wchar_t *ver = wcsrchr(dir, L'\\'); /* the folder is named after the version */
        if (ver && _wtoi(ver + 1) >= MIN_RUNTIME)
            return 1;
    }
    return 0;
}

typedef HRESULT(STDMETHODCALLTYPE *create_env_t)(BOOL, int, LPCWSTR, IUnknown *,
                                                  ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *);

static create_env_t runtime_entry(void)
{
    static create_env_t f;
    if (f)
        return f;
    wchar_t dir[MAX_PATH], dll[MAX_PATH + 80];
    if (!runtime_dir(dir, MAX_PATH))
        return NULL;
#if defined(__aarch64__) || defined(_M_ARM64)
    const wchar_t *arch = L"arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    const wchar_t *arch = L"x64";
#else
    const wchar_t *arch = L"x86";
#endif
    swprintf(dll, MAX_PATH + 80, L"%ls\\EBWebView\\%ls\\EmbeddedBrowserWebView.dll", dir, arch);
    HMODULE m = LoadLibraryExW(dll, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (m)
        f = (create_env_t)(void *)GetProcAddress(m, "CreateWebViewEnvironmentWithOptionsInternal");
    return f;
}

/* ---------------- COM objects handed to WebView2 (static, so reference counts don't matter) ---------------- */

#define UNKNOWN_METHODS(T, IID_T)                                                                                    \
    static HRESULT STDMETHODCALLTYPE T##_qi(T *self, REFIID riid, void **out)                                     \
    {                                                                                                              \
        if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_T)) {                                         \
            *out = self;                                                                                           \
            return S_OK;                                                                                           \
        }                                                                                                          \
        *out = NULL;                                                                                               \
        return E_NOINTERFACE;                                                                                      \
    }                                                                                                              \
    static ULONG STDMETHODCALLTYPE T##_ref(T *self) { return 1; }

static wchar_t *co_str(const wchar_t *s)
{
    size_t n = (wcslen(s) + 1) * sizeof(wchar_t);
    wchar_t *w = CoTaskMemAlloc(n);
    if (w)
        memcpy(w, s, n);
    return w;
}

/* environment options: let the viewer's video and sound start without a click first */
typedef ICoreWebView2EnvironmentOptions opts_t;
UNKNOWN_METHODS(opts_t, IID_ICoreWebView2EnvironmentOptions)
static HRESULT STDMETHODCALLTYPE opts_args(opts_t *o, LPWSTR *v)
{
    *v = co_str(L"--autoplay-policy=no-user-gesture-required");
    return *v ? S_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE opts_lang(opts_t *o, LPWSTR *v)
{
    *v = co_str(L"");
    return *v ? S_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE opts_target(opts_t *o, LPWSTR *v)
{
    *v = co_str(L"86.0.616.0");
    return *v ? S_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE opts_put(opts_t *o, LPCWSTR v) { return S_OK; }
static HRESULT STDMETHODCALLTYPE opts_sso(opts_t *o, BOOL *v)
{
    *v = FALSE;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE opts_put_sso(opts_t *o, BOOL v) { return S_OK; }
static const struct ICoreWebView2EnvironmentOptionsVtbl opts_vt = {
    opts_t_qi, opts_t_ref, opts_t_ref, opts_args, opts_put, opts_lang, opts_put, opts_target, opts_put, opts_sso, opts_put_sso,
};
static opts_t g_opts = { &opts_vt };

static void title_changed(void);
static void fullscreen_changed(void);
static void controller_ready(ICoreWebView2Controller *c);
static void env_failed(void);

/* environment created → a controller in our window */
typedef ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler env_done_t;
typedef ICoreWebView2CreateCoreWebView2ControllerCompletedHandler ctl_done_t;
UNKNOWN_METHODS(env_done_t, IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)
UNKNOWN_METHODS(ctl_done_t, IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)
static HRESULT STDMETHODCALLTYPE ctl_done(ctl_done_t *self, HRESULT hr, ICoreWebView2Controller *c)
{
    g_creating = 0;
    if (FAILED(hr) || !c) {
        env_failed();
        return S_OK;
    }
    controller_ready(c);
    return S_OK;
}
static const struct ICoreWebView2CreateCoreWebView2ControllerCompletedHandlerVtbl ctl_done_vt = { ctl_done_t_qi, ctl_done_t_ref,
                                                                                                    ctl_done_t_ref, ctl_done };
static ctl_done_t g_ctl_done = { &ctl_done_vt };

static HRESULT STDMETHODCALLTYPE env_done(env_done_t *self, HRESULT hr, ICoreWebView2Environment *e)
{
    if (FAILED(hr) || !e) {
        g_creating = 0;
        env_failed();
        return S_OK;
    }
    if (!g_hwnd) { /* closed while starting */
        g_creating = 0;
        return S_OK;
    }
    e->lpVtbl->AddRef(e);
    g_env = e;
    if (FAILED(e->lpVtbl->CreateCoreWebView2Controller(e, g_hwnd, &g_ctl_done))) {
        g_creating = 0;
        env_failed();
    }
    return S_OK;
}
static const struct ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandlerVtbl env_done_vt = { env_done_t_qi, env_done_t_ref,
                                                                                                    env_done_t_ref, env_done };
static env_done_t g_env_done = { &env_done_vt };

/* a link to another site: to the default browser */
typedef ICoreWebView2NavigationStartingEventHandler nav_t;
UNKNOWN_METHODS(nav_t, IID_ICoreWebView2NavigationStartingEventHandler)
static HRESULT STDMETHODCALLTYPE nav_starting(nav_t *self, ICoreWebView2 *v, ICoreWebView2NavigationStartingEventArgs *a)
{
    LPWSTR uri = NULL;
    BOOL user = FALSE;
    wchar_t h[256];
    if (SUCCEEDED(a->lpVtbl->get_Uri(a, &uri)) && uri && SUCCEEDED(a->lpVtbl->get_IsUserInitiated(a, &user)) && user &&
        host_of(uri, h, 256) && !same_panel(uri)) {
        a->lpVtbl->put_Cancel(a, TRUE);
        open_browser(uri);
    }
    CoTaskMemFree(uri);
    return S_OK;
}
static const struct ICoreWebView2NavigationStartingEventHandlerVtbl nav_vt = { nav_t_qi, nav_t_ref, nav_t_ref, nav_starting };
static nav_t g_nav = { &nav_vt };

/* target="_blank" / window.open(): the panel's pages load here, other sites go to the browser */
typedef ICoreWebView2NewWindowRequestedEventHandler neww_t;
UNKNOWN_METHODS(neww_t, IID_ICoreWebView2NewWindowRequestedEventHandler)
static HRESULT STDMETHODCALLTYPE new_window(neww_t *self, ICoreWebView2 *v, ICoreWebView2NewWindowRequestedEventArgs *a)
{
    LPWSTR uri = NULL;
    wchar_t h[256];
    if (SUCCEEDED(a->lpVtbl->get_Uri(a, &uri)) && uri && host_of(uri, h, 256)) {
        a->lpVtbl->put_Handled(a, TRUE);
        if (same_panel(uri))
            v->lpVtbl->Navigate(v, uri);
        else
            open_browser(uri);
    }
    CoTaskMemFree(uri);
    return S_OK;
}
static const struct ICoreWebView2NewWindowRequestedEventHandlerVtbl neww_vt = { neww_t_qi, neww_t_ref, neww_t_ref, new_window };
static neww_t g_neww = { &neww_vt };

typedef ICoreWebView2DocumentTitleChangedEventHandler title_t;
UNKNOWN_METHODS(title_t, IID_ICoreWebView2DocumentTitleChangedEventHandler)
static HRESULT STDMETHODCALLTYPE on_title(title_t *self, ICoreWebView2 *v, IUnknown *a)
{
    title_changed();
    return S_OK;
}
static const struct ICoreWebView2DocumentTitleChangedEventHandlerVtbl title_vt = { title_t_qi, title_t_ref, title_t_ref, on_title };
static title_t g_title = { &title_vt };

typedef ICoreWebView2ContainsFullScreenElementChangedEventHandler fs_t;
UNKNOWN_METHODS(fs_t, IID_ICoreWebView2ContainsFullScreenElementChangedEventHandler)
static HRESULT STDMETHODCALLTYPE on_fullscreen(fs_t *self, ICoreWebView2 *v, IUnknown *a)
{
    fullscreen_changed();
    return S_OK;
}
static const struct ICoreWebView2ContainsFullScreenElementChangedEventHandlerVtbl fs_vt = { fs_t_qi, fs_t_ref, fs_t_ref, on_fullscreen };
static fs_t g_fs = { &fs_vt };

/* ---------------- the window ---------------- */

static void fit(void)
{
    RECT r;
    if (g_ctl && GetClientRect(g_hwnd, &r))
        g_ctl->lpVtbl->put_Bounds(g_ctl, r);
}

static void controller_ready(ICoreWebView2Controller *c)
{
    if (!g_hwnd) {
        c->lpVtbl->Close(c);
        return;
    }
    c->lpVtbl->AddRef(c);
    g_ctl = c;
    if (FAILED(c->lpVtbl->get_CoreWebView2(c, &g_view)) || !g_view) {
        env_failed();
        return;
    }
    EventRegistrationToken t;
    g_view->lpVtbl->add_NavigationStarting(g_view, &g_nav, &t);
    g_view->lpVtbl->add_NewWindowRequested(g_view, &g_neww, &t);
    g_view->lpVtbl->add_DocumentTitleChanged(g_view, &g_title, &t);
    g_view->lpVtbl->add_ContainsFullScreenElementChanged(g_view, &g_fs, &t);
    c->lpVtbl->put_IsVisible(c, TRUE);
    fit();
    g_view->lpVtbl->Navigate(g_view, g_url);
    if (GetForegroundWindow() == g_hwnd)
        c->lpVtbl->MoveFocus(c, COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
}

/* WebView2 could not start after all: the browser it is */
static void env_failed(void)
{
    if (g_hwnd)
        DestroyWindow(g_hwnd);
    open_browser(g_url);
}

static void title_changed(void)
{
    LPWSTR t = NULL;
    if (g_view && SUCCEEDED(g_view->lpVtbl->get_DocumentTitle(g_view, &t)) && t && t[0])
        SetWindowTextW(g_hwnd, t);
    else
        SetWindowTextW(g_hwnd, L"RemoDroid");
    CoTaskMemFree(t);
}

/* the page's full screen (the viewer's Full screen button): the window covers its monitor, without a frame */
static void fullscreen_changed(void)
{
    BOOL on = FALSE;
    if (!g_view || FAILED(g_view->lpVtbl->get_ContainsFullScreenElement(g_view, &on)) || !on == !g_fullscreen)
        return;
    g_fullscreen = on;
    LONG_PTR style = GetWindowLongPtrW(g_hwnd, GWL_STYLE);
    if (on) {
        MONITORINFO mi = { sizeof mi };
        GetWindowPlacement(g_hwnd, &g_place);
        GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, (style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(g_hwnd, &g_place);
        SetWindowPos(g_hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
}

static LRESULT CALLBACK web_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        fit();
        return 0;
    case WM_MOVE:
        if (g_ctl)
            g_ctl->lpVtbl->NotifyParentWindowPositionChanged(g_ctl);
        return 0;
    case WM_SETFOCUS:
        if (g_ctl)
            g_ctl->lpVtbl->MoveFocus(g_ctl, COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_RELOAD) {
            if (g_view)
                g_view->lpVtbl->Reload(g_view);
            return 0;
        }
        if ((wp & 0xFFF0) == SC_BROWSER) {
            LPWSTR src = NULL;
            if (g_view && SUCCEEDED(g_view->lpVtbl->get_Source(g_view, &src)) && src && host_of(src, (wchar_t[256]){ 0 }, 256))
                open_browser(src);
            else
                open_browser(g_url);
            CoTaskMemFree(src);
            return 0;
        }
        break;
    case WM_DESTROY:
        if (g_ctl) {
            g_ctl->lpVtbl->Close(g_ctl);
            g_ctl->lpVtbl->Release(g_ctl);
        }
        if (g_view)
            g_view->lpVtbl->Release(g_view);
        if (g_env) /* a fresh one next time: the browser process ends with its last window */
            g_env->lpVtbl->Release(g_env);
        g_env = NULL;
        g_ctl = NULL;
        g_view = NULL;
        g_hwnd = NULL;
        g_fullscreen = 0;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static int create_window(HICON icon)
{
    static int registered;
    if (!registered) {
        WNDCLASSW wc = { .lpfnWndProc = web_proc, .hInstance = g_inst, .lpszClassName = WEB_CLASS, .hIcon = icon,
                         .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW), .hbrBackground = GetSysColorBrush(COLOR_WINDOW) };
        if (!RegisterClassW(&wc))
            return -1;
        registered = 1;
    }
    /* 1200×800 at 96 dpi, kept inside the work area */
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    HDC dc = GetDC(NULL);
    int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(NULL, dc);
    int w = MulDiv(1200, dpi, 96), hgt = MulDiv(800, dpi, 96);
    if (w > wa.right - wa.left)
        w = wa.right - wa.left;
    if (hgt > wa.bottom - wa.top)
        hgt = wa.bottom - wa.top;
    g_hwnd = CreateWindowExW(0, WEB_CLASS, L"RemoDroid", WS_OVERLAPPEDWINDOW, wa.left + (wa.right - wa.left - w) / 2,
                             wa.top + (wa.bottom - wa.top - hgt) / 2, w, hgt, NULL, NULL, g_inst, NULL);
    if (!g_hwnd)
        return -1;
    HMENU m = GetSystemMenu(g_hwnd, FALSE);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, SC_RELOAD, L"Reload\tCtrl+R");
    AppendMenuW(m, MF_STRING, SC_BROWSER, L"Open in browser");
    return 0;
}

int rd_web_show(HINSTANCE inst, HICON icon, const char *url)
{
    g_inst = inst;
    wchar_t u[2048];
    if (!MultiByteToWideChar(CP_UTF8, 0, url, -1, u, 2048) || !host_of(u, (wchar_t[256]){ 0 }, 256))
        return -1;
    if (g_hwnd) { /* already open: to the front, on this panel */
        if (wcscmp(u, g_url)) {
            wcscpy(g_url, u);
            if (g_view)
                g_view->lpVtbl->Navigate(g_view, g_url);
        }
        if (IsIconic(g_hwnd))
            ShowWindow(g_hwnd, SW_RESTORE);
        SetForegroundWindow(g_hwnd);
        return 0;
    }
    if (g_creating)
        return 0;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED); /* WebView2 wants an STA on this (the UI) thread */
    create_env_t create = runtime_entry();
    if (!create)
        return -1; /* no WebView2 Runtime */
    wcscpy(g_url, u);
    if (create_window(icon) < 0)
        return -1;
    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(g_hwnd);
    g_creating = 1;
    wchar_t data[MAX_PATH + 40] = L"";
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", data, MAX_PATH);
    if (n && n < MAX_PATH) {
        wcscat(data, L"\\RemoDroid");
        CreateDirectoryW(data, NULL);
        wcscat(data, L"\\WebView2");
        CreateDirectoryW(data, NULL);
    }
    if (FAILED(create(TRUE, 0 /* installed runtime */, data[0] ? data : NULL, (IUnknown *)&g_opts, &g_env_done))) {
        g_creating = 0;
        DestroyWindow(g_hwnd);
        return -1;
    }
    return 0;
}
