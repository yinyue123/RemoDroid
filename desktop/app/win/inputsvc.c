/* remodroid-input.exe: lets the agent control windows of elevated programs (desktop/win/inputpipe.h).
 *
 *   remodroid-input install     (administrator) copy this program to %ProgramFiles%\RemoDroid and
 *                               set up the RemoDroidInput service (SYSTEM, starts with Windows)
 *   remodroid-input uninstall   remove the service and that copy
 *   remodroid-input service     what the service manager runs
 *   remodroid-input helper      what the service starts in each signed-in session
 *
 * The service starts a helper, as SYSTEM, in every active session and again after it ends or the
 * session changes. The helper takes the agent's events on \\.\pipe\RemoDroidInput-<session> and
 * injects them; only programs in its own session can connect. It injects on the normal desktop, or,
 * while the session is locked, on the secure desktop of the lock screen, which it also shows the agent
 * on \\.\pipe\RemoDroidScreen-<session> (inputpipe.h): so a locked computer can be unlocked from the
 * viewer, with its password. An unlocked session's secure desktop, a UAC prompt, stays out of reach:
 * nothing can answer UAC through it. */
#ifndef UNICODE
#define UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wtsapi32.h>

#include "inputpipe.h"

void ri_apply(const ri_msg_t *m, const char *text); /* desktop/win/input.c, built with RD_INPUT_HELPER */

#define MAX_SESSIONS 16

static struct {
    SERVICE_STATUS_HANDLE ss;
    SERVICE_STATUS st;
    HANDLE stop, kick; /* events: stop; look at the sessions again */
    struct { DWORD sid; HANDLE proc; ULONGLONG retry; } h[MAX_SESSIONS];
    int nh;
} S;

/* ---------------- log: %ProgramData%\RemoDroid\input-service.log ---------------- */

static void say(const wchar_t *fmt, ...)
{
    wchar_t dir[MAX_PATH], path[MAX_PATH + 40], line[600];
    if (SHGetFolderPathW(NULL, CSIDL_COMMON_APPDATA, NULL, 0, dir) != S_OK)
        return;
    swprintf(path, MAX_PATH + 40, L"%ls\\RemoDroid", dir);
    CreateDirectoryW(path, NULL);
    wcscat(path, L"\\input-service.log");
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = swprintf(line, 600, L"%04d-%02d-%02d %02d:%02d:%02d ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                     t.wSecond);
    va_list ap;
    va_start(ap, fmt);
    vswprintf(line + n, 600 - (size_t)n, fmt, ap);
    va_end(ap);
    char u8[1800];
    int k = WideCharToMultiByte(CP_UTF8, 0, line, -1, u8, sizeof u8 - 2, NULL, NULL);
    if (k <= 0)
        return;
    strcpy(u8 + k - 1, "\r\n");
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return;
    DWORD w;
    WriteFile(f, u8, (DWORD)strlen(u8), &w, NULL);
    CloseHandle(f);
}

/* ---------------- the helper (SYSTEM, in a user's session) ---------------- */

/* This thread onto the desktop that takes the input: the lock screen's while the session is locked, else
 * the normal one. Looked at again every 250 ms. */
typedef struct {
    HDESK cur;
    int locked;
    ULONGLONG at;
} follow_t;

static void follow(follow_t *f)
{
    ULONGLONG now = GetTickCount64();
    if (f->cur && now - f->at < 250)
        return;
    f->at = now;
    int locked = ri_session_locked();
    if (f->cur && locked == f->locked && !locked)
        return;
    HDESK d = locked ? OpenInputDesktop(0, FALSE, GENERIC_ALL) : OpenDesktopW(L"Default", 0, FALSE, GENERIC_ALL);
    if (d && SetThreadDesktop(d)) {
        if (f->cur)
            CloseDesktop(f->cur);
        f->cur = d;
        f->locked = locked;
    } else if (d) {
        CloseDesktop(d);
    }
}

static DWORD WINAPI serve(LPVOID arg)
{
    HANDLE p = arg;
    ri_msg_t m;
    DWORD n;
    char *text = NULL;
    follow_t fw = { 0 };
    while (ReadFile(p, &m, sizeof m, &n, NULL) && n == sizeof m) {
        follow(&fw);
        if (m.op == RI_TEXT) {
            if (m.a <= 0 || m.a >= RI_TEXT_MAX)
                break;
            text = realloc(text, (size_t)m.a + 1);
            DWORD got = 0;
            while (got < (DWORD)m.a && ReadFile(p, text + got, (DWORD)m.a - got, &n, NULL) && n)
                got += n;
            if (got != (DWORD)m.a)
                break;
            text[m.a] = 0;
            ri_apply(&m, text);
        } else {
            ri_apply(&m, NULL);
        }
    }
    free(text);
    if (fw.cur)
        CloseDesktop(fw.cur);
    DisconnectNamedPipe(p);
    CloseHandle(p);
    return 0;
}

/* The lock screen's picture: for each request, while the session is locked, the pixels of the input
 * desktop (GDI: the secure desktop has no duplication for us); else "not locked". */
static int put_all(HANDLE p, const void *b, DWORD n)
{
    DWORD w;
    for (const char *q = b; n; q += w, n -= w)
        if (!WriteFile(p, q, n, &w, NULL) || !w)
            return -1;
    return 0;
}

static DWORD WINAPI serve_screen(LPVOID arg)
{
    HANDLE p = arg;
    rs_req_t q;
    DWORD n;
    uint8_t *bits = NULL;
    int bw = 0, bh = 0;
    HBITMAP bmp = NULL;
    HDC mdc = CreateCompatibleDC(NULL);
    follow_t fw = { 0 };
    while (ReadFile(p, &q, sizeof q, &n, NULL) && n == sizeof q) {
        rs_reply_t r = { 0 };
        fw.at = 0;
        follow(&fw);
        if (fw.locked && q.w > 0 && q.h > 0 && (int64_t)q.w * q.h <= RS_MAX_PIXELS) {
            if (q.w != bw || q.h != bh) {
                if (bmp)
                    DeleteObject(bmp);
                BITMAPINFO bi = { .bmiHeader = { .biSize = sizeof bi.bmiHeader, .biWidth = q.w, .biHeight = -q.h,
                                                 .biPlanes = 1, .biBitCount = 32, .biCompression = BI_RGB } };
                bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
                bw = bmp ? q.w : 0;
                bh = bmp ? q.h : 0;
            }
            HDC sdc = bmp ? GetDC(NULL) : NULL; /* the desktop this thread is on now */
            if (sdc) {
                HGDIOBJ old = SelectObject(mdc, bmp);
                if (BitBlt(mdc, 0, 0, q.w, q.h, sdc, q.x, q.y, SRCCOPY | CAPTUREBLT)) {
                    GdiFlush();
                    r.locked = 1;
                    r.w = q.w;
                    r.h = q.h;
                }
                SelectObject(mdc, old);
                ReleaseDC(NULL, sdc);
            }
        }
        if (put_all(p, &r, sizeof r) < 0 || (r.locked && put_all(p, bits, (DWORD)r.w * r.h * 4) < 0))
            break;
    }
    if (bmp)
        DeleteObject(bmp);
    DeleteDC(mdc);
    if (fw.cur)
        CloseDesktop(fw.cur);
    DisconnectNamedPipe(p);
    CloseHandle(p);
    return 0;
}

/* A pipe server: name, its security, and the thread each client of this session gets. */
typedef struct {
    const wchar_t *fmt;
    DWORD access;
    LPTHREAD_START_ROUTINE fn;
} pipe_srv_t;

static DWORD WINAPI listen_pipe(LPVOID arg)
{
    const pipe_srv_t *ps = arg;
    DWORD sid = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &sid);
    wchar_t name[64];
    swprintf(name, 64, ps->fmt, (unsigned long)sid);
    /* owned by SYSTEM (the agent checks), for SYSTEM and the interactive users; no low integrity */
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, FALSE };
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:SYD:P(A;;GA;;;SY)(A;;GRGW;;;IU)S:(ML;;NW;;;ME)",
                                                              SDDL_REVISION_1, &sa.lpSecurityDescriptor, NULL)) {
        say(L"helper %lu: no security descriptor (error %lu)", sid, GetLastError());
        return 1;
    }
    for (int first = 1;; first = 0) {
        HANDLE p = CreateNamedPipeW(name, ps->access | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                    PIPE_UNLIMITED_INSTANCES, 1 << 20, 65536, 0, &sa);
        if (p == INVALID_HANDLE_VALUE) {
            say(L"helper %lu: cannot create %ls (error %lu)", sid, name, GetLastError());
            return 1;
        }
        if (first)
            say(L"helper %lu: ready on %ls", sid, name);
        if (!ConnectNamedPipe(p, NULL) && GetLastError() != ERROR_PIPE_CONNECTED) {
            CloseHandle(p);
            continue;
        }
        ULONG csid = (ULONG)-1;
        if (!GetNamedPipeClientSessionId(p, &csid) || csid != sid) { /* only this session's programs */
            DisconnectNamedPipe(p);
            CloseHandle(p);
            continue;
        }
        HANDLE t = CreateThread(NULL, 0, ps->fn, p, 0, NULL);
        if (t)
            CloseHandle(t);
        else
            CloseHandle(p);
    }
}

static int helper(void)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); /* pixels as the agent counts them */
    static const pipe_srv_t input = { RI_PIPE_FMT, PIPE_ACCESS_INBOUND, serve },
                            screen = { RS_PIPE_FMT, PIPE_ACCESS_DUPLEX, serve_screen };
    HANDLE t = CreateThread(NULL, 0, listen_pipe, (LPVOID)&screen, 0, NULL);
    if (t)
        CloseHandle(t);
    return (int)listen_pipe((LPVOID)&input);
}

/* ---------------- the service ---------------- */

static HANDLE start_helper(DWORD sid)
{
    HANDLE me = NULL, tok = NULL;
    HANDLE proc = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &me) &&
        DuplicateTokenEx(me, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, TokenPrimary, &tok) &&
        SetTokenInformation(tok, TokenSessionId, &sid, sizeof sid)) {
        wchar_t exe[MAX_PATH], cmd[MAX_PATH + 20];
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        swprintf(cmd, MAX_PATH + 20, L"\"%ls\" helper", exe);
        STARTUPINFOW si = { .cb = sizeof si, .lpDesktop = L"winsta0\\default" };
        PROCESS_INFORMATION pi;
        if (CreateProcessAsUserW(tok, exe, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread);
            proc = pi.hProcess;
            say(L"service: helper started in session %lu", sid);
        }
    }
    if (!proc)
        say(L"service: cannot start a helper in session %lu (error %lu)", sid, GetLastError());
    if (tok)
        CloseHandle(tok);
    if (me)
        CloseHandle(me);
    return proc;
}

/* A helper in each active session; none in sessions that ended. */
static void reconcile(void)
{
    WTS_SESSION_INFOW *si = NULL;
    DWORD n = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &si, &n))
        n = 0;
    ULONGLONG now = GetTickCount64();
    for (int i = 0; i < S.nh; i++) { /* ended helpers and sessions */
        int active = 0;
        for (DWORD j = 0; j < n; j++)
            if (si[j].SessionId == S.h[i].sid && si[j].State == WTSActive)
                active = 1;
        int alive = S.h[i].proc && WaitForSingleObject(S.h[i].proc, 0) == WAIT_TIMEOUT;
        if (!alive && S.h[i].proc) {
            CloseHandle(S.h[i].proc);
            S.h[i].proc = NULL;
            S.h[i].retry = now + 2000;
        }
        if (!active) {
            if (S.h[i].proc) {
                TerminateProcess(S.h[i].proc, 0);
                CloseHandle(S.h[i].proc);
            }
            S.h[i--] = S.h[--S.nh];
        }
    }
    for (DWORD j = 0; j < n; j++) {
        if (si[j].State != WTSActive || si[j].SessionId == 0)
            continue;
        int k = -1;
        for (int i = 0; i < S.nh; i++)
            if (S.h[i].sid == si[j].SessionId)
                k = i;
        if (k < 0 && S.nh < MAX_SESSIONS) {
            k = S.nh++;
            S.h[k].sid = si[j].SessionId;
            S.h[k].proc = NULL;
            S.h[k].retry = 0;
        }
        if (k >= 0 && !S.h[k].proc && now >= S.h[k].retry) {
            S.h[k].proc = start_helper(S.h[k].sid);
            if (!S.h[k].proc)
                S.h[k].retry = now + 10000;
        }
    }
    WTSFreeMemory(si);
}

static void report(DWORD state)
{
    S.st.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    S.st.dwCurrentState = state;
    S.st.dwControlsAccepted = state == SERVICE_RUNNING
                                  ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE
                                  : 0;
    S.st.dwWaitHint = state == SERVICE_STOP_PENDING ? 5000 : 0;
    SetServiceStatus(S.ss, &S.st);
}

static DWORD WINAPI on_control(DWORD ctl, DWORD type, LPVOID data, LPVOID ctx)
{
    switch (ctl) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        report(SERVICE_STOP_PENDING);
        SetEvent(S.stop);
        return NO_ERROR;
    case SERVICE_CONTROL_SESSIONCHANGE:
        SetEvent(S.kick);
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    }
    return ERROR_CALL_NOT_IMPLEMENTED;
}

static void WINAPI service_main(DWORD argc, LPWSTR *argv)
{
    S.ss = RegisterServiceCtrlHandlerExW(RI_SERVICE, on_control, NULL);
    if (!S.ss)
        return;
    S.stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    S.kick = CreateEventW(NULL, FALSE, FALSE, NULL);
    report(SERVICE_RUNNING);
    say(L"service: started");
    for (;;) {
        reconcile();
        HANDLE w[2 + MAX_SESSIONS] = { S.stop, S.kick };
        DWORD nw = 2;
        for (int i = 0; i < S.nh; i++)
            if (S.h[i].proc)
                w[nw++] = S.h[i].proc;
        DWORD r = WaitForMultipleObjects(nw, w, FALSE, 3000);
        if (r == WAIT_OBJECT_0)
            break;
        if (r == WAIT_OBJECT_0 + 1)
            Sleep(500); /* a session change: let the session finish signing in */
    }
    for (int i = 0; i < S.nh; i++)
        if (S.h[i].proc)
            TerminateProcess(S.h[i].proc, 0);
    say(L"service: stopped");
    report(SERVICE_STOPPED);
}

/* ---------------- install / uninstall ---------------- */

static int problem(const wchar_t *what)
{
    DWORD e = GetLastError();
    if (e == ERROR_ACCESS_DENIED)
        fwprintf(stderr, L"%ls: access denied. Run it as an administrator.\n", what);
    else
        fwprintf(stderr, L"%ls: error %lu\n", what, e);
    return 1;
}

/* %ProgramFiles%\RemoDroid\remodroid-input.exe: a folder only administrators can change, as a
 * program the service runs as SYSTEM must be */
static void installed_path(wchar_t *dir, wchar_t *exe)
{
    wchar_t pf[MAX_PATH];
    if (SHGetFolderPathW(NULL, CSIDL_PROGRAM_FILES, NULL, 0, pf) != S_OK)
        wcscpy(pf, L"C:\\Program Files");
    swprintf(dir, MAX_PATH, L"%ls\\RemoDroid", pf);
    swprintf(exe, MAX_PATH, L"%ls\\" RI_EXE, dir);
}

static void stop_service(SC_HANDLE s)
{
    SERVICE_STATUS st;
    if (ControlService(s, SERVICE_CONTROL_STOP, &st))
        for (int i = 0; i < 40 && QueryServiceStatus(s, &st) && st.dwCurrentState != SERVICE_STOPPED; i++)
            Sleep(250);
}

static int install(void)
{
    wchar_t dir[MAX_PATH], exe[MAX_PATH], me[MAX_PATH], cmd[MAX_PATH + 20];
    installed_path(dir, exe);
    GetModuleFileNameW(NULL, me, MAX_PATH);
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    if (!scm)
        return problem(L"install");
    SC_HANDLE s = OpenServiceW(scm, RI_SERVICE, SERVICE_ALL_ACCESS);
    if (s)
        stop_service(s); /* to replace the program */
    CreateDirectoryW(dir, NULL);
    if (_wcsicmp(me, exe) && !CopyFileW(me, exe, FALSE)) {
        int r = problem(exe);
        if (s)
            CloseServiceHandle(s);
        CloseServiceHandle(scm);
        return r;
    }
    swprintf(cmd, MAX_PATH + 20, L"\"%ls\" service", exe);
    if (s)
        ChangeServiceConfigW(s, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, cmd, NULL, NULL,
                             NULL, NULL, NULL, L"RemoDroid Input");
    else
        s = CreateServiceW(scm, RI_SERVICE, L"RemoDroid Input", SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                           SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, cmd, NULL, NULL, NULL, NULL, NULL);
    if (!s) {
        int r = problem(L"install");
        CloseServiceHandle(scm);
        return r;
    }
    SERVICE_DESCRIPTIONW d = { L"Lets the RemoDroid agent control windows of programs that run as administrator, "
                               L"and show and unlock the lock screen." };
    ChangeServiceConfig2W(s, SERVICE_CONFIG_DESCRIPTION, &d);
    SC_ACTION acts[3] = { { SC_ACTION_RESTART, 5000 }, { SC_ACTION_RESTART, 5000 }, { SC_ACTION_RESTART, 30000 } };
    SERVICE_FAILURE_ACTIONSW fa = { .dwResetPeriod = 86400, .cActions = 3, .lpsaActions = acts };
    ChangeServiceConfig2W(s, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);
    int r = StartServiceW(s, 0, NULL) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING ? 0 : problem(L"start");
    CloseServiceHandle(s);
    CloseServiceHandle(scm);
    if (!r)
        wprintf(L"RemoDroid Input is installed: the agent can control windows of programs that run as "
                L"administrator, and the lock screen.\n");
    return r;
}

static int uninstall(void)
{
    wchar_t dir[MAX_PATH], exe[MAX_PATH];
    installed_path(dir, exe);
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    SC_HANDLE s = scm ? OpenServiceW(scm, RI_SERVICE, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE) : NULL;
    if (!s) {
        int r = GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST ? 0 : problem(L"uninstall");
        if (scm)
            CloseServiceHandle(scm);
        return r;
    }
    stop_service(s);
    int r = DeleteService(s) ? 0 : problem(L"uninstall");
    CloseServiceHandle(s);
    CloseServiceHandle(scm);
    if (!DeleteFileW(exe)) /* this very program, when run from there */
        MoveFileExW(exe, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    RemoveDirectoryW(dir); /* if empty */
    if (!r)
        wprintf(L"RemoDroid Input is removed.\n");
    return r;
}

int wmain(int argc, wchar_t **argv)
{
    const wchar_t *cmd = argc > 1 ? argv[1] : L"";
    if (!wcscmp(cmd, L"helper"))
        return helper();
    if (!wcscmp(cmd, L"service")) {
        SERVICE_TABLE_ENTRYW t[] = { { (LPWSTR)RI_SERVICE, service_main }, { NULL, NULL } };
        if (!StartServiceCtrlDispatcherW(t)) {
            fwprintf(stderr, L"service: this is run by the Windows service manager (remodroid-input install)\n");
            return 1;
        }
        return 0;
    }
    if (!wcscmp(cmd, L"install"))
        return install();
    if (!wcscmp(cmd, L"uninstall"))
        return uninstall();
    fwprintf(stderr, L"usage: remodroid-input install | uninstall   (as an administrator)\n"
                     L"  lets the RemoDroid agent control windows of programs that run as administrator\n");
    return 2;
}
