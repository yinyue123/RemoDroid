/* AI control API, router side (skill-plan.md §7): every request is one ADB stream
 * to the phone server's localabstract:remodroid-api. The agent only moves bytes:
 * the phone builds the UI JSON and compresses screenshots. Shared by the local
 * HTTP API (http.c) and requests relayed by the server (wsc.c).
 *
 *   → {"op": "ui", "args": {…}}\n
 *   ← {"status": 200, "type": "application/json", "len": N, "headers": {…}}\n  N body bytes */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent.h"
#include "msg.h"
#include "ws.h"

#define API_SERVICE "localabstract:remodroid-api"
#define API_MAX_HEAD (64 * 1024)
#define API_MAX_BODY (32u << 20)
#define API_MAX_INFLIGHT 4
#define API_TIMEOUT_S 40.
#define WS_RELAY_MAX WS_MAX_MESSAGE

struct api_req {
    adb_stream_t *s;
    api_cb_t cb;
    void *ud;
    char *line;
    rd_buf_t head;
    int got_head, done, in_close;
    size_t left;
    ev_timer timer;
};

static int inflight;

int api_error_status(const char *code)
{
    static const struct { const char *c; int s; } map[] = {
        { "bad_request", 400 }, { "unauthorized", 401 }, { "not_found", 404 }, { "offline", 503 },
        { "adb_unauthorized", 503 }, { "busy", 503 }, { "timeout", 504 }, { "too_large", 413 },
    };
    for (size_t i = 0; i < RD_ARRAY_LEN(map); i++)
        if (!strcmp(map[i].c, code))
            return map[i].s;
    return 502;
}

char *api_error_body(const char *code, const char *text, int retryable)
{
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "error", code);
    cJSON_AddStringToObject(b, "msg", text);
    cJSON_AddBoolToObject(b, "retryable", retryable);
    char *s = cJSON_PrintUnformatted(b);
    cJSON_Delete(b);
    return s;
}

/* Ends the request. With code set, the caller gets a synthesized error response
 * (only possible before the phone's header arrived). */
static void finish(api_req_t *r, int ok, const char *code, const char *text)
{
    if (r->done)
        return;
    r->done = 1;
    inflight--;
    ev_timer_stop(A.loop, &r->timer);
    if (code && !r->got_head) {
        char *body = api_error_body(code, text, 1);
        size_t n = strlen(body);
        r->cb.on_head(r, api_error_status(code), "application/json", n, NULL, r->ud);
        r->cb.on_body(r, (const uint8_t *)body, n, r->ud);
        free(body);
        ok = 1;
    }
    r->cb.on_done(r, ok, r->ud);
    if (!r->in_close && r->s) {
        adb_stream_t *s = r->s;
        r->s = NULL;
        adb_close(s); /* frees r through on_free */
    }
}

static void on_timeout(struct ev_loop *loop, ev_timer *w, int revents)
{
    finish(w->data, 0, "timeout", "the phone did not answer in time");
}

static void st_open(adb_stream_t *s, int ok, void *ud)
{
    api_req_t *r = ud;
    if (!ok) {
        r->in_close = 1; /* the stream is freed after this callback */
        finish(r, 0, "offline", "the phone server is not running");
        return;
    }
    adb_write(s, r->line, strlen(r->line));
}

static void body(api_req_t *r, const uint8_t *d, size_t n)
{
    size_t take = RD_MIN(n, r->left);
    if (take) {
        r->left -= take;
        r->cb.on_body(r, d, take, r->ud);
    }
    if (!r->done && !r->left)
        finish(r, 1, NULL, NULL);
}

static void st_data(adb_stream_t *s, const uint8_t *d, size_t n, void *ud)
{
    api_req_t *r = ud;
    if (r->done)
        return;
    if (r->got_head) {
        body(r, d, n);
        return;
    }
    rd_buf_append(&r->head, d, n);
    uint8_t *p = rd_buf_data(&r->head);
    size_t av = rd_buf_avail(&r->head);
    uint8_t *nl = memchr(p, '\n', av);
    if (!nl) {
        if (av > API_MAX_HEAD)
            finish(r, 0, "io_error", "bad response from the phone");
        return;
    }
    cJSON *h = cJSON_ParseWithLength((const char *)p, (size_t)(nl - p));
    double len = msg_num(h, "len", -1);
    if (!cJSON_IsObject(h) || len < 0 || len > API_MAX_BODY) {
        cJSON_Delete(h);
        finish(r, 0, "io_error", "bad response header from the phone");
        return;
    }
    r->got_head = 1;
    r->left = (size_t)len;
    cJSON *hs = cJSON_GetObjectItemCaseSensitive(h, "headers");
    r->cb.on_head(r, (int)msg_num(h, "status", 502), msg_str(h, "type", "application/octet-stream"), r->left,
                  cJSON_IsObject(hs) ? hs : NULL, r->ud);
    cJSON_Delete(h);
    if (r->done)
        return;
    size_t rest = av - (size_t)(nl - p) - 1;
    const uint8_t *rp = nl + 1;
    if (rest || !r->left)
        body(r, rp, rest);
}

static void st_close(adb_stream_t *s, void *ud)
{
    api_req_t *r = ud;
    r->in_close = 1;
    r->s = NULL;
    finish(r, 0, "io_error", "the phone closed the request");
}

static void st_free(void *ud)
{
    api_req_t *r = ud;
    rd_buf_free(&r->head);
    free(r->line);
    free(r);
}

api_req_t *api_request(const char *op, cJSON *args, api_cb_t cb, void *ud, const char **err_code, const char **err)
{
    int st = adb_state(A.adb);
    if (st != ADB_ONLINE) {
        cJSON_Delete(args);
        *err_code = st == ADB_UNAUTHORIZED ? "adb_unauthorized" : "offline";
        *err = st == ADB_UNAUTHORIZED ? "accept \"Allow USB debugging\" on the phone" : "no phone connected over ADB";
        return NULL;
    }
    if (inflight >= API_MAX_INFLIGHT) {
        cJSON_Delete(args);
        *err_code = "busy";
        *err = "too many requests at once";
        return NULL;
    }
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "op", op);
    cJSON_AddItemToObject(m, "args", args ? args : cJSON_CreateObject());
    char *s = cJSON_PrintUnformatted(m);
    cJSON_Delete(m);
    api_req_t *r = calloc(1, sizeof *r);
    size_t n = strlen(s);
    r->line = malloc(n + 2);
    memcpy(r->line, s, n);
    memcpy(r->line + n, "\n", 2);
    free(s);
    r->cb = cb;
    r->ud = ud;
    rd_buf_init(&r->head);
    r->s = adb_open(A.adb, API_SERVICE, (adb_stream_cb_t){ st_open, st_data, st_close, st_free }, r);
    if (!r->s) {
        st_free(r);
        *err_code = "offline";
        *err = "cannot open an ADB stream to the phone";
        return NULL;
    }
    inflight++;
    ev_timer_init(&r->timer, on_timeout, API_TIMEOUT_S, 0.);
    r->timer.data = r;
    ev_timer_start(A.loop, &r->timer);
    LOGD("api: %s", op);
    return r;
}

void api_cancel(api_req_t *r)
{
    if (!r || r->done)
        return;
    r->done = 1;
    inflight--;
    ev_timer_stop(A.loop, &r->timer);
    adb_stream_t *s = r->s;
    r->s = NULL;
    if (s && !r->in_close)
        adb_close(s);
}

/* ---------------- requests relayed by the server ----------------
 * {"t": "api", "id": N, "d": {"op", "args"}} → one binary WebSocket message:
 *   u32 header length | {"id", "status", "type", "headers"} | body
 * The server's WebSocket limit (1 MB) caps the whole message. */

typedef struct {
    long id;
    cJSON *head;
    rd_buf_t body;
    int too_large;
} relay_t;

static void relay_head(api_req_t *r, int status, const char *type, size_t len, cJSON *headers, void *ud)
{
    relay_t *x = ud;
    x->head = cJSON_CreateObject();
    cJSON_AddNumberToObject(x->head, "id", (double)x->id);
    cJSON_AddNumberToObject(x->head, "status", status);
    cJSON_AddStringToObject(x->head, "type", type);
    if (headers)
        cJSON_AddItemToObject(x->head, "headers", cJSON_Duplicate(headers, 1));
    if (len > WS_RELAY_MAX - 4096)
        x->too_large = 1;
}

static void relay_body(api_req_t *r, const uint8_t *d, size_t n, void *ud)
{
    relay_t *x = ud;
    if (!x->too_large && rd_buf_avail(&x->body) + n <= WS_RELAY_MAX - 4096)
        rd_buf_append(&x->body, d, n);
    else
        x->too_large = 1;
}

static void relay_done(api_req_t *r, int ok, void *ud)
{
    relay_t *x = ud;
    if (!ok) {
        wsc_send(msg_error(x->id, "io_error", "the phone stopped answering mid-response"));
    } else if (x->too_large) {
        wsc_send(msg_error(x->id, "too_large", "the response exceeds 1 MB through the server; lower max_edge or quality"));
    } else {
        char *h = cJSON_PrintUnformatted(x->head);
        size_t hl = strlen(h), bl = rd_buf_avail(&x->body);
        uint8_t *m = malloc(4 + hl + bl);
        rd_put_be32(m, (uint32_t)hl);
        memcpy(m + 4, h, hl);
        memcpy(m + 4 + hl, rd_buf_data(&x->body), bl);
        wsc_send_binary(m, 4 + hl + bl);
        free(m);
        free(h);
    }
    cJSON_Delete(x->head);
    rd_buf_free(&x->body);
    free(x);
}

void api_relay(long id, cJSON *d)
{
    const char *op = msg_str(d, "op", "");
    cJSON *args = cJSON_GetObjectItemCaseSensitive(d, "args");
    relay_t *x = calloc(1, sizeof *x);
    x->id = id;
    rd_buf_init(&x->body);
    const char *code = NULL, *err = NULL;
    if (!api_request(op, cJSON_IsObject(args) ? cJSON_Duplicate(args, 1) : NULL,
                     (api_cb_t){ relay_head, relay_body, relay_done }, x, &code, &err)) {
        wsc_send(msg_error(id, code, err));
        rd_buf_free(&x->body);
        free(x);
    }
}
