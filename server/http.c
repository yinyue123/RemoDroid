/* HTTP/1.1 + WebSocket connections on one libev loop. */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "msg.h"
#include "picohttpparser.h"
#include "server.h"

#define MAX_HEADERS 40
#define MAX_REQUEST (64 * 1024)
#define MAX_OUT (4 * 1024 * 1024)

static unsigned next_gen = 1;
static conn_t *dead;
static ev_check reaper;
static void reap(struct ev_loop *loop, ev_check *w, int revents);

static const char *status_text(int s)
{
    switch (s) {
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 204: return "No Content";
    case 302: return "Found";
    case 308: return "Permanent Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default:  return "Status";
    }
}

/* ---------------- request helpers ---------------- */

const char *req_header(const http_req_t *r, const char *name, size_t *len)
{
    size_t nl = strlen(name);
    for (size_t i = 0; i < r->nheaders; i++) {
        if (r->headers[i].name_len == nl && !strncasecmp(r->headers[i].name, name, nl)) {
            if (len)
                *len = r->headers[i].value_len;
            return r->headers[i].value;
        }
    }
    return NULL;
}

int req_is(const http_req_t *r, const char *method, const char *path)
{
    return r->method_len == strlen(method) && !memcmp(r->method, method, r->method_len) &&
           r->path_len == strlen(path) && !memcmp(r->path, path, r->path_len);
}

int req_query_int(const http_req_t *r, const char *key, int def)
{
    size_t kl = strlen(key);
    const char *p = r->query, *end = r->query + r->query_len;
    while (p && p < end) {
        const char *amp = memchr(p, '&', (size_t)(end - p));
        if (!amp)
            amp = end;
        if ((size_t)(amp - p) > kl && !memcmp(p, key, kl) && p[kl] == '=')
            return atoi(p + kl + 1);
        p = amp + 1;
    }
    return def;
}

static int header_has_token(const http_req_t *r, const char *name, const char *token)
{
    size_t vl, tl = strlen(token);
    const char *v = req_header(r, name, &vl);
    for (size_t i = 0; v && i + tl <= vl; i++)
        if (!strncasecmp(v + i, token, tl))
            return 1;
    return 0;
}

/* ---------------- output ---------------- */

static void flush_out(conn_t *c);

void conn_send(conn_t *c, const void *data, size_t n)
{
    if (c->fd < 0)
        return;
    if (rd_buf_avail(&c->out) + n > MAX_OUT) {
        LOGW("%s: output backlog too large, closing", c->ip);
        conn_close(c);
        return;
    }
    rd_buf_append(&c->out, data, n);
    flush_out(c);
}

void http_respond(conn_t *c, int status, const char *ctype, const void *body, size_t n, const char *extra)
{
    char h[1024];
    int hl = snprintf(h, sizeof h,
                      "HTTP/1.1 %d %s\r\nServer: remodroid\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n%s%s%s%s\r\n",
                      status, status_text(status), n, ctype ? "Content-Type: " : "", ctype ? ctype : "",
                      ctype ? "\r\n" : "", extra ? extra : "");
    rd_buf_append(&c->out, h, (size_t)hl);
    conn_send(c, body, n);
}

void http_json(conn_t *c, int status, cJSON *body, const char *extra)
{
    char *s = body ? cJSON_PrintUnformatted(body) : NULL;
    http_respond(c, status, "application/json", s ? s : "", s ? strlen(s) : 0, extra);
    free(s);
    cJSON_Delete(body);
}

void http_error(conn_t *c, int status, const char *code, const char *text)
{
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "error", code);
    cJSON_AddStringToObject(b, "msg", text ? text : code);
    http_json(c, status, b, NULL);
}

void ws_send_json(conn_t *c, cJSON *m)
{
    char *s = msg_print(m);
    cJSON_Delete(m);
    if (!s || c->kind != CONN_WS)
        goto out;
    LOGT("ws %s -> %s", c->user, s);
    rd_buf_t fr;
    rd_buf_init(&fr);
    ws_build(&fr, WS_OP_TEXT, s, strlen(s), 0);
    conn_send(c, rd_buf_data(&fr), rd_buf_avail(&fr));
    rd_buf_free(&fr);
out:
    free(s);
}

static void ws_send_raw(conn_t *c, int op, const void *data, size_t n)
{
    rd_buf_t fr;
    rd_buf_init(&fr);
    ws_build(&fr, op, data, n, 0);
    conn_send(c, rd_buf_data(&fr), rd_buf_avail(&fr));
    rd_buf_free(&fr);
}

static ssize_t conn_read(conn_t *c, void *buf, size_t n)
{
    if (c->ssl)
        return tls_read(c, buf, n);
    ssize_t r = read(c->fd, buf, n);
    return r < 0 && (errno == EAGAIN || errno == EINTR) ? TLS_AGAIN : r;
}

static ssize_t conn_write(conn_t *c, const void *buf, size_t n)
{
    if (c->ssl)
        return tls_write(c, buf, n);
    ssize_t r = write(c->fd, buf, n);
    return r < 0 && (errno == EAGAIN || errno == EINTR) ? TLS_AGAIN : r;
}

/* TLS can't use sendfile(): feed the file through the output buffer instead. */
static void fill_from_file(conn_t *c)
{
    size_t want = (size_t)RD_MIN(c->file_end - c->file_off, (off_t)65536);
    ssize_t n = want && rd_buf_reserve(&c->out, want) == 0 ? pread(c->file_fd, c->out.p + c->out.len, want, c->file_off)
                                                           : 0;
    if (n > 0) {
        c->out.len += (size_t)n;
        c->file_off += n;
    }
    if (n <= 0 || c->file_off >= c->file_end) {
        close(c->file_fd);
        c->file_fd = -1;
    }
}

static void flush_out(conn_t *c)
{
    for (;;) {
        if (c->ssl && !rd_buf_avail(&c->out) && c->file_fd >= 0)
            fill_from_file(c);
        if (!rd_buf_avail(&c->out))
            break;
        ssize_t w = conn_write(c, rd_buf_data(&c->out), rd_buf_avail(&c->out));
        if (w == TLS_AGAIN)
            break;
        if (w <= 0) {
            conn_close(c);
            return;
        }
        rd_buf_consume(&c->out, (size_t)w);
    }
    while (!c->ssl && !rd_buf_avail(&c->out) && c->file_fd >= 0) {
        ssize_t w = sendfile(c->fd, c->file_fd, &c->file_off, (size_t)(c->file_end - c->file_off));
        if (w < 0 && (errno == EAGAIN || errno == EINTR))
            break;
        if (w <= 0 || c->file_off >= c->file_end) {
            close(c->file_fd);
            c->file_fd = -1;
        }
    }
    int pending = rd_buf_avail(&c->out) || c->file_fd >= 0;
    /* a TLS write waiting for readability resumes from on_read, not on writability */
    if ((pending && !c->ssl_wr_wants_r) || c->ssl_rd_wants_w)
        ev_io_start(S.loop, &c->wio);
    else
        ev_io_stop(S.loop, &c->wio);
    if (!pending && c->close_after_write)
        conn_close(c);
}

/* ---------------- static files ---------------- */

static const char *mime_type(const char *path)
{
    static const char *map[][2] = {
        { ".html", "text/html; charset=utf-8" }, { ".js", "text/javascript; charset=utf-8" },
        { ".mjs", "text/javascript; charset=utf-8" }, { ".css", "text/css; charset=utf-8" },
        { ".json", "application/json" }, { ".webmanifest", "application/manifest+json" },
        { ".png", "image/png" }, { ".jpg", "image/jpeg" }, { ".svg", "image/svg+xml" },
        { ".ico", "image/x-icon" }, { ".woff2", "font/woff2" }, { ".txt", "text/plain; charset=utf-8" },
    };
    const char *dot = strrchr(path, '.');
    for (size_t i = 0; dot && i < RD_ARRAY_LEN(map); i++)
        if (!strcmp(dot, map[i][0]))
            return map[i][1];
    return "application/octet-stream";
}

void http_serve_static(conn_t *c, const http_req_t *r, const char *extra)
{
    char rel[512];
    if (r->path_len == 0 || r->path_len >= sizeof rel - 16) {
        http_error(c, 400, "bad_request", "bad path");
        return;
    }
    memcpy(rel, r->path, r->path_len);
    rel[r->path_len] = 0;
    if (strstr(rel, "..") || strchr(rel, '\\') || strchr(rel, '%') || rel[0] != '/') {
        http_error(c, 400, "bad_request", "bad path");
        return;
    }
    if (rel[strlen(rel) - 1] == '/')
        strcat(rel, "index.html");
    char full[1024];
    snprintf(full, sizeof full, "%s%s", S.web_root, rel);
    int fd = open(full, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        if (fd >= 0)
            close(fd);
        http_error(c, 404, "not_found", "no such file");
        return;
    }
    int is_head = r->method_len == 4 && !memcmp(r->method, "HEAD", 4);
    char h[1024];
    int hl = snprintf(h, sizeof h,
                      "HTTP/1.1 200 OK\r\nServer: remodroid\r\nContent-Type: %s\r\nContent-Length: %lld\r\n"
                      "Cache-Control: no-cache\r\n%s\r\n",
                      mime_type(full), (long long)st.st_size, extra ? extra : "");
    rd_buf_append(&c->out, h, (size_t)hl);
    if (is_head || st.st_size == 0) {
        close(fd);
    } else {
        c->file_fd = fd;
        c->file_off = 0;
        c->file_end = st.st_size;
    }
    flush_out(c);
}

/* ---------------- WebSocket upgrade ---------------- */

static int origin_ok(const http_req_t *r)
{
    size_t ol, hl;
    const char *o = req_header(r, "Origin", &ol);
    if (!o)
        return 1; /* non-browser client */
    if (S.origins && *S.origins) {
        const char *p = S.origins;
        while (*p) {
            while (*p == ' ' || *p == ',')
                p++;
            const char *e = p;
            while (*e && *e != ',' && *e != ' ')
                e++;
            if ((size_t)(e - p) == ol && !strncasecmp(p, o, ol))
                return 1;
            p = e;
        }
    }
    const char *h = req_header(r, "Host", &hl);
    const char *ss = memmem(o, ol, "://", 3);
    if (!h || !ss)
        return 0;
    ss += 3;
    return (size_t)(o + ol - ss) == hl && !strncasecmp(ss, h, hl);
}

static int ws_upgrade(conn_t *c, const http_req_t *r, int role, const char *extra)
{
    size_t kl;
    const char *key = req_header(r, "Sec-WebSocket-Key", &kl);
    if (!header_has_token(r, "Upgrade", "websocket") || !header_has_token(r, "Connection", "upgrade") || !key ||
        kl > 64) {
        http_error(c, 400, "bad_request", "expected a WebSocket upgrade");
        return -1;
    }
    char k[65], acc[29], h[1024];
    memcpy(k, key, kl);
    k[kl] = 0;
    ws_accept_key(k, acc);
    int hl = snprintf(h, sizeof h,
                      "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                      "Sec-WebSocket-Accept: %s\r\n%s\r\n",
                      acc, extra ? extra : "");
    c->kind = CONN_WS;
    c->role = role;
    c->last_rx = ev_now(S.loop);
    conn_send(c, h, (size_t)hl);
    return 0;
}

/* ---------------- routing ---------------- */

/* Plain HTTP once a certificate is loaded ([tls] plain_http = redirect): send
 * everything to the same host on the HTTPS port. WebSocket clients don't follow
 * redirects, so agents must then use wss://. */
static void redirect_https(conn_t *c, const http_req_t *r)
{
    size_t hl;
    const char *h = req_header(r, "Host", &hl);
    char host[256];
    if (!h || hl == 0 || hl >= sizeof host || memchr(h, '/', hl) || memchr(h, '@', hl)) {
        http_error(c, 400, "bad_request", "missing Host header");
        return;
    }
    memcpy(host, h, hl);
    host[hl] = 0;
    char *colon = strrchr(host, ':'), *bracket = strrchr(host, ']');
    if (colon && (!bracket || colon > bracket) && (bracket || colon == strchr(host, ':')))
        *colon = 0; /* drop the plain HTTP port: "name:80", "[v6]:80" */
    char port[16] = "";
    if (S.tls_port && S.tls_port != 443)
        snprintf(port, sizeof port, ":%d", S.tls_port);
    const char *path = r->path; /* path and query are contiguous in the request line */
    size_t pl = r->path_len + (r->query_len ? r->query_len + 1 : 0);
    char extra[1024];
    if ((size_t)snprintf(extra, sizeof extra, "Location: https://%s%s%.*s\r\n", host, port, (int)pl, path) >= sizeof extra) {
        http_error(c, 400, "bad_request", "URL too long");
        return;
    }
    http_respond(c, 308, "text/plain; charset=utf-8", "Use HTTPS\n", 10, extra);
}

static void handle_request(conn_t *c, const http_req_t *r)
{
    char name[64] = "", cookie[256] = "";
    uint64_t exp = 0;
    int is_get = (r->method_len == 3 && !memcmp(r->method, "GET", 3)) ||
                 (r->method_len == 4 && !memcmp(r->method, "HEAD", 4));
    LOGD("%s %.*s %.*s", c->ip, (int)r->method_len, r->method, (int)r->path_len, r->path);

    /* ACME http-01 challenges: answered before any auth, on either listener. */
    static const char WK[] = "/.well-known/acme-challenge/";
    if (is_get && r->path_len > sizeof WK - 1 && !memcmp(r->path, WK, sizeof WK - 1)) {
        char ka[300];
        if (acme_challenge(r->path + sizeof WK - 1, r->path_len - (sizeof WK - 1), ka, sizeof ka) == 0)
            http_respond(c, 200, "application/octet-stream", ka, strlen(ka), NULL);
        else
            http_error(c, 404, "not_found", "no such challenge");
        return;
    }
    if (!c->tls && S.tls_redirect && tls_ready()) {
        redirect_https(c, r);
        return;
    }
    if (S.setup) { /* fresh install: only the wizard (setup.c) */
        setup_request(c, r);
        return;
    }
    if (req_is(r, "POST", "/api/v1/enroll")) { /* a router redeeming a code: no login */
        enroll_public(c, r);
        return;
    }

    if (req_is(r, "GET", "/ws/agent")) {
        const char *sid = auth_cookie_session(r, 1, name, sizeof name, NULL);
        if (!sid) {
            int a = auth_basic(r, c, 1, name, sizeof name);
            if (a == -2) { http_error(c, 429, "rate_limited", "too many failed logins"); return; }
            if (a != 1) { http_error(c, 401, "unauthorized", "device credentials required"); return; }
            auth_cookie_header(auth_new_session(name, 1), c->tls, cookie, sizeof cookie);
        }
        snprintf(c->user, sizeof c->user, "%s", name);
        c->dev = hub_device(name);
        if (!c->dev) { http_error(c, 403, "forbidden", "unknown device"); return; }
        if (ws_upgrade(c, r, ROLE_AGENT, cookie) == 0)
            hub_agent_open(c);
        return;
    }

    const char *sess = auth_cookie_session(r, 0, name, sizeof name, &exp);

    if (req_is(r, "GET", "/ws/viewer")) {
        if (!sess) { http_error(c, 401, "unauthorized", "log in first"); return; }
        if (!origin_ok(r)) { http_error(c, 403, "forbidden", "origin not allowed"); return; }
        snprintf(c->user, sizeof c->user, "%s", name);
        if (ws_upgrade(c, r, ROLE_VIEWER, NULL) == 0)
            hub_viewer_open(c);
        return;
    }

    if (r->path_len >= 8 && !memcmp(r->path, "/api/v1/", 8)) {
        if (!sess) { http_error(c, 401, "unauthorized", "log in first"); return; }
        api_handle(c, r, name, exp, sess);
        return;
    }

    if (!is_get) {
        http_error(c, 405, "bad_request", "method not allowed");
        return;
    }
    /* The Home Screen icon and manifest: iOS fetches them without the login cookie. */
    if (!sess && (req_is(r, "GET", "/manifest.webmanifest") || req_is(r, "GET", "/icon.svg") || req_is(r, "GET", "/icon-180.png"))) {
        http_serve_static(c, r, NULL);
        return;
    }
    /* Pages: cookie, or Basic auth that creates the cookie, or a 401 challenge. */
    if (!sess) {
        int a = auth_basic(r, c, 0, name, sizeof name);
        if (a == -2) { http_error(c, 429, "rate_limited", "too many failed logins, try again later"); return; }
        if (a != 1) {
            http_respond(c, 401, "text/plain; charset=utf-8", "Login required\n", 15,
                         "WWW-Authenticate: Basic realm=\"RemoDroid\", charset=\"UTF-8\"\r\n");
            return;
        }
        auth_cookie_header(auth_new_session(name, 0), c->tls, cookie, sizeof cookie);
    }
    if (req_is(r, "GET", "/login")) { /* ?next=/agent.html: back to the page that needed the login */
        char extra[600], next[200] = "/";
        if (r->query_len > 5 && !memcmp(r->query, "next=/", 6) && r->query[6] != '/' && r->query_len < 150) {
            snprintf(next, sizeof next, "%.*s", (int)r->query_len - 5, r->query + 5);
            next[strcspn(next, "&\r\n\\ ")] = 0;
        }
        snprintf(extra, sizeof extra, "Location: %s\r\n%s", next, cookie);
        http_respond(c, 302, NULL, "", 0, extra);
        return;
    }
    http_serve_static(c, r, cookie);
}

/* ---------------- input ---------------- */

static void process_http(conn_t *c)
{
    while (c->fd >= 0 && c->kind == CONN_HTTP && !c->busy && rd_buf_avail(&c->in)) {
        const char *method, *path;
        size_t ml, pl, nh = MAX_HEADERS;
        int minor;
        struct phr_header hs[MAX_HEADERS];
        int n = phr_parse_request((const char *)rd_buf_data(&c->in), rd_buf_avail(&c->in), &method, &ml, &path, &pl,
                                  &minor, hs, &nh, 0);
        if (n == -2) {
            if (rd_buf_avail(&c->in) > MAX_REQUEST) {
                c->close_after_write = 1;
                http_error(c, 413, "too_large", "request too large");
            }
            return;
        }
        if (n < 0) {
            c->close_after_write = 1;
            http_error(c, 400, "bad_request", "malformed request");
            return;
        }
        http_req_t r = { .method = method, .method_len = ml, .headers = hs, .nheaders = nh };
        const char *q = memchr(path, '?', pl);
        r.path = path;
        r.path_len = q ? (size_t)(q - path) : pl;
        r.query = q ? q + 1 : "";
        r.query_len = q ? pl - r.path_len - 1 : 0;
        size_t cl_len;
        const char *cl = req_header(&r, "Content-Length", &cl_len);
        size_t body = cl ? (size_t)strtoul(cl, NULL, 10) : 0;
        if (body > MAX_REQUEST) {
            c->close_after_write = 1;
            http_error(c, 413, "too_large", "body too large");
            return;
        }
        if (rd_buf_avail(&c->in) < (size_t)n + body)
            return;
        r.body = rd_buf_data(&c->in) + n;
        r.body_len = body;
        if ((minor == 0 && !header_has_token(&r, "Connection", "keep-alive")) || header_has_token(&r, "Connection", "close"))
            c->close_after_write = 1;
        handle_request(c, &r);
        if (c->fd < 0)
            return;
        rd_buf_consume(&c->in, (size_t)n + body);
    }
}

static void process_ws(conn_t *c)
{
    while (c->fd >= 0 && rd_buf_avail(&c->in)) {
        ws_frame_t f;
        int n = ws_parse(rd_buf_data(&c->in), rd_buf_avail(&c->in), 1, &f);
        if (n == 0)
            return;
        if (n < 0) {
            LOGW("%s: bad WebSocket frame", c->ip);
            conn_close(c);
            return;
        }
        int op;
        const uint8_t *m;
        size_t ml;
        int ok = ws_asm_feed(&c->wsa, &f, &op, &m, &ml);
        if (ok < 0) {
            conn_close(c);
            return;
        }
        c->last_rx = ev_now(S.loop);
        if (ok == 1) {
            if (op == WS_OP_PING) {
                ws_send_raw(c, WS_OP_PONG, m, ml);
            } else if (op == WS_OP_CLOSE) {
                ws_send_raw(c, WS_OP_CLOSE, m, ml < 2 ? ml : 2);
                c->close_after_write = 1;
                rd_buf_consume(&c->in, (size_t)n);
                flush_out(c);
                return;
            } else if (op == WS_OP_TEXT) {
                cJSON *j = cJSON_ParseWithLength((const char *)m, ml);
                if (cJSON_IsObject(j)) {
                    LOGT("ws %s <- %.*s", c->user, (int)RD_MIN(ml, 400), m);
                    hub_on_message(c, j);
                } else {
                    ws_send_json(c, msg_error(0, "bad_request", "expected a JSON object"));
                }
                cJSON_Delete(j);
            }
        }
        if (c->fd < 0)
            return;
        rd_buf_consume(&c->in, (size_t)n);
    }
}

static void on_read(struct ev_loop *loop, ev_io *w, int revents)
{
    conn_t *c = w->data;
    for (;;) {
        if (rd_buf_reserve(&c->in, 16384) < 0) {
            conn_close(c);
            return;
        }
        ssize_t n = conn_read(c, c->in.p + c->in.len, c->in.cap - c->in.len);
        if (n > 0) {
            c->in.len += (size_t)n;
            if (!c->ssl && (size_t)n < 16384)
                break;
            continue; /* TLS: drain every decrypted record */
        }
        if (n == TLS_AGAIN)
            break;
        conn_close(c);
        return;
    }
    if (c->ssl_rd_wants_w) /* handshake or key update needs the socket writable */
        ev_io_start(S.loop, &c->wio);
    if (c->kind == CONN_HTTP)
        process_http(c);
    else
        process_ws(c);
    if (c->fd >= 0 && c->ssl_wr_wants_r) /* a TLS write waited for this readability */
        flush_out(c);
}

static void on_write(struct ev_loop *loop, ev_io *w, int revents)
{
    conn_t *c = w->data;
    flush_out(c);
    if (c->fd >= 0 && c->ssl_rd_wants_w)
        on_read(loop, &c->rio, EV_READ);
}

/* ---------------- lifecycle ---------------- */

void conn_close(conn_t *c)
{
    if (c->fd < 0)
        return;
    LOGD("close %s (%s)", c->ip, c->user);
    ev_io_stop(S.loop, &c->rio);
    ev_io_stop(S.loop, &c->wio);
    tls_conn_free(c);
    close(c->fd);
    c->fd = -1;
    if (c->file_fd >= 0)
        close(c->file_fd);
    c->file_fd = -1;
    hub_conn_closed(c);
    if (c->prev)
        c->prev->next = c->next;
    else
        S.conns = c->next;
    if (c->next)
        c->next->prev = c->prev;
    c->gen = 0;
    /* Freed later from the loop's check watcher, so callers holding c stay safe. */
    c->next = dead;
    dead = c;
    ev_check_start(S.loop, &reaper);
}

static void reap(struct ev_loop *loop, ev_check *w, int revents)
{
    while (dead) {
        conn_t *c = dead;
        dead = c->next;
        rd_buf_free(&c->in);
        rd_buf_free(&c->out);
        ws_asm_free(&c->wsa);
        free(c);
    }
    ev_check_stop(loop, w);
}

static void on_accept(struct ev_loop *loop, ev_io *w, int revents)
{
    listener_t *l = w->data;
    for (;;) {
        struct sockaddr_storage ss;
        socklen_t sl = sizeof ss;
        int fd = accept4(l->fd, (struct sockaddr *)&ss, &sl, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0)
            return;
        if (l->tls && !tls_ready()) { /* no certificate yet (ACME still running) */
            close(fd);
            continue;
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, 1 /* TCP_NODELAY */, &one, sizeof one);
        conn_t *c = calloc(1, sizeof *c);
        c->fd = fd;
        c->file_fd = -1;
        c->gen = next_gen++;
        if (!next_gen)
            next_gen = 1;
        if (ss.ss_family == AF_INET6) {
            struct sockaddr_in6 *a = (struct sockaddr_in6 *)&ss;
            if (IN6_IS_ADDR_V4MAPPED(&a->sin6_addr))
                inet_ntop(AF_INET, &a->sin6_addr.s6_addr[12], c->ip, sizeof c->ip);
            else
                inet_ntop(AF_INET6, &a->sin6_addr, c->ip, sizeof c->ip);
        } else {
            inet_ntop(AF_INET, &((struct sockaddr_in *)&ss)->sin_addr, c->ip, sizeof c->ip);
        }
        c->tls = l->tls;
        if (c->tls && tls_conn_start(c) < 0) {
            close(fd);
            free(c);
            continue;
        }
        ev_io_init(&c->rio, on_read, fd, EV_READ);
        ev_io_init(&c->wio, on_write, fd, EV_WRITE);
        c->rio.data = c->wio.data = c;
        c->next = S.conns;
        if (S.conns)
            S.conns->prev = c;
        S.conns = c;
        ev_io_start(S.loop, &c->rio);
    }
}

int server_listen(const char *addr, int tls)
{
    char host[128], port[16];
    if (S.nlisteners == (int)RD_ARRAY_LEN(S.listeners))
        return -1;
    if (rd_split_hostport(addr, host, sizeof host, port, sizeof port, tls ? "443" : "80") < 0) {
        LOGE("bad listen address: %s", addr);
        return -1;
    }
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM, .ai_flags = AI_PASSIVE }, *ai;
    int e = getaddrinfo(strcmp(host, "*") ? host : NULL, port, &hints, &ai);
    if (e) {
        LOGE("listen %s: %s", addr, gai_strerror(e));
        return -1;
    }
    int fd = socket(ai->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (fd < 0 || bind(fd, ai->ai_addr, ai->ai_addrlen) < 0 || listen(fd, 128) < 0) {
        LOGE("listen %s: %s", addr, strerror(errno));
        freeaddrinfo(ai);
        if (fd >= 0)
            close(fd);
        return -1;
    }
    freeaddrinfo(ai);
    listener_t *l = &S.listeners[S.nlisteners++];
    l->fd = fd;
    l->tls = tls;
    if (tls)
        S.tls_port = atoi(port);
    static int reaper_ready;
    if (!reaper_ready++)
        ev_check_init(&reaper, reap);
    ev_io_init(&l->io, on_accept, fd, EV_READ);
    l->io.data = l;
    ev_io_start(S.loop, &l->io);
    LOGI("listening on %s://%s", tls ? "https" : "http", addr);
    return 0;
}

/* Called every ping interval: ping live WebSockets, drop silent ones. */
void server_tick_ping(void)
{
    ev_tstamp now = ev_now(S.loop);
    for (conn_t *c = S.conns, *n; c; c = n) {
        n = c->next;
        if (c->kind != CONN_WS)
            continue;
        if (now - c->last_rx > S.timeout_s) {
            LOGI("%s (%s): no traffic for %ds, closing", c->ip, c->user, S.timeout_s);
            conn_close(c);
            continue;
        }
        ws_send_raw(c, WS_OP_PING, "rd", 2);
    }
}
