/* RemoDroid Server for Windows: the notification-area icon and the settings window, in
 * plain Win32. The server itself is the RemoDroidServer service (server/win.c); this
 * app installs and controls it, and edits the setup wizard's settings through
 * `remodroid-server settings` / `configure`, so the checks are the wizard's own.
 * It runs as an administrator (the manifest): the service, server.ini and the log
 * are for administrators only.
 *
 *   RemoDroidServer.exe [--background] */
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

#include "cJSON.h"
#include "icon.h"

#define WM_TRAY (WM_APP + 1)
#define WM_SHOWWIN (WM_APP + 2)
#define SERVICE_NAME L"RemoDroidServer"
#define TASK_NAME L"RemoDroid Server"
#define CONTROL_RESTART 128 /* server/win.c */
#define LOG_TAIL (256 * 1024)

enum {
    ID_STATUS = 100, ID_OPEN, ID_WINDOW, ID_LOG, ID_START, ID_STOP, ID_RESTART, ID_LOGIN, ID_QUIT,
    IDC_TITLE = 200, IDC_STATE, IDC_OPENPANEL, IDC_INSTALL, IDC_STARTSTOP, IDC_RESTART, IDC_SHOWLOG,
    IDC_H_ADMIN, IDC_L_USER, IDC_USER, IDC_L_PASS, IDC_PASS,
    IDC_H_WEB, IDC_L_PORT, IDC_PORT, IDC_L_DOMAIN, IDC_DOMAIN, IDC_L_EMAIL, IDC_EMAIL, IDC_L_TLSPORT, IDC_TLSPORT,
    IDC_REDIRECT, IDC_WEBHINT,
    IDC_H_TURN, IDC_TURN, IDC_L_TURNPORT, IDC_TURNPORT, IDC_L_PUBIP, IDC_PUBIP, IDC_TURNHINT,
    IDC_SAVE, IDC_MSG, IDC_LOGIN,
    IDC_LOGTEXT = 300, IDC_LOGPATH, IDC_LOGCOPY, IDC_LOGFOLDER,
};

static const wchar_t TRAY_CLASS[] = L"RemoDroidServerTray", WIN_CLASS[] = L"RemoDroidServerWindow",
                     LOG_CLASS[] = L"RemoDroidServerLog";
static HINSTANCE g_inst;
static HWND g_tray, g_win, g_log;
static HICON g_icons[2];
static UINT WM_TASKBARCREATED;
static NOTIFYICONDATAW g_nid;
static HFONT g_font, g_bold, g_log_font, g_log_mono;
static DWORD g_state;      /* the service: 0 = not installed, else SERVICE_RUNNING, SERVICE_STOPPED, … */
static cJSON *g_settings;  /* `remodroid-server settings`, NULL until the service is installed */
static int g_login = -1;   /* the sign-in task exists; -1 = not looked up yet */
static wchar_t g_msg[400];
static int g_msg_bad;
static wchar_t g_data[MAX_PATH], g_exe_dir[MAX_PATH];
static LONGLONG g_log_size = -1;

static void show_log(void);
static void layout(void);

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

static void set_text(int id, const char *s)
{
    wchar_t *w = wide(s ? s : "");
    SetDlgItemTextW(g_win, id, w);
    free(w);
}

static const char *jstr(const char *key)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(g_settings, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static int jnum(const char *key, int def)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(g_settings, key);
    return cJSON_IsNumber(v) ? v->valueint : def;
}

static int jbool(const char *key) { return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(g_settings, key)); }

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

/* Runs a program hidden: its exit code (-1 if it didn't run) and, with out, what it
 * printed; input goes to its stdin. */
static int run(const wchar_t *cmdline, const char *input, char **out)
{
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE in_r = NULL, in_w = NULL, out_r = NULL, out_w = NULL;
    if (!CreatePipe(&in_r, &in_w, &sa, 0) || !CreatePipe(&out_r, &out_w, &sa, 0))
        return -1;
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = { .cb = sizeof si, .dwFlags = STARTF_USESTDHANDLES, .hStdInput = in_r, .hStdOutput = out_w,
                        .hStdError = out_w };
    PROCESS_INFORMATION pi;
    wchar_t *cmd = _wcsdup(cmdline);
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    free(cmd);
    CloseHandle(in_r);
    CloseHandle(out_w);
    int code = -1;
    if (ok) {
        DWORD n;
        if (input)
            WriteFile(in_w, input, (DWORD)strlen(input), &n, NULL);
        CloseHandle(in_w);
        in_w = NULL;
        size_t len = 0, cap = 4096;
        char *buf = malloc(cap);
        while (ReadFile(out_r, buf + len, (DWORD)(cap - len - 1), &n, NULL) && n) {
            len += n;
            if (cap - len < 1024)
                buf = realloc(buf, cap *= 2);
        }
        buf[len] = 0;
        WaitForSingleObject(pi.hProcess, 60000);
        DWORD c;
        if (GetExitCodeProcess(pi.hProcess, &c))
            code = (int)c;
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        if (out)
            *out = buf;
        else
            free(buf);
    }
    if (in_w)
        CloseHandle(in_w);
    CloseHandle(out_r);
    return code;
}

/* remodroid-server.exe (next to this program) with arguments */
static int server_cmd(const wchar_t *args, const char *input, char **out)
{
    wchar_t cmd[MAX_PATH + 600];
    swprintf(cmd, MAX_PATH + 600, L"\"%ls\\remodroid-server.exe\" -c \"%ls\\server.ini\" %ls", g_exe_dir, g_data, args);
    return run(cmd, input, out);
}

/* ---------------- the service ---------------- */

static DWORD service_state(void)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT), s;
    DWORD st = 0;
    if (scm && (s = OpenServiceW(scm, SERVICE_NAME, SERVICE_QUERY_STATUS))) {
        SERVICE_STATUS ss;
        if (QueryServiceStatus(s, &ss))
            st = ss.dwCurrentState;
        CloseServiceHandle(s);
    }
    if (scm)
        CloseServiceHandle(scm);
    return st;
}

static const wchar_t *state_text(void)
{
    switch (g_state) {
    case 0: return L"Not installed";
    case SERVICE_RUNNING: return L"Running";
    case SERVICE_START_PENDING: return L"Starting…";
    case SERVICE_STOP_PENDING: return L"Stopping…";
    default: return L"Stopped";
    }
}

static void load_settings(void)
{
    cJSON_Delete(g_settings);
    g_settings = NULL;
    char *out = NULL;
    if (g_state && server_cmd(L"settings", NULL, &out) == 0)
        g_settings = cJSON_Parse(out);
    free(out);
}

/* start, stop or restart, as the service manager's operations (we are an administrator) */
static void service_do(int id)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT), s = NULL;
    if (scm)
        s = OpenServiceW(scm, SERVICE_NAME, SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS |
                                                SERVICE_USER_DEFINED_CONTROL);
    SERVICE_STATUS ss;
    BOOL ok = FALSE;
    if (s) {
        DWORD now = QueryServiceStatus(s, &ss) ? ss.dwCurrentState : 0;
        if (id == ID_STOP)
            ok = ControlService(s, SERVICE_CONTROL_STOP, &ss);
        else if (id == ID_RESTART && now == SERVICE_RUNNING)
            ok = ControlService(s, CONTROL_RESTART, &ss);
        else
            ok = StartServiceW(s, 0, NULL) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
    }
    if (!ok) {
        swprintf(g_msg, 400, L"Could not %ls the service (error %lu).", id == ID_STOP ? L"stop" : L"start",
                 GetLastError());
        g_msg_bad = 1;
    }
    if (s)
        CloseServiceHandle(s);
    if (scm)
        CloseServiceHandle(scm);
}

static void install(void)
{
    SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_WAIT));
    char *out = NULL;
    int code = server_cmd(L"install", NULL, &out);
    if (code) {
        wchar_t *w = wide(out && *out ? out : "remodroid-server.exe did not run");
        swprintf(g_msg, 400, L"Install failed: %ls", w);
        free(w);
        g_msg_bad = 1;
    } else {
        swprintf(g_msg, 400, L"Installed. The server starts with Windows; set it up below.");
        g_msg_bad = 0;
    }
    free(out);
    g_state = service_state();
    load_settings();
}

/* ---------------- the panel's address ---------------- */

static void panel_url(wchar_t *out, size_t cap)
{
    const char *dom = jstr("domain");
    int port = jnum("http_port", 80), tls = jnum("tls_port", 443);
    if (!jbool("configured")) { /* the wizard, with its code */
        wchar_t path[MAX_PATH + 20], tok[40] = L"";
        swprintf(path, MAX_PATH + 20, L"%ls\\setup-token", g_data);
        FILE *f = _wfopen(path, L"r");
        if (f) {
            if (!fgetws(tok, 40, f))
                tok[0] = 0;
            tok[wcscspn(tok, L"\r\n")] = 0;
            fclose(f);
        }
        swprintf(out, cap, L"http://localhost:%d/wizard.html%ls%ls", port, *tok ? L"?token=" : L"", tok);
    } else if (*dom) {
        wchar_t *d = wide(dom);
        if (tls == 443)
            swprintf(out, cap, L"https://%ls/", d);
        else
            swprintf(out, cap, L"https://%ls:%d/", d, tls);
        free(d);
    } else {
        swprintf(out, cap, L"http://localhost:%d/", port);
    }
}

static void open_panel(void)
{
    if (!g_settings)
        return;
    wchar_t u[600];
    panel_url(u, 600);
    ShellExecuteW(NULL, L"open", u, NULL, NULL, SW_SHOWNORMAL);
}

/* ---------------- the icon at sign-in (a scheduled task: it starts us elevated) ---------------- */

static int login_enabled(void)
{
    if (g_login < 0) {
        wchar_t cmd[200];
        swprintf(cmd, 200, L"schtasks /Query /TN \"%ls\"", TASK_NAME);
        g_login = run(cmd, NULL, NULL) == 0;
    }
    return g_login;
}

static void set_login(int on)
{
    wchar_t cmd[MAX_PATH * 2 + 300], exe[MAX_PATH], user[300] = L"";
    if (on) {
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        const wchar_t *dom = _wgetenv(L"USERDOMAIN"), *name = _wgetenv(L"USERNAME");
        if (name)
            swprintf(user, 300, L" /RU \"%ls%ls%ls\" /IT", dom ? dom : L"", dom ? L"\\" : L"", name);
        swprintf(cmd, MAX_PATH * 2 + 300, L"schtasks /Create /F /TN \"%ls\" /SC ONLOGON /RL HIGHEST%ls /TR \"\\\"%ls\\\" --background\"",
                 TASK_NAME, user, exe);
    } else {
        swprintf(cmd, MAX_PATH * 2 + 300, L"schtasks /Delete /F /TN \"%ls\"", TASK_NAME);
    }
    run(cmd, NULL, NULL);
    g_login = -1;
}

/* ---------------- tray ---------------- */

static void tray_update(int add)
{
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = g_tray;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = g_icons[g_state == SERVICE_RUNNING];
    swprintf(g_nid.szTip, 128, L"RemoDroid Server — %ls", state_text());
    if (add) {
        Shell_NotifyIconW(NIM_ADD, &g_nid);
        g_nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
    } else {
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    }
}

static void show_window(void);

/* The service's state, every two seconds and after each action. */
static void refresh(void)
{
    DWORD was = g_state;
    g_state = service_state();
    if (g_state != was) {
        if ((!was) != (!g_state) || (g_state == SERVICE_RUNNING && !g_settings))
            load_settings();
        tray_update(0);
        layout();
    }
}

static void do_action(int id)
{
    g_msg[0] = 0;
    service_do(id);
    Sleep(300);
    refresh();
    layout();
}

static void tray_menu(int x, int y)
{
    HMENU m = CreatePopupMenu();
    wchar_t line[200];
    swprintf(line, 200, L"RemoDroid Server — %ls", state_text());
    AppendMenuW(m, MF_STRING | MF_GRAYED, ID_STATUS, line);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    if (g_settings)
        AppendMenuW(m, MF_STRING, ID_OPEN, L"Open panel");
    AppendMenuW(m, MF_STRING, ID_WINDOW, g_state ? L"Settings…" : L"Install…");
    AppendMenuW(m, MF_STRING, ID_LOG, L"Show log");
    if (g_state) {
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        if (g_state == SERVICE_STOPPED)
            AppendMenuW(m, MF_STRING, ID_START, L"Start the server");
        else
            AppendMenuW(m, MF_STRING, ID_STOP, L"Stop the server");
        AppendMenuW(m, MF_STRING, ID_RESTART, L"Restart the server");
    }
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (login_enabled() ? MF_CHECKED : 0), ID_LOGIN, L"Show this icon when I sign in");
    AppendMenuW(m, MF_STRING, ID_QUIT, L"Quit (the server keeps running)");
    SetForegroundWindow(g_tray); /* so the menu closes when clicking elsewhere */
    int cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, x, y, g_tray, NULL);
    DestroyMenu(m);
    switch (cmd) {
    case ID_OPEN: open_panel(); break;
    case ID_WINDOW: show_window(); break;
    case ID_LOG: show_log(); break;
    case ID_START:
    case ID_STOP:
    case ID_RESTART: do_action(cmd); break;
    case ID_LOGIN: set_login(!login_enabled()); break;
    case ID_QUIT: PostMessageW(g_tray, WM_CLOSE, 0, 0); break;
    }
}

/* ---------------- the settings window ---------------- */

typedef struct {
    int id;
    const wchar_t *cls, *text;
    DWORD style;
} ctl_t;

static const ctl_t CTLS[] = {
    { IDC_TITLE, L"STATIC", L"RemoDroid Server", SS_LEFT },
    { IDC_STATE, L"STATIC", L"", SS_LEFT | SS_NOPREFIX },
    { IDC_OPENPANEL, L"BUTTON", L"Open panel", WS_TABSTOP | BS_PUSHBUTTON },
    { IDC_INSTALL, L"BUTTON", L"Install and start", WS_TABSTOP | BS_DEFPUSHBUTTON },
    { IDC_STARTSTOP, L"BUTTON", L"Stop", WS_TABSTOP | BS_PUSHBUTTON },
    { IDC_RESTART, L"BUTTON", L"Restart", WS_TABSTOP | BS_PUSHBUTTON },
    { IDC_SHOWLOG, L"BUTTON", L"Show log", WS_TABSTOP | BS_PUSHBUTTON },
    { IDC_H_ADMIN, L"STATIC", L"Administrator (signs in to the web panel)", SS_LEFT },
    { IDC_L_USER, L"STATIC", L"Username", SS_LEFT },
    { IDC_USER, L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL },
    { IDC_L_PASS, L"STATIC", L"Password", SS_LEFT },
    { IDC_PASS, L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL | ES_PASSWORD },
    { IDC_H_WEB, L"STATIC", L"Web panel and HTTPS", SS_LEFT },
    { IDC_L_PORT, L"STATIC", L"Web port", SS_LEFT },
    { IDC_PORT, L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_NUMBER },
    { IDC_L_DOMAIN, L"STATIC", L"Domain", SS_LEFT },
    { IDC_DOMAIN, L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL },
    { IDC_L_EMAIL, L"STATIC", L"Email", SS_LEFT },
    { IDC_EMAIL, L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL },
    { IDC_L_TLSPORT, L"STATIC", L"HTTPS port", SS_LEFT },
    { IDC_TLSPORT, L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_NUMBER },
    { IDC_REDIRECT, L"BUTTON", L"Send http:// visitors to https://", WS_TABSTOP | BS_AUTOCHECKBOX },
    { IDC_WEBHINT, L"STATIC",
      L"With a domain, the server gets a free certificate (Let's Encrypt) and serves HTTPS for that name. "
      L"Its DNS must point here, with the web port reachable as port 80.",
      SS_LEFT },
    { IDC_H_TURN, L"STATIC", L"STUN / TURN relay", SS_LEFT },
    { IDC_TURN, L"BUTTON", L"Built-in STUN/TURN server (UDP)", WS_TABSTOP | BS_AUTOCHECKBOX },
    { IDC_L_TURNPORT, L"STATIC", L"Port", SS_LEFT },
    { IDC_TURNPORT, L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_NUMBER },
    { IDC_L_PUBIP, L"STATIC", L"Public IPv4", SS_LEFT },
    { IDC_PUBIP, L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL },
    { IDC_TURNHINT, L"STATIC",
      L"The address phones reach this server on (on a cloud server, its public or elastic IP). "
      L"Open UDP for this port and 49152-49252 in the cloud firewall too.",
      SS_LEFT },
    { IDC_SAVE, L"BUTTON", L"Save and restart", WS_TABSTOP | BS_DEFPUSHBUTTON },
    { IDC_MSG, L"STATIC", L"", SS_LEFT | SS_NOPREFIX },
    { IDC_LOGIN, L"BUTTON", L"Show this icon when I sign in", WS_TABSTOP | BS_AUTOCHECKBOX },
};

static void place(int id, int x, int y, int w, int h, int show)
{
    HWND c = GetDlgItem(g_win, id);
    UINT d = dpi_of(g_win);
    SetWindowPos(c, NULL, MulDiv(x, (int)d, 96), MulDiv(y, (int)d, 96), MulDiv(w, (int)d, 96), MulDiv(h, (int)d, 96),
                 SWP_NOZORDER | (show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
}

static int is_heading(int id) { return id == IDC_TITLE || id == IDC_H_ADMIN || id == IDC_H_WEB || id == IDC_H_TURN; }

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
    lf.lfWeight = FW_SEMIBOLD;
    g_bold = CreateFontIndirectW(&lf);
    for (size_t i = 0; i < sizeof CTLS / sizeof CTLS[0]; i++)
        SendDlgItemMessageW(g_win, CTLS[i].id, WM_SETFONT, (WPARAM)(is_heading(CTLS[i].id) ? g_bold : g_font), TRUE);
}

/* the form from the settings */
static void fill(void)
{
    if (!g_win || !g_settings)
        return;
    char n[16];
    set_text(IDC_USER, jstr("username"));
    SetDlgItemTextW(g_win, IDC_PASS, L"");
    SendDlgItemMessageW(g_win, IDC_PASS, EM_SETCUEBANNER, TRUE,
                        (LPARAM)(*jstr("username") ? L"unchanged" : L"at least 8 characters"));
    snprintf(n, sizeof n, "%d", jnum("http_port", 80));
    set_text(IDC_PORT, n);
    set_text(IDC_DOMAIN, jstr("domain"));
    set_text(IDC_EMAIL, jstr("email"));
    snprintf(n, sizeof n, "%d", jnum("tls_port", 443));
    set_text(IDC_TLSPORT, n);
    CheckDlgButton(g_win, IDC_REDIRECT, jbool("redirect_http") ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_win, IDC_TURN, jbool("turn_enabled") || !jbool("configured") ? BST_CHECKED : BST_UNCHECKED);
    snprintf(n, sizeof n, "%d", jnum("turn_port", 3478));
    set_text(IDC_TURNPORT, n);
    set_text(IDC_PUBIP, jstr("public_ip"));
}

static void layout(void)
{
    if (!g_win)
        return;
    const int W = 500, X = 20, FW = W - 40, LW = 96;
    int inst = g_state != 0, form = inst && g_settings;
    wchar_t buf[700], url[600] = L"";
    if (form)
        panel_url(url, 600);
    if (!inst)
        swprintf(buf, 700, L"Not installed. Install the server as a Windows service: it then starts with Windows, "
                            L"whether or not anyone signs in.");
    else if (form && !jbool("configured"))
        swprintf(buf, 700, L"%ls — no administrator yet: fill in the settings below.", state_text());
    else
        swprintf(buf, 700, L"%ls%ls%ls", state_text(), *url ? L" — " : L"", url);
    SetDlgItemTextW(g_win, IDC_STATE, buf);
    SetDlgItemTextW(g_win, IDC_STARTSTOP, g_state == SERVICE_STOPPED ? L"Start" : L"Stop");
    int y = 16;
    place(IDC_TITLE, X, y, FW, 24, 1);
    y += 28;
    place(IDC_STATE, X, y, FW, 36, 1);
    y += 42;
    place(IDC_INSTALL, X, y, 150, 30, !inst);
    place(IDC_OPENPANEL, X, y, 110, 30, form);
    place(IDC_STARTSTOP, X + 118, y, 80, 30, inst);
    place(IDC_RESTART, X + 206, y, 80, 30, inst);
    place(IDC_SHOWLOG, W - 20 - 90, y, 90, 30, 1);
    y += 46;
    for (int id = IDC_H_ADMIN; id <= IDC_SAVE; id++)
        place(id, 0, 0, 0, 0, 0);
    if (form) {
        int w2 = (FW - 2 * LW - 20) / 2;
        place(IDC_H_ADMIN, X, y, FW, 22, 1);
        y += 26;
        place(IDC_L_USER, X, y + 4, LW, 22, 1);
        place(IDC_USER, X + LW, y, w2, 26, 1);
        place(IDC_L_PASS, X + LW + w2 + 20, y + 4, LW, 22, 1);
        place(IDC_PASS, X + 2 * LW + w2 + 20, y, w2, 26, 1);
        y += 40;
        place(IDC_H_WEB, X, y, FW, 22, 1);
        y += 26;
        place(IDC_L_PORT, X, y + 4, LW, 22, 1);
        place(IDC_PORT, X + LW, y, 80, 26, 1);
        y += 32;
        place(IDC_L_DOMAIN, X, y + 4, LW, 22, 1);
        place(IDC_DOMAIN, X + LW, y, FW - LW, 26, 1);
        y += 32;
        int https = GetWindowTextLengthW(GetDlgItem(g_win, IDC_DOMAIN)) > 0;
        place(IDC_L_EMAIL, X, y + 4, LW, 22, https);
        place(IDC_EMAIL, X + LW, y, w2, 26, https);
        place(IDC_L_TLSPORT, X + LW + w2 + 20, y + 4, LW, 22, https);
        place(IDC_TLSPORT, X + 2 * LW + w2 + 20, y, 80, 26, https);
        if (https) {
            y += 32;
            place(IDC_REDIRECT, X + LW, y, FW - LW, 24, 1);
            y += 28;
        }
        place(IDC_WEBHINT, X + LW, y, FW - LW, 36, 1);
        y += 46;
        place(IDC_H_TURN, X, y, FW, 22, 1);
        y += 26;
        place(IDC_TURN, X, y, FW, 24, 1);
        y += 30;
        int turn = IsDlgButtonChecked(g_win, IDC_TURN) == BST_CHECKED;
        if (turn) {
            place(IDC_L_TURNPORT, X, y + 4, LW, 22, 1);
            place(IDC_TURNPORT, X + LW, y, 80, 26, 1);
            place(IDC_L_PUBIP, X + LW + w2 + 20, y + 4, LW, 22, 1);
            place(IDC_PUBIP, X + 2 * LW + w2 + 20, y, w2, 26, 1);
            y += 32;
            place(IDC_TURNHINT, X + LW, y, FW - LW, 36, 1);
            y += 44;
        }
        place(IDC_SAVE, W - 20 - 150, y, 150, 30, 1);
        y += 38;
    }
    place(IDC_MSG, X, y, FW, g_msg[0] ? 40 : 0, g_msg[0] != 0);
    SetDlgItemTextW(g_win, IDC_MSG, g_msg);
    if (g_msg[0])
        y += 44;
    CheckDlgButton(g_win, IDC_LOGIN, login_enabled() ? BST_CHECKED : BST_UNCHECKED);
    place(IDC_LOGIN, X, y, FW, 24, 1);
    y += 40;
    UINT d = dpi_of(g_win);
    RECT r = { 0, 0, MulDiv(W, (int)d, 96), MulDiv(y, (int)d, 96) };
    AdjustWindowRectEx(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0);
    SetWindowPos(g_win, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(g_win, NULL, TRUE);
}

/* The form, as the setup wizard would send it, through `remodroid-server configure`. */
static void save(void)
{
    cJSON *in = cJSON_CreateObject();
    char *s;
    cJSON_AddStringToObject(in, "username", s = text_of(IDC_USER));
    free(s);
    cJSON_AddStringToObject(in, "password", s = text_of(IDC_PASS));
    free(s);
    cJSON_AddNumberToObject(in, "http_port", atoi(s = text_of(IDC_PORT)));
    free(s);
    cJSON_AddStringToObject(in, "domain", s = text_of(IDC_DOMAIN));
    free(s);
    cJSON_AddStringToObject(in, "email", s = text_of(IDC_EMAIL));
    free(s);
    cJSON_AddNumberToObject(in, "tls_port", atoi(s = text_of(IDC_TLSPORT)));
    free(s);
    cJSON_AddBoolToObject(in, "redirect_http", IsDlgButtonChecked(g_win, IDC_REDIRECT) == BST_CHECKED);
    cJSON_AddBoolToObject(in, "turn_enabled", IsDlgButtonChecked(g_win, IDC_TURN) == BST_CHECKED);
    cJSON_AddNumberToObject(in, "turn_port", atoi(s = text_of(IDC_TURNPORT)));
    free(s);
    cJSON_AddStringToObject(in, "public_ip", s = text_of(IDC_PUBIP));
    free(s);
    cJSON_AddStringToObject(in, "ice_extra", jstr("ice_extra")); /* not on this form: kept */
    char *body = cJSON_PrintUnformatted(in), *out = NULL;
    cJSON_Delete(in);
    SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_WAIT));
    int code = server_cmd(L"configure", body, &out);
    free(body);
    cJSON *res = out ? cJSON_Parse(out) : NULL;
    cJSON *err = cJSON_GetObjectItemCaseSensitive(res, "error");
    if (code == 0) {
        g_msg_bad = 0;
        swprintf(g_msg, 400, L"Saved. The server is restarting with the new settings.");
        service_do(ID_RESTART);
        Sleep(1500);
        g_state = service_state();
        load_settings();
        fill();
    } else {
        wchar_t *w = wide(cJSON_IsString(err) ? err->valuestring : out && *out ? out : "remodroid-server.exe did not run");
        swprintf(g_msg, 400, L"Not saved: %ls", w);
        free(w);
        g_msg_bad = 1;
    }
    cJSON_Delete(res);
    free(out);
    tray_update(0);
    layout();
}

static LRESULT CALLBACK win_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_win = h;
        for (size_t i = 0; i < sizeof CTLS / sizeof CTLS[0]; i++)
            CreateWindowExW(0, CTLS[i].cls, CTLS[i].text, WS_CHILD | CTLS[i].style, 0, 0, 10, 10, h,
                            (HMENU)(INT_PTR)CTLS[i].id, g_inst, NULL);
        SendDlgItemMessageW(h, IDC_DOMAIN, EM_SETCUEBANNER, TRUE, (LPARAM)L"optional, e.g. remodroid.example.com");
        SendDlgItemMessageW(h, IDC_EMAIL, EM_SETCUEBANNER, TRUE, (LPARAM)L"for expiry notices (optional)");
        SendDlgItemMessageW(h, IDC_PUBIP, EM_SETCUEBANNER, TRUE, (LPARAM)L"e.g. 203.0.113.10");
        set_fonts();
        fill();
        layout();
        return 0;
    case WM_DPICHANGED: {
        RECT *r = (RECT *)lp;
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER);
        set_fonts();
        layout();
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        if ((HWND)lp == GetDlgItem(h, IDC_MSG) && g_msg_bad)
            SetTextColor((HDC)wp, RGB(0xB4, 0x23, 0x18));
        else if ((HWND)lp == GetDlgItem(h, IDC_WEBHINT) || (HWND)lp == GetDlgItem(h, IDC_TURNHINT))
            SetTextColor((HDC)wp, GetSysColor(COLOR_GRAYTEXT));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_INSTALL:
            install();
            if (!g_msg_bad && !login_enabled())
                set_login(1); /* the icon comes back after signing in, like the server after a reboot */
            fill();
            tray_update(0);
            layout();
            return 0;
        case IDC_OPENPANEL: open_panel(); return 0;
        case IDC_STARTSTOP: do_action(g_state == SERVICE_STOPPED ? ID_START : ID_STOP); return 0;
        case IDC_RESTART: do_action(ID_RESTART); return 0;
        case IDC_SHOWLOG: show_log(); return 0;
        case IDC_SAVE: save(); return 0;
        case IDOK: /* Enter: the default button */
            if (g_settings)
                save();
            else if (!g_state)
                install();
            return 0;
        case IDC_TURN: layout(); return 0;
        case IDC_DOMAIN:
            if (HIWORD(wp) == EN_CHANGE)
                layout();
            return 0;
        case IDC_LOGIN: set_login(IsDlgButtonChecked(h, IDC_LOGIN) == BST_CHECKED); return 0;
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
        UINT d = dpi_of(NULL);
        CreateWindowExW(0, WIN_CLASS, L"RemoDroid Server", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                        wa.right - MulDiv(560, (int)d, 96), wa.top + MulDiv(40, (int)d, 96), 520, 640, NULL, NULL,
                        g_inst, NULL);
    } else {
        fill();
    }
    layout();
    ShowWindow(g_win, SW_SHOWNORMAL);
    SetForegroundWindow(g_win);
}

/* ---------------- the log window (server.log in the data folder) ---------------- */

static void log_path(wchar_t *out) { swprintf(out, MAX_PATH + 20, L"%ls\\server.log", g_data); }

/* The end of server.log, with \r\n line ends for the edit control. */
static wchar_t *read_log(LONGLONG *size)
{
    wchar_t p[MAX_PATH + 20];
    log_path(p);
    HANDLE f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           0, NULL);
    LARGE_INTEGER sz = { 0 };
    if (f == INVALID_HANDLE_VALUE || !GetFileSizeEx(f, &sz)) {
        if (f != INVALID_HANDLE_VALUE)
            CloseHandle(f);
        *size = -1;
        return _wcsdup(L"(no log yet: the server writes it once the service runs)");
    }
    *size = sz.QuadPart;
    LONGLONG from = sz.QuadPart > LOG_TAIL ? sz.QuadPart - LOG_TAIL : 0;
    LARGE_INTEGER off = { .QuadPart = from };
    SetFilePointerEx(f, off, NULL, FILE_BEGIN);
    size_t want = (size_t)(sz.QuadPart - from);
    char *b = malloc(want + 1);
    DWORD n = 0;
    if (!ReadFile(f, b, (DWORD)want, &n, NULL))
        n = 0;
    CloseHandle(f);
    b[n] = 0;
    char *start = b;
    if (from && (start = strchr(b, '\n'))) /* from the first whole line */
        start++;
    else
        start = b;
    char *crlf = malloc(strlen(start) * 2 + 1), *o = crlf;
    for (const char *s = start; *s; s++) {
        if (*s == '\n' && (s == start || s[-1] != '\r'))
            *o++ = '\r';
        *o++ = *s;
    }
    *o = 0;
    free(b);
    wchar_t *w = wide(crlf);
    free(crlf);
    return w;
}

static void log_refresh(int force)
{
    if (!g_log)
        return;
    LONGLONG size;
    wchar_t *w = read_log(&size);
    if (!force && size == g_log_size) {
        free(w);
        return;
    }
    g_log_size = size;
    HWND e = GetDlgItem(g_log, IDC_LOGTEXT);
    SCROLLINFO si = { .cbSize = sizeof si, .fMask = SIF_ALL };
    int at_end = force || !GetScrollInfo(e, SB_VERT, &si) || si.nPos + (int)si.nPage > si.nMax;
    int first = (int)SendMessageW(e, EM_GETFIRSTVISIBLELINE, 0, 0);
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

static void copy_log(void)
{
    LONGLONG size;
    wchar_t *w = read_log(&size);
    size_t n = (wcslen(w) + 1) * sizeof(wchar_t);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n);
    memcpy(GlobalLock(g), w, n);
    GlobalUnlock(g);
    free(w);
    if (OpenClipboard(g_log)) {
        EmptyClipboard();
        SetClipboardData(CF_UNICODETEXT, g);
        CloseClipboard();
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

static LRESULT CALLBACK log_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        CreateWindowExW(0, L"EDIT", L"",
                        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY |
                            ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                        0, 0, 10, 10, h, (HMENU)(INT_PTR)IDC_LOGTEXT, g_inst, NULL);
        SendDlgItemMessageW(h, IDC_LOGTEXT, EM_SETLIMITTEXT, 0, 0); /* no 32K cap */
        wchar_t p[MAX_PATH + 20];
        log_path(p);
        CreateWindowExW(0, L"STATIC", p, WS_CHILD | WS_VISIBLE | SS_PATHELLIPSIS | SS_NOPREFIX, 0, 0, 10, 10, h,
                        (HMENU)(INT_PTR)IDC_LOGPATH, g_inst, NULL);
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
        if (LOWORD(wp) == IDC_LOGCOPY) {
            copy_log();
        } else if (LOWORD(wp) == IDC_LOGFOLDER) {
            wchar_t p[MAX_PATH + 20], args[MAX_PATH + 40];
            log_path(p);
            swprintf(args, MAX_PATH + 40, L"/select,\"%ls\"", p); /* Explorer, with server.log selected */
            ShellExecuteW(NULL, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
        }
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
        CreateWindowExW(0, LOG_CLASS, L"RemoDroid Server log", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                        MulDiv(900, (int)d, 96), MulDiv(520, (int)d, 96), NULL, NULL, g_inst, NULL);
        if (!g_log)
            return;
    }
    log_refresh(1);
    SetTimer(g_log, 1, 1000, NULL);
    ShowWindow(g_log, SW_SHOWNORMAL);
    SetForegroundWindow(g_log);
}

/* ---------------- main ---------------- */

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
    case WM_TIMER:
        refresh();
        return 0;
    case WM_SHOWWIN:
        show_window();
        return 0;
    case WM_CLOSE:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        if (g_win)
            DestroyWindow(g_win);
        if (g_log)
            DestroyWindow(g_log);
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
    /* one instance: a second start shows the first one's window */
    HANDLE mtx = CreateMutexW(NULL, TRUE, L"Local\\RemoDroidServerTray");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(TRAY_CLASS, NULL);
        if (other)
            PostMessageW(other, WM_SHOWWIN, 0, 0);
        return 0;
    }
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    const wchar_t *pd = _wgetenv(L"ProgramData");
    swprintf(g_data, MAX_PATH, L"%ls\\RemoDroid", pd && *pd ? pd : L"C:\\ProgramData");
    GetModuleFileNameW(NULL, g_exe_dir, MAX_PATH);
    wchar_t *sl = wcsrchr(g_exe_dir, L'\\');
    if (sl)
        *sl = 0;

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
    ChangeWindowMessageFilterEx(NULL, WM_TASKBARCREATED, MSGFLT_ALLOW, NULL); /* we are elevated, Explorer isn't */
    g_tray = CreateWindowExW(0, TRAY_CLASS, L"RemoDroid Server", 0, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    ChangeWindowMessageFilterEx(g_tray, WM_TASKBARCREATED, MSGFLT_ALLOW, NULL);
    int sz = GetSystemMetrics(SM_CXSMICON) * (int)dpi_of(NULL) / 96;
    if (sz < 16)
        sz = 16;
    g_icons[0] = make_icon(sz, RD_ICON_SERVER_OFF);
    g_icons[1] = make_icon(sz, RD_ICON_SERVER_ON);
    g_state = service_state();
    load_settings();
    tray_update(1);
    SetTimer(g_tray, 1, 2000, NULL);
    if (!background || !g_state || !jbool("configured"))
        show_window();
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (g_win && IsDialogMessageW(g_win, &m)) /* Tab between fields, Enter presses the default button */
            continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    cJSON_Delete(g_settings);
    ReleaseMutex(mtx);
    return 0;
}
