/* Local HTTP API for AI control (skill-plan.md §7–8), off unless agent.ini has
 *
 *   [api]
 *   listen = 0.0.0.0:8790     ; empty = off; a non-loopback address needs a token
 *   token = <long random>     ; callers send "Authorization: Bearer <token>"
 *
 * Routes match the server's, so one client works against either:
 *   GET  /api/v1/devices
 *   GET  /api/v1/devices/{id|local}/capabilities | /ui?view=compact | /screenshot?format=jpeg&max_edge=1280
 *   POST /api/v1/devices/{id|local}/actions
 *   GET  /api/v1/devices/{id|local}/socket?service=chrome   (Connection: Upgrade, Upgrade: remodroid-socket)
 * Each request becomes one phone API request (api.c) whose response is streamed
 * back as it arrives; one request per connection (Connection: close).
 *
 * /socket is `adb forward` over HTTP: after "101 Switching Protocols" the connection is a raw
 * byte stream to a socket on the phone, opened through adbd (as the shell user), so browser
 * debugging sockets work where a connection from the phone helper is refused by SELinux.
 * Services: localabstract:NAME or tcp:PORT; "chrome" and "firefox" are aliases. Either side
 * closing closes the other; both directions are flow-controlled. */
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "agent.h"
#include "msg.h"
#include "picohttpparser.h"

#define MAX_REQUEST (1024 * 1024)
#define MAX_OUT (16u << 20)
#define MAX_HEADERS 32
#define READ_TIMEOUT_S 30.
#define SOCK_MAX 16                /* forwarded sockets at once */
#define SOCK_HIGH (1024u * 1024)   /* pause a direction above this much unsent data */
#define SOCK_LOW (256u * 1024)     /* and resume below this */

typedef struct hconn {
    int fd, closing, responded;
    ev_io rio, wio;
    ev_timer timer;
    rd_buf_t in, out;
    api_req_t *req;
    char ip[INET6_ADDRSTRLEN];
    /* /socket */
    adb_stream_t *sock;
    int upgraded, held, counted;
    size_t skip;          /* request bytes before the raw stream */
    ev_timer resume;      /* waits for the phone to take our backlog */
    char service[128];
} hconn_t;

static int nsocks;

static struct {
    int fd, loopback;
    ev_io io;
    char addr[128];
} H = { .fd = -1 };

/* ---------------- connection ---------------- */

static void hc_close(hconn_t *c)
{
    if (c->req) {
        api_req_t *r = c->req;
        c->req = NULL;
        api_cancel(r);
    }
    if (c->sock) {
        adb_stream_t *s = c->sock;
        c->sock = NULL;
        adb_close(s); /* no more callbacks */
    }
    if (c->counted) {
        nsocks--;
        LOGD("api: socket %s closed (%s)", c->service, c->ip);
    }
    ev_timer_stop(A.loop, &c->resume);
    ev_io_stop(A.loop, &c->rio);
    ev_io_stop(A.loop, &c->wio);
    ev_timer_stop(A.loop, &c->timer);
    close(c->fd);
    rd_buf_free(&c->in);
    rd_buf_free(&c->out);
    free(c);
}

static void hc_flush(hconn_t *c)
{
    while (rd_buf_avail(&c->out)) {
        ssize_t n = send(c->fd, rd_buf_data(&c->out), rd_buf_avail(&c->out), MSG_NOSIGNAL);
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            break;
        if (n <= 0) {
            hc_close(c);
            return;
        }
        rd_buf_consume(&c->out, (size_t)n);
    }
    if (c->held && c->sock && rd_buf_avail(&c->out) < SOCK_LOW) { /* the client caught up */
        c->held = 0;
        adb_hold(c->sock, 0);
    }
    if (rd_buf_avail(&c->out)) {
        ev_io_start(A.loop, &c->wio);
    } else {
        ev_io_stop(A.loop, &c->wio);
        if (c->closing)
            hc_close(c);
    }
}

static const char *status_text(int s)
{
    switch (s) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 426: return "Upgrade Required";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default: return "Status";
    }
}

static void head(hconn_t *c, int status, const char *type, size_t len, cJSON *headers)
{
    rd_buf_printf(&c->out,
                  "HTTP/1.1 %d %s\r\nServer: remodroid-agent\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                  "Cache-Control: no-store\r\nConnection: close\r\n",
                  status, status_text(status), type, len);
    for (cJSON *h = headers ? headers->child : NULL; h; h = h->next) {
        char v[128];
        if (cJSON_IsNumber(h))
            snprintf(v, sizeof v, "%.0f", h->valuedouble);
        else if (cJSON_IsString(h))
            snprintf(v, sizeof v, "%s", h->valuestring);
        else
            continue;
        if (strncmp(h->string, "X-RD-", 5) || strpbrk(h->string, "\r\n: ") || strpbrk(v, "\r\n"))
            continue;
        rd_buf_printf(&c->out, "%s: %s\r\n", h->string, v);
    }
    rd_buf_append(&c->out, "\r\n", 2);
    c->responded = 1;
}

static void respond(hconn_t *c, int status, const char *type, const void *body, size_t n)
{
    head(c, status, type, n, NULL);
    rd_buf_append(&c->out, body, n);
    c->closing = 1;
    hc_flush(c);
}

static void respond_error(hconn_t *c, int status, const char *code, const char *text)
{
    char *b = api_error_body(code, text, status >= 500);
    respond(c, status, "application/json", b, strlen(b));
    free(b);
}

/* ---------------- phone API callbacks ---------------- */

static void on_head(api_req_t *r, int status, const char *type, size_t len, cJSON *headers, void *ud)
{
    hconn_t *c = ud;
    head(c, status, type, len, headers);
}

static void on_body(api_req_t *r, const uint8_t *d, size_t n, void *ud)
{
    hconn_t *c = ud;
    if (rd_buf_avail(&c->out) + n > MAX_OUT) { /* the client stopped reading */
        LOGW("api: %s is not reading; dropping it", c->ip);
        c->req = NULL;
        api_cancel(r);
        c->closing = 1;
        rd_buf_reset(&c->out);
        hc_flush(c);
        return;
    }
    rd_buf_append(&c->out, d, n);
    hc_flush(c);
}

static void on_done(api_req_t *r, int ok, void *ud)
{
    hconn_t *c = ud;
    if (c->req != r)
        return;
    c->req = NULL;
    if (!ok) { /* cut short: the client sees a truncated body (Content-Length) */
        hc_close(c);
        return;
    }
    c->closing = 1;
    hc_flush(c);
}

/* ---------------- requests ---------------- */

/* ---------------- /socket: a raw stream to a phone socket ---------------- */

static void sk_open(adb_stream_t *s, int ok, void *ud)
{
    hconn_t *c = ud;
    if (!ok) { /* the stream is freed after this callback */
        c->sock = NULL;
        char msg[256];
        snprintf(msg, sizeof msg, "nothing listens on %s on the phone (is the browser running, with debugging on?)",
                 c->service);
        respond_error(c, 502, "no_socket", msg);
        return;
    }
    LOGI("api: %s connected to %s", c->ip, c->service);
    rd_buf_printf(&c->out, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: remodroid-socket\r\nConnection: Upgrade\r\n\r\n");
    c->upgraded = c->responded = 1;
    rd_buf_consume(&c->in, RD_MIN(c->skip, rd_buf_avail(&c->in)));
    if (rd_buf_avail(&c->in)) /* bytes the client sent right after the request */
        adb_write(s, rd_buf_data(&c->in), rd_buf_avail(&c->in));
    rd_buf_reset(&c->in);
    hc_flush(c);
}

static void sk_data(adb_stream_t *s, const uint8_t *d, size_t n, void *ud)
{
    hconn_t *c = ud;
    rd_buf_append(&c->out, d, n);
    if (!c->held && rd_buf_avail(&c->out) > SOCK_HIGH) { /* the client is slow: stop the phone */
        c->held = 1;
        adb_hold(s, 1);
    }
    hc_flush(c);
}

static void sk_close(adb_stream_t *s, void *ud)
{
    hconn_t *c = ud;
    c->sock = NULL; /* freed after this callback */
    LOGD("api: %s closed by the phone", c->service);
    c->closing = 1; /* close once the client has everything */
    hc_flush(c);
}

static void sk_resume(struct ev_loop *loop, ev_timer *w, int revents)
{
    hconn_t *c = w->data;
    if (!c->sock || adb_backlog(c->sock) < SOCK_LOW) {
        ev_timer_stop(loop, w);
        ev_io_start(loop, &c->rio);
    }
}

/* service=localabstract:NAME | tcp:PORT | chrome | firefox */
static int sock_service(const char *in, char *out, size_t cap)
{
    if (!strcmp(in, "chrome"))
        in = "localabstract:chrome_devtools_remote";
    else if (!strcmp(in, "firefox"))
        in = "localabstract:org.mozilla.firefox/firefox-debugger-socket";
    if (!strncmp(in, "localabstract:", 14)) {
        size_t l = strlen(in + 14);
        if (!l || l > 100 || strspn(in + 14, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-/@:") != l)
            return -1;
    } else if (!strncmp(in, "tcp:", 4)) {
        char *end;
        long port = strtol(in + 4, &end, 10);
        if (*end || port < 1 || port > 65535)
            return -1;
    } else {
        return -1; /* never shell:, sync:, … */
    }
    snprintf(out, cap, "%s", in);
    return 0;
}

static void socket_open(hconn_t *c, const struct phr_header *hs, size_t nh, cJSON *q, size_t skip)
{
    int upgrade = 0;
    for (size_t i = 0; i < nh; i++)
        if (hs[i].name_len == 7 && !strncasecmp(hs[i].name, "Upgrade", 7) && hs[i].value_len == 16 &&
            !strncasecmp(hs[i].value, "remodroid-socket", 16))
            upgrade = 1;
    const char *svc = msg_str(q, "service", "");
    if (!upgrade)
        respond_error(c, 426, "bad_request", "send Connection: Upgrade and Upgrade: remodroid-socket");
    else if (sock_service(svc, c->service, sizeof c->service) < 0)
        respond_error(c, 400, "bad_request", "service is localabstract:NAME, tcp:PORT, chrome or firefox");
    else if (adb_state(A.adb) != ADB_ONLINE)
        respond_error(c, 503, "offline", "no phone connected over ADB");
    else if (nsocks >= SOCK_MAX)
        respond_error(c, 503, "busy", "too many forwarded sockets");
    else if (!(c->sock = adb_open(A.adb, c->service, (adb_stream_cb_t){ sk_open, sk_data, sk_close, NULL }, c)))
        respond_error(c, 503, "offline", "cannot open an ADB stream to the phone");
    else {
        nsocks++;
        c->counted = 1;
        c->skip = skip;
        ev_timer_init(&c->resume, sk_resume, 0.02, 0.02);
        c->resume.data = c;
    }
    cJSON_Delete(q);
}

static int token_ok(const struct phr_header *hs, size_t nh)
{
    const char *tok = agent_conf("api", "token", "");
    if (!*tok)
        return H.loopback; /* a token removed by a reload does not open a network listener */
    size_t tl = strlen(tok);
    for (size_t i = 0; i < nh; i++) {
        if (hs[i].name_len != 13 || strncasecmp(hs[i].name, "Authorization", 13))
            continue;
        if (hs[i].value_len != 7 + tl || strncasecmp(hs[i].value, "Bearer ", 7))
            return 0;
        unsigned diff = 0;
        for (size_t k = 0; k < tl; k++)
            diff |= (unsigned char)hs[i].value[7 + k] ^ (unsigned char)tok[k];
        return diff == 0;
    }
    return 0;
}

static void handle(hconn_t *c, const char *method, size_t ml, const char *path, size_t pl, const struct phr_header *hs,
                   size_t nh, const uint8_t *body, size_t bl, size_t consumed)
{
    const char *qm = memchr(path, '?', pl);
    size_t plen = qm ? (size_t)(qm - path) : pl;
    int get = ml == 3 && !memcmp(method, "GET", 3), post = ml == 4 && !memcmp(method, "POST", 4);
    LOGD("api: %s %.*s %.*s", c->ip, (int)ml, method, (int)pl, path);
    if (!token_ok(hs, nh)) {
        LOGW("api: bad token from %s", c->ip);
        respond_error(c, 401, "unauthorized", "send Authorization: Bearer <[api] token>");
        return;
    }
    static const char pre[] = "/api/v1/devices";
    if (plen == sizeof pre - 1 && !memcmp(path, pre, plen) && get) {
        cJSON *b = cJSON_CreateObject();
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "id", A.device_id[0] ? A.device_id : "local");
        cJSON_AddBoolToObject(d, "online", adb_state(A.adb) == ADB_ONLINE);
        cJSON_AddItemToObject(d, "phone", phone_status_json());
        cJSON_AddItemToArray(cJSON_AddArrayToObject(b, "devices"), d);
        char *s = cJSON_PrintUnformatted(b);
        cJSON_Delete(b);
        respond(c, 200, "application/json", s, strlen(s));
        free(s);
        return;
    }
    if (plen <= sizeof pre || memcmp(path, pre, sizeof pre - 1) || path[sizeof pre - 1] != '/') {
        respond_error(c, 404, "not_found", "no such endpoint");
        return;
    }
    const char *id = path + sizeof pre, *end = path + plen;
    const char *slash = memchr(id, '/', (size_t)(end - id));
    size_t il = (size_t)((slash ? slash : end) - id);
    int mine = (il == 5 && !memcmp(id, "local", 5)) || (il && il == strlen(A.device_id) && !memcmp(id, A.device_id, il));
    if (!mine) {
        respond_error(c, 404, "not_found", "unknown device (use \"local\" or this router's device_id)");
        return;
    }
    const char *suffix = slash ? slash : "";
    size_t sl = (size_t)(end - suffix);
    const char *op = NULL;
    cJSON *args = NULL;
#define IS(s) (sl == sizeof(s) - 1 && !memcmp(suffix, s, sl))
    if (IS("/capabilities") && get)
        op = "capabilities";
    else if (IS("/ui") && get)
        op = "ui";
    else if (IS("/screenshot") && get)
        op = "screenshot";
    else if (IS("/actions") && post)
        op = "action";
    else if (IS("/socket") && get) {
        socket_open(c, hs, nh, msg_query_json(qm ? qm + 1 : "", qm ? pl - plen - 1 : 0), consumed);
        return;
    }
#undef IS
    if (!op) {
        respond_error(c, 404, "not_found", "no such endpoint");
        return;
    }
    if (post) {
        args = cJSON_ParseWithLength((const char *)body, bl);
        if (!cJSON_IsObject(args)) {
            cJSON_Delete(args);
            respond_error(c, 400, "bad_request", "the body must be a JSON object");
            return;
        }
    } else {
        args = msg_query_json(qm ? qm + 1 : "", qm ? pl - plen - 1 : 0);
    }
    const char *code, *err;
    c->req = api_request(op, args, (api_cb_t){ on_head, on_body, on_done }, c, &code, &err);
    if (!c->req)
        respond_error(c, api_error_status(code), code, err);
}

static void hc_read(struct ev_loop *loop, ev_io *w, int revents)
{
    hconn_t *c = w->data;
    if (rd_buf_reserve(&c->in, 16384) < 0) {
        hc_close(c);
        return;
    }
    ssize_t n = recv(c->fd, c->in.p + c->in.len, c->in.cap - c->in.len, 0);
    if (n < 0 && (errno == EAGAIN || errno == EINTR))
        return;
    if (n <= 0) { /* the client went away: cancel the phone request */
        hc_close(c);
        return;
    }
    c->in.len += (size_t)n;
    if (c->upgraded) { /* raw stream: client → phone */
        if (c->sock) {
            adb_write(c->sock, rd_buf_data(&c->in), rd_buf_avail(&c->in));
            if (adb_backlog(c->sock) > SOCK_HIGH) { /* the phone is slow: stop reading the client */
                ev_io_stop(A.loop, &c->rio);
                ev_timer_again(A.loop, &c->resume);
            }
        }
        rd_buf_reset(&c->in);
        return;
    }
    if (c->responded || c->req || c->sock)
        return; /* one request per connection */
    const char *method, *path;
    size_t ml, pl, nh = MAX_HEADERS;
    int minor;
    struct phr_header hs[MAX_HEADERS];
    int hl = phr_parse_request((const char *)rd_buf_data(&c->in), rd_buf_avail(&c->in), &method, &ml, &path, &pl,
                               &minor, hs, &nh, 0);
    if (hl == -2 && rd_buf_avail(&c->in) < MAX_REQUEST)
        return;
    if (hl < 0) {
        respond_error(c, hl == -2 ? 413 : 400, hl == -2 ? "too_large" : "bad_request", "malformed request");
        return;
    }
    size_t bl = 0;
    for (size_t i = 0; i < nh; i++)
        if (hs[i].name_len == 14 && !strncasecmp(hs[i].name, "Content-Length", 14))
            bl = strtoul(hs[i].value, NULL, 10);
    if (bl > MAX_REQUEST) {
        respond_error(c, 413, "too_large", "body too large");
        return;
    }
    if (rd_buf_avail(&c->in) < (size_t)hl + bl)
        return;
    ev_timer_stop(A.loop, &c->timer);
    handle(c, method, ml, path, pl, hs, nh, rd_buf_data(&c->in) + hl, bl, (size_t)hl + bl);
}

static void hc_write(struct ev_loop *loop, ev_io *w, int revents) { hc_flush(w->data); }

static void hc_timeout(struct ev_loop *loop, ev_timer *w, int revents) { hc_close(w->data); }

static void on_accept(struct ev_loop *loop, ev_io *w, int revents)
{
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    int fd = accept4(H.fd, (struct sockaddr *)&ss, &sl, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0)
        return;
    hconn_t *c = calloc(1, sizeof *c);
    c->fd = fd;
    getnameinfo((struct sockaddr *)&ss, sl, c->ip, sizeof c->ip, NULL, 0, NI_NUMERICHOST);
    rd_buf_init(&c->in);
    rd_buf_init(&c->out);
    ev_io_init(&c->rio, hc_read, fd, EV_READ);
    ev_io_init(&c->wio, hc_write, fd, EV_WRITE);
    ev_timer_init(&c->timer, hc_timeout, READ_TIMEOUT_S, 0.);
    c->rio.data = c->wio.data = c->timer.data = c;
    ev_io_start(loop, &c->rio);
    ev_timer_start(loop, &c->timer);
}

/* ---------------- listener ---------------- */

static int is_loopback(const struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET)
        return (ntohl(((const struct sockaddr_in *)sa)->sin_addr.s_addr) >> 24) == 127;
    if (sa->sa_family == AF_INET6) {
        const struct in6_addr *a = &((const struct sockaddr_in6 *)sa)->sin6_addr;
        return IN6_IS_ADDR_LOOPBACK(a) ||
               (IN6_IS_ADDR_V4MAPPED(a) && a->s6_addr[12] == 127);
    }
    return 0;
}

int http_api_start(void)
{
    const char *addr = agent_conf("api", "listen", "");
    if (H.fd >= 0 && !strcmp(addr, H.addr))
        return 0;
    if (H.fd >= 0) {
        ev_io_stop(A.loop, &H.io);
        close(H.fd);
        H.fd = -1;
        LOGI("api: stopped listening on %s", H.addr);
    }
    snprintf(H.addr, sizeof H.addr, "%s", addr);
    if (!*addr)
        return 0;
    char host[100], port[16];
    if (rd_split_hostport(addr, host, sizeof host, port, sizeof port, "8790") < 0) {
        LOGE("[api] listen: bad address %s", addr);
        return -1;
    }
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM, .ai_flags = AI_PASSIVE }, *res;
    if (getaddrinfo(*host ? host : NULL, port, &hints, &res)) {
        LOGE("[api] listen: cannot resolve %s", addr);
        return -1;
    }
    H.loopback = is_loopback(res->ai_addr);
    if (!H.loopback && !*agent_conf("api", "token", "")) {
        freeaddrinfo(res);
        LOGE("[api] listen = %s is reachable from the network: set [api] token first", addr);
        return -1;
    }
    int fd = socket(res->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0), one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (fd < 0 || bind(fd, res->ai_addr, res->ai_addrlen) < 0 || listen(fd, 16) < 0) {
        LOGE("[api] listen %s: %s", addr, strerror(errno));
        if (fd >= 0)
            close(fd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    H.fd = fd;
    ev_io_init(&H.io, on_accept, fd, EV_READ);
    ev_io_start(A.loop, &H.io);
    LOGI("api: listening on %s%s", addr, *agent_conf("api", "token", "") ? " (token required)" : "");
    return 0;
}
