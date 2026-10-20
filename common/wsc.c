#include "wsc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define strncasecmp _strnicmp
#else
#include <netinet/tcp.h>
#include <strings.h>
#endif

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include "cas.h"
#include "msg.h"
#include "picohttpparser.h"
#include "plat.h"
#include "rd.h"
#include "ws.h"

enum { W_IDLE, W_CONNECTING, W_TLS, W_HANDSHAKE, W_OPEN };

static struct {
    struct ev_loop *loop;
    wsc_cb_t cb;
    void *ud;
    wsc_conf_t c;
    int inited, started, stopped;
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
    char problem[200];
    /* wss:// */
    int tls, ssl_on;
    size_t tls_wlen;    /* length of a TLS write to repeat after WANT_WRITE */
    unsigned attempt;   /* picks the server address to try */
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
} W;

static struct {
    int ready;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509_crt ca;
    char ca_from[256];
} T;

static void schedule_reconnect(void);
static void ws_close_conn(const char *why);

int wsc_online(void) { return W.state == W_OPEN; }
const char *wsc_problem(void) { return W.problem; }

static void set_problem(const char *p)
{
    snprintf(W.problem, sizeof W.problem, "%s", p ? p : "");
    if (W.cb.on_state)
        W.cb.on_state(W.ud, W.state == W_OPEN, p);
}

/* ---------------- transport: plain or TLS ---------------- */

#define IO_AGAIN (-2)

static const char *tls_err(int r)
{
    static char buf[160];
    mbedtls_strerror(r, buf, sizeof buf);
    return buf;
}

static int bio_send(void *ctx, const unsigned char *b, size_t n)
{
    ssize_t r = send(W.fd, (const char *)b, n, MSG_NOSIGNAL);
    if (r < 0)
        return rd_sock_again(rd_sock_error()) ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
    return (int)r;
}

static int bio_recv(void *ctx, unsigned char *b, size_t n)
{
    ssize_t r = recv(W.fd, (char *)b, n, 0);
    if (r < 0)
        return rd_sock_again(rd_sock_error()) ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
    return (int)r; /* 0 = EOF */
}

/* Trusted roots: [server] ca_file, else the system's. */
static int tls_global(void)
{
    if (T.ready)
        return 0;
    mbedtls_entropy_init(&T.entropy);
    mbedtls_ctr_drbg_init(&T.drbg);
    mbedtls_x509_crt_init(&T.ca);
    if (mbedtls_ctr_drbg_seed(&T.drbg, mbedtls_entropy_func, &T.entropy, (const unsigned char *)"remodroid-wss", 13))
        return -1;
    char err[256];
    if (rd_load_cas(&T.ca, W.c.ca_file, T.ca_from, sizeof T.ca_from, err, sizeof err) < 0) {
        LOGE("wss: %s", err);
        mbedtls_x509_crt_free(&T.ca);
        mbedtls_x509_crt_init(&T.ca);
        return -1;
    }
    LOGD("wss: trusting the CAs in %s", T.ca_from);
    T.ready = 1;
    return 0;
}

static void tls_free(void)
{
    if (!W.ssl_on)
        return;
    mbedtls_ssl_free(&W.ssl);
    mbedtls_ssl_config_free(&W.conf);
    W.ssl_on = 0;
    W.tls_wlen = 0;
}

static ssize_t io_read(void *buf, size_t n)
{
    if (!W.tls) {
        ssize_t r = recv(W.fd, (char *)buf, n, 0);
        return r < 0 && rd_sock_again(rd_sock_error()) ? IO_AGAIN : r;
    }
    int r = mbedtls_ssl_read(&W.ssl, buf, n);
    if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
        return IO_AGAIN;
    if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
        return 0;
    return r < 0 ? -1 : r;
}

static ssize_t io_write(const void *buf, size_t n)
{
    if (!W.tls) {
        ssize_t r = send(W.fd, (const char *)buf, n, MSG_NOSIGNAL);
        return r < 0 && rd_sock_again(rd_sock_error()) ? IO_AGAIN : r;
    }
    /* after WANT_WRITE mbedTLS must be called again with the same length */
    size_t len = W.tls_wlen ? W.tls_wlen : RD_MIN(n, (size_t)16384);
    int r = mbedtls_ssl_write(&W.ssl, buf, len);
    if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
        W.tls_wlen = len;
        return IO_AGAIN;
    }
    W.tls_wlen = 0;
    return r < 0 ? -1 : r;
}

static void send_upgrade(void);

static void tls_step(void)
{
    int r = mbedtls_ssl_handshake(&W.ssl);
    if (r == MBEDTLS_ERR_SSL_WANT_READ) {
        ev_io_stop(W.loop, &W.wio);
        return;
    }
    if (r == MBEDTLS_ERR_SSL_WANT_WRITE) {
        ev_io_start(W.loop, &W.wio);
        return;
    }
    if (r) {
        uint32_t flags = mbedtls_ssl_get_verify_result(&W.ssl);
        char why[300];
        if (flags && flags != (uint32_t)-1) {
            char info[200];
            mbedtls_x509_crt_verify_info(info, sizeof info, "", flags);
            info[strcspn(info, "\n")] = 0;
            snprintf(why, sizeof why, "certificate of %s not trusted: %s", W.host, info);
            LOGW("wss: %s (CAs from %s; is the clock right?)", why, T.ca_from);
        } else {
            snprintf(why, sizeof why, "TLS handshake: %s", tls_err(r));
        }
        ws_close_conn(why);
        return;
    }
    LOGD("wss: %s, %s", mbedtls_ssl_get_version(&W.ssl), mbedtls_ssl_get_ciphersuite(&W.ssl));
    W.state = W_HANDSHAKE;
    ev_io_stop(W.loop, &W.wio);
    send_upgrade();
}

static void tls_start(void)
{
    if (tls_global() < 0) {
        ws_close_conn("no trusted CAs");
        return;
    }
    mbedtls_ssl_init(&W.ssl);
    mbedtls_ssl_config_init(&W.conf);
    W.ssl_on = 1;
    int r = mbedtls_ssl_config_defaults(&W.conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT);
    if (!r) {
        mbedtls_ssl_conf_authmode(&W.conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&W.conf, &T.ca, NULL);
        mbedtls_ssl_conf_rng(&W.conf, mbedtls_ctr_drbg_random, &T.drbg);
        r = mbedtls_ssl_setup(&W.ssl, &W.conf);
    }
    if (!r)
        r = mbedtls_ssl_set_hostname(&W.ssl, W.host); /* SNI + name check */
    if (r) {
        ws_close_conn(tls_err(r));
        return;
    }
    mbedtls_ssl_set_bio(&W.ssl, NULL, bio_send, bio_recv, NULL);
    W.state = W_TLS;
    tls_step();
}

static void flush(void)
{
    while (rd_buf_avail(&W.out)) {
        ssize_t n = io_write(rd_buf_data(&W.out), rd_buf_avail(&W.out));
        if (n == IO_AGAIN)
            break;
        if (n <= 0) {
            ws_close_conn(n == 0 ? "connection closed" : W.tls ? "TLS write failed" : rd_sock_strerror(rd_sock_error()));
            return;
        }
        rd_buf_consume(&W.out, (size_t)n);
    }
    if (rd_buf_avail(&W.out))
        ev_io_start(W.loop, &W.wio);
    else
        ev_io_stop(W.loop, &W.wio);
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

void wsc_send_binary(const void *data, size_t n) { send_frame(WS_OP_BIN, data, n); }

static void send_status(void)
{
    if (!W.cb.status)
        return;
    cJSON *m = msg_new("status", 0, NULL);
    cJSON_ReplaceItemInObjectCaseSensitive(m, "d", W.cb.status(W.ud));
    wsc_send(m);
    W.last_status = ev_now(W.loop);
}

static void on_opened(void)
{
    if (W.cb.on_open)
        W.cb.on_open(W.ud); /* hello */
    send_status();
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
    int was_open = W.state == W_OPEN;
    if (was_open)
        LOGW("server connection lost: %s", why);
    else
        LOGD("server connect failed: %s", why);
    ev_io_stop(W.loop, &W.rio);
    ev_io_stop(W.loop, &W.wio);
    ev_timer_stop(W.loop, &W.ping);
    tls_free();
    if (W.fd >= 0)
        rd_sock_close(W.fd);
    W.fd = -1;
    W.state = W_IDLE;
    rd_buf_reset(&W.in);
    rd_buf_reset(&W.out);
    ws_asm_free(&W.wsa);
    if (strcmp(why, "restart") && strcmp(why, "stopped"))
        set_problem(why);
    else if (W.cb.on_state)
        W.cb.on_state(W.ud, 0, NULL);
    if (was_open && W.cb.on_lost)
        W.cb.on_lost(W.ud);
    if (!W.stopped)
        schedule_reconnect();
}

/* ---------------- handshake ---------------- */

static void send_upgrade(void)
{
    const char *defport = W.tls ? "443" : "80";
    ws_client_key(W.key);
    char auth[512] = "";
    if (W.cookie[0]) {
        snprintf(auth, sizeof auth, "Cookie: %s\r\n", W.cookie);
        W.sent_basic = 0;
    } else {
        char cred[256], b64[400];
        int n = snprintf(cred, sizeof cred, "%s:%s", W.c.device_id, W.c.device_secret);
        rd_b64_encode(cred, (size_t)n, b64, sizeof b64);
        snprintf(auth, sizeof auth, "Authorization: Basic %s\r\n", b64);
        W.sent_basic = 1;
    }
    rd_buf_printf(&W.out,
                  "GET %s HTTP/1.1\r\nHost: %s%s%s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                  "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nUser-Agent: %s\r\n%s\r\n",
                  W.path, W.host, strcmp(W.port, defport) ? ":" : "", strcmp(W.port, defport) ? W.port : "", W.key,
                  W.c.user_agent[0] ? W.c.user_agent : "remodroid/" RD_VERSION, auth);
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
            ws_close_conn("session expired");
        } else {
            LOGE("server rejected device '%s' (check device_id/device_secret)", W.c.device_id);
            ws_close_conn("the server rejected this device (unpaired on the panel?)");
        }
        return -1;
    }
    if (status != 101) {
        char why[64];
        snprintf(why, sizeof why, "HTTP %d", status);
        if (status == 429)
            W.backoff = W.c.reconnect_max_s;
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
    W.attempt--; /* stay on the address that worked */
    ev_timer_stop(W.loop, &W.timer); /* connect timeout covers TCP, TLS and the upgrade */
    W.backoff = 0;
    W.last_rx = ev_now(W.loop);
    LOGI("connected to the server at %s://%s:%s", W.tls ? "wss" : "ws", W.host, W.port);
    ev_timer_set(&W.ping, W.c.ping_s, W.c.ping_s);
    ev_timer_start(W.loop, &W.ping);
    set_problem(NULL);
    on_opened();
    return 0;
}

/* ---------------- messages ---------------- */

static void handle_message(cJSON *m)
{
    if (!strcmp(msg_type(m), "welcome")) {
        cJSON *d = msg_d(m);
        int p = (int)msg_num(d, "ping_s", W.c.ping_s), to = (int)msg_num(d, "timeout_s", W.c.timeout_s);
        int hb = (int)msg_num(d, "heartbeat_s", W.c.heartbeat_s);
        if (p > 0)
            W.c.ping_s = p;
        if (to > 0)
            W.c.timeout_s = to;
        if (hb > 0)
            W.c.heartbeat_s = hb;
        return;
    }
    if (W.cb.on_message)
        W.cb.on_message(W.ud, m);
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
        W.last_rx = ev_now(W.loop);
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
    if (W.state == W_TLS) {
        tls_step();
        return;
    }
    for (;;) {
        rd_buf_reserve(&W.in, 16384);
        ssize_t n = io_read(W.in.p + W.in.len, W.in.cap - W.in.len);
        if (n == IO_AGAIN)
            break;
        if (n <= 0) {
            ws_close_conn(n == 0 ? "connection closed" : W.tls ? "TLS read failed" : rd_sock_strerror(rd_sock_error()));
            return;
        }
        W.in.len += (size_t)n;
        if (!W.tls)
            break; /* TLS: drain every decrypted record before waiting again */
    }
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
        getsockopt(W.fd, SOL_SOCKET, SO_ERROR, (char *)&err, &el);
        if (err) {
            ws_close_conn(rd_sock_strerror(err));
            return;
        }
        ev_io_start(loop, &W.rio);
        if (W.tls) {
            tls_start();
            return;
        }
        W.state = W_HANDSHAKE;
        send_upgrade();
        return;
    }
    if (W.state == W_TLS) {
        tls_step();
        return;
    }
    flush();
}

static void on_ping(struct ev_loop *loop, ev_timer *w, int revents)
{
    ev_tstamp now = ev_now(loop);
    if (now - W.last_rx > W.c.timeout_s) {
        ws_close_conn("no reply from the server");
        return;
    }
    send_frame(WS_OP_PING, "rd", 2);
    if (now - W.last_status >= W.c.heartbeat_s - 0.5)
        send_status();
    ev_timer_set(w, RD_MIN(W.c.ping_s, W.c.heartbeat_s), RD_MIN(W.c.ping_s, W.c.heartbeat_s));
    ev_timer_again(loop, w);
}

static void connect_now(void)
{
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM, .ai_flags = AI_ADDRCONFIG }, *res, *ai;
    rd_net_init();
    int e = getaddrinfo(W.host, W.port, &hints, &res);
    if (e) {
        char why[200];
        snprintf(why, sizeof why, "cannot resolve %s: %s", W.host, gai_strerror(e));
        LOGW("server %s", why);
        W.state = W_CONNECTING; /* so close logic schedules a retry */
        W.fd = -1;
        ws_close_conn(why);
        return;
    }
    /* each failed attempt moves to the next address (e.g. IPv4 when IPv6 has no route) */
    int n = 0;
    for (ai = res; ai; ai = ai->ai_next)
        n++;
    ai = res;
    for (unsigned k = W.attempt % (unsigned)n; k; k--)
        ai = ai->ai_next;
    W.attempt++;
    W.fd = rd_socket(ai->ai_family, SOCK_STREAM, 0);
    W.state = W_CONNECTING;
    if (W.fd < 0) {
        freeaddrinfo(res);
        ws_close_conn(rd_sock_strerror(rd_sock_error()));
        return;
    }
    int one = 1;
    setsockopt(W.fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    setsockopt(W.fd, SOL_SOCKET, SO_KEEPALIVE, (const char *)&one, sizeof one);
    if (connect(W.fd, ai->ai_addr, (socklen_t)ai->ai_addrlen) < 0 && !rd_sock_again(rd_sock_error())) {
        int err = rd_sock_error();
        freeaddrinfo(res);
        ws_close_conn(rd_sock_strerror(err));
        return;
    }
    freeaddrinfo(res);
    ev_io_init(&W.rio, on_read, W.fd, EV_READ);
    ev_io_init(&W.wio, on_write, W.fd, EV_WRITE);
    ev_io_start(W.loop, &W.wio);
    ev_timer_stop(W.loop, &W.timer);
    ev_timer_set(&W.timer, 10., 0.);
    ev_timer_start(W.loop, &W.timer);
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
    double base = W.backoff ? RD_MIN(W.backoff * 2, W.c.reconnect_max_s) : 1.;
    W.backoff = base;
    uint8_t r;
    rd_random(&r, 1);
    double delay = base * (0.75 + (r / 255.) * 0.5);
    ev_timer_stop(W.loop, &W.timer);
    ev_timer_set(&W.timer, delay, 0.);
    ev_timer_start(W.loop, &W.timer);
}

static int parse_url(void)
{
    const char *u = W.c.url;
    if (!strncmp(u, "ws://", 5)) {
        W.tls = 0;
        u += 5;
    } else if (!strncmp(u, "wss://", 6)) {
        W.tls = 1;
        u += 6;
    } else {
        LOGE("[server] url must start with ws:// or wss:// (got %s)", u);
        return -1;
    }
    const char *slash = strchr(u, '/');
    char hp[160];
    size_t hl = slash ? (size_t)(slash - u) : strlen(u);
    if (hl >= sizeof hp)
        return -1;
    memcpy(hp, u, hl);
    hp[hl] = 0;
    snprintf(W.path, sizeof W.path, "%s", slash ? slash : "/ws/agent");
    return rd_split_hostport(hp, W.host, sizeof W.host, W.port, sizeof W.port, W.tls ? "443" : "80");
}

void wsc_init(struct ev_loop *loop, wsc_cb_t cb, void *ud)
{
    W.loop = loop;
    W.cb = cb;
    W.ud = ud;
    W.fd = -1;
    ev_timer_init(&W.timer, on_timer, 0., 0.);
    ev_timer_init(&W.ping, on_ping, 20., 20.);
    W.inited = 1;
}

void wsc_restart(void)
{
    if (!W.inited)
        return;
    if (W.state != W_IDLE) {
        W.stopped = 1; /* no backoff timer from the close */
        ws_close_conn("restart");
    }
    W.stopped = 0;
    ev_timer_stop(W.loop, &W.timer);
    if (!W.c.url[0]) {
        W.started = 0;
        return;
    }
    if (parse_url() < 0) {
        set_problem("bad server URL");
        LOGE("server connection disabled: bad url");
        return;
    }
    W.started = 1;
    W.backoff = 0;
    ev_timer_set(&W.timer, 0.1, 0.);
    ev_timer_start(W.loop, &W.timer);
}

void wsc_configure(const wsc_conf_t *c)
{
    wsc_conf_t n = *c;
    if (n.ping_s < 1) n.ping_s = 20;
    if (n.timeout_s < 1) n.timeout_s = 60;
    if (n.heartbeat_s < 1) n.heartbeat_s = 30;
    if (n.reconnect_max_s < 1) n.reconnect_max_s = 30;
    int changed = strcmp(n.url, W.c.url) || strcmp(n.device_id, W.c.device_id) ||
                  strcmp(n.device_secret, W.c.device_secret) || strcmp(n.ca_file, W.c.ca_file);
    if (strcmp(n.ca_file, W.c.ca_file) && T.ready) { /* reload the roots on the next connect */
        mbedtls_x509_crt_free(&T.ca);
        mbedtls_x509_crt_init(&T.ca);
        mbedtls_ctr_drbg_free(&T.drbg);
        mbedtls_entropy_free(&T.entropy);
        T.ready = 0;
    }
    if (strcmp(n.device_id, W.c.device_id) || strcmp(n.url, W.c.url))
        W.cookie[0] = 0;
    W.c = n;
    if (changed || (!W.started && n.url[0] && !W.stopped))
        wsc_restart();
}

void wsc_stop(void)
{
    if (!W.inited)
        return;
    W.stopped = 1;
    ev_timer_stop(W.loop, &W.timer);
    ws_close_conn("stopped");
    W.started = 0;
}
