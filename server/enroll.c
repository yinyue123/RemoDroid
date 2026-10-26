/* Adding routers with one-time codes.
 *
 *   web/agent.html          POST /api/v1/enroll/codes {name}  → {code: "X5SX-K3M7", expires_in: 600}
 *   on the router           remodroid enroll --panel https://… --code X5SX-K3M7
 *                           → POST /api/v1/enroll {code, name, arch}   (no login: the code is the proof)
 *                           ← {device_id, device_secret, ws_path}      written to agent.ini
 *   web/agent.html          GET /api/v1/enroll/codes/X5SX-K3M7 → {state: "used", device: "router-1"}
 *
 * Codes live in memory, are single-use and expire after 10 minutes. A wrong
 * code counts towards the per-IP lockout shared with logins. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "msg.h"
#include "server.h"

#define CODE_TTL_MS (10 * 60 * 1000)
#define MAX_CODES 32

typedef struct {
    char code[10];   /* "X5SX-K3M7" */
    char name[64];   /* name chosen on the page (optional) */
    uint64_t expires;
    char device[64]; /* set once used */
} code_t;

static code_t codes[MAX_CODES];

static const char CODE_ABC[] = "23456789ABCDEFGHJKMNPQRSTUVWXYZ"; /* no 0/O, 1/I/L */

static void expire_codes(void)
{
    uint64_t now = rd_now_ms();
    for (int i = 0; i < MAX_CODES; i++) /* keep used codes a little longer so the page can see the result */
        if (codes[i].code[0] && now > codes[i].expires + (codes[i].device[0] ? CODE_TTL_MS : 0))
            memset(&codes[i], 0, sizeof codes[i]);
}

/* "x5sxk3m7", "X5SX-K3M7", " x5sx-k3m7 " → "X5SX-K3M7" */
static int normalize(const char *in, char out[10])
{
    int n = 0;
    for (const char *p = in; *p; p++) {
        if (*p == '-' || *p == ' ')
            continue;
        char ch = (char)toupper((unsigned char)*p);
        if (!strchr(CODE_ABC, ch) || n == 8)
            return -1;
        out[n < 4 ? n : n + 1] = ch;
        n++;
    }
    if (n != 8)
        return -1;
    out[4] = '-';
    out[9] = 0;
    return 0;
}

static code_t *find_code(const char *code)
{
    for (int i = 0; i < MAX_CODES; i++)
        if (codes[i].code[0] && !strcmp(codes[i].code, code))
            return &codes[i];
    return NULL;
}

/* "Living-room router!" → "living-room-router"; unique among devices. */
static void make_id(const char *name, char *out, size_t cap)
{
    char base[33];
    size_t n = 0;
    for (const char *p = name; *p && n < sizeof base - 1; p++) {
        char ch = (char)tolower((unsigned char)*p);
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'))
            base[n++] = ch;
        else if (n && base[n - 1] != '-')
            base[n++] = '-';
    }
    while (n && base[n - 1] == '-')
        n--;
    base[n] = 0;
    if (!n)
        snprintf(base, sizeof base, "router");
    snprintf(out, cap, "%s", base);
    for (int i = 2; hub_device(out); i++)
        snprintf(out, cap, "%.28s-%d", base, i);
}

static void panel_url(conn_t *c, const http_req_t *r, char *out, size_t cap)
{
    size_t hl;
    const char *h = req_header(r, "Host", &hl);
    snprintf(out, cap, "%s://%.*s/", c->tls ? "https" : "http", h ? (int)RD_MIN(hl, 200) : 9, h ? h : "localhost");
}

void enroll_public(conn_t *c, const http_req_t *r)
{
    expire_codes();
    if (auth_locked(c)) {
        http_error(c, 429, "rate_limited", "too many wrong codes, try again later");
        return;
    }
    cJSON *in = cJSON_ParseWithLength((const char *)r->body, r->body_len);
    char code[10];
    code_t *k = NULL;
    if (normalize(msg_str(in, "code", ""), code) == 0)
        k = find_code(code);
    if (!k || k->device[0] || rd_now_ms() > k->expires) {
        auth_fail(c);
        LOGW("enroll: bad or used code from %s", c->ip);
        http_error(c, 404, "bad_code", "this code is wrong, used or expired; make a new one on the panel (Agents → Add)");
        cJSON_Delete(in);
        return;
    }
    char host[64];
    snprintf(host, sizeof host, "%s", msg_str(in, "name", ""));
    const char *name = k->name[0] ? k->name : host[0] ? host : "router";
    char id[48], secret[40];
    make_id(name, id, sizeof id);
    uint8_t rnd[24];
    rd_random(rnd, sizeof rnd);
    rd_b64_encode(rnd, sizeof rnd, secret, sizeof secret);
    for (char *p = secret; *p; p++)
        *p = *p == '+' ? '-' : *p == '/' ? '_' : *p;
    char *hash = auth_hash_password(secret);
    if (!hash || server_conf_set("devices", id, hash) < 0 || server_conf_set("device_names", id, name) < 0) {
        free(hash);
        cJSON_Delete(in);
        http_error(c, 500, "io_error", "could not write the configuration file");
        return;
    }
    device_t *d = hub_add_device(id, name, hash);
    free(hash);
    snprintf(k->device, sizeof k->device, "%s", id);
    LOGI("enroll: %s added as \"%s\" (%s, %s) from %s", id, name, host[0] ? host : "?", msg_str(in, "arch", "?"), c->ip);
    hub_device_changed(d);
    cJSON_Delete(in);

    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "device_id", id);
    cJSON_AddStringToObject(b, "device_secret", secret);
    cJSON_AddStringToObject(b, "name", name);
    cJSON_AddStringToObject(b, "ws_path", "/ws/agent");
    http_json(c, 200, b, NULL);
}

int enroll_api(conn_t *c, const http_req_t *r, const char *user)
{
    static const char pre[] = "/api/v1/enroll/codes";
    size_t pl = sizeof pre - 1;
    if (r->path_len < pl || memcmp(r->path, pre, pl))
        return 0;
    expire_codes();
    if (r->path_len == pl && req_is(r, "POST", pre)) {
        int slot = -1;
        for (int i = 0; i < MAX_CODES && slot < 0; i++)
            if (!codes[i].code[0])
                slot = i;
        if (slot < 0) {
            http_error(c, 429, "rate_limited", "too many open codes; wait a few minutes");
            return 1;
        }
        cJSON *in = cJSON_ParseWithLength((const char *)r->body, r->body_len);
        code_t *k = &codes[slot];
        do { /* 31^8 ≈ 8.5e11 codes */
            uint8_t rnd[8];
            rd_random(rnd, sizeof rnd);
            for (int i = 0, o = 0; i < 8; i++, o++) {
                if (i == 4)
                    k->code[o++] = '-';
                k->code[o] = CODE_ABC[rnd[i] % (sizeof CODE_ABC - 1)];
            }
            k->code[9] = 0;
        } while (find_code(k->code) != k);
        snprintf(k->name, sizeof k->name, "%s", msg_str(in, "name", ""));
        k->expires = rd_now_ms() + CODE_TTL_MS;
        k->device[0] = 0;
        cJSON_Delete(in);
        LOGI("enroll: %s made a code for %s", user, k->name[0] ? k->name : "a new router");
        cJSON *b = cJSON_CreateObject();
        cJSON_AddStringToObject(b, "code", k->code);
        cJSON_AddNumberToObject(b, "expires_in", CODE_TTL_MS / 1000);
        char panel[300];
        panel_url(c, r, panel, sizeof panel);
        cJSON_AddStringToObject(b, "panel", panel);
        http_json(c, 200, b, NULL);
        return 1;
    }
    if (r->path_len > pl + 1 && r->path[pl] == '/' && r->method_len == 3 && !memcmp(r->method, "GET", 3)) {
        char want[20], code[10];
        snprintf(want, sizeof want, "%.*s", (int)RD_MIN(r->path_len - pl - 1, sizeof want - 1), r->path + pl + 1);
        code_t *k = normalize(want, code) == 0 ? find_code(code) : NULL;
        cJSON *b = cJSON_CreateObject();
        if (!k) {
            cJSON_AddStringToObject(b, "state", "expired");
        } else if (k->device[0]) {
            cJSON_AddStringToObject(b, "state", "used");
            cJSON_AddStringToObject(b, "device", k->device);
            device_t *d = hub_device(k->device);
            cJSON_AddBoolToObject(b, "online", d && d->agent && d->hello);
        } else {
            cJSON_AddStringToObject(b, "state", "pending");
            cJSON_AddNumberToObject(b, "expires_in", (double)(k->expires - rd_now_ms()) / 1000);
        }
        http_json(c, 200, b, NULL);
        return 1;
    }
    http_error(c, 404, "not_found", "no such endpoint");
    return 1;
}
