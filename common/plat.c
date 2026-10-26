#include "plat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

int rd_net_init(void)
{
    static int done;
    WSADATA w;
    if (done)
        return 0;
    if (WSAStartup(MAKEWORD(2, 2), &w))
        return -1;
    done = 1;
    return 0;
}

int rd_sock_nonblock(int fd)
{
    u_long on = 1;
    return ioctlsocket((SOCKET)fd, FIONBIO, &on) ? -1 : 0;
}

int rd_socket(int af, int type, int proto)
{
    rd_net_init();
    SOCKET s = WSASocketW(af, type, proto, NULL, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (s == INVALID_SOCKET)
        return -1;
    if (rd_sock_nonblock((int)s) < 0) {
        closesocket(s);
        return -1;
    }
    if (type == SOCK_DGRAM) { /* an ICMP "port unreachable" must not break recvfrom on a UDP socket */
        BOOL off = FALSE;
        DWORD ret = 0;
        WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &off, sizeof off, NULL, 0, &ret, NULL, NULL);
    }
    return (int)s;
}

int rd_sock_close(int fd) { return closesocket((SOCKET)fd); }
int rd_sock_error(void) { return WSAGetLastError(); }
int rd_sock_again(int e) { return e == WSAEWOULDBLOCK || e == WSAEINTR || e == WSAEINPROGRESS; }

int rd_sock_accept(int lfd, struct sockaddr *sa, socklen_t *len)
{
    SOCKET s = accept((SOCKET)lfd, sa, len);
    if (s == INVALID_SOCKET)
        return -1;
    rd_sock_nonblock((int)s);
    return (int)s;
}

const char *rd_sock_strerror(int e)
{
    static __thread char buf[160];
    if (!FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, (DWORD)e, 0, buf,
                        sizeof buf, NULL))
        snprintf(buf, sizeof buf, "error %d", e);
    buf[strcspn(buf, "\r\n")] = 0;
    return buf;
}

typedef struct {
    void *(*fn)(void *);
    void *arg;
} tstart_t;

static DWORD WINAPI thread_tramp(LPVOID p)
{
    tstart_t t = *(tstart_t *)p;
    free(p);
    t.fn(t.arg);
    return 0;
}

int rd_thread_start(rd_thread_t *t, void *(*fn)(void *), void *arg)
{
    tstart_t *s = malloc(sizeof *s);
    s->fn = fn;
    s->arg = arg;
    *t = CreateThread(NULL, 0, thread_tramp, s, 0, NULL);
    if (!*t) {
        free(s);
        return -1;
    }
    return 0;
}

void rd_thread_join(rd_thread_t t)
{
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}

int rd_thread_detached(void *(*fn)(void *), void *arg)
{
    rd_thread_t t;
    if (rd_thread_start(&t, fn, arg) < 0)
        return -1;
    CloseHandle(t);
    return 0;
}

void rd_mutex_init(rd_mutex_t *m) { InitializeSRWLock(m); }
void rd_mutex_lock(rd_mutex_t *m) { AcquireSRWLockExclusive(m); }
void rd_mutex_unlock(rd_mutex_t *m) { ReleaseSRWLockExclusive(m); }
void rd_cond_init(rd_cond_t *c) { InitializeConditionVariable(c); }
void rd_cond_wait(rd_cond_t *c, rd_mutex_t *m) { SleepConditionVariableSRW(c, m, INFINITE, 0); }
void rd_cond_signal(rd_cond_t *c) { WakeConditionVariable(c); }
void rd_sleep_ms(int ms) { Sleep((DWORD)ms); }

const char *rd_machine(void)
{
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    switch (si.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64: return "x86_64";
    case PROCESSOR_ARCHITECTURE_ARM64: return "aarch64";
    case PROCESSOR_ARCHITECTURE_INTEL: return "x86";
    default: return "unknown";
    }
}

void rd_hostname(char *out, size_t cap)
{
    DWORD n = (DWORD)cap;
    if (!GetComputerNameA(out, &n) || !out[0])
        snprintf(out, cap, "windows-pc");
}

const char *rd_home_dir(void)
{
    static char home[MAX_PATH];
    if (!home[0]) {
        const char *p = getenv("USERPROFILE");
        snprintf(home, sizeof home, "%s", p && *p ? p : "C:\\");
    }
    return home;
}

#else /* POSIX */

#include <fcntl.h>
#include <pwd.h>
#include <sys/utsname.h>
#include <time.h>

int rd_net_init(void) { return 0; }

int rd_sock_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    return fl < 0 ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

int rd_socket(int af, int type, int proto)
{
#ifdef SOCK_NONBLOCK
    return socket(af, type | SOCK_NONBLOCK | SOCK_CLOEXEC, proto);
#else /* macOS */
    int fd = socket(af, type, proto);
    if (fd < 0)
        return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    rd_sock_nonblock(fd);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    return fd;
#endif
}

int rd_sock_close(int fd) { return close(fd); }
int rd_sock_error(void) { return errno; }
int rd_sock_again(int e) { return e == EAGAIN || e == EWOULDBLOCK || e == EINTR || e == EINPROGRESS; }
const char *rd_sock_strerror(int e) { return strerror(e); }

int rd_sock_accept(int lfd, struct sockaddr *sa, socklen_t *len)
{
#if defined(SOCK_NONBLOCK) && !defined(__APPLE__)
    return accept4(lfd, sa, len, SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
    int fd = accept(lfd, sa, len);
    if (fd >= 0) {
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        rd_sock_nonblock(fd);
    }
    return fd;
#endif
}

int rd_thread_start(rd_thread_t *t, void *(*fn)(void *), void *arg) { return pthread_create(t, NULL, fn, arg) ? -1 : 0; }
void rd_thread_join(rd_thread_t t) { pthread_join(t, NULL); }

int rd_thread_detached(void *(*fn)(void *), void *arg)
{
    pthread_t t;
    if (pthread_create(&t, NULL, fn, arg))
        return -1;
    pthread_detach(t);
    return 0;
}
void rd_mutex_init(rd_mutex_t *m) { pthread_mutex_init(m, NULL); }
void rd_mutex_lock(rd_mutex_t *m) { pthread_mutex_lock(m); }
void rd_mutex_unlock(rd_mutex_t *m) { pthread_mutex_unlock(m); }
void rd_cond_init(rd_cond_t *c) { pthread_cond_init(c, NULL); }
void rd_cond_wait(rd_cond_t *c, rd_mutex_t *m) { pthread_cond_wait(c, m); }
void rd_cond_signal(rd_cond_t *c) { pthread_cond_signal(c); }

void rd_sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

const char *rd_machine(void)
{
    static struct utsname u;
    if (!u.machine[0])
        uname(&u);
    return !strcmp(u.machine, "arm64") ? "aarch64" : u.machine; /* macOS says arm64 */
}

void rd_hostname(char *out, size_t cap)
{
    if (gethostname(out, cap) < 0 || !out[0])
        snprintf(out, cap, "computer");
    out[cap - 1] = 0;
    char *dot = strchr(out, '.'); /* macOS: "name.local" */
    if (dot && dot != out)
        *dot = 0;
}

const char *rd_home_dir(void)
{
    static char home[512];
    if (!home[0]) {
        const char *h = getenv("HOME");
        if (!h || !*h) {
            struct passwd *pw = getpwuid(getuid());
            h = pw ? pw->pw_dir : "/tmp";
        }
        snprintf(home, sizeof home, "%s", h);
    }
    return home;
}

#endif
