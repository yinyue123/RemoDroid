/* Page mode for the remote browser (browser/dom-mirror.md): the viewer gets the page's elements instead of
 * a picture of them. The work is rd-mirror's, next to the browser; the agent only carries its frames
 * between a Unix socket ([mirror] socket in desktop.ini) and the session's "dom" data channel, and plays
 * the touches rd-mirror asks for on the touch screen.
 *
 *   rd-mirror → here   a kind byte, a 4-byte big-endian length, the body
 *       'J' JSON, deflated     → "dom": 'J', a "more follows" byte, then up to CHUNK bytes of it
 *       'R' a resource         → "dom": the same, with 'R'
 *       'T' an input message   → carried out here (input.c), as if the viewer had sent it
 *   viewer → here      a text message on "dom" → frame 'v' to rd-mirror
 *
 * A thread reads the socket, so megabytes of a page never hold up the session's loop; it slows down while
 * the data channel has more than BEHIND bytes waiting. */
#include "desk.h"

#ifdef _WIN32

int mirror_available(void) { return 0; }
int mirror_on(void) { return -1; }
void mirror_off(void) {}
int mirror_is_on(void) { return 0; }
void mirror_message(const uint8_t *d, size_t n) {}

#else

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "rd_rtc.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0 /* macOS: the agent ignores SIGPIPE */
#endif
/* Under one packet: a "dom" message of 1538 bytes never reached the viewer (Chrome 157, 2026-11-10), while
 * those under about 1200 always did. Why the stack loses a message it has to split is not known yet. */
#define CHUNK 1000
#define BEHIND (512u << 10)
#define FRAME_MAX (96u << 20)

typedef struct {
    int fd;
    volatile int stop;
    volatile size_t buffered; /* the data channels' queue, as the loop last saw it */
    volatile int asked;
    rd_thread_t th;
} mirror_t;

static mirror_t *M; /* loop thread only */

int mirror_available(void) { return *desk_conf("mirror", "socket", "") != 0; }
int mirror_is_on(void) { return M != NULL; }

static void on_buffered(void *arg)
{
    mirror_t *m = arg;
    m->buffered = rtc_dc_buffered(rd_rtc_stack());
    m->asked = 0;
}

static void on_touch(void *arg)
{
    uint8_t *d = arg;
    if (M) /* still in Page mode */
        session_message("input", 1, d + 1, d[0]);
    free(d);
}

/* rd-mirror went, or sent nonsense: the viewer is told, and gets its picture back */
static void on_lost(void *arg)
{
    if (M == arg)
        session_mirror(0, "the browser's Page mode stopped");
}

static int read_all(mirror_t *m, uint8_t *d, size_t n)
{
    while (n && !m->stop) {
        ssize_t r = recv(m->fd, d, n, 0);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return -1;
        d += r;
        n -= (size_t)r;
    }
    return m->stop ? -1 : 0;
}

static void *reader(void *arg)
{
    mirror_t *m = arg;
    uint8_t *body = NULL, *msg = malloc(CHUNK + 2);
    size_t cap = 0;
    while (msg && !m->stop) {
        uint8_t h[5];
        if (read_all(m, h, 5) < 0)
            break;
        size_t n = (size_t)h[1] << 24 | (size_t)h[2] << 16 | (size_t)h[3] << 8 | h[4];
        if (n > FRAME_MAX)
            break;
        if (n > cap) {
            uint8_t *b = realloc(body, n);
            if (!b)
                break;
            body = b;
            cap = n;
        }
        if (read_all(m, body, n) < 0)
            break;
        LOGD("mirror: frame '%c', %zu bytes (channel queue %zu)", h[0], n, (size_t)m->buffered);
        if (h[0] == 'T') {
            uint8_t *t = n && n < 64 ? malloc(n + 1) : NULL;
            if (t) {
                t[0] = (uint8_t)n;
                memcpy(t + 1, body, n);
                rd_rtc_call(on_touch, t);
            }
            continue;
        }
        if (h[0] != 'J' && h[0] != 'R')
            continue;
        size_t off = 0;
        do {
            size_t c = n - off < CHUNK ? n - off : CHUNK;
            msg[0] = h[0];
            msg[1] = off + c < n;
            memcpy(msg + 2, body + off, c);
            rd_rtc_dc_send("dom", 1, msg, c + 2);
            off += c;
            m->buffered += c + 2;
            if (!m->asked) {
                m->asked = 1;
                rd_rtc_call(on_buffered, m);
            }
            while (m->buffered > BEHIND && !m->stop) { /* a slow link: the page waits here, not in the loop's memory */
                usleep(5000);
                if (!m->asked) {
                    m->asked = 1;
                    rd_rtc_call(on_buffered, m);
                }
            }
        } while (off < n && !m->stop);
    }
    free(body);
    free(msg);
    if (!m->stop)
        rd_rtc_call(on_lost, m);
    return NULL;
}

int mirror_on(void)
{
    if (M)
        return 0;
    const char *path = desk_conf("mirror", "socket", "");
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    if (!*path || strlen(path) >= sizeof a.sun_path)
        return -1;
    memcpy(a.sun_path, path, strlen(path));
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct timeval tv = { 2, 0 }; /* a send to an rd-mirror that stopped reading gives up */
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a)) {
        LOGW("mirror: %s: %s", path, strerror(errno));
        if (fd >= 0)
            close(fd);
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    mirror_t *m = calloc(1, sizeof *m);
    if (!m) {
        close(fd);
        return -1;
    }
    m->fd = fd;
    rd_rtc_dc_open("dom");
    if (rd_thread_start(&m->th, reader, m) < 0) {
        close(fd);
        free(m);
        return -1;
    }
    M = m;
    LOGI("mirror: Page mode on");
    return 0;
}

static void on_free(void *arg) { free(arg); } /* after every call the thread queued */

void mirror_off(void)
{
    mirror_t *m = M;
    if (!m)
        return;
    M = NULL;
    m->stop = 1;
    shutdown(m->fd, SHUT_RDWR); /* wakes the reader */
    pthread_join(m->th, NULL);
    close(m->fd);
    rd_rtc_call(on_free, m);
    LOGI("mirror: Page mode off");
}

void mirror_message(const uint8_t *d, size_t n)
{
    if (!M || n > (1u << 20))
        return;
    uint8_t h[5] = { 'v', (uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n };
    struct iovec v[2] = { { h, 5 }, { (void *)d, n } };
    struct msghdr mh = { .msg_iov = v, .msg_iovlen = 2 };
    if (sendmsg(M->fd, &mh, MSG_NOSIGNAL) != (ssize_t)(n + 5))
        session_mirror(0, "the browser's Page mode stopped");
}

#endif
