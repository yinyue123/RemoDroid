/* Portability layer for the code shared by Android, Linux, macOS and Windows
 * (rtc/, common/, desktop/): sockets, threads, time and paths. On POSIX it is
 * a thin set of names; on Windows it maps them to Winsock and Win32. Sockets
 * are plain ints everywhere (a Winsock SOCKET fits; libev is built with
 * EV_FD_TO_WIN32_HANDLE as the identity, see desktop/desktop.mk). */
#ifndef RD_PLAT_H
#define RD_PLAT_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#ifndef FD_SETSIZE
#define FD_SETSIZE 1024
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <sys/types.h>
#define RD_PATH_SEP '\\'
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0 /* sockets are non-blocking already */
#endif
#ifdef gai_strerror /* UNICODE picks gai_strerrorW; messages here are char */
#undef gai_strerror
#define gai_strerror gai_strerrorA
#endif
#include <time.h>
#define strtok_r strtok_s
#ifndef strcasecmp
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#endif
static inline struct tm *rd_gmtime_r(const time_t *t, struct tm *tm) { return gmtime_s(tm, t) ? NULL : tm; }
#define gmtime_r rd_gmtime_r
typedef SRWLOCK rd_mutex_t;
typedef CONDITION_VARIABLE rd_cond_t;
typedef HANDLE rd_thread_t;
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#define RD_PATH_SEP '/'
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0 /* macOS: SO_NOSIGPIPE is set on each socket instead */
#endif
typedef pthread_mutex_t rd_mutex_t;
typedef pthread_cond_t rd_cond_t;
typedef pthread_t rd_thread_t;
#endif

#if defined(_WIN32)
#define RD_EXPORT __declspec(dllexport)
#else
#define RD_EXPORT __attribute__((visibility("default")))
#endif

/* ---- sockets ---- */
int         rd_net_init(void);                   /* WSAStartup once; no-op on POSIX */
int         rd_socket(int af, int type, int proto); /* non-blocking, close-on-exec; -1 on error */
int         rd_sock_close(int fd);
int         rd_sock_nonblock(int fd);
int         rd_sock_error(void);                  /* errno / WSAGetLastError() */
int         rd_sock_again(int err);               /* would block / interrupted / in progress */
const char *rd_sock_strerror(int err);
/* Accepts a connection on a listening socket, non-blocking; -1 on error. */
int         rd_sock_accept(int lfd);

/* ---- threads ---- */
int  rd_thread_start(rd_thread_t *t, void *(*fn)(void *), void *arg);
void rd_thread_join(rd_thread_t t);
void rd_mutex_init(rd_mutex_t *m);
void rd_mutex_lock(rd_mutex_t *m);
void rd_mutex_unlock(rd_mutex_t *m);
void rd_cond_init(rd_cond_t *c);
void rd_cond_wait(rd_cond_t *c, rd_mutex_t *m);
void rd_cond_signal(rd_cond_t *c);
void rd_sleep_ms(int ms);

/* ---- system information ---- */
/* "x86_64", "aarch64", … */
const char *rd_machine(void);
/* This computer's name (gethostname / GetComputerName), never empty. */
void        rd_hostname(char *out, size_t cap);
/* The user's home folder, without a trailing separator. */
const char *rd_home_dir(void);

#endif
