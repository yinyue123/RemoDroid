/* Routes control messages between viewers and agents (plan Appendix A.4/A.5). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "msg.h"
#include "server.h"

typedef struct pend {
    long id;
    conn_t *c;
    unsigned gen;
    device_t *dev;
    int raw;           /* AI API: the agent answers with one binary message (hub_on_binary) */
    ev_timer t;
    struct pend *next;
} pend_t;

static pend_t *pends;
static long next_id = 1;

/* ---------------- devices ---------------- */

device_t *hub_add_device(const char *id, const char *name, const char *hash)
{
    device_t *d = calloc(1, sizeof *d);
    d->id = rd_strdup(id);
    d->hash = rd_strdup(hash);
    d->name = rd_strdup(name && *name ? name : id);
    S.devices = realloc(S.devices, sizeof *S.devices * (size_t)(S.ndevices + 1));
    S.devices[S.ndevices++] = d;
    return d;
}

static void add_device(const char *key, const char *val, void *ud)
{
    if (!hub_device(key))
        hub_add_device(key, rd_conf_get(S.conf, "device_names", key, key), val);
}

void hub_init_devices(void) { rd_conf_each(S.conf, "devices", add_device, NULL); }

device_t *hub_device(const char *id)
{
    for (int i = 0; i < S.ndevices; i++)
        if (!strcmp(S.devices[i]->id, id))
            return S.devices[i];
    return NULL;
}

static int online(const device_t *d) { return d->agent && d->hello; }

cJSON *hub_device_json(device_t *d)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "id", d->id);
    cJSON_AddStringToObject(j, "name", d->name);
    cJSON_AddBoolToObject(j, "online", online(d));
    cJSON_AddNumberToObject(j, "last_seen", (double)d->last_seen);
    cJSON *a = cJSON_AddObjectToObject(j, "agent");
    if (d->hello) {
        cJSON_AddStringToObject(a, "version", msg_str(d->hello, "version", ""));
        cJSON_AddStringToObject(a, "arch", msg_str(d->hello, "arch", ""));
    }
    if (online(d))
        cJSON_AddNumberToObject(a, "uptime_s", (double)((rd_now_ms() - d->online_since) / 1000));
    cJSON_AddStringToObject(a, "public_ip", d->public_ip);
    cJSON *ph = d->status ? cJSON_GetObjectItemCaseSensitive(d->status, "phone") : NULL;
    if (cJSON_IsObject(ph)) {
        cJSON_AddItemToObject(j, "phone", cJSON_Duplicate(ph, 1));
    } else {
        cJSON *p = cJSON_AddObjectToObject(j, "phone");
        cJSON_AddBoolToObject(p, "attached", 0);
    }
    cJSON *s = cJSON_AddObjectToObject(j, "session");
    cJSON_AddBoolToObject(s, "active", d->vs != NULL);
    if (d->vs) {
        cJSON_AddStringToObject(s, "sid", d->vs->sid);
        cJSON_AddNumberToObject(s, "since", (double)d->vs->since);
        if (d->vs->viewer)
            cJSON_AddStringToObject(s, "viewer", d->vs->viewer->user);
    }
    return j;
}

static void broadcast(cJSON *m);
static void end_session(vsession_t *vs, const char *reason, int tell_agent, int tell_viewer);

void hub_rename_device(device_t *d, const char *name)
{
    free(d->name);
    d->name = rd_strdup(name);
    cJSON *m = msg_new("device.update", 0, NULL);
    cJSON_AddStringToObject(msg_d(m), "id", d->id);
    cJSON_AddItemToObject(msg_d(m), "patch", hub_device_json(d));
    broadcast(m);
}

/* Disconnects the agent, ends its session and forgets the device. */
void hub_remove_device(device_t *d)
{
    if (d->vs)
        end_session(d->vs, "device_removed", 0, 1);
    conn_t *agent = d->agent;
    if (agent) {
        conn_close(agent); /* hub_conn_closed clears d->agent */
        agent->dev = NULL;
    }
    for (int i = 0; i < S.ndevices; i++) {
        if (S.devices[i] == d) {
            memmove(&S.devices[i], &S.devices[i + 1], sizeof *S.devices * (size_t)(S.ndevices - i - 1));
            S.ndevices--;
            break;
        }
    }
    cJSON *m = msg_new("device.removed", 0, NULL);
    cJSON_AddStringToObject(msg_d(m), "id", d->id);
    broadcast(m);
    cJSON_Delete(d->hello);
    cJSON_Delete(d->status);
    free(d->id);
    free(d->name);
    free(d->hash);
    free(d);
}

static void broadcast(cJSON *m)
{
    for (conn_t *c = S.conns; c; c = c->next)
        if (c->kind == CONN_WS && c->role == ROLE_VIEWER)
            ws_send_json(c, cJSON_Duplicate(m, 1));
    cJSON_Delete(m);
}

void hub_device_changed(device_t *d)
{
    cJSON *m = msg_new("device.update", 0, NULL);
    cJSON *dd = msg_d(m);
    cJSON_AddStringToObject(dd, "id", d->id);
    cJSON_AddItemToObject(dd, "patch", hub_device_json(d));
    broadcast(m);
}

static void broadcast_event(device_t *d, const char *kind, cJSON *info)
{
    cJSON *m = msg_new("device.event", 0, NULL);
    cJSON *dd = msg_d(m);
    cJSON_AddStringToObject(dd, "id", d->id);
    cJSON_AddStringToObject(dd, "kind", kind);
    cJSON_AddNumberToObject(dd, "ts", (double)rd_now_ms());
    cJSON_AddItemToObject(dd, "info", info ? info : cJSON_CreateObject());
    broadcast(m);
}

/* ---------------- stream sessions ---------------- */

static void end_session(vsession_t *vs, const char *reason, int tell_agent, int tell_viewer)
{
    if (!vs)
        return;
    device_t *d = vs->dev;
    LOGI("session %s on %s ended: %s", vs->sid, d->id, reason);
    if (tell_agent && d->agent) {
        cJSON *m = msg_new("session.end", 0, vs->sid);
        cJSON_AddStringToObject(msg_d(m), "sid", vs->sid);
        cJSON_AddStringToObject(msg_d(m), "reason", reason);
        ws_send_json(d->agent, m);
    }
    if (tell_viewer && vs->viewer) {
        cJSON *m = msg_new("session.ended", 0, vs->sid);
        cJSON_AddStringToObject(msg_d(m), "reason", reason);
        ws_send_json(vs->viewer, m);
    }
    if (vs->viewer)
        vs->viewer->vs = NULL;
    d->vs = NULL;
    free(vs);
    hub_device_changed(d);
}

/* [ice] extra = stun:stun.example.com:3478, turn:turn.example.com:3478?transport=udp user secret
 * (comma-separated; each entry is a URL, optionally followed by its username and credential) */
static void extra_ice(cJSON *arr)
{
    char *list = rd_strdup(rd_conf_get(S.conf, "ice", "extra", "")), *save = NULL;
    for (char *e = strtok_r(list, ",", &save); e; e = strtok_r(NULL, ",", &save)) {
        char *s2 = NULL, *url = strtok_r(e, " \t", &s2), *u = strtok_r(NULL, " \t", &s2), *pw = strtok_r(NULL, " \t", &s2);
        if (!url || (strncmp(url, "stun:", 5) && strncmp(url, "turn:", 5) && strncmp(url, "turns:", 6)))
            continue;
        cJSON *srv = cJSON_CreateObject();
        cJSON_AddItemToObject(srv, "urls", cJSON_CreateStringArray((const char *[]){ url }, 1));
        if (u && pw) {
            cJSON_AddStringToObject(srv, "username", u);
            cJSON_AddStringToObject(srv, "credential", pw);
        }
        cJSON_AddItemToArray(arr, srv);
    }
    free(list);
}

static cJSON *ice_servers(const char *user)
{
    cJSON *arr = cJSON_CreateArray();
    if (!S.turn_urls || !*S.turn_urls) {
        extra_ice(arr);
        return arr;
    }
    cJSON *srv = cJSON_CreateObject();
    cJSON *urls = cJSON_AddArrayToObject(srv, "urls");
    char *list = rd_strdup(S.turn_urls), *save = NULL;
    for (char *u = strtok_r(list, ", ", &save); u; u = strtok_r(NULL, ", ", &save))
        cJSON_AddItemToArray(urls, cJSON_CreateString(u));
    free(list);
    if (S.turn_secret && *S.turn_secret) {
        /* coturn REST API format: username "<expiry>:<user>", credential base64(HMAC-SHA1). */
        char uname[128], cred[64];
        uint8_t mac[20];
        snprintf(uname, sizeof uname, "%llu:%s", (unsigned long long)(rd_now_ms() / 1000 + (uint64_t)S.turn_ttl_min * 60),
                 user);
        rd_hmac_sha1(S.turn_secret, strlen(S.turn_secret), uname, strlen(uname), mac);
        rd_b64_encode(mac, sizeof mac, cred, sizeof cred);
        cJSON_AddStringToObject(srv, "username", uname);
        cJSON_AddStringToObject(srv, "credential", cred);
    }
    cJSON_AddItemToArray(arr, srv);
    extra_ice(arr);
    return arr;
}

static void viewer_session_request(conn_t *c, cJSON *m)
{
    long id = msg_id(m);
    cJSON *d = msg_d(m);
    device_t *dev = hub_device(msg_str(d, "device", ""));
    if (!dev) { ws_send_json(c, msg_error(id, "not_found", "unknown device")); return; }
    if (!online(dev)) { ws_send_json(c, msg_error(id, "offline", "the router agent is offline")); return; }
    if (dev->vs && dev->vs->viewer != c) {
        if (!msg_bool(d, "takeover", 0)) { ws_send_json(c, msg_error(id, "busy", "device is already in a session")); return; }
        /* "Take over": the other viewer is told why and the phone drops its peer before the new one starts */
        LOGI("%s takes over %s from %s", c->user, dev->id, dev->vs->viewer ? dev->vs->viewer->user : "?");
        end_session(dev->vs, "taken_over", 1, 1);
    }
    if (c->vs)
        end_session(c->vs, "replaced", 1, 0);
    vsession_t *vs = calloc(1, sizeof *vs);
    uint8_t r[8];
    rd_random(r, sizeof r);
    memcpy(vs->sid, "s_", 2);
    rd_hex(r, sizeof r, vs->sid + 2);
    vs->dev = dev;
    vs->viewer = c;
    vs->since = rd_now_ms();
    dev->vs = vs;
    c->vs = vs;

    cJSON *ice = ice_servers(c->user);
    cJSON *start = msg_new("session.start", 0, vs->sid);
    cJSON *sd = msg_d(start);
    cJSON_AddStringToObject(sd, "sid", vs->sid);
    cJSON_AddItemToObject(sd, "ice_servers", cJSON_Duplicate(ice, 1));
    cJSON *q = cJSON_GetObjectItemCaseSensitive(d, "quality");
    if (q)
        cJSON_AddItemToObject(sd, "quality", cJSON_Duplicate(q, 1));
    if (msg_bool(d, "force_relay", 0)) /* "Always relay through AWS" */
        cJSON_AddBoolToObject(sd, "force_relay", 1);
    ws_send_json(dev->agent, start);

    cJSON *ack = cJSON_CreateObject();
    cJSON_AddStringToObject(ack, "sid", vs->sid);
    ws_send_json(c, msg_ack(id, ack));
    cJSON *im = msg_new("session.ice", 0, vs->sid);
    cJSON_AddItemToObject(msg_d(im), "ice_servers", ice);
    ws_send_json(c, im);
    LOGI("session %s: %s -> %s", vs->sid, c->user, dev->id);
    hub_device_changed(dev);
}

/* ---------------- viewer messages ---------------- */

static void viewer_message(conn_t *c, cJSON *m)
{
    const char *t = msg_type(m);
    long id = msg_id(m);
    if (!strcmp(t, "session.request")) {
        viewer_session_request(c, m);
    } else if (!strcmp(t, "signal")) {
        if (!c->vs || !c->vs->dev->agent) {
            ws_send_json(c, msg_error(id, "offline", "no active session"));
            return;
        }
        cJSON *f = msg_new("signal", 0, c->vs->sid);
        cJSON_ReplaceItemInObjectCaseSensitive(f, "d", cJSON_Duplicate(msg_d(m), 1));
        ws_send_json(c->vs->dev->agent, f);
    } else if (!strcmp(t, "session.end")) {
        end_session(c->vs, "viewer_left", 1, 0);
        if (id)
            ws_send_json(c, msg_ack(id, NULL));
    } else if (!strcmp(t, "ping")) {
        cJSON *d = cJSON_Duplicate(msg_d(m), 1);
        cJSON_AddNumberToObject(d, "server_ts", (double)rd_now_ms());
        cJSON *p = msg_new("pong", id, NULL);
        cJSON_ReplaceItemInObjectCaseSensitive(p, "d", d);
        ws_send_json(c, p);
    } else {
        ws_send_json(c, msg_error(id, "unsupported", t));
    }
}

/* ---------------- agent messages ---------------- */

static void finish_pend(long id, cJSON *m);

static void agent_message(conn_t *c, cJSON *m)
{
    device_t *d = c->dev;
    const char *t = msg_type(m);
    d->last_seen = rd_now_ms();
    if (!strcmp(t, "hello")) {
        int was_online = online(d);
        cJSON_Delete(d->hello);
        d->hello = cJSON_Duplicate(msg_d(m), 1);
        d->online_since = rd_now_ms();
        snprintf(d->public_ip, sizeof d->public_ip, "%s", c->ip);
        cJSON *w = msg_new("welcome", 0, NULL);
        cJSON *wd = msg_d(w);
        cJSON_AddNumberToObject(wd, "proto", RD_PROTO);
        cJSON_AddNumberToObject(wd, "server_time", (double)rd_now_ms());
        cJSON_AddNumberToObject(wd, "ping_s", S.ping_s);
        cJSON_AddNumberToObject(wd, "timeout_s", S.timeout_s);
        cJSON_AddNumberToObject(wd, "heartbeat_s", S.heartbeat_s);
        ws_send_json(c, w);
        LOGI("agent %s online (%s, %s %s)", d->id, c->ip, msg_str(d->hello, "version", "?"), msg_str(d->hello, "arch", "?"));
        cJSON *ph = cJSON_GetObjectItemCaseSensitive(d->hello, "phone");
        if (cJSON_IsObject(ph)) {
            if (!d->status)
                d->status = cJSON_CreateObject();
            cJSON_DeleteItemFromObjectCaseSensitive(d->status, "phone");
            cJSON_AddItemToObject(d->status, "phone", cJSON_Duplicate(ph, 1));
        }
        if (!was_online)
            broadcast_event(d, "agent.online", NULL);
        hub_device_changed(d);
    } else if (!d->hello) {
        ws_send_json(c, msg_error(msg_id(m), "bad_request", "send hello first"));
    } else if (!strcmp(t, "status")) {
        cJSON_Delete(d->status);
        d->status = cJSON_Duplicate(msg_d(m), 1);
        hub_device_changed(d);
    } else if (!strcmp(t, "event")) {
        cJSON *dd = msg_d(m);
        const char *kind = msg_str(dd, "kind", "unknown");
        cJSON *info = cJSON_GetObjectItemCaseSensitive(dd, "info");
        notify_on_event(d, kind, info);
        broadcast_event(d, kind, cJSON_Duplicate(info, 1));
    } else if (!strcmp(t, "signal")) {
        const char *sid = msg_sid(m);
        if (d->vs && d->vs->viewer && sid && !strcmp(sid, d->vs->sid)) {
            cJSON *f = msg_new("signal", 0, sid);
            cJSON_ReplaceItemInObjectCaseSensitive(f, "d", cJSON_Duplicate(msg_d(m), 1));
            ws_send_json(d->vs->viewer, f);
        }
    } else if (!strcmp(t, "session.ended")) {
        const char *sid = msg_sid(m);
        if (!sid)
            sid = msg_str(msg_d(m), "sid", NULL);
        if (d->vs && sid && !strcmp(sid, d->vs->sid))
            end_session(d->vs, msg_str(msg_d(m), "reason", "phone_ended"), 0, 1);
    } else if (!strcmp(t, "ack") || !strcmp(t, "error")) {
        finish_pend(msg_id(m), m);
    } else {
        LOGD("agent %s: ignoring %s", d->id, t);
    }
}

void hub_on_message(conn_t *c, cJSON *m)
{
    if (c->role == ROLE_VIEWER)
        viewer_message(c, m);
    else if (c->role == ROLE_AGENT)
        agent_message(c, m);
}

void hub_viewer_open(conn_t *c)
{
    LOGI("viewer %s connected from %s", c->user, c->ip);
    cJSON *m = msg_new("devices", 0, NULL);
    cJSON *arr = cJSON_AddArrayToObject(msg_d(m), "devices");
    for (int i = 0; i < S.ndevices; i++)
        cJSON_AddItemToArray(arr, hub_device_json(S.devices[i]));
    ws_send_json(c, m);
}

void hub_agent_open(conn_t *c)
{
    device_t *d = c->dev;
    if (d->agent && d->agent != c) {
        LOGI("agent %s reconnected; closing the old connection", d->id);
        conn_t *old = d->agent;
        d->agent = NULL;
        old->dev = NULL;
        conn_close(old);
    }
    d->agent = c;
}

void hub_conn_closed(conn_t *c)
{
    for (pend_t *p = pends; p; p = p->next)
        if (p->c == c)
            p->c = NULL;
    if (c->role == ROLE_VIEWER) {
        if (c->vs)
            end_session(c->vs, "viewer_left", 1, 0);
        LOGI("viewer %s disconnected", c->user);
    } else if (c->role == ROLE_AGENT && c->dev && c->dev->agent == c) {
        device_t *d = c->dev;
        d->agent = NULL;
        int was = d->hello != NULL;
        cJSON_Delete(d->hello);
        d->hello = NULL;
        if (d->vs)
            end_session(d->vs, "agent_offline", 0, 1);
        LOGI("agent %s offline", d->id);
        if (was) {
            broadcast_event(d, "agent.offline", NULL);
            hub_device_changed(d);
        }
    }
}

/* ---------------- REST → agent request/reply ---------------- */

static void pend_free(pend_t *p)
{
    for (pend_t **pp = &pends; *pp; pp = &(*pp)->next) {
        if (*pp == p) {
            *pp = p->next;
            break;
        }
    }
    ev_timer_stop(S.loop, &p->t);
    free(p);
}

static int code_status(const char *code)
{
    static const struct { const char *c; int s; } map[] = {
        { "bad_request", 400 }, { "unauthorized", 401 }, { "forbidden", 403 }, { "not_found", 404 },
        { "offline", 409 }, { "busy", 409 }, { "adb_unauthorized", 409 }, { "too_large", 413 },
        { "rate_limited", 429 }, { "timeout", 504 }, { "io_error", 500 }, { "unsupported", 501 },
    };
    for (size_t i = 0; i < RD_ARRAY_LEN(map); i++)
        if (!strcmp(map[i].c, code))
            return map[i].s;
    return 500;
}

static void finish_pend(long id, cJSON *m)
{
    for (pend_t *p = pends; p; p = p->next) {
        if (p->id != id)
            continue;
        conn_t *c = p->c;
        if (c && c->gen == p->gen && c->fd >= 0) {
            c->busy = 0;
            if (!strcmp(msg_type(m), "ack")) {
                cJSON *b = cJSON_CreateObject();
                cJSON_AddItemToObject(b, "result", cJSON_Duplicate(msg_d(m), 1));
                http_json(c, 200, b, NULL);
            } else {
                const char *code = msg_str(msg_d(m), "code", "io_error");
                http_error(c, code_status(code), code, msg_str(msg_d(m), "msg", code));
            }
            ev_feed_fd_event(S.loop, c->fd, EV_READ); /* resume pipelined requests */
        }
        pend_free(p);
        return;
    }
}

static void pend_timeout(struct ev_loop *loop, ev_timer *w, int revents)
{
    pend_t *p = w->data;
    if (p->c && p->c->gen == p->gen && p->c->fd >= 0) {
        p->c->busy = 0;
        http_error(p->c, 504, "timeout", "the agent did not answer in time");
    }
    pend_free(p);
}

static void forward(conn_t *c, device_t *d, const char *type, cJSON *payload, int raw, double timeout)
{
    if (!online(d)) {
        cJSON_Delete(payload);
        http_error(c, 409, "offline", "the router agent is offline");
        return;
    }
    pend_t *p = calloc(1, sizeof *p);
    p->id = next_id++;
    p->c = c;
    p->gen = c->gen;
    p->dev = d;
    p->raw = raw;
    ev_timer_init(&p->t, pend_timeout, timeout, 0.);
    p->t.data = p;
    ev_timer_start(S.loop, &p->t);
    p->next = pends;
    pends = p;
    c->busy = 1;
    cJSON *m = msg_new(type, p->id, NULL);
    cJSON_ReplaceItemInObjectCaseSensitive(m, "d", payload);
    ws_send_json(d->agent, m);
}

void hub_forward_cmd(conn_t *c, device_t *d, const char *type, cJSON *payload) { forward(c, d, type, payload, 0, 15.); }

void hub_forward_api(conn_t *c, device_t *d, const char *op, cJSON *args)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "op", op);
    cJSON_AddItemToObject(p, "args", args ? args : cJSON_CreateObject());
    forward(c, d, "api", p, 1, 45.); /* the phone may wait up to 30 s for the UI */
}

/* An AI API response from the agent: u32 header length | {"id", "status", "type", "headers"} | body. */
void hub_on_binary(conn_t *c, const uint8_t *m, size_t n)
{
    if (c->role != ROLE_AGENT || !c->dev || n < 4)
        return;
    uint32_t hl = rd_get_be32(m);
    if (hl > n - 4)
        return;
    cJSON *h = cJSON_ParseWithLength((const char *)m + 4, hl);
    long id = (long)msg_num(h, "id", 0);
    for (pend_t *p = pends; p; p = p->next) {
        if (p->id != id || !p->raw || p->dev != c->dev)
            continue;
        conn_t *v = p->c;
        if (v && v->gen == p->gen && v->fd >= 0) {
            char extra[1024] = "";
            size_t el = 0;
            cJSON *hs = cJSON_GetObjectItemCaseSensitive(h, "headers");
            for (cJSON *it = cJSON_IsObject(hs) ? hs->child : NULL; it; it = it->next) {
                char val[128];
                if (cJSON_IsNumber(it))
                    snprintf(val, sizeof val, "%.0f", it->valuedouble);
                else if (cJSON_IsString(it))
                    snprintf(val, sizeof val, "%s", it->valuestring);
                else
                    continue;
                if (strncmp(it->string, "X-RD-", 5) || strpbrk(it->string, "\r\n: ") || strpbrk(val, "\r\n"))
                    continue;
                int w = snprintf(extra + el, sizeof extra - el, "%s: %s\r\n", it->string, val);
                if (w < 0 || (size_t)w >= sizeof extra - el)
                    break;
                el += (size_t)w;
            }
            const char *type = msg_str(h, "type", "application/octet-stream");
            int status = (int)msg_num(h, "status", 502);
            v->busy = 0;
            http_respond(v, status >= 100 && status < 600 ? status : 502, strpbrk(type, "\r\n") ? "application/octet-stream" : type,
                         m + 4 + hl, n - 4 - hl, extra);
            ev_feed_fd_event(S.loop, v->fd, EV_READ);
        }
        pend_free(p);
        break;
    }
    cJSON_Delete(h);
}
