/* WebSocket client to the AWS control server (plan §4.5, Appendix A.5):
 * Basic auth on first connect, then the session cookie; ping/pong keepalive;
 * reconnect with exponential backoff and jitter. */
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "agent.h"
#include "msg.h"
#include "picohttpparser.h"
#include "ws.h"

enum { W_IDLE, W_CONNECTING, W_HANDSHAKE, W_OPEN };

static struct {
    int state, fd;
    ev_io rio, wio;
    ev_timer timer;     /* connect timeout / reconnect backoff */
    ev_timer ping;      /* ping + pong timeout + heartbeat */
    ev_tstamp last_rx, last_status;
    rd_buf_t in, out;
    ws_asm_t wsa;
    char key[25];
    char cookie[128];   /* "rd_session=…" */
    char host[128], port[16], path[128];
    double backoff;
    int sent_basic;
    char *pending[32];  /* events raised while disconnected, sent after hello */
    int npending;
} W;

static void schedule_reconnect(void);
static void ws_close_conn(const char *why);

int wsc_online(void) { return W.state == W_OPEN; }

static void flush(void)
{
    while (rd_buf_avail(&W.out)) {
        ssize_t n = write(W.fd, rd_buf_data(&W.out), rd_buf_avail(&W.out));
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR)
                break;
            ws_close_conn(strerror(errno));
            return;
        }
        rd_buf_consume(&W.out, (size_t)n);
    }
    if (rd_buf_avail(&W.out))
        ev_io_start(A.loop, &W.wio);
    else
        ev_io_stop(A.loop, &W.wio);
}

static void send_frame(int op, const void *data, size_t n)
{
    if (W.state != W_OPEN)
        return;
    ws_build(&W.out, op, data, n, 1);
    flush();
}

void wsc_send(cJSON *m)
{
    char *s = msg_print(m);
    if (W.state == W_OPEN) {
        LOGT("ws -> %s", s);
        send_frame(WS_OP_TEXT, s, strlen(s));
        free(s);
    } else if (!strcmp(msg_type(m), "event")) {
        if (W.npending == (int)RD_ARRAY_LEN(W.pending)) { /* keep the newest */
            free(W.pending[0]);
            memmove(W.pending, W.pending + 1, sizeof W.pending[0] * (RD_ARRAY_LEN(W.pending) - 1));
            W.npending--;
        }
        W.pending[W.npending++] = s;
    } else {
        free(s);
    }
    cJSON_Delete(m);
}

static void send_hello(void)
{
    struct utsname u;
    uname(&u);
    cJSON *m = msg_new("hello", 0, NULL);
    cJSON *d = msg_d(m);
    cJSON_AddNumberToObject(d, "proto", RD_PROTO);
    cJSON_AddStringToObject(d, "version", RD_VERSION);
    cJSON_AddStringToObject(d, "arch", u.machine);
    cJSON_AddItemToObject(d, "phone", phone_status_json());
    wsc_send(m);
    cJSON *st = msg_new("status", 0, NULL);
    cJSON_ReplaceItemInObjectCaseSensitive(st, "d", agent_status_json());
    wsc_send(st);
    W.last_status = ev_now(A.loop);
    for (int i = 0; i < W.npending; i++) {
        send_frame(WS_OP_TEXT, W.pending[i], strlen(W.pending[i]));
        free(W.pending[i]);
    }
    W.npending = 0;
}

static void ws_close_conn(const char *why)
{
    if (W.state == W_IDLE)
        return;
    if (W.state == W_OPEN)
        LOGW("server connection lost: %s", why);
    else
        LOGD("server connect failed: %s", why);
    ev_io_stop(A.loop, &W.rio);
    ev_io_stop(A.loop, &W.wio);
    ev_timer_stop(A.loop, &W.ping);
    close(W.fd);
    W.fd = -1;
    W.state = W_IDLE;
    rd_buf_reset(&W.in);
    rd_buf_reset(&W.out);
    ws_asm_free(&W.wsa);
    schedule_reconnect();
}

/* ---------------- handshake ---------------- */

static void send_upgrade(void)
{
    ws_client_key(W.key);
    char auth[512] = "";
    if (W.cookie[0]) {
        snprintf(auth, sizeof auth, "Cookie: %s\r\n", W.cookie);
        W.sent_basic = 0;
    } else {
        char cred[256], b64[400];
        int n = snprintf(cred, sizeof cred, "%s:%s", A.device_id, A.device_secret);
        rd_b64_encode(cred, (size_t)n, b64, sizeof b64);
        snprintf(auth, sizeof auth, "Authorization: Basic %s\r\n", b64);
        W.sent_basic = 1;
    }
    rd_buf_printf(&W.out,
                  "GET %s HTTP/1.1\r\nHost: %s%s%s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                  "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nUser-Agent: remodroid-agent/%s\r\n%s\r\n",
                  W.path, W.host, strcmp(W.port, "80") ? ":" : "", strcmp(W.port, "80") ? W.port : "", W.key,
                  RD_VERSION, auth);
    flush();
}

static int handle_handshake(void)
{
    int minor, status;
    const char *msg;
    size_t msg_len, nh = 20;
    struct phr_header hs[20];
    int n = phr_parse_response((const char *)rd_buf_data(&W.in), rd_buf_avail(&W.in), &minor, &status, &msg, &msg_len,
                               hs, &nh, 0);
    if (n == -2)
        return 0;
    if (n < 0) {
        ws_close_conn("bad HTTP response");
        return -1;
    }
    char accept[29] = "", expect[29];
    for (size_t i = 0; i < nh; i++) {
        if (hs[i].name_len == 10 && !strncasecmp(hs[i].name, "Set-Cookie", 10)) {
            const char *semi = memchr(hs[i].value, ';', hs[i].value_len);
            size_t l = semi ? (size_t)(semi - hs[i].value) : hs[i].value_len;
            if (l < sizeof W.cookie) {
                memcpy(W.cookie, hs[i].value, l);
                W.cookie[l] = 0;
            }
        } else if (hs[i].name_len == 20 && !strncasecmp(hs[i].name, "Sec-WebSocket-Accept", 20) && hs[i].value_len < 29) {
            memcpy(accept, hs[i].value, hs[i].value_len);
            accept[hs[i].value_len] = 0;
        }
    }
    if (status == 401) {
        if (!W.sent_basic) {
            LOGI("server session expired; logging in again");
            W.cookie[0] = 0;
            W.backoff = 0; /* retry right away with Basic auth */
        } else {
            LOGE("server rejected device '%s' (check device_id/device_secret in agent.ini)", A.device_id);
        }
        ws_close_conn("unauthorized");
        return -1;
    }
    if (status != 101) {
        char why[64];
        snprintf(why, sizeof why, "HTTP %d", status);
        if (status == 429)
            W.backoff = A.reconnect_max_s;
        ws_close_conn(why);
        return -1;
    }
    ws_accept_key(W.key, expect);
    if (strcmp(accept, expect)) {
        ws_close_conn("bad Sec-WebSocket-Accept");
        return -1;
    }
    rd_buf_consume(&W.in, (size_t)n);
    W.state = W_OPEN;
    W.backoff = 0;
    W.last_rx = ev_now(A.loop);
    LOGI("connected to the server at %s:%s", W.host, W.port);
    ev_timer_set(&W.ping, A.ping_s, A.ping_s);
    ev_timer_start(A.loop, &W.ping);
    send_hello();
    return 0;
}

/* ---------------- messages ---------------- */

static void handle_message(cJSON *m)
{
    const char *t = msg_type(m);
    long id = msg_id(m);
    if (!strcmp(t, "welcome")) {
        cJSON *d = msg_d(m);
        int p = (int)msg_num(d, "ping_s", A.ping_s), to = (int)msg_num(d, "timeout_s", A.timeout_s);
        int hb = (int)msg_num(d, "heartbeat_s", A.heartbeat_s);
        if (p > 0)
            A.ping_s = p;
        if (to > 0)
            A.timeout_s = to;
        if (hb > 0)
            A.heartbeat_s = hb;
    } else if (!strcmp(t, "cmd")) {
        cJSON *d = msg_d(m);
        const char *cmd = msg_str(d, "cmd", "");
        char err[200] = "";
        LOGI("server command: %s", cmd);
        int r = phone_run_cmd(cmd, cJSON_GetObjectItemCaseSensitive(d, "args"), err, sizeof err);
        if (r == 0) {
            cJSON *res = cJSON_CreateObject();
            cJSON_AddStringToObject(res, "cmd", cmd);
            wsc_send(msg_ack(id, res));
        } else if (r > 0) {
            /* the phone answers later (forwarded with the same id) */
        } else {
            wsc_send(msg_error(id, err[0] ? "offline" : "unsupported", err[0] ? err : cmd));
        }
    } else if (!strcmp(t, "diag")) {
        int lines = (int)msg_num(msg_d(m), "lines", 200);
        cJSON *res = cJSON_CreateObject();
        cJSON_AddItemToObject(res, "agent_log", agent_log_tail(lines));
        cJSON_AddItemToObject(res, "server_stderr", phone_server_log_tail(lines));
        wsc_send(msg_ack(id, res));
    } else if (!strncmp(t, "session.", 8) || !strcmp(t, "signal")) {
        phone_on_server_msg(m);
    } else if (!strcmp(t, "error")) {
        LOGW("server error: %s", msg_str(msg_d(m), "msg", "?"));
    } else if (strcmp(t, "ack")) {
        LOGD("ignoring server message %s", t);
    }
}

static void process_frames(void)
{
    while (W.state == W_OPEN && rd_buf_avail(&W.in)) {
        ws_frame_t f;
        int n = ws_parse(rd_buf_data(&W.in), rd_buf_avail(&W.in), 0, &f);
        if (n == 0)
            return;
        if (n < 0) {
            ws_close_conn("bad frame");
            return;
        }
        int op;
        const uint8_t *msg;
        size_t ml;
        int ok = ws_asm_feed(&W.wsa, &f, &op, &msg, &ml);
        if (ok < 0) {
            ws_close_conn("bad fragment");
            return;
        }
        W.last_rx = ev_now(A.loop);
        if (ok == 1) {
            if (op == WS_OP_PING) {
                send_frame(WS_OP_PONG, msg, ml);
            } else if (op == WS_OP_CLOSE) {
                ws_close_conn("closed by server");
                return;
            } else if (op == WS_OP_TEXT) {
                LOGT("ws <- %.*s", (int)RD_MIN(ml, 400), msg);
                cJSON *j = cJSON_ParseWithLength((const char *)msg, ml);
                if (cJSON_IsObject(j))
                    handle_message(j);
                cJSON_Delete(j);
            }
        }
        if (W.state != W_OPEN)
            return;
        rd_buf_consume(&W.in, (size_t)n);
    }
}

/* ---------------- I/O ---------------- */

static void on_read(struct ev_loop *loop, ev_io *w, int revents)
{
    rd_buf_reserve(&W.in, 16384);
    ssize_t n = read(W.fd, W.in.p + W.in.len, W.in.cap - W.in.len);
    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            return;
        ws_close_conn(n == 0 ? "connection closed" : strerror(errno));
        return;
    }
    W.in.len += (size_t)n;
    if (W.state == W_HANDSHAKE && handle_handshake() < 0)
        return;
    if (W.state == W_OPEN)
        process_frames();
}

static void on_write(struct ev_loop *loop, ev_io *w, int revents)
{
    if (W.state == W_CONNECTING) {
        int err = 0;
        socklen_t el = sizeof err;
        getsockopt(W.fd, SOL_SOCKET, SO_ERROR, &err, &el);
        if (err) {
            ws_close_conn(strerror(err));
            return;
        }
        ev_timer_stop(loop, &W.timer);
        W.state = W_HANDSHAKE;
        ev_io_start(loop, &W.rio);
        send_upgrade();
        return;
    }
    flush();
}

static void on_ping(struct ev_loop *loop, ev_timer *w, int revents)
{
    ev_tstamp now = ev_now(loop);
    if (now - W.last_rx > A.timeout_s) {
        ws_close_conn("no reply from the server");
        return;
    }
    send_frame(WS_OP_PING, "rd", 2);
    if (now - W.last_status >= A.heartbeat_s - 0.5) {
        cJSON *m = msg_new("status", 0, NULL);
        cJSON_ReplaceItemInObjectCaseSensitive(m, "d", agent_status_json());
        wsc_send(m);
        W.last_status = now;
    }
    ev_timer_set(w, RD_MIN(A.ping_s, A.heartbeat_s), RD_MIN(A.ping_s, A.heartbeat_s));
    ev_timer_again(loop, w);
}

static void connect_now(void)
{
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *ai;
    int e = getaddrinfo(W.host, W.port, &hints, &ai);
    if (e) {
        LOGW("server %s: %s", W.host, gai_strerror(e));
        W.state = W_CONNECTING; /* so close logic schedules a retry */
        W.fd = -1;
        ws_close_conn("dns");
        return;
    }
    W.fd = socket(ai->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int one = 1;
    setsockopt(W.fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(W.fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
    W.state = W_CONNECTING;
    if (W.fd < 0 || (connect(W.fd, ai->ai_addr, ai->ai_addrlen) < 0 && errno != EINPROGRESS)) {
        freeaddrinfo(ai);
        ws_close_conn(strerror(errno));
        return;
    }
    freeaddrinfo(ai);
    ev_io_init(&W.rio, on_read, W.fd, EV_READ);
    ev_io_init(&W.wio, on_write, W.fd, EV_WRITE);
    ev_io_start(A.loop, &W.wio);
    ev_timer_stop(A.loop, &W.timer);
    ev_timer_set(&W.timer, 10., 0.);
    ev_timer_start(A.loop, &W.timer);
}

static void on_timer(struct ev_loop *loop, ev_timer *w, int revents)
{
    if (W.state == W_IDLE)
        connect_now();
    else if (W.state != W_OPEN)
        ws_close_conn("connect timeout");
}

static void schedule_reconnect(void)
{
    double base = W.backoff ? RD_MIN(W.backoff * 2, A.reconnect_max_s) : 1.;
    W.backoff = base;
    uint8_t r;
    rd_random(&r, 1);
    double delay = base * (0.75 + (r / 255.) * 0.5);
    ev_timer_stop(A.loop, &W.timer);
    ev_timer_set(&W.timer, delay, 0.);
    ev_timer_start(A.loop, &W.timer);
}

static int parse_url(void)
{
    const char *u = A.url;
    if (strncmp(u, "ws://", 5)) {
        LOGE("[server] url must start with ws:// (got %s)", u);
        return -1;
    }
    u += 5;
    const char *slash = strchr(u, '/');
    char hp[160];
    size_t hl = slash ? (size_t)(slash - u) : strlen(u);
    if (hl >= sizeof hp)
        return -1;
    memcpy(hp, u, hl);
    hp[hl] = 0;
    snprintf(W.path, sizeof W.path, "%s", slash ? slash : "/ws/agent");
    return rd_split_hostport(hp, W.host, sizeof W.host, W.port, sizeof W.port, "80");
}

void wsc_start(void)
{
    W.fd = -1;
    ev_timer_init(&W.timer, on_timer, 0., 0.);
    ev_timer_init(&W.ping, on_ping, A.ping_s, A.ping_s);
    if (parse_url() < 0) {
        LOGE("server connection disabled: bad url");
        return;
    }
    ev_timer_start(A.loop, &W.timer);
}

void wsc_restart(void)
{
    if (W.state != W_IDLE)
        ws_close_conn("restart");
    if (parse_url() == 0) {
        W.backoff = 0;
        ev_timer_stop(A.loop, &W.timer);
        ev_timer_set(&W.timer, 0.1, 0.);
        ev_timer_start(A.loop, &W.timer);
    }
}
