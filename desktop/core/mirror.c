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
 * A thread reads the socket, so megabytes of a page never hold up the session's loop, and hands each frame
 * to the loop. The loop sends it piece by piece, and a piece the channel has no room for (rtc_dc_send fails
 * when the SCTP send buffer is full) is sent again a moment later, never dropped: the file channel works the
 * same way (files.c). The thread waits while more than BEHIND bytes are queued. */
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
#define CHUNK 16000
#define BEHIND (512u << 10)
#define FRAME_MAX (96u << 20)

typedef struct out {
    struct out *next;
    size_t n, off; /* off: how much of it has gone */
    uint8_t kind, d[];
} out_t;

typedef struct {
    int fd;
    volatile int stop;
    size_t queued;           /* bytes handed to the loop and not yet sent: the thread waits above BEHIND */
    rd_thread_t th;
} mirror_t;

static mirror_t *M; /* loop thread only */
static out_t *q_head, *q_tail; /* loop thread only: frames to send on "dom", oldest first */
static ev_timer q_timer;
static int q_timer_inited;

int mirror_available(void) { return *desk_conf("mirror", "socket", "") != 0; }
int mirror_is_on(void) { return M != NULL; }

static void q_clear(void)
{
    while (q_head) {
        out_t *o = q_head;
        q_head = o->next;
        free(o);
    }
    q_tail = NULL;
    if (q_timer_inited && ev_is_active(&q_timer))
        ev_timer_stop(D.loop, &q_timer);
}

/* as much of the queue as the channel takes now; the rest from the timer */
static void q_pump(void)
{
    rtc_t *r = rd_rtc_stack();
    static uint8_t msg[CHUNK + 2];
    while (M && r && q_head) {
        out_t *o = q_head;
        size_t c = o->n - o->off < CHUNK ? o->n - o->off : CHUNK;
        msg[0] = o->kind;
        msg[1] = o->off + c < o->n;
        memcpy(msg + 2, o->d + o->off, c);
        int rc = rtc_dc_buffered(r) > BEHIND ? -2 : rtc_dc_send(r, "dom", 1, msg, c + 2);
        if (rc < 0)
            break; /* full: again in a moment */
        o->off += c;
        __atomic_sub_fetch(&M->queued, c, __ATOMIC_RELAXED);
        if (o->off == o->n) {
            q_head = o->next;
            if (!q_head)
                q_tail = NULL;
            free(o);
        }
    }
    if (q_head && !ev_is_active(&q_timer))
        ev_timer_start(D.loop, &q_timer);
    else if (!q_head && ev_is_active(&q_timer))
        ev_timer_stop(D.loop, &q_timer);
}

static void on_q_timer(struct ev_loop *loop, ev_timer *w, int revents) { q_pump(); }

static void on_frame(void *arg)
{
    out_t *o = arg;
    if (!M) { /* Page mode ended meanwhile */
        free(o);
        return;
    }
    o->next = NULL;
    if (q_tail)
        q_tail->next = o;
    else
        q_head = o;
    q_tail = o;
    q_pump();
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
    uint8_t *body = NULL;
    size_t cap = 0;
    while (!m->stop) {
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
        LOGD("mirror: frame '%c', %zu bytes (queued %zu)", h[0], n, (size_t)__atomic_load_n(&m->queued, __ATOMIC_RELAXED));
        if (h[0] == 'T') {
            uint8_t *t = n && n < 64 ? malloc(n + 1) : NULL;
            if (t) {
                t[0] = (uint8_t)n;
                memcpy(t + 1, body, n);
                rd_rtc_call(on_touch, t);
            }
            continue;
        }
        if ((h[0] != 'J' && h[0] != 'R') || !n)
            continue;
        out_t *o = malloc(sizeof *o + n);
        if (!o)
            break;
        o->n = n;
        o->off = 0;
        o->kind = h[0];
        memcpy(o->d, body, n);
        __atomic_add_fetch(&m->queued, n, __ATOMIC_RELAXED);
        rd_rtc_call(on_frame, o);
        /* a slow link: the page waits here (and in rd-mirror), not in the agent's memory */
        while (__atomic_load_n(&m->queued, __ATOMIC_RELAXED) > BEHIND && !m->stop)
            usleep(5000);
    }
    free(body);
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
    if (!q_timer_inited) {
        ev_timer_init(&q_timer, on_q_timer, 0.005, 0.005);
        q_timer_inited = 1;
    }
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
    q_clear();
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
