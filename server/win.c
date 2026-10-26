/* Windows: remodroid-server as a service, and the supervisor that restarts it.
 *
 * Windows has no exec(), so the server can't restart itself on new ports after the
 * wizard. The process the user or the service manager starts is a small supervisor:
 * it runs the server as a child (RD_SUPERVISED set) and starts it again when it exits
 * with SERVER_EXIT_RESTART. The child reads "stop" and "restart" lines from its stdin,
 * a pipe from the supervisor; the pipe closing (the supervisor gone) also stops it.
 *
 *   remodroid-server [-c server.ini]          in this console, until Ctrl+C
 *   remodroid-server [-c server.ini] install  the data folder, server.ini, a firewall rule and the
 *                                             RemoDroidServer service (starts with Windows); starts it
 *   remodroid-server uninstall                stops and removes the service (keeps the data folder)
 *   remodroid-server start | stop | restart
 *   remodroid-server -c server.ini service    what the service manager runs
 *
 * The data folder, %ProgramData%\RemoDroid, holds server.ini, the state and server.log;
 * only SYSTEM and Administrators can open it (server.ini holds the password hashes). */
#ifdef _WIN32

#include "server.h" /* first: Winsock before windows.h */

#include <aclapi.h>
#include <fcntl.h>
#include <io.h>
#include <sddl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SERVICE_NAME L"RemoDroidServer"
#define FIREWALL_RULE "RemoDroid Server"
#define LOG_MAX (8 * 1024 * 1024) /* server.log moves to server.log.1 beyond this, at service start */
#define CONTROL_RESTART 128       /* our service control code: restart the server */

static struct {
    int service;
    HANDLE child, child_in;  /* the running server and the write end of its stdin */
    HANDLE stop, kick;       /* events: stop for good; restart the server */
    SERVICE_STATUS_HANDLE ss;
    SERVICE_STATUS st;
} W;

static wchar_t *wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = malloc(sizeof(wchar_t) * (size_t)(n > 0 ? n : 1));
    if (n <= 0 || !MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n))
        w[0] = 0;
    return w;
}

static const char *join(const char *dir, const char *name)
{
    if (!name)
        return dir;
    size_t n = strlen(dir) + strlen(name) + 2;
    char *p = malloc(n);
    snprintf(p, n, "%s\\%s", dir, name);
    return p;
}

const char *win_data_path(const char *name)
{
    static char dir[MAX_PATH];
    if (!dir[0]) {
        const char *pd = getenv("ProgramData");
        snprintf(dir, sizeof dir, "%s\\RemoDroid", pd && *pd ? pd : "C:\\ProgramData");
    }
    return join(dir, name);
}

const char *win_exe_path(const char *name)
{
    static char dir[MAX_PATH];
    if (!dir[0]) {
        GetModuleFileNameA(NULL, dir, sizeof dir); /* UTF-8: the manifest's code page */
        char *s = strrchr(dir, '\\');
        if (s)
            *s = 0;
    }
    return join(dir, name);
}

static const char *exe_file(void)
{
    static char exe[MAX_PATH];
    if (!exe[0])
        GetModuleFileNameA(NULL, exe, sizeof exe);
    return exe;
}

/* Runs a command line hidden and waits; its exit code, or -1. */
static int run_hidden(const char *cmdline)
{
    wchar_t *cmd = wide(cmdline);
    STARTUPINFOW si = { .cb = sizeof si };
    PROCESS_INFORMATION pi;
    int ret = -1;
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 60000);
        DWORD code;
        if (GetExitCodeProcess(pi.hProcess, &code))
            ret = (int)code;
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    free(cmd);
    return ret;
}

/* ---------------- the supervised server ---------------- */

static ev_async cmd_async;
static volatile LONG cmd_restart;

static void on_cmd(struct ev_loop *loop, ev_async *w, int revents)
{
    if (cmd_restart)
        server_restart_soon();
    else
        server_stop();
}

static void *stdin_reader(void *arg)
{
    char line[64];
    while (fgets(line, sizeof line, stdin)) {
        if (!strncmp(line, "restart", 7)) {
            InterlockedExchange(&cmd_restart, 1);
            break;
        }
        if (!strncmp(line, "stop", 4))
            break;
    }
    ev_async_send(S.loop, &cmd_async); /* also on end of file: the supervisor is gone */
    return NULL;
}

void win_worker_start(void)
{
    ev_async_init(&cmd_async, on_cmd);
    ev_async_start(S.loop, &cmd_async);
    if (rd_thread_detached(stdin_reader, NULL) < 0)
        LOGW("cannot watch for the supervisor's requests");
}

/* ---------------- the supervisor ---------------- */

static int spawn(void)
{
    char cmd[2048];
    int n = snprintf(cmd, sizeof cmd, "\"%s\" -c \"%s\"", exe_file(), S.conf_path);
    for (int v = RD_LOG_INFO; v < rd_log_level && n < (int)sizeof cmd - 4; v++)
        n += snprintf(cmd + n, sizeof cmd - (size_t)n, " -v");
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0))
        return -1;
    SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE), err = GetStdHandle(STD_ERROR_HANDLE);
    SetHandleInformation(out, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    SetHandleInformation(err, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    STARTUPINFOW si = { .cb = sizeof si, .dwFlags = STARTF_USESTDHANDLES, .hStdInput = rd, .hStdOutput = out,
                        .hStdError = err };
    PROCESS_INFORMATION pi;
    wchar_t *w = wide(cmd);
    SetEnvironmentVariableA(WIN_SUPERVISED, "1");
    BOOL ok = CreateProcessW(NULL, w, NULL, NULL, TRUE, W.service ? CREATE_NO_WINDOW : 0, NULL, NULL, &si, &pi);
    free(w);
    CloseHandle(rd);
    if (!ok) {
        LOGE("cannot start %s: error %lu", exe_file(), GetLastError());
        CloseHandle(wr);
        return -1;
    }
    CloseHandle(pi.hThread);
    W.child = pi.hProcess;
    W.child_in = wr;
    return 0;
}

static void tell_child(const char *line)
{
    DWORD n;
    WriteFile(W.child_in, line, (DWORD)strlen(line), &n, NULL);
}

/* Runs the server until a stop: again after each restart it asks for and, as a service,
 * after it fails (a broken server.ini is retried, so fixing the file is enough). */
static int supervise(void)
{
    for (;;) {
        if (spawn() < 0)
            return 1;
        HANDLE hs[3] = { W.child, W.stop, W.kick };
        DWORD r = WaitForMultipleObjects(3, hs, FALSE, INFINITE);
        if (r != WAIT_OBJECT_0) {
            int stop = r == WAIT_OBJECT_0 + 1;
            tell_child(stop ? "stop\n" : "restart\n");
            if (WaitForSingleObject(W.child, 10000) == WAIT_TIMEOUT) {
                LOGW("the server did not %s in 10 s; ending it", stop ? "stop" : "restart");
                TerminateProcess(W.child, stop ? 0 : SERVER_EXIT_RESTART);
                WaitForSingleObject(W.child, 5000);
            }
        }
        DWORD code = 1;
        GetExitCodeProcess(W.child, &code);
        CloseHandle(W.child);
        CloseHandle(W.child_in);
        W.child = W.child_in = NULL;
        if (WaitForSingleObject(W.stop, 0) == WAIT_OBJECT_0)
            return 0;
        if (code == SERVER_EXIT_RESTART)
            continue;
        if (!W.service)
            return (int)code;
        LOGW("service: the server stopped (exit code %lu); starting it again in 10 s", code);
        if (WaitForSingleObject(W.stop, 10000) == WAIT_OBJECT_0)
            return 0;
    }
}

static BOOL WINAPI on_console(DWORD ev)
{
    if (ev == CTRL_C_EVENT || ev == CTRL_BREAK_EVENT)
        return TRUE; /* the server shares the console: it gets the Ctrl+C and stops, then so do we */
    SetEvent(W.stop);
    return TRUE;
}

/* ---------------- the service ---------------- */

static void report(DWORD state, DWORD exit_code)
{
    W.st.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    W.st.dwCurrentState = state;
    W.st.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    W.st.dwWin32ExitCode = exit_code ? ERROR_SERVICE_SPECIFIC_ERROR : NO_ERROR;
    W.st.dwServiceSpecificExitCode = exit_code;
    W.st.dwWaitHint = state == SERVICE_STOP_PENDING ? 20000 : 0;
    W.st.dwCheckPoint = state == SERVICE_STOP_PENDING ? W.st.dwCheckPoint + 1 : 0;
    SetServiceStatus(W.ss, &W.st);
}

static DWORD WINAPI on_control(DWORD ctl, DWORD type, LPVOID data, LPVOID ctx)
{
    switch (ctl) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        report(SERVICE_STOP_PENDING, 0);
        SetEvent(W.stop);
        return NO_ERROR;
    case CONTROL_RESTART:
        SetEvent(W.kick);
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    }
    return ERROR_CALL_NOT_IMPLEMENTED;
}

static void WINAPI service_main(DWORD argc, LPWSTR *argv)
{
    W.ss = RegisterServiceCtrlHandlerExW(SERVICE_NAME, on_control, NULL);
    if (!W.ss)
        return;
    report(SERVICE_RUNNING, 0);
    LOGI("service: started (%s)", S.conf_path);
    int code = supervise();
    LOGI("service: stopped");
    report(SERVICE_STOPPED, (DWORD)code);
}

/* The service's log: the server's and ours, in the data folder. */
static void open_log(void)
{
    const char *log = win_data_path("server.log");
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExA(log, GetFileExInfoStandard, &fa) && (fa.nFileSizeHigh || fa.nFileSizeLow > LOG_MAX)) {
        char old[MAX_PATH + 4];
        snprintf(old, sizeof old, "%s.1", log);
        MoveFileExA(log, old, MOVEFILE_REPLACE_EXISTING);
    }
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE h = CreateFileA(log, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    SetStdHandle(STD_OUTPUT_HANDLE, h);
    SetStdHandle(STD_ERROR_HANDLE, h);
    int fd = _open_osfhandle((intptr_t)h, _O_APPEND);
    if (fd >= 0)
        _dup2(fd, 2); /* our own log lines (stderr) go there too */
}

/* ---------------- install / uninstall / start / stop ---------------- */

static SC_HANDLE open_service(DWORD access, SC_HANDLE *scm)
{
    *scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!*scm)
        return NULL;
    SC_HANDLE s = OpenServiceW(*scm, SERVICE_NAME, access);
    if (!s) {
        DWORD e = GetLastError();
        CloseServiceHandle(*scm);
        SetLastError(e);
    }
    return s;
}

static int problem(const char *what)
{
    DWORD e = GetLastError();
    if (e == ERROR_ACCESS_DENIED)
        fprintf(stderr, "%s: access denied. Run it as an administrator.\n", what);
    else if (e == ERROR_SERVICE_DOES_NOT_EXIST)
        fprintf(stderr, "%s: the service isn't installed (remodroid-server install).\n", what);
    else
        fprintf(stderr, "%s: %s\n", what, rd_sock_strerror((int)e));
    return 1;
}

static DWORD state_of(SC_HANDLE s)
{
    SERVICE_STATUS st;
    return QueryServiceStatus(s, &st) ? st.dwCurrentState : 0;
}

static int wait_state(SC_HANDLE s, DWORD want, int ms)
{
    for (int t = 0; t < ms; t += 250) {
        if (state_of(s) == want)
            return 0;
        Sleep(250);
    }
    return state_of(s) == want ? 0 : -1;
}

static int control(const char *cmd)
{
    SC_HANDLE scm, s = open_service(SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_USER_DEFINED_CONTROL,
                                    &scm);
    if (!s)
        return problem(cmd);
    SERVICE_STATUS st;
    int ret = 0;
    DWORD now = state_of(s);
    if (!strcmp(cmd, "stop")) {
        if (now != SERVICE_STOPPED && !ControlService(s, SERVICE_CONTROL_STOP, &st) &&
            GetLastError() != ERROR_SERVICE_NOT_ACTIVE)
            ret = problem(cmd);
        else if (wait_state(s, SERVICE_STOPPED, 20000) < 0)
            ret = (fprintf(stderr, "stop: the service is still stopping\n"), 1);
    } else if (!strcmp(cmd, "restart") && now == SERVICE_RUNNING) {
        if (!ControlService(s, CONTROL_RESTART, &st))
            ret = problem(cmd);
    } else { /* start, or restart a stopped service */
        if (now != SERVICE_RUNNING && !StartServiceW(s, 0, NULL) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING)
            ret = problem(cmd);
        else if (wait_state(s, SERVICE_RUNNING, 10000) < 0)
            ret = (fprintf(stderr, "%s: the service did not start; see %s\n", cmd, win_data_path("server.log")), 1);
    }
    CloseServiceHandle(s);
    CloseServiceHandle(scm);
    return ret;
}

/* The data folder for SYSTEM and Administrators only, also for what is already in it. */
static int make_data_dir(void)
{
    const char *dir = win_data_path(NULL);
    if (rd_mkdirs(dir, 0700) < 0)
        return -1;
    PSECURITY_DESCRIPTOR sd;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", SDDL_REVISION_1,
                                                              &sd, NULL))
        return -1;
    BOOL present, defaulted;
    PACL dacl = NULL;
    GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted);
    wchar_t *w = wide(dir);
    DWORD e = SetNamedSecurityInfoW(w, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                    NULL, NULL, dacl, NULL);
    free(w);
    LocalFree(sd);
    return e == ERROR_SUCCESS ? 0 : -1;
}

/* A first server.ini: the example from the zip, minus the Linux web folder (the default
 * is the web folder next to the program). */
static int make_conf(void)
{
    if (GetFileAttributesA(S.conf_path) != INVALID_FILE_ATTRIBUTES)
        return 0;
    if (!CopyFileA(win_exe_path("server.ini.example"), S.conf_path, TRUE))
        return rd_conf_write(S.conf_path, "http", "listen", "0.0.0.0:5580");
    return rd_conf_write(S.conf_path, "http", "web_root", NULL);
}

static int install(void)
{
    char conf[MAX_PATH];
    if (!GetFullPathNameA(S.conf_path, sizeof conf, conf, NULL))
        snprintf(conf, sizeof conf, "%s", S.conf_path);
    S.conf_path = conf;
    if (make_data_dir() < 0)
        return problem(win_data_path(NULL));
    if (make_conf() < 0)
        return problem(S.conf_path);

    char cmd[1200];
    snprintf(cmd, sizeof cmd, "netsh advfirewall firewall delete rule name=\"" FIREWALL_RULE "\"");
    run_hidden(cmd);
    snprintf(cmd, sizeof cmd,
             "netsh advfirewall firewall add rule name=\"" FIREWALL_RULE "\" dir=in action=allow program=\"%s\" "
             "enable=yes profile=any",
             exe_file());
    if (run_hidden(cmd) != 0)
        fprintf(stderr, "install: could not add the Windows Firewall rule; allow %s by hand\n", exe_file());

    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    if (!scm)
        return problem("install");
    snprintf(cmd, sizeof cmd, "\"%s\" -c \"%s\" service", exe_file(), S.conf_path);
    wchar_t *bin = wide(cmd);
    SC_HANDLE s = OpenServiceW(scm, SERVICE_NAME, SERVICE_ALL_ACCESS);
    if (s)
        ChangeServiceConfigW(s, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, bin, NULL, NULL,
                             NULL, NULL, NULL, L"RemoDroid Server");
    else
        s = CreateServiceW(scm, SERVICE_NAME, L"RemoDroid Server", SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                           SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, bin, NULL, NULL, NULL, NULL, NULL);
    free(bin);
    if (!s) {
        int r = problem("install");
        CloseServiceHandle(scm);
        return r;
    }
    SERVICE_DESCRIPTIONW d = { L"RemoDroid control server: the web panel, logins, STUN/TURN relay and HTTPS." };
    ChangeServiceConfig2W(s, SERVICE_CONFIG_DESCRIPTION, &d);
    SC_ACTION acts[3] = { { SC_ACTION_RESTART, 5000 }, { SC_ACTION_RESTART, 5000 }, { SC_ACTION_RESTART, 30000 } };
    SERVICE_FAILURE_ACTIONSW fa = { .dwResetPeriod = 86400, .cActions = 3, .lpsaActions = acts };
    ChangeServiceConfig2W(s, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);
    CloseServiceHandle(s);
    CloseServiceHandle(scm);
    int r = control("restart");
    if (!r)
        printf("RemoDroid Server is running as the Windows service \"RemoDroid Server\" (starts with Windows).\n"
               "  settings:  %s\n  log:       %s\n"
               "Change the settings in RemoDroid-Server.exe, or open http://localhost:<port>/ for the setup wizard\n"
               "(its code is in %s).\n",
               S.conf_path, win_data_path("server.log"), win_data_path("setup-token"));
    return r;
}

static int uninstall(void)
{
    SC_HANDLE scm, s = open_service(SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE, &scm);
    if (!s)
        return problem("uninstall");
    SERVICE_STATUS st;
    if (ControlService(s, SERVICE_CONTROL_STOP, &st))
        wait_state(s, SERVICE_STOPPED, 20000);
    int r = DeleteService(s) ? 0 : problem("uninstall");
    CloseServiceHandle(s);
    CloseServiceHandle(scm);
    run_hidden("netsh advfirewall firewall delete rule name=\"" FIREWALL_RULE "\"");
    if (!r)
        printf("The service is removed. The settings and data stay in %s.\n", win_data_path(NULL));
    return r;
}

int win_command(const char *cmd, int argc, char **argv)
{
    W.stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    W.kick = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!*cmd) {
        SetConsoleCtrlHandler(on_console, TRUE);
        return supervise();
    }
    if (!strcmp(cmd, "service")) {
        W.service = 1;
        open_log();
        SERVICE_TABLE_ENTRYW t[] = { { (LPWSTR)SERVICE_NAME, service_main }, { NULL, NULL } };
        if (!StartServiceCtrlDispatcherW(t)) {
            fprintf(stderr, "service: this is run by the Windows service manager (see: remodroid-server install)\n");
            return 1;
        }
        return 0;
    }
    if (!strcmp(cmd, "install"))
        return install();
    if (!strcmp(cmd, "uninstall"))
        return uninstall();
    if (!strcmp(cmd, "start") || !strcmp(cmd, "stop") || !strcmp(cmd, "restart"))
        return control(cmd);
    fprintf(stderr, "unknown command: %s (remodroid-server -h)\n", cmd);
    return 2;
}

#endif
