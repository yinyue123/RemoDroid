/* The panel in a window of its own on Windows: "Open window" in the tray menu shows the panel (device list,
 * viewer) in WebView2 — the Edge engine that ships with Windows 11 and comes with Edge on Windows 10 — instead
 * of the default browser. Without the WebView2 Runtime, rd_web_show() fails and the caller opens the browser.
 *
 * The runtime is found the way WebView2Loader.dll finds it: the EdgeUpdate ClientState key of the stable
 * channel names its folder, whose EmbeddedBrowserWebView.dll creates the environment. The profile (the sign-in
 * cookie among it) lives in %LOCALAPPDATA%\RemoDroid\WebView2. Links to other sites open in the default
 * browser; WebView2 handles downloads, file pickers and dialogs itself. The window menu (Alt+Space) has New tab,
 * New window, Reload and Open in browser.
 *
 * Tabs work as in Chrome. A strip across the top of the window holds them, with "+" after the last (Ctrl+T;
 * Ctrl+N for a window, Ctrl+W closes a tab, Ctrl+Tab and Ctrl+1…9 switch). A tab is dragged along the strip to
 * reorder it, pulled off the strip to become a window of its own that follows the pointer, and held over another
 * window's strip to join it. Every tab is a WebView2 controller of one shared environment; moving a tab moves its
 * controller to the other window (put_ParentWindow), so nothing reloads and a running viewer keeps going.
 * Closing a window closes its pages, and any viewer session in them. */
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
static ICoreWebView2Environment *g_env; /* one for every tab: one browser process, one profile */
static wchar_t g_url[2048];             /* the panel's address */

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

struct tab;
static void title_changed(ICoreWebView2 *v);
static void fullscreen_changed(ICoreWebView2 *v);
static void controller_ready(struct tab *t, ICoreWebView2Controller *c);
static void env_failed(void);
static void env_ready(void);

/* environment created → a controller per tab (env_ready); each tab is its own completion handler */
typedef ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler env_done_t;
typedef ICoreWebView2CreateCoreWebView2ControllerCompletedHandler ctl_done_t;
UNKNOWN_METHODS(env_done_t, IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)
UNKNOWN_METHODS(ctl_done_t, IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)
static HRESULT STDMETHODCALLTYPE ctl_done(ctl_done_t *self, HRESULT hr, ICoreWebView2Controller *c)
{
    if (FAILED(hr) || !c) {
        env_failed();
        return S_OK;
    }
    controller_ready((struct tab *)(void *)self, c);
    return S_OK;
}
static const struct ICoreWebView2CreateCoreWebView2ControllerCompletedHandlerVtbl ctl_done_vt = { ctl_done_t_qi, ctl_done_t_ref,
                                                                                                    ctl_done_t_ref, ctl_done };

static HRESULT STDMETHODCALLTYPE env_done(env_done_t *self, HRESULT hr, ICoreWebView2Environment *e)
{
    if (FAILED(hr) || !e) {
        env_failed();
        return S_OK;
    }
    e->lpVtbl->AddRef(e);
    g_env = e;
    env_ready();
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
    title_changed(v);
    return S_OK;
}
static const struct ICoreWebView2DocumentTitleChangedEventHandlerVtbl title_vt = { title_t_qi, title_t_ref, title_t_ref, on_title };
static title_t g_title = { &title_vt };

typedef ICoreWebView2ContainsFullScreenElementChangedEventHandler fs_t;
UNKNOWN_METHODS(fs_t, IID_ICoreWebView2ContainsFullScreenElementChangedEventHandler)
static HRESULT STDMETHODCALLTYPE on_fullscreen(fs_t *self, ICoreWebView2 *v, IUnknown *a)
{
    fullscreen_changed(v);
    return S_OK;
}
static const struct ICoreWebView2ContainsFullScreenElementChangedEventHandlerVtbl fs_vt = { fs_t_qi, fs_t_ref, fs_t_ref, on_fullscreen };
static fs_t g_fs = { &fs_vt };

/* ---------------- tabs and windows ---------------- */

#define MAX_WINS 32
#define MAX_TABS 64
#define WM_APP_DISCARD (WM_APP + 1) /* a window a drag emptied: destroyed once the drag is out of its handler */
#define SC_NEWTAB 0x0030
#define SC_NEWWIN 0x0040

struct win;

/* One tab: a WebView2 controller and its page. The controller's completion handler is the first member, so
 * the handler's self pointer is the tab. */
struct tab {
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler done;
    struct win *win;
    ICoreWebView2Controller *ctl;
    ICoreWebView2 *view;
    int requested;  /* its controller has been asked for */
    int closed;     /* closed before the controller arrived: dropped when it does */
    wchar_t url[2048];
    wchar_t title[256];
};

struct win {
    HWND hwnd;
    struct tab *tabs[MAX_TABS]; /* strip order */
    int n, sel;
    int fullscreen;
    WINDOWPLACEMENT place;
    HFONT font;
    int dpi;
    int hover, hover_close, hover_plus, tracking; /* the pointer over the strip; -1 when over no tab */
    int press_close, press_plus;                 /* a click that acts on release */
    int step, cell;                              /* one tab plus its gap, and one tab, as last laid out */
    struct tab *lifted;                          /* the tab being dragged along this strip */
    int lifted_x;                                /* its left edge, from the first tab's slot */
};

static struct win *g_wins[MAX_WINS];
static int g_nwin;
static struct win *g_front; /* the most recently active window */
static HICON g_icon;
static int g_env_state; /* 0 none, 1 being created, 2 ready */
static int g_dark;

/* the drag of a tab (one at a time): pressed, sliding along a strip, or carrying a window of its own */
enum { DRAG_NONE, DRAG_PRESSED, DRAG_SLIDING, DRAG_CARRYING };
static struct {
    int phase;
    struct tab *t;
    POINT start;  /* where the press was, on screen */
    int grab;     /* the pointer's offset into the tab */
    POINT anchor; /* the pointer minus the carried window's origin */
} g_drag;

static void request_controller(struct tab *t);
static void env_start(void);
static void win_select(struct win *w, int i);
static void tab_close(struct tab *t);
static void new_tab(struct win *w, const wchar_t *url);
static struct win *win_new(const RECT *frame);

static int S(const struct win *w, int px) { return MulDiv(px, w->dpi ? w->dpi : 96, 96); }
static int strip_h(const struct win *w) { return w->fullscreen ? 0 : S(w, 36); }
static int tabs_x(const struct win *w) { return S(w, 8); }

static struct win *win_of(HWND h)
{
    if (!h)
        return NULL;
    for (int i = 0; i < g_nwin; i++)
        if (g_wins[i]->hwnd == h)
            return g_wins[i];
    return NULL;
}

static struct tab *tab_of_view(ICoreWebView2 *v)
{
    for (int i = 0; i < g_nwin; i++)
        for (int j = 0; j < g_wins[i]->n; j++)
            if (g_wins[i]->tabs[j]->view == v)
                return g_wins[i]->tabs[j];
    return NULL;
}

static struct tab *tab_of_ctl(ICoreWebView2Controller *c)
{
    for (int i = 0; i < g_nwin; i++)
        for (int j = 0; j < g_wins[i]->n; j++)
            if (g_wins[i]->tabs[j]->ctl == c)
                return g_wins[i]->tabs[j];
    return NULL;
}

static int index_of(const struct win *w, const struct tab *t)
{
    for (int i = 0; i < w->n; i++)
        if (w->tabs[i] == t)
            return i;
    return -1;
}

static struct tab *selected(const struct win *w) { return w->n ? w->tabs[w->sel] : NULL; }

static void tab_remove(struct win *w, int i)
{
    memmove(&w->tabs[i], &w->tabs[i + 1], (size_t)(w->n - i - 1) * sizeof w->tabs[0]);
    w->n--;
}

static void tab_insert(struct win *w, int i, struct tab *t)
{
    memmove(&w->tabs[i + 1], &w->tabs[i], (size_t)(w->n - i) * sizeof w->tabs[0]);
    w->tabs[i] = t;
    w->n++;
    t->win = w;
}

/* the page area: the client area under the strip */
static void fit(struct tab *t)
{
    RECT r;
    if (t && t->ctl && GetClientRect(t->win->hwnd, &r)) {
        r.top = strip_h(t->win);
        t->ctl->lpVtbl->put_Bounds(t->ctl, r);
    }
}

static void strip_changed(struct win *w)
{
    RECT r;
    if (!w->hwnd || !GetClientRect(w->hwnd, &r))
        return;
    r.bottom = strip_h(w);
    InvalidateRect(w->hwnd, &r, FALSE);
}

/* the window's title (the taskbar's) is the selected tab's */
static void win_title(struct win *w)
{
    struct tab *t = selected(w);
    SetWindowTextW(w->hwnd, t && t->title[0] ? t->title : L"RemoDroid");
}

/* the dark or light strip follows the Windows app mode; the title bar does too, where DWM allows it */
static void read_theme(void)
{
    DWORD v = 1, n = sizeof v;
    g_dark = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                          L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &v, &n) == ERROR_SUCCESS && v == 0;
}

typedef HRESULT(WINAPI *dwm_attr_t)(HWND, DWORD, void *, DWORD);
static dwm_attr_t dwm(const char *name)
{
    static HMODULE m;
    if (!m)
        m = LoadLibraryW(L"dwmapi.dll");
    return m ? (dwm_attr_t)(void *)GetProcAddress(m, name) : NULL;
}

static void dark_frame(HWND h)
{
    dwm_attr_t set = dwm("DwmSetWindowAttribute");
    BOOL on = g_dark;
    if (set)
        set(h, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &on, sizeof on);
}

/* a window that is on screen but not drawn (an app on another virtual desktop) */
static int cloaked(HWND h)
{
    dwm_attr_t get = dwm("DwmGetWindowAttribute");
    DWORD c = 0;
    return get && SUCCEEDED(get(h, 14 /* DWMWA_CLOAKED */, &c, sizeof c)) && c;
}

static void win_font(struct win *w)
{
    NONCLIENTMETRICSW m = { sizeof m };
    if (w->font)
        DeleteObject(w->font);
    w->dpi = GetDpiForWindow(w->hwnd);
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof m, &m, 0, (UINT)w->dpi);
    w->font = CreateFontIndirectW(&m.lfMessageFont);
}

/* ---------------- the strip: layout, hit testing, drawing ---------------- */

/* Every tab gets an equal slice of the strip, up to a Chrome-like width: more tabs squeeze rather than
 * scroll, and a title too long for its tab ends in "…". The "+" follows the last tab. */
static void layout(struct win *w)
{
    RECT r;
    GetClientRect(w->hwnd, &r);
    int room = r.right - tabs_x(w) - S(w, 8) - S(w, 32);
    if (room < S(w, 40))
        room = S(w, 40);
    int slot = w->n ? room / w->n : room;
    int gap = S(w, 2);
    w->cell = slot - gap < S(w, 240) ? slot - gap : S(w, 240);
    if (w->cell < 1)
        w->cell = 1;
    w->step = w->cell + gap;
}

static int closable(const struct win *w) { return w->cell >= S(w, 72); }

static RECT tab_rect(const struct win *w, int x)
{
    RECT r = { tabs_x(w) + x, S(w, 6), tabs_x(w) + x + w->cell, strip_h(w) };
    return r;
}

static RECT close_rect(const struct win *w, RECT tab)
{
    int s = S(w, 18), cy = (tab.top + tab.bottom) / 2;
    RECT r = { tab.right - S(w, 8) - s, cy - s / 2, tab.right - S(w, 8), cy - s / 2 + s };
    return r;
}

static RECT plus_rect(const struct win *w)
{
    int s = S(w, 26), cy = (S(w, 6) + strip_h(w)) / 2, x = tabs_x(w) + w->n * w->step + S(w, 4);
    RECT r = { x, cy - s / 2, x + s, cy - s / 2 + s };
    return r;
}

/* which tab (and whether its close button) is at client point p; -1 for none */
static int hit_tab(struct win *w, POINT p, int *on_close)
{
    *on_close = 0;
    if (p.y < 0 || p.y >= strip_h(w))
        return -1;
    layout(w);
    for (int i = 0; i < w->n; i++) {
        RECT r = tab_rect(w, i * w->step);
        if (!PtInRect(&r, p))
            continue;
        RECT c = close_rect(w, r);
        *on_close = closable(w) && PtInRect(&c, p);
        return i;
    }
    return -1;
}

static int hit_plus(struct win *w, POINT p)
{
    layout(w);
    RECT r = plus_rect(w);
    return PtInRect(&r, p);
}

static COLORREF col_strip(void) { return g_dark ? RGB(32, 33, 36) : RGB(222, 225, 230); }
static COLORREF col_tab(void) { return g_dark ? RGB(53, 54, 58) : RGB(255, 255, 255); }
static COLORREF col_hover(void) { return g_dark ? RGB(44, 45, 49) : RGB(236, 238, 241); }
static COLORREF col_text(void) { return g_dark ? RGB(232, 234, 237) : RGB(32, 33, 36); }
static COLORREF col_glyph(void) { return g_dark ? RGB(154, 160, 166) : RGB(95, 99, 104); }
static COLORREF col_glyph_bg(void) { return g_dark ? RGB(70, 72, 77) : RGB(218, 220, 224); }

static void fill_round(HDC dc, RECT r, int radius, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, r.left, r.top, r.right + 1, r.bottom + 1, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
}

static void glyph_line(HDC dc, int x0, int y0, int x1, int y1)
{
    MoveToEx(dc, x0, y0, NULL);
    LineTo(dc, x1, y1);
}

static void draw_tab(struct win *w, HDC dc, int i, int x)
{
    struct tab *t = w->tabs[i];
    RECT r = tab_rect(w, x);
    int on = i == w->sel;
    if (on) {
        RECT body = r;
        body.bottom += S(w, 10); /* the bottom corners fall below the strip: the tab runs into the page */
        fill_round(dc, body, S(w, 16), col_tab());
    } else if (i == w->hover && !g_drag.t) {
        RECT body = r;
        body.bottom -= S(w, 4);
        fill_round(dc, body, S(w, 16), col_hover());
    }
    RECT text = { r.left + S(w, 12), r.top, r.right - S(w, 10), r.bottom };
    if (closable(w)) {
        RECT c = close_rect(w, r);
        text.right = c.left - S(w, 4);
        if (i == w->hover && w->hover_close && !g_drag.t)
            fill_round(dc, c, c.right - c.left, col_glyph_bg());
        HPEN pen = CreatePen(PS_SOLID, S(w, 1) + (w->dpi >= 144), col_glyph());
        HGDIOBJ op = SelectObject(dc, pen);
        int cx = (c.left + c.right) / 2, cy = (c.top + c.bottom) / 2, k = S(w, 4);
        glyph_line(dc, cx - k, cy - k, cx + k + 1, cy + k + 1);
        glyph_line(dc, cx + k, cy - k, cx - k - 1, cy + k + 1);
        SelectObject(dc, op);
        DeleteObject(pen);
    }
    const wchar_t *title = t->title[0] ? t->title : L"New tab";
    wchar_t shown[256];
    wcsncpy(shown, title, 255);
    shown[255] = 0;
    size_t n = wcslen(shown), suffix = wcslen(L" \x00b7 RemoDroid");
    if (n > suffix && !wcscmp(shown + n - suffix, L" \x00b7 RemoDroid"))
        shown[n - suffix] = 0; /* "Pixel 7 · RemoDroid" → "Pixel 7" */
    SetTextColor(dc, col_text());
    DrawTextW(dc, shown, -1, &text, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX | DT_LEFT);
}

static void paint(struct win *w)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(w->hwnd, &ps);
    RECT cr;
    GetClientRect(w->hwnd, &cr);
    int h = strip_h(w);
    if (h > 0 && cr.right > 0) {
        HDC m = CreateCompatibleDC(dc);
        HBITMAP bm = CreateCompatibleBitmap(dc, cr.right, h);
        HGDIOBJ obm = SelectObject(m, bm), ofont = SelectObject(m, w->font);
        RECT all = { 0, 0, cr.right, h };
        HBRUSH bg = CreateSolidBrush(col_strip());
        FillRect(m, &all, bg);
        DeleteObject(bg);
        SetBkMode(m, TRANSPARENT);
        layout(w);
        int lifted = w->lifted ? index_of(w, w->lifted) : -1;
        for (int i = 0; i < w->n; i++)
            if (i != lifted)
                draw_tab(w, m, i, i * w->step);
        if (lifted >= 0) { /* the dragged tab follows the pointer, over the tabs it passes */
            int x = w->lifted_x, end = (w->n - 1) * w->step;
            draw_tab(w, m, lifted, x < 0 ? 0 : x > end ? end : x);
        }
        RECT p = plus_rect(w);
        if (w->hover_plus && !g_drag.t)
            fill_round(m, p, p.right - p.left, col_hover());
        HPEN pen = CreatePen(PS_SOLID, S(w, 1) + (w->dpi >= 144), col_glyph());
        HGDIOBJ op = SelectObject(m, pen);
        int cx = (p.left + p.right) / 2, cy = (p.top + p.bottom) / 2, k = S(w, 6);
        glyph_line(m, cx - k, cy, cx + k + 1, cy);
        glyph_line(m, cx, cy - k, cx, cy + k + 1);
        SelectObject(m, op);
        DeleteObject(pen);
        BitBlt(dc, 0, 0, cr.right, h, m, 0, 0, SRCCOPY);
        SelectObject(m, ofont);
        SelectObject(m, obm);
        DeleteObject(bm);
        DeleteDC(m);
    }
    if (!selected(w) || !selected(w)->ctl) { /* the page area until the page is there */
        RECT below = { 0, h, cr.right, cr.bottom };
        HBRUSH b = CreateSolidBrush(col_tab());
        FillRect(dc, &below, b);
        DeleteObject(b);
    }
    EndPaint(w->hwnd, &ps);
}

/* ---------------- tabs: selecting, opening, closing ---------------- */

/* shows tab i of w and hides the rest (each tab keeps its page, so a hidden viewer keeps running) */
static void win_select(struct win *w, int i)
{
    if (i < 0 || i >= w->n)
        return;
    w->sel = i;
    for (int j = 0; j < w->n; j++)
        if (w->tabs[j]->ctl && j != i)
            w->tabs[j]->ctl->lpVtbl->put_IsVisible(w->tabs[j]->ctl, FALSE);
    struct tab *t = w->tabs[i];
    if (t->ctl) {
        fit(t);
        t->ctl->lpVtbl->put_IsVisible(t->ctl, TRUE);
        if (GetForegroundWindow() == w->hwnd)
            t->ctl->lpVtbl->MoveFocus(t->ctl, COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    }
    win_title(w);
    InvalidateRect(w->hwnd, NULL, FALSE);
}

static void win_step(struct win *w, int d)
{
    if (w->n > 1)
        win_select(w, ((w->sel + d) % w->n + w->n) % w->n);
}

static struct tab *tab_alloc(const wchar_t *url);

static void new_tab(struct win *w, const wchar_t *url)
{
    if (w->n >= MAX_TABS)
        return;
    struct tab *t = tab_alloc(url);
    if (!t)
        return;
    tab_insert(w, w->n, t);
    win_select(w, w->n - 1);
    if (g_env_state == 2)
        request_controller(t);
    else
        env_start();
}

/* where a new tab opens: the device page of this panel, to choose what it shows */
static void devices_url(wchar_t *out, size_t cap)
{
    wcsncpy(out, g_url, cap - 1);
    out[cap - 1] = 0;
    out[wcscspn(out, L"?#")] = 0;
    wchar_t *host = wcsstr(out, L"://");
    wchar_t *slash = wcsrchr(host ? host + 3 : out, L'/');
    if (slash)
        slash[1] = 0;
    else if (wcslen(out) + 1 < cap)
        wcscat(out, L"/");
    if (wcslen(out) + 20 < cap)
        wcscat(out, L"panel.html#devices");
}

static void open_tab(struct win *w)
{
    wchar_t u[2048];
    devices_url(u, 2048);
    new_tab(w, u);
}

static void open_window(struct win *from)
{
    RECT r;
    GetWindowRect(from->hwnd, &r);
    int d = S(from, 28);
    OffsetRect(&r, d, d);
    struct win *w = win_new(&r);
    if (!w)
        return;
    ShowWindow(w->hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(w->hwnd);
    open_tab(w);
}

static void tab_free(struct tab *t)
{
    if (t->ctl) {
        t->ctl->lpVtbl->Close(t->ctl);
        t->ctl->lpVtbl->Release(t->ctl);
    }
    if (t->view)
        t->view->lpVtbl->Release(t->view);
    if (t->requested && !t->ctl)
        t->closed = 1; /* its controller is on the way: dropped (and the tab freed) when it arrives */
    else
        free(t);
}

/* Closes a tab and its page. The tab that slides into its place is shown, as in Chrome; the last tab
 * closes the window. */
static void tab_close(struct tab *t)
{
    struct win *w = t->win;
    int i = index_of(w, t);
    if (i < 0)
        return;
    if (g_drag.t == t)
        g_drag.t = NULL, g_drag.phase = DRAG_NONE;
    int was = w->sel;
    tab_remove(w, i);
    tab_free(t);
    if (!w->n) {
        DestroyWindow(w->hwnd);
        return;
    }
    win_select(w, i == was ? (i < w->n ? i : w->n - 1) : (i < was ? was - 1 : was));
}

/* ---------------- dragging tabs ---------------- */

static RECT strip_on_screen(struct win *w)
{
    RECT r = { 0, 0, 0, strip_h(w) };
    RECT cr;
    GetClientRect(w->hwnd, &cr);
    r.right = cr.right;
    MapWindowPoints(w->hwnd, NULL, (POINT *)&r, 2);
    return r;
}

/* moves the dragged tab along its strip to x (its left edge from the first slot); it takes the slot its
 * middle is over */
static void slide(struct win *w, struct tab *t, int x)
{
    layout(w);
    int last = w->n - 1, step = w->step > 0 ? w->step : 1;
    if (x < 0)
        x = 0;
    if (x > last * step)
        x = last * step;
    int to = (x + step / 2) / step, from = index_of(w, t);
    if (to > last)
        to = last;
    if (from >= 0 && from != to) {
        tab_remove(w, from);
        tab_insert(w, to, t);
        w->sel = to;
    }
    w->lifted = t;
    w->lifted_x = x;
    strip_changed(w);
}

static void put_parent(struct tab *t, struct win *w)
{
    t->win = w;
    if (t->ctl)
        t->ctl->lpVtbl->put_ParentWindow(t->ctl, w->hwnd);
}

/* Takes the dragged tab out of its window into a window of its own, placed so the tab (first in its new
 * strip) stays under the pointer; the tab next to it takes over the window it leaves. The page moves with
 * the tab (put_ParentWindow): nothing reloads. */
static void tear_off(POINT p)
{
    struct tab *t = g_drag.t;
    struct win *from = t->win;
    int i = index_of(from, t);
    RECT frame;
    GetWindowRect(from->hwnd, &frame);
    struct win *w = win_new(&frame);
    if (!w)
        return;
    tab_remove(from, i);
    from->lifted = NULL;
    win_select(from, i < from->n ? i : from->n - 1);
    tab_insert(w, 0, t);
    put_parent(t, w);
    w->sel = 0;
    /* the window's client origin sits so the tab is under the pointer, as it was grabbed */
    POINT origin = { 0, 0 };
    ClientToScreen(w->hwnd, &origin);
    int dx = (p.x - g_drag.grab - tabs_x(w)) - origin.x, dy = (p.y - strip_h(w) / 2) - origin.y;
    SetWindowPos(w->hwnd, HWND_TOP, frame.left + dx, frame.top + dy, 0, 0, SWP_NOSIZE);
    ShowWindow(w->hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(w->hwnd);
    win_select(w, 0);
    SetCapture(w->hwnd);
    RECT now;
    GetWindowRect(w->hwnd, &now);
    g_drag.anchor.x = p.x - now.left;
    g_drag.anchor.y = p.y - now.top;
    g_drag.phase = DRAG_CARRYING;
}

/* the other window of ours whose strip is under the pointer, unless another window covers it there */
static struct win *strip_host(POINT p, struct win *self)
{
    for (HWND h = GetTopWindow(NULL); h; h = GetWindow(h, GW_HWNDNEXT)) {
        RECT r;
        if (h == self->hwnd || !IsWindowVisible(h) || IsIconic(h) || !GetWindowRect(h, &r) || !PtInRect(&r, p))
            continue;
        if ((GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TRANSPARENT) || cloaked(h))
            continue;
        struct win *o = win_of(h);
        if (!o || o->fullscreen)
            return NULL;
        RECT s = strip_on_screen(o);
        return PtInRect(&s, p) ? o : NULL;
    }
    return NULL;
}

/* puts the carried tab into host's strip where the pointer is, shows it there, and goes on as a slide */
static void join(struct win *host, POINT p)
{
    struct tab *t = g_drag.t;
    struct win *old = t->win;
    layout(host);
    RECT s = strip_on_screen(host);
    int x = p.x - g_drag.grab - s.left - tabs_x(host), step = host->step > 0 ? host->step : 1;
    int at = x < 0 ? 0 : (x + step / 2) / step;
    if (at > host->n)
        at = host->n;
    tab_remove(old, index_of(old, t));
    tab_insert(host, at, t);
    put_parent(t, host);
    SetForegroundWindow(host->hwnd);
    win_select(host, at);
    SetCapture(host->hwnd);
    ShowWindow(old->hwnd, SW_HIDE);
    PostMessageW(old->hwnd, WM_APP_DISCARD, 0, 0);
    g_drag.phase = DRAG_SLIDING;
    slide(host, t, x);
}

static void drag_move(POINT p)
{
    struct tab *t = g_drag.t;
    if (!t)
        return;
    struct win *w = t->win;
    if (g_drag.phase == DRAG_PRESSED) {
        if (abs(p.x - g_drag.start.x) < GetSystemMetrics(SM_CXDRAG) && abs(p.y - g_drag.start.y) < GetSystemMetrics(SM_CYDRAG))
            return;
        RECT r;
        GetWindowRect(w->hwnd, &r);
        g_drag.anchor.x = p.x - r.left;
        g_drag.anchor.y = p.y - r.top;
        g_drag.phase = w->n > 1 ? DRAG_SLIDING : DRAG_CARRYING;
        if (g_drag.phase == DRAG_CARRYING && IsZoomed(w->hwnd))
            g_drag.phase = DRAG_NONE; /* a maximized window's only tab stays put */
        strip_changed(w);
    }
    if (g_drag.phase == DRAG_SLIDING) {
        RECT s = strip_on_screen(w);
        int tear = MulDiv(24, w->dpi, 96);
        if ((p.y > s.top - tear && p.y < s.bottom + tear) || w->n < 2) {
            slide(w, t, p.x - g_drag.grab - s.left - tabs_x(w));
            return;
        }
        tear_off(p);
        return;
    }
    if (g_drag.phase == DRAG_CARRYING) {
        SetWindowPos(w->hwnd, NULL, p.x - g_drag.anchor.x, p.y - g_drag.anchor.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        struct win *host = strip_host(p, w);
        if (host)
            join(host, p);
    }
}

static void drag_end(void)
{
    struct tab *t = g_drag.t;
    g_drag.t = NULL;
    g_drag.phase = DRAG_NONE;
    for (int i = 0; i < g_nwin; i++)
        if (g_wins[i]->lifted) {
            g_wins[i]->lifted = NULL;
            strip_changed(g_wins[i]);
        }
    if (t && GetCapture() == t->win->hwnd)
        ReleaseCapture();
}

/* ---------------- the window ---------------- */

/* Ctrl+T, Ctrl+N, Ctrl+W, Ctrl+Tab / Ctrl+Shift+Tab, Ctrl+PgDn / Ctrl+PgUp, Ctrl+1…8 and Ctrl+9 (the last
 * tab), as in a browser; 1 if the key was one of them */
static int tab_key(struct win *w, UINT vk)
{
    if (GetKeyState(VK_CONTROL) >= 0 && GetAsyncKeyState(VK_CONTROL) >= 0)
        return 0;
    if ((GetKeyState(VK_MENU) | GetAsyncKeyState(VK_MENU)) < 0)
        return 0;
    int shift = (GetKeyState(VK_SHIFT) | GetAsyncKeyState(VK_SHIFT)) < 0;
    struct tab *t = selected(w);
    switch (vk) {
    case 'T': open_tab(w); return 1;
    case 'N': open_window(w); return 1;
    case 'W': if (t) tab_close(t); return 1;
    case VK_TAB: win_step(w, shift ? -1 : 1); return 1;
    case VK_NEXT: win_step(w, 1); return 1;
    case VK_PRIOR: win_step(w, -1); return 1;
    case '9': win_select(w, w->n - 1); return 1;
    default:
        if (vk >= '1' && vk <= '8') {
            win_select(w, (int)(vk - '1') < w->n ? (int)(vk - '1') : w->n - 1);
            return 1;
        }
    }
    return 0;
}

typedef ICoreWebView2AcceleratorKeyPressedEventHandler keys_t;
UNKNOWN_METHODS(keys_t, IID_ICoreWebView2AcceleratorKeyPressedEventHandler)
static HRESULT STDMETHODCALLTYPE on_key(keys_t *self, ICoreWebView2Controller *c, ICoreWebView2AcceleratorKeyPressedEventArgs *a)
{
    int kind = -1;
    UINT vk = 0;
    struct tab *t = tab_of_ctl(c);
    if (!t || FAILED(a->lpVtbl->get_KeyEventKind(a, &kind)) || kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN ||
        FAILED(a->lpVtbl->get_VirtualKey(a, &vk)))
        return S_OK;
    if (tab_key(t->win, vk))
        a->lpVtbl->put_Handled(a, TRUE);
    return S_OK;
}
static const struct ICoreWebView2AcceleratorKeyPressedEventHandlerVtbl keys_vt = { keys_t_qi, keys_t_ref, keys_t_ref, on_key };
static keys_t g_keys = { &keys_vt };

static void controller_ready(struct tab *t, ICoreWebView2Controller *c)
{
    if (t->closed) { /* the tab went before its page came */
        c->lpVtbl->Close(c);
        free(t);
        return;
    }
    c->lpVtbl->AddRef(c);
    t->ctl = c;
    if (FAILED(c->lpVtbl->get_CoreWebView2(c, &t->view)) || !t->view) {
        env_failed();
        return;
    }
    ICoreWebView2 *v = t->view;
    EventRegistrationToken tok;
    v->lpVtbl->add_NavigationStarting(v, &g_nav, &tok);
    v->lpVtbl->add_NewWindowRequested(v, &g_neww, &tok);
    v->lpVtbl->add_DocumentTitleChanged(v, &g_title, &tok);
    v->lpVtbl->add_ContainsFullScreenElementChanged(v, &g_fs, &tok);
    c->lpVtbl->add_AcceleratorKeyPressed(c, &g_keys, &tok);
    c->lpVtbl->put_ParentWindow(c, t->win->hwnd); /* it may have been dragged to another window meanwhile */
    v->lpVtbl->Navigate(v, t->url);
    struct win *w = t->win;
    if (selected(w) == t)
        win_select(w, w->sel);
    else
        c->lpVtbl->put_IsVisible(c, FALSE);
}

/* WebView2 could not start after all: the browser it is */
static void env_failed(void)
{
    while (g_nwin)
        DestroyWindow(g_wins[g_nwin - 1]->hwnd);
    open_browser(g_url);
}

static void title_changed(ICoreWebView2 *v)
{
    struct tab *t = tab_of_view(v);
    LPWSTR s = NULL;
    if (!t)
        return;
    t->title[0] = 0;
    if (SUCCEEDED(v->lpVtbl->get_DocumentTitle(v, &s)) && s) {
        wcsncpy(t->title, s, 255);
        t->title[255] = 0;
    }
    CoTaskMemFree(s);
    if (selected(t->win) == t)
        win_title(t->win);
    strip_changed(t->win);
}

/* the page's full screen (the viewer's Full screen button): the window covers its monitor, without a frame
 * or the strip */
static void fullscreen_changed(ICoreWebView2 *v)
{
    struct tab *t = tab_of_view(v);
    BOOL on = FALSE;
    if (!t || FAILED(v->lpVtbl->get_ContainsFullScreenElement(v, &on)))
        return;
    struct win *w = t->win;
    if (!on == !w->fullscreen)
        return;
    w->fullscreen = on;
    HWND h = w->hwnd;
    LONG_PTR style = GetWindowLongPtrW(h, GWL_STYLE);
    if (on) {
        MONITORINFO mi = { sizeof mi };
        GetWindowPlacement(h, &w->place);
        GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongPtrW(h, GWL_STYLE, (style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongPtrW(h, GWL_STYLE, (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(h, &w->place);
        SetWindowPos(h, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    fit(selected(w));
}

static void hover(struct win *w, POINT p)
{
    int on_close, i = hit_tab(w, p, &on_close), plus = hit_plus(w, p);
    if (i != w->hover || on_close != w->hover_close || plus != w->hover_plus) {
        w->hover = i;
        w->hover_close = on_close;
        w->hover_plus = plus;
        strip_changed(w);
    }
    if (!w->tracking) {
        TRACKMOUSEEVENT tme = { sizeof tme, TME_LEAVE, w->hwnd, 0 };
        w->tracking = TrackMouseEvent(&tme);
    }
}

static LRESULT CALLBACK web_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    struct win *w = win_of(h);
    if (!w)
        return DefWindowProcW(h, msg, wp, lp);
    POINT p = { (short)LOWORD(lp), (short)HIWORD(lp) };
    switch (msg) {
    case WM_PAINT:
        paint(w);
        return 0;
    case WM_ERASEBKGND:
        return 1; /* WM_PAINT covers it all */
    case WM_SIZE:
        fit(selected(w));
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_MOVE:
        if (selected(w) && selected(w)->ctl)
            selected(w)->ctl->lpVtbl->NotifyParentWindowPositionChanged(selected(w)->ctl);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE)
            g_front = w;
        break;
    case WM_SETFOCUS:
        if (selected(w) && selected(w)->ctl)
            selected(w)->ctl->lpVtbl->MoveFocus(selected(w)->ctl, COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        return 0;
    case WM_DPICHANGED: {
        RECT *r = (RECT *)lp;
        win_font(w);
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (lp && !wcscmp((const wchar_t *)lp, L"ImmersiveColorSet")) {
            read_theme();
            for (int i = 0; i < g_nwin; i++) {
                dark_frame(g_wins[i]->hwnd);
                InvalidateRect(g_wins[i]->hwnd, NULL, FALSE);
            }
        }
        break;
    case WM_KEYDOWN:
        if (tab_key(w, (UINT)wp))
            return 0;
        break;
    case WM_MOUSEMOVE:
        if (g_drag.t) {
            POINT s;
            GetCursorPos(&s);
            drag_move(s);
        } else
            hover(w, p);
        return 0;
    case WM_MOUSELEAVE:
        w->tracking = 0;
        w->hover = -1;
        w->hover_close = w->hover_plus = 0;
        strip_changed(w);
        return 0;
    case WM_LBUTTONDOWN: {
        int on_close, i = hit_tab(w, p, &on_close);
        w->press_close = w->press_plus = 0;
        if (i >= 0 && on_close) {
            w->press_close = 1;
            return 0;
        }
        if (i >= 0) { /* a press selects the tab, as in Chrome; moving it starts a drag */
            win_select(w, i);
            g_drag.t = w->tabs[i];
            g_drag.phase = DRAG_PRESSED;
            GetCursorPos(&g_drag.start);
            g_drag.grab = p.x - tab_rect(w, i * w->step).left;
            SetCapture(h);
            return 0;
        }
        if (hit_plus(w, p))
            w->press_plus = 1;
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g_drag.t) {
            drag_end();
            return 0;
        }
        int on_close, i = hit_tab(w, p, &on_close), was_close = w->press_close, was_plus = w->press_plus;
        w->press_close = w->press_plus = 0;
        if (was_close && i >= 0 && on_close)
            tab_close(w->tabs[i]); /* the last tab takes the window (and w) with it */
        else if (was_plus && hit_plus(w, p))
            open_tab(w);
        return 0;
    }
    case WM_MBUTTONUP: { /* middle click closes, as in a browser */
        int on_close, i = hit_tab(w, p, &on_close);
        if (i >= 0)
            tab_close(w->tabs[i]);
        return 0;
    }
    case WM_LBUTTONDBLCLK: /* on the empty strip: a new tab */
        if (hit_tab(w, p, &(int){ 0 }) < 0 && p.y < strip_h(w) && !hit_plus(w, p))
            open_tab(w);
        return 0;
    case WM_CAPTURECHANGED:
        if (g_drag.t && !win_of((HWND)lp))
            drag_end(); /* lost to something else (Alt+Tab, a dialog): the drag ends where it is */
        return 0;
    case WM_APP_DISCARD:
        if (!w->n)
            DestroyWindow(h);
        return 0;
    case WM_SYSCOMMAND: {
        struct tab *t = selected(w);
        switch (wp & 0xFFF0) {
        case SC_RELOAD:
            if (t && t->view)
                t->view->lpVtbl->Reload(t->view);
            return 0;
        case SC_BROWSER: {
            LPWSTR src = NULL;
            if (t && t->view && SUCCEEDED(t->view->lpVtbl->get_Source(t->view, &src)) && src && host_of(src, (wchar_t[256]){ 0 }, 256))
                open_browser(src);
            else
                open_browser(g_url);
            CoTaskMemFree(src);
            return 0;
        }
        case SC_NEWTAB:
            open_tab(w);
            return 0;
        case SC_NEWWIN:
            open_window(w);
            return 0;
        }
        break;
    }
    case WM_DESTROY: {
        if (g_drag.t && g_drag.t->win == w)
            drag_end();
        while (w->n) {
            struct tab *t = w->tabs[--w->n];
            tab_free(t);
        }
        if (w->font)
            DeleteObject(w->font);
        int i = 0;
        while (i < g_nwin && g_wins[i] != w)
            i++;
        if (i < g_nwin)
            memmove(&g_wins[i], &g_wins[i + 1], (size_t)(g_nwin - i - 1) * sizeof g_wins[0]);
        g_nwin--;
        if (g_front == w)
            g_front = g_nwin ? g_wins[g_nwin - 1] : NULL;
        free(w);
        if (!g_nwin && g_env_state == 2) { /* a fresh one next time: the browser process ends with its last window */
            g_env->lpVtbl->Release(g_env);
            g_env = NULL;
            g_env_state = 0;
        }
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* a window, hidden: frame is its outer rectangle, or NULL for 1200×800 (at 96 dpi) in the middle of the
 * work area */
static struct win *win_new(const RECT *frame)
{
    static int registered;
    if (g_nwin >= MAX_WINS)
        return NULL;
    if (!registered) {
        WNDCLASSW wc = { .style = CS_DBLCLKS, .lpfnWndProc = web_proc, .hInstance = g_inst, .lpszClassName = WEB_CLASS,
                         .hIcon = g_icon, .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW) };
        if (!RegisterClassW(&wc))
            return NULL;
        registered = 1;
        read_theme();
    }
    RECT r;
    if (frame)
        r = *frame;
    else {
        RECT wa;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        HDC dc = GetDC(NULL);
        int dpi = GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(NULL, dc);
        int cw = MulDiv(1200, dpi, 96), ch = MulDiv(800, dpi, 96);
        if (cw > wa.right - wa.left)
            cw = wa.right - wa.left;
        if (ch > wa.bottom - wa.top)
            ch = wa.bottom - wa.top;
        r.left = wa.left + (wa.right - wa.left - cw) / 2;
        r.top = wa.top + (wa.bottom - wa.top - ch) / 2;
        r.right = r.left + cw;
        r.bottom = r.top + ch;
    }
    struct win *w = calloc(1, sizeof *w);
    if (!w)
        return NULL;
    w->hover = -1;
    g_wins[g_nwin++] = w; /* before CreateWindow, so its first messages find it */
    w->hwnd = CreateWindowExW(0, WEB_CLASS, L"RemoDroid", WS_OVERLAPPEDWINDOW, r.left, r.top, r.right - r.left,
                              r.bottom - r.top, NULL, NULL, g_inst, NULL);
    if (!w->hwnd) {
        g_nwin--;
        free(w);
        return NULL;
    }
    win_font(w);
    dark_frame(w->hwnd);
    HMENU m = GetSystemMenu(w->hwnd, FALSE);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, SC_NEWTAB, L"New tab\tCtrl+T");
    AppendMenuW(m, MF_STRING, SC_NEWWIN, L"New window\tCtrl+N");
    AppendMenuW(m, MF_STRING, SC_RELOAD, L"Reload\tCtrl+R");
    AppendMenuW(m, MF_STRING, SC_BROWSER, L"Open in browser");
    return w;
}

/* ---------------- the environment, and the way in ---------------- */

static struct tab *tab_alloc(const wchar_t *url)
{
    struct tab *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->done.lpVtbl = &ctl_done_vt;
    wcsncpy(t->url, url, 2047);
    return t;
}

static void request_controller(struct tab *t)
{
    if (t->requested || !g_env)
        return;
    t->requested = 1;
    if (FAILED(g_env->lpVtbl->CreateCoreWebView2Controller(g_env, t->win->hwnd, &t->done))) {
        t->requested = 0;
        env_failed();
    }
}

/* every tab waiting for the environment gets its controller */
static void env_ready(void)
{
    if (!g_nwin) { /* every window closed while it started */
        g_env->lpVtbl->Release(g_env);
        g_env = NULL;
        g_env_state = 0;
        return;
    }
    g_env_state = 2;
    for (int i = 0; i < g_nwin; i++)
        for (int j = 0; j < g_wins[i]->n; j++)
            request_controller(g_wins[i]->tabs[j]);
}

static void env_start(void)
{
    if (g_env_state)
        return;
    create_env_t create = runtime_entry();
    wchar_t data[MAX_PATH + 40] = L"";
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", data, MAX_PATH);
    if (n && n < MAX_PATH) {
        wcscat(data, L"\\RemoDroid");
        CreateDirectoryW(data, NULL);
        wcscat(data, L"\\WebView2");
        CreateDirectoryW(data, NULL);
    }
    g_env_state = 1;
    if (!create || FAILED(create(TRUE, 0 /* installed runtime */, data[0] ? data : NULL, (IUnknown *)&g_opts, &g_env_done))) {
        g_env_state = 0;
        env_failed();
    }
}

int rd_web_show(HINSTANCE inst, HICON icon, const char *url)
{
    g_inst = inst;
    g_icon = icon;
    wchar_t u[2048];
    if (!MultiByteToWideChar(CP_UTF8, 0, url, -1, u, 2048) || !host_of(u, (wchar_t[256]){ 0 }, 256))
        return -1;
    struct win *w = g_front ? g_front : g_nwin ? g_wins[g_nwin - 1] : NULL;
    if (w) { /* already open: the last active window to the front, on this panel */
        if (wcscmp(u, g_url)) {
            wcscpy(g_url, u);
            struct tab *t = selected(w);
            if (t && t->view)
                t->view->lpVtbl->Navigate(t->view, g_url);
        }
        if (IsIconic(w->hwnd))
            ShowWindow(w->hwnd, SW_RESTORE);
        SetForegroundWindow(w->hwnd);
        return 0;
    }
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED); /* WebView2 wants an STA on this (the UI) thread */
    if (!runtime_entry())
        return -1; /* no WebView2 Runtime */
    wcscpy(g_url, u);
    if (!(w = win_new(NULL)))
        return -1;
    ShowWindow(w->hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(w->hwnd);
    new_tab(w, g_url);
    return 0; /* if WebView2 failed to start after all, env_failed() has opened the browser */
}
