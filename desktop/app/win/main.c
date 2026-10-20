/* RemoDroid for Windows: the notification-area icon, its menu and the pairing /
 * status window, in plain Win32. Everything else is in remodroid.dll
 * (desktop/core/rd_agent.h); its callbacks arrive on the agent's thread and are
 * posted to this one.
 *
 *   RemoDroid.exe [--background] [--conf path\desktop.ini] */
#ifndef UNICODE
#define UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "icon.h"
#include "rd_agent.h"

#define WM_TRAY (WM_APP + 1)
#define WM_STATUS (WM_APP + 2)
#define WM_PAIRED (WM_APP + 3)
#define WM_CONFIRM (WM_APP + 4)
#define WM_TOAST (WM_APP + 5)
#define WM_SHOWWIN (WM_APP + 6)

enum {
    ID_STATUS = 100, ID_VIEWER, ID_DISCONNECT, ID_VIEWONLY, ID_OPEN, ID_WINDOW, ID_LOG, ID_LOGIN, ID_QUIT,
    IDC_TITLE = 200, IDC_STATE, IDC_L_LINE, IDC_LINE, IDC_HINT, IDC_L_NAME, IDC_NAME,
    IDC_INFO, IDC_UNPAIR, IDC_SHOWLOG, IDC_VIEWONLY, IDC_LOGIN, IDC_DISC, IDC_OPENPANEL,
    IDC_LOGTEXT = 300, IDC_LOGPATH, IDC_LOGCOPY, IDC_LOGFOLDER,
};
#define IDC_PAIR IDOK /* Enter in the window presses it */

static const wchar_t TRAY_CLASS[] = L"RemoDroidTray", WIN_CLASS[] = L"RemoDroidWindow", LOG_CLASS[] = L"RemoDroidLog";
static HINSTANCE g_inst;
static HWND g_tray, g_win;
static rd_status_t g_st;
static HICON g_icons[4];
static UINT WM_TASKBARCREATED;
static NOTIFYICONDATAW g_nid;
static HFONT g_font, g_bold;
static int g_pairing;
static wchar_t g_pair_msg[300];
static HWND g_log;
static HFONT g_log_font, g_log_mono;
static unsigned g_log_seq;

static void show_log(void);

/* ---------------- helpers ---------------- */

static wchar_t *wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = malloc(sizeof(wchar_t) * (size_t)(n > 0 ? n : 1));
    if (n <= 0 || !MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n))
        w[0] = 0;
    return w;
}

static char *utf8(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = malloc(n > 0 ? (size_t)n : 1);
    if (n <= 0 || !WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL))
        s[0] = 0;
    return s;
}

static char *text_of(int id)
{
    wchar_t w[512];
    GetDlgItemTextW(g_win, id, w, 512);
    return utf8(w);
}

static UINT dpi_of(HWND h)
{
    typedef UINT(WINAPI * f_t)(HWND);
    static f_t f;
    if (!f)
        f = (f_t)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    UINT d = f && h ? f(h) : 0;
    if (!d) {
        HDC dc = GetDC(NULL);
        d = (UINT)GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(NULL, dc);
    }
    return d ? d : 96;
}

static HICON make_icon(int size, int style)
{
    uint32_t *px = malloc(sizeof *px * (size_t)size * size);
    rd_icon_render(px, size, style);
    BITMAPV5HEADER bh = { .bV5Size = sizeof bh, .bV5Width = size, .bV5Height = -size, .bV5Planes = 1, .bV5BitCount = 32,
                          .bV5Compression = BI_BITFIELDS, .bV5RedMask = 0x00FF0000, .bV5GreenMask = 0x0000FF00,
                          .bV5BlueMask = 0x000000FF, .bV5AlphaMask = 0xFF000000 };
    void *bits;
    HDC dc = GetDC(NULL);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO *)&bh, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, dc);
    memcpy(bits, px, sizeof *px * (size_t)size * size);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, NULL);
    ICONINFO ii = { .fIcon = TRUE, .hbmMask = mask, .hbmColor = color };
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    free(px);
    return ic;
}

static const wchar_t *state_text(void)
{
    switch (g_st.state) {
    case RD_UNPAIRED: return L"Not paired";
    case RD_CONNECTING: return L"Connecting…";
    case RD_ONLINE: return L"Online";
    case RD_IN_SESSION: return L"Someone is viewing";
    default: return L"Stopped";
    }
}

/* ---------------- start at login (HKCU Run) ---------------- */

static const wchar_t RUN_KEY[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

static int login_enabled(void)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &k))
        return 0;
    int on = RegQueryValueExW(k, L"RemoDroid", NULL, NULL, NULL, NULL) == ERROR_SUCCESS;
    RegCloseKey(k);
    return on;
}

static void set_login(int on)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k))
        return;
    if (on) {
        wchar_t exe[MAX_PATH], cmd[MAX_PATH + 32];
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        swprintf(cmd, MAX_PATH + 32, L"\"%ls\" --background", exe);
        RegSetValueExW(k, L"RemoDroid", 0, REG_SZ, (const BYTE *)cmd, (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, L"RemoDroid");
    }
    RegCloseKey(k);
}

/* ---------------- tray ---------------- */

static void tray_update(int add)
{
    int style = g_st.state == RD_IN_SESSION ? 2 : g_st.state == RD_ONLINE ? 1 : 0;
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = g_tray;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = g_icons[style];
    if (g_st.state == RD_IN_SESSION && g_st.viewer[0]) {
        wchar_t *v = wide(g_st.viewer);
        swprintf(g_nid.szTip, 128, L"RemoDroid — %ls is viewing", v);
        free(v);
    } else {
        swprintf(g_nid.szTip, 128, L"RemoDroid — %ls", state_text());
    }
    if (add) {
        Shell_NotifyIconW(NIM_ADD, &g_nid);
        g_nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
    } else {
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    }
}

static void toast(const wchar_t *text)
{
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_INFO;
    wcsncpy(n.szInfoTitle, L"RemoDroid", 63);
    wcsncpy(n.szInfo, text, 255);
    n.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

static void open_panel(void)
{
    if (!g_st.panel[0])
        return;
    wchar_t *u = wide(g_st.panel);
    ShellExecuteW(NULL, L"open", u, NULL, NULL, SW_SHOWNORMAL);
    free(u);
}

static void show_window(void);

static void tray_menu(int x, int y)
{
    HMENU m = CreatePopupMenu();
    wchar_t line[200];
    swprintf(line, 200, L"RemoDroid — %ls", state_text());
    AppendMenuW(m, MF_STRING | MF_GRAYED, ID_STATUS, line);
    if (g_st.state == RD_IN_SESSION) {
        wchar_t *v = wide(g_st.viewer[0] ? g_st.viewer : "a viewer");
        swprintf(line, 200, L"Viewer: %ls", v);
        free(v);
        AppendMenuW(m, MF_STRING | MF_GRAYED, ID_VIEWER, line);
    }
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    if (g_st.state == RD_IN_SESSION)
        AppendMenuW(m, MF_STRING, ID_DISCONNECT, L"Disconnect viewer");
    AppendMenuW(m, MF_STRING | (g_st.view_only ? MF_CHECKED : 0), ID_VIEWONLY, L"View only (ignore mouse and keyboard)");
    if (g_st.panel[0])
        AppendMenuW(m, MF_STRING, ID_OPEN, L"Open panel");
    AppendMenuW(m, MF_STRING, ID_WINDOW, g_st.state == RD_UNPAIRED ? L"Pair with a panel…" : L"Pairing && status…");
    AppendMenuW(m, MF_STRING, ID_LOG, L"Show log");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (login_enabled() ? MF_CHECKED : 0), ID_LOGIN, L"Start at login");
    AppendMenuW(m, MF_STRING, ID_QUIT, L"Quit RemoDroid");
    SetForegroundWindow(g_tray); /* so the menu closes when clicking elsewhere */
    int cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, x, y, g_tray, NULL);
    DestroyMenu(m);
    switch (cmd) {
    case ID_DISCONNECT: rd_agent_end_session(); break;
    case ID_VIEWONLY: rd_agent_set("desktop.view_only", g_st.view_only ? "false" : "true"); break;
    case ID_OPEN: open_panel(); break;
    case ID_WINDOW: show_window(); break;
    case ID_LOG: show_log(); break;
    case ID_LOGIN: set_login(!login_enabled()); break;
    case ID_QUIT: PostMessageW(g_tray, WM_CLOSE, 0, 0); break;
    }
}

/* ---------------- the pairing / status window ---------------- */

typedef struct {
    int id;
    const wchar_t *cls, *text;
    DWORD style;
} ctl_t;

static const ctl_t CTLS[] = {
    { IDC_TITLE, L"STATIC", L"", SS_LEFT },
    { IDC_STATE, L"STATIC", L"", SS_LEFT },
    { IDC_L_LINE, L"STATIC", L"Pairing command", SS_LEFT },
    { IDC_LINE, L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL },
    { IDC_HINT, L"STATIC", L"Paste the whole line from the panel: Agents → Add → Computer.", SS_LEFT },
    { IDC_L_NAME, L"STATIC", L"Name", SS_LEFT },
    { IDC_NAME, L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL },
    { IDC_PAIR, L"BUTTON", L"Pair", WS_TABSTOP | BS_DEFPUSHBUTTON },
    { IDC_INFO, L"STATIC", L"", SS_LEFT },
    { IDC_OPENPANEL, L"BUTTON", L"Open panel", WS_TABSTOP | BS_PUSHBUTTON },
    { IDC_DISC, L"BUTTON", L"Disconnect viewer", WS_TABSTOP | BS_PUSHBUTTON },
    { IDC_UNPAIR, L"BUTTON", L"Unpair", WS_TABSTOP | BS_PUSHBUTTON },
    { IDC_VIEWONLY, L"BUTTON", L"View only: ignore the viewer's mouse and keyboard", WS_TABSTOP | BS_AUTOCHECKBOX },
    { IDC_LOGIN, L"BUTTON", L"Start RemoDroid when I log in", WS_TABSTOP | BS_AUTOCHECKBOX },
    { IDC_SHOWLOG, L"BUTTON", L"Show log", WS_TABSTOP | BS_PUSHBUTTON },
};

static void place(int id, int x, int y, int w, int h, int show)
{
    HWND c = GetDlgItem(g_win, id);
    UINT d = dpi_of(g_win);
    SetWindowPos(c, NULL, MulDiv(x, (int)d, 96), MulDiv(y, (int)d, 96), MulDiv(w, (int)d, 96), MulDiv(h, (int)d, 96),
                 SWP_NOZORDER | (show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
}

static void set_fonts(void)
{
    UINT d = dpi_of(g_win);
    NONCLIENTMETRICSW nm = { .cbSize = sizeof nm };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof nm, &nm, 0);
    LOGFONTW lf = nm.lfMessageFont;
    lf.lfHeight = -MulDiv(10, (int)d, 72);
    if (g_font)
        DeleteObject(g_font);
    if (g_bold)
        DeleteObject(g_bold);
    g_font = CreateFontIndirectW(&lf);
    lf.lfHeight = -MulDiv(13, (int)d, 72);
    lf.lfWeight = FW_SEMIBOLD;
    g_bold = CreateFontIndirectW(&lf);
    for (size_t i = 0; i < sizeof CTLS / sizeof CTLS[0]; i++)
        SendDlgItemMessageW(g_win, CTLS[i].id, WM_SETFONT, (WPARAM)(CTLS[i].id == IDC_TITLE ? g_bold : g_font), TRUE);
}

static void layout(void)
{
    if (!g_win)
        return;
    int paired = g_st.state != RD_UNPAIRED && g_st.state != RD_STOPPED;
    const int W = 420, X = 20, FW = W - 40;
    wchar_t buf[600];
    SetDlgItemTextW(g_win, IDC_TITLE, paired ? L"This computer is paired" : L"Pair this computer with your panel");
    if (g_pairing)
        swprintf(buf, 600, L"Pairing…");
    else if (g_pair_msg[0])
        swprintf(buf, 600, L"%ls", g_pair_msg);
    else if (g_st.problem[0]) {
        wchar_t *p = wide(g_st.problem);
        swprintf(buf, 600, L"%ls — %ls", state_text(), p);
        free(p);
    } else
        swprintf(buf, 600, L"%ls", state_text());
    SetDlgItemTextW(g_win, IDC_STATE, buf);
    int y = 16;
    place(IDC_TITLE, X, y, FW, 26, 1);
    y += 32;
    place(IDC_STATE, X, y, FW, 40, 1);
    y += 46;
    for (int id = IDC_L_LINE; id <= IDC_NAME; id++)
        place(id, 0, 0, 0, 0, 0);
    place(IDC_PAIR, 0, 0, 0, 0, 0);
    place(IDC_INFO, 0, 0, 0, 0, 0);
    place(IDC_OPENPANEL, 0, 0, 0, 0, 0);
    place(IDC_DISC, 0, 0, 0, 0, 0);
    place(IDC_UNPAIR, 0, 0, 0, 0, 0);
    if (!paired) {
        place(IDC_L_LINE, X, y, FW, 22, 1);
        y += 24;
        place(IDC_LINE, X, y, FW, 26, 1);
        y += 30;
        place(IDC_HINT, X, y, FW, 22, 1);
        y += 34;
        place(IDC_L_NAME, X, y + 4, 60, 22, 1);
        place(IDC_NAME, X + 66, y, FW - 66, 26, 1);
        y += 38;
        place(IDC_PAIR, W - 20 - 100, y, 100, 30, 1);
        EnableWindow(GetDlgItem(g_win, IDC_PAIR), !g_pairing);
        y += 44;
    } else {
        wchar_t *id = wide(g_st.device_id), *pn = wide(g_st.panel);
        swprintf(buf, 600, L"Panel:  %ls\nDevice: %ls", pn, id);
        free(id);
        free(pn);
        SetDlgItemTextW(g_win, IDC_INFO, buf);
        place(IDC_INFO, X, y, FW, 42, 1);
        y += 52;
        int bx = X;
        place(IDC_OPENPANEL, bx, y, 110, 30, 1);
        bx += 118;
        if (g_st.state == RD_IN_SESSION) {
            place(IDC_DISC, bx, y, 140, 30, 1);
            bx += 148;
        }
        place(IDC_UNPAIR, W - 20 - 90, y, 90, 30, 1);
        y += 44;
    }
    CheckDlgButton(g_win, IDC_VIEWONLY, g_st.view_only ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_win, IDC_LOGIN, login_enabled() ? BST_CHECKED : BST_UNCHECKED);
    place(IDC_VIEWONLY, X, y, FW, 24, 1);
    y += 28;
    place(IDC_LOGIN, X, y, FW, 24, 1);
    y += 36;
    place(IDC_SHOWLOG, X, y, 90, 28, 1);
    y += 44;
    UINT d = dpi_of(g_win);
    RECT r = { 0, 0, MulDiv(W, (int)d, 96), MulDiv(y, (int)d, 96) };
    AdjustWindowRectEx(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0);
    SetWindowPos(g_win, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* ---------------- the log window ---------------- */

static void copy_log(void)
{
    char *log = rd_agent_log(1000);
    wchar_t *w = wide(log);
    rd_agent_free(log);
    size_t n = (wcslen(w) + 1) * sizeof(wchar_t);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n);
    memcpy(GlobalLock(g), w, n);
    GlobalUnlock(g);
    free(w);
    if (OpenClipboard(g_log)) {
        EmptyClipboard();
        SetClipboardData(CF_UNICODETEXT, g);
        CloseClipboard();
        toast(L"The log is on the clipboard.");
    } else {
        GlobalFree(g);
    }
}

static void log_fonts(HWND h)
{
    UINT d = dpi_of(h);
    NONCLIENTMETRICSW nm = { .cbSize = sizeof nm };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof nm, &nm, 0);
    LOGFONTW lf = nm.lfMessageFont;
    lf.lfHeight = -MulDiv(10, (int)d, 72);
    if (g_log_font)
        DeleteObject(g_log_font);
    if (g_log_mono)
        DeleteObject(g_log_mono);
    g_log_font = CreateFontIndirectW(&lf);
    LOGFONTW mf = { .lfHeight = -MulDiv(9, (int)d, 72), .lfWeight = FW_NORMAL, .lfCharSet = DEFAULT_CHARSET,
                    .lfPitchAndFamily = FIXED_PITCH | FF_MODERN };
    wcscpy(mf.lfFaceName, L"Consolas");
    g_log_mono = CreateFontIndirectW(&mf);
    SendDlgItemMessageW(h, IDC_LOGTEXT, WM_SETFONT, (WPARAM)g_log_mono, TRUE);
    for (int id = IDC_LOGPATH; id <= IDC_LOGFOLDER; id++)
        SendDlgItemMessageW(h, id, WM_SETFONT, (WPARAM)g_log_font, TRUE);
}

static void log_layout(HWND h)
{
    RECT r;
    GetClientRect(h, &r);
    int d = (int)dpi_of(h), m = MulDiv(10, d, 96), bh = MulDiv(28, d, 96), bw = MulDiv(104, d, 96);
    int W = r.right, H = r.bottom, y = H - m - bh;
    MoveWindow(GetDlgItem(h, IDC_LOGTEXT), m, m, W - 2 * m, y - 2 * m, TRUE);
    MoveWindow(GetDlgItem(h, IDC_LOGPATH), m, y + MulDiv(6, d, 96), W - 4 * m - 2 * bw, bh, TRUE);
    MoveWindow(GetDlgItem(h, IDC_LOGCOPY), W - 2 * m - 2 * bw, y, bw, bh, TRUE);
    MoveWindow(GetDlgItem(h, IDC_LOGFOLDER), W - m - bw, y, bw, bh, TRUE);
}

/* New lines appear as they are logged; the view follows the end unless scrolled up. */
static void log_refresh(int force)
{
    unsigned seq = rd_agent_log_seq();
    if (!g_log || (!force && seq == g_log_seq))
        return;
    g_log_seq = seq;
    HWND e = GetDlgItem(g_log, IDC_LOGTEXT);
    SCROLLINFO si = { .cbSize = sizeof si, .fMask = SIF_ALL };
    int at_end = force || !GetScrollInfo(e, SB_VERT, &si) || si.nPos + (int)si.nPage > si.nMax;
    int first = (int)SendMessageW(e, EM_GETFIRSTVISIBLELINE, 0, 0);
    char *log = rd_agent_log(1000);
    size_t n = strlen(log), lf = 0;
    for (size_t i = 0; i < n; i++)
        lf += log[i] == '\n';
    char *crlf = malloc(n + lf + 1); /* an edit control breaks lines at \r\n */
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (log[i] == '\n')
            crlf[o++] = '\r';
        crlf[o++] = log[i];
    }
    crlf[o] = 0;
    rd_agent_free(log);
    wchar_t *w = wide(crlf);
    free(crlf);
    SendMessageW(e, WM_SETREDRAW, FALSE, 0);
    SetWindowTextW(e, w);
    free(w);
    if (at_end) {
        int len = GetWindowTextLengthW(e);
        SendMessageW(e, EM_SETSEL, len, len);
        SendMessageW(e, EM_SCROLLCARET, 0, 0);
    } else {
        SendMessageW(e, EM_LINESCROLL, 0, first);
    }
    SendMessageW(e, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(e, NULL, TRUE);
}

static void open_log_folder(void)
{
    wchar_t *p = wide(rd_agent_log_path()), args[700];
    swprintf(args, 700, L"/select,\"%ls\"", p); /* Explorer, with desktop.log selected */
    free(p);
    ShellExecuteW(NULL, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
}

static LRESULT CALLBACK log_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        CreateWindowExW(0, L"EDIT", L"",
                        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY |
                            ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                        0, 0, 10, 10, h, (HMENU)(INT_PTR)IDC_LOGTEXT, g_inst, NULL);
        SendDlgItemMessageW(h, IDC_LOGTEXT, EM_SETLIMITTEXT, 0, 0); /* no 32K cap */
        wchar_t *p = wide(rd_agent_log_path());
        CreateWindowExW(0, L"STATIC", p, WS_CHILD | WS_VISIBLE | SS_PATHELLIPSIS | SS_NOPREFIX, 0, 0, 10, 10, h,
                        (HMENU)(INT_PTR)IDC_LOGPATH, g_inst, NULL);
        free(p);
        CreateWindowExW(0, L"BUTTON", L"Copy", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10, h,
                        (HMENU)(INT_PTR)IDC_LOGCOPY, g_inst, NULL);
        CreateWindowExW(0, L"BUTTON", L"Open folder", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10,
                        h, (HMENU)(INT_PTR)IDC_LOGFOLDER, g_inst, NULL);
        g_log = h;
        log_fonts(h);
        return 0;
    }
    case WM_SIZE: log_layout(h); return 0;
    case WM_DPICHANGED: {
        RECT *r = (RECT *)lp;
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER);
        log_fonts(h);
        log_layout(h);
        return 0;
    }
    case WM_TIMER: log_refresh(0); return 0;
    case WM_CTLCOLORSTATIC: /* the read-only edit and the path: on the window colour, not grey */
        SetBkColor((HDC)wp, GetSysColor(COLOR_WINDOW));
        if ((HWND)lp == GetDlgItem(h, IDC_LOGPATH))
            SetTextColor((HDC)wp, GetSysColor(COLOR_GRAYTEXT));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_LOGCOPY)
            copy_log();
        else if (LOWORD(wp) == IDC_LOGFOLDER)
            open_log_folder();
        return 0;
    case WM_CLOSE:
        KillTimer(h, 1);
        ShowWindow(h, SW_HIDE);
        return 0;
    case WM_DESTROY:
        g_log = NULL;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void show_log(void)
{
    if (!g_log) {
        UINT d = dpi_of(NULL);
        CreateWindowExW(0, LOG_CLASS, L"RemoDroid log", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                        MulDiv(840, (int)d, 96), MulDiv(500, (int)d, 96), NULL, NULL, g_inst, NULL);
        if (!g_log)
            return;
    }
    log_refresh(1);
    SetTimer(g_log, 1, 500, NULL);
    ShowWindow(g_log, SW_SHOWNORMAL);
    SetForegroundWindow(g_log);
}

static void on_paired(void *ud, int ok, const char *msg);

static void start_pair(void)
{
    char *line = text_of(IDC_LINE), *name = text_of(IDC_NAME);
    const char *bad = rd_agent_pair_line(line, name, on_paired, NULL);
    g_pairing = !bad;
    g_pair_msg[0] = 0;
    if (bad) {
        wchar_t *w = wide(bad);
        swprintf(g_pair_msg, 300, L"Pairing failed: %ls", w);
        free(w);
    }
    free(line);
    free(name);
    layout();
}

static LRESULT CALLBACK win_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        g_win = h;
        for (size_t i = 0; i < sizeof CTLS / sizeof CTLS[0]; i++)
            CreateWindowExW(0, CTLS[i].cls, CTLS[i].text, WS_CHILD | CTLS[i].style, 0, 0, 10, 10, h,
                            (HMENU)(INT_PTR)CTLS[i].id, g_inst, NULL);
        wchar_t host[64];
        DWORD n = 64;
        if (GetComputerNameW(host, &n))
            SetDlgItemTextW(h, IDC_NAME, host);
        SendDlgItemMessageW(h, IDC_LINE, EM_SETCUEBANNER, TRUE, (LPARAM)L"remodroid enroll --panel https://… --code XXXX-XXXX");
        set_fonts();
        layout();
        return 0;
    }
    case WM_DPICHANGED: {
        RECT *r = (RECT *)lp;
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER);
        set_fonts();
        layout();
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        if ((HWND)lp == GetDlgItem(h, IDC_STATE) && (g_st.problem[0] || g_pair_msg[0]))
            SetTextColor((HDC)wp, RGB(0xB4, 0x23, 0x18));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_PAIR:
            if (g_st.state == RD_UNPAIRED && !g_pairing)
                start_pair();
            return 0;
        case IDC_UNPAIR:
            if (MessageBoxW(h, L"Unpair this computer? Viewers can't reach it until you pair it again.", L"RemoDroid",
                            MB_OKCANCEL | MB_ICONQUESTION) == IDOK)
                rd_agent_unpair();
            return 0;
        case IDC_OPENPANEL: open_panel(); return 0;
        case IDC_DISC: rd_agent_end_session(); return 0;
        case IDC_VIEWONLY:
            rd_agent_set("desktop.view_only", IsDlgButtonChecked(h, IDC_VIEWONLY) ? "true" : "false");
            return 0;
        case IDC_LOGIN: set_login(IsDlgButtonChecked(h, IDC_LOGIN) == BST_CHECKED); return 0;
        case IDC_SHOWLOG: show_log(); return 0;
        }
        break;
    case WM_CLOSE:
        ShowWindow(h, SW_HIDE);
        return 0;
    case WM_DESTROY:
        g_win = NULL;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void show_window(void)
{
    if (!g_win) {
        RECT wa;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        CreateWindowExW(0, WIN_CLASS, L"RemoDroid", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                        wa.right - 520, wa.bottom - 460, 440, 400, NULL, NULL, g_inst, NULL);
    }
    layout();
    ShowWindow(g_win, SW_SHOWNORMAL);
    SetForegroundWindow(g_win);
    if (g_st.state == RD_UNPAIRED)
        SetFocus(GetDlgItem(g_win, IDC_LINE));
}

/* ---------------- agent callbacks (agent thread → posted here) ---------------- */

static void on_status(void *ud, const rd_status_t *s)
{
    rd_status_t *c = malloc(sizeof *c);
    *c = *s;
    PostMessageW(g_tray, WM_STATUS, 0, (LPARAM)c);
}

static void on_paired(void *ud, int ok, const char *msg) { PostMessageW(g_tray, WM_PAIRED, ok, (LPARAM)wide(msg)); }
static void on_confirm(void *ud, const char *viewer) { PostMessageW(g_tray, WM_CONFIRM, 0, (LPARAM)wide(viewer)); }
static void on_notify(void *ud, const char *text) { PostMessageW(g_tray, WM_TOAST, 0, (LPARAM)wide(text)); }

static LRESULT CALLBACK tray_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (WM_TASKBARCREATED && msg == WM_TASKBARCREATED) { /* Explorer restarted: the icon is gone */
        tray_update(1);
        return 0;
    }
    switch (msg) {
    case WM_TRAY:
        switch (LOWORD(lp)) {
        case WM_CONTEXTMENU:
            tray_menu(GET_X_LPARAM(wp), GET_Y_LPARAM(wp)); /* NOTIFYICON_VERSION_4: the anchor point */
            break;
        case NIN_SELECT:
        case NIN_KEYSELECT:
        case WM_LBUTTONUP:
            show_window();
            break;
        }
        return 0;
    case WM_STATUS: {
        rd_status_t *s = (rd_status_t *)lp;
        int was = g_st.state;
        g_st = *s;
        free(s);
        if (was == RD_UNPAIRED && g_st.state != RD_UNPAIRED)
            g_pair_msg[0] = 0;
        tray_update(0);
        layout();
        return 0;
    }
    case WM_PAIRED: {
        wchar_t *m = (wchar_t *)lp;
        g_pairing = 0;
        if (wp)
            swprintf(g_pair_msg, 300, L"Paired as \"%ls\".", m);
        else
            swprintf(g_pair_msg, 300, L"Pairing failed: %ls", m);
        free(m);
        layout();
        if (wp)
            toast(g_pair_msg);
        return 0;
    }
    case WM_CONFIRM: {
        wchar_t *v = (wchar_t *)lp, q[300];
        swprintf(q, 300, L"%ls wants to see and control this computer.\n\nAllow?", v);
        free(v);
        int r = MessageBoxW(NULL, q, L"RemoDroid", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_TOPMOST | MB_SETFOREGROUND);
        rd_agent_confirm(r == IDYES);
        return 0;
    }
    case WM_TOAST: {
        wchar_t *t = (wchar_t *)lp;
        toast(t);
        free(t);
        return 0;
    }
    case WM_SHOWWIN:
        show_window();
        return 0;
    case WM_CLOSE:
        rd_agent_stop();
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        if (g_win)
            DestroyWindow(g_win);
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    g_inst = inst;
    int background = wcsstr(cmdline, L"--background") != NULL;
    char *conf = NULL;
    const wchar_t *c = wcsstr(cmdline, L"--conf ");
    if (c) {
        wchar_t path[MAX_PATH];
        swscanf(c + 7, L"%259ls", path);
        conf = utf8(path);
    }
    /* one instance: a second start shows the first one's window */
    HANDLE mtx = CreateMutexW(NULL, TRUE, L"Local\\RemoDroidDesktop");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(TRAY_CLASS, NULL);
        if (other)
            PostMessageW(other, WM_SHOWWIN, 0, 0);
        return 0;
    }
    WNDCLASSW tc = { .lpfnWndProc = tray_proc, .hInstance = inst, .lpszClassName = TRAY_CLASS };
    RegisterClassW(&tc);
    WNDCLASSW wc = { .lpfnWndProc = win_proc, .hInstance = inst, .lpszClassName = WIN_CLASS,
                     .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW), .hbrBackground = GetSysColorBrush(COLOR_WINDOW),
                     .hIcon = LoadIconW(inst, L"APP") };
    RegisterClassW(&wc);
    WNDCLASSW lc = wc;
    lc.lpfnWndProc = log_proc;
    lc.lpszClassName = LOG_CLASS;
    RegisterClassW(&lc);
    WM_TASKBARCREATED = RegisterWindowMessageW(L"TaskbarCreated");
    g_tray = CreateWindowExW(0, TRAY_CLASS, L"RemoDroid", 0, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    int sz = GetSystemMetrics(SM_CXSMICON) * (int)dpi_of(NULL) / 96;
    if (sz < 16)
        sz = 16;
    g_icons[0] = make_icon(sz, RD_ICON_OFF);
    g_icons[1] = make_icon(sz, RD_ICON_ON);
    g_icons[2] = make_icon(sz, RD_ICON_LIVE);
    g_st.state = RD_STOPPED;
    tray_update(1);
    rd_agent_cb_t cb = { on_status, on_confirm, on_notify };
    if (rd_agent_start(conf, cb, NULL) < 0) {
        MessageBoxW(NULL, L"RemoDroid could not start. See desktop.log in %APPDATA%\\RemoDroid.", L"RemoDroid",
                    MB_OK | MB_ICONERROR);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        return 1;
    }
    rd_agent_status(&g_st);
    tray_update(0);
    if (!background || g_st.state == RD_UNPAIRED)
        show_window();
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (g_win && IsDialogMessageW(g_win, &m)) /* Tab between fields, Enter presses Pair */
            continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    free(conf);
    ReleaseMutex(mtx);
    return 0;
}
