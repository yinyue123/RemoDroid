/* OAuth 2.1 for the MCP endpoint (mcp.md §6.4): how claude.ai, ChatGPT and other apps that
 * can't send a fixed header get an API token. This server is both the protected resource
 * and its own authorization server:
 *
 *   GET  /.well-known/oauth-protected-resource/mcp/<user>/<device>   RFC 9728 (from mcp.c's 401)
 *   GET  /.well-known/oauth-authorization-server                     RFC 8414
 *   POST /oauth/register      dynamic client registration (RFC 7591); public clients, PKCE
 *   GET  /oauth/authorize     web/oauth.html: log in, then "Allow <app> to use <phone>?"
 *        GET  /api/v1/oauth/request?<the authorize query>     (logged in) what the page shows
 *        POST /api/v1/oauth/approve {query, scopes, allow}     → {"redirect": "<redirect_uri>?code=…"}
 *   POST /oauth/token         code + code_verifier → an API token ([api_tokens], user=, device=)
 *
 * The token is an ordinary API token, listed and revoked in the panel; authorizing the same
 * app for the same phone again replaces it. Clients live in <state_dir>/oauth.json, codes in
 * memory for 5 minutes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "msg.h"
#include "server.h"

#define CODE_TTL_MS (5 * 60 * 1000)
#define MAX_CLIENTS 200
#define MAX_CODES 64

static const char CORS[] = "Access-Control-Allow-Origin: *\r\n"
                           "Access-Control-Allow-Headers: Authorization, Content-Type, MCP-Protocol-Version\r\n"
                           "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";

/* ---------------- clients (<state_dir>/oauth.json) ---------------- */

static cJSON *clients; /* {"<client_id>": {"name", "redirect_uris": [...], "created"}} */

static void clients_path(char *out, size_t cap) { snprintf(out, cap, "%s/oauth.json", S.state_dir); }

static void clients_load(void)
{
    if (clients)
        return;
    char path[400];
    clients_path(path, sizeof path);
    FILE *f = fopen(path, "rb");
    rd_buf_t b;
    rd_buf_init(&b);
    if (f) {
        char chunk[4096];
        size_t n;
        while ((n = fread(chunk, 1, sizeof chunk, f)) > 0 && b.len < (4u << 20))
            rd_buf_append(&b, chunk, n);
        fclose(f);
    }
    cJSON *j = cJSON_ParseWithLength((const char *)rd_buf_data(&b), rd_buf_avail(&b));
    rd_buf_free(&b);
    clients = cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(j, "clients"))
                  ? cJSON_DetachItemFromObjectCaseSensitive(j, "clients") : cJSON_CreateObject();
    cJSON_Delete(j);
}

static int clients_save(void)
{
    char path[400], tmp[420];
    clients_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (rd_mkdirs(S.state_dir, 0700) < 0)
        return -1;
    cJSON *j = cJSON_CreateObject();
    cJSON_AddItemReferenceToObject(j, "clients", clients);
    char *s = cJSON_Print(j);
    cJSON_Delete(j);
    FILE *f = fopen(tmp, "wb");
    int ok = f && s && fputs(s, f) >= 0;
    if (f)
        ok &= fclose(f) == 0;
    free(s);
#ifdef _WIN32 /* rename() won't replace a file there */
    ok = ok && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
#else
    ok = ok && rename(tmp, path) == 0;
#endif
    if (!ok) {
        LOGE("cannot write %s", path);
        return -1;
    }
    return 0;
}

/* https://, a loopback http:// (desktop apps), or an app's own scheme (cursor://...). */
static int redirect_ok(const char *u)
{
    size_t n = strlen(u);
    if (!n || n > 500 || strpbrk(u, " \t\r\n\"<>\\#"))
        return 0;
    if (!strncmp(u, "https://", 8))
        return n > 8;
    if (!strncmp(u, "http://localhost", 16) || !strncmp(u, "http://127.0.0.1", 16) || !strncmp(u, "http://[::1]", 12))
        return 1;
    size_t sl = strspn(u, "abcdefghijklmnopqrstuvwxyz0123456789+.-");
    if (!sl || u[sl] != ':' || u[0] < 'a' || u[0] > 'z')
        return 0;
    static const char *BAD[] = { "http", "javascript", "data", "file", "vbscript", "blob", "about" };
    for (size_t i = 0; i < RD_ARRAY_LEN(BAD); i++)
        if (sl == strlen(BAD[i]) && !strncmp(u, BAD[i], sl))
            return 0;
    return 1;
}

static void oauth_error(conn_t *c, int status, const char *err, const char *desc)
{
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "error", err);
    cJSON_AddStringToObject(b, "error_description", desc);
    http_json(c, status, b, CORS);
}

static void do_register(conn_t *c, const http_req_t *r)
{
    clients_load();
    cJSON *in = cJSON_ParseWithLength((const char *)r->body, r->body_len);
    cJSON *uris = cJSON_GetObjectItemCaseSensitive(in, "redirect_uris"), *u;
    int n = 0, bad = !cJSON_IsArray(uris);
    cJSON *list = bad ? NULL : uris;
    cJSON_ArrayForEach(u, list) {
        n++;
        bad |= !cJSON_IsString(u) || !redirect_ok(u->valuestring);
    }
    if (bad || !n || n > 10) {
        oauth_error(c, 400, "invalid_redirect_uri", "redirect_uris: 1 to 10 https:// or loopback URLs");
        cJSON_Delete(in);
        return;
    }
    while (cJSON_GetArraySize(clients) >= MAX_CLIENTS) /* registration is open to anyone: keep the newest */
        cJSON_DeleteItemFromArray(clients, 0);
    uint8_t rnd[16];
    char id[40] = "c_";
    rd_random(rnd, sizeof rnd);
    rd_hex(rnd, sizeof rnd, id + 2);
    char name[64];
    snprintf(name, sizeof name, "%s", msg_str(in, "client_name", "An app"));
    for (char *p = name; *p; p++)
        if ((unsigned char)*p < 32)
            *p = ' ';
    cJSON *cl = cJSON_CreateObject();
    cJSON_AddStringToObject(cl, "name", *name ? name : "An app");
    cJSON_AddItemToObject(cl, "redirect_uris", cJSON_Duplicate(uris, 1));
    cJSON_AddNumberToObject(cl, "created", (double)rd_now_ms());
    cJSON_AddItemToObject(clients, id, cl);
    if (clients_save() < 0) {
        oauth_error(c, 500, "server_error", "could not save the client");
        cJSON_Delete(in);
        return;
    }
    LOGI("oauth: registered client %s (%s)", id, name);
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "client_id", id);
    cJSON_AddNumberToObject(b, "client_id_issued_at", (double)(rd_now_ms() / 1000));
    cJSON_AddStringToObject(b, "client_name", msg_str(cl, "name", ""));
    cJSON_AddItemToObject(b, "redirect_uris", cJSON_Duplicate(uris, 1));
    cJSON *gt = cJSON_AddArrayToObject(b, "grant_types");
    cJSON_AddItemToArray(gt, cJSON_CreateString("authorization_code"));
    cJSON *rt = cJSON_AddArrayToObject(b, "response_types");
    cJSON_AddItemToArray(rt, cJSON_CreateString("code"));
    cJSON_AddStringToObject(b, "token_endpoint_auth_method", "none");
    http_json(c, 201, b, CORS);
    cJSON_Delete(in);
}

/* ---------------- metadata ---------------- */

static void scopes_array(cJSON *o, const char *key)
{
    cJSON *a = cJSON_AddArrayToObject(o, key);
    cJSON_AddItemToArray(a, cJSON_CreateString("ui"));
    cJSON_AddItemToArray(a, cJSON_CreateString("screenshot"));
    cJSON_AddItemToArray(a, cJSON_CreateString("control"));
}

static void resource_metadata(conn_t *c, const http_req_t *r, const char *rest, size_t rl)
{
    char origin[256], res[600];
    mcp_origin(c, r, origin, sizeof origin);
    snprintf(res, sizeof res, "%s%.*s", origin, (int)RD_MIN(rl, 300), rest);
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "resource", res);
    cJSON_AddItemToArray(cJSON_AddArrayToObject(b, "authorization_servers"), cJSON_CreateString(origin));
    scopes_array(b, "scopes_supported");
    cJSON_AddItemToArray(cJSON_AddArrayToObject(b, "bearer_methods_supported"), cJSON_CreateString("header"));
    cJSON_AddStringToObject(b, "resource_name", "RemoDroid");
    char doc[300];
    snprintf(doc, sizeof doc, "%s/mcp/help", origin);
    cJSON_AddStringToObject(b, "resource_documentation", doc);
    http_json(c, 200, b, CORS);
}

static void server_metadata(conn_t *c, const http_req_t *r)
{
    char origin[256], u[320];
    mcp_origin(c, r, origin, sizeof origin);
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "issuer", origin);
    snprintf(u, sizeof u, "%s/oauth/authorize", origin);
    cJSON_AddStringToObject(b, "authorization_endpoint", u);
    snprintf(u, sizeof u, "%s/oauth/token", origin);
    cJSON_AddStringToObject(b, "token_endpoint", u);
    snprintf(u, sizeof u, "%s/oauth/register", origin);
    cJSON_AddStringToObject(b, "registration_endpoint", u);
    cJSON_AddItemToArray(cJSON_AddArrayToObject(b, "response_types_supported"), cJSON_CreateString("code"));
    cJSON_AddItemToArray(cJSON_AddArrayToObject(b, "grant_types_supported"), cJSON_CreateString("authorization_code"));
    cJSON_AddItemToArray(cJSON_AddArrayToObject(b, "code_challenge_methods_supported"), cJSON_CreateString("S256"));
    cJSON_AddItemToArray(cJSON_AddArrayToObject(b, "token_endpoint_auth_methods_supported"), cJSON_CreateString("none"));
    scopes_array(b, "scopes_supported");
    cJSON_AddBoolToObject(b, "authorization_response_iss_parameter_supported", 1);
    http_json(c, 200, b, CORS);
}

/* ---------------- authorize ---------------- */

typedef struct {
    char code[65], client_id[40], user[64], device[64], scopes[64], challenge[64];
    char *redirect_uri;
    uint64_t expires;
} code_t;

static code_t codes[MAX_CODES];

/* The authorize request, checked: everything the consent page and approve need. */
typedef struct {
    const cJSON *client;
    const char *client_id, *redirect_uri, *state, *challenge;
    char user[64], device[64], scopes[64];
    const char *redirect_err; /* set: an error to send back to the app (redirect_uri is valid) */
} authz_t;

/* Returns NULL when fine, or an error shown on the page (no valid redirect_uri to send it to). */
static const char *authz_check(const cJSON *q, const char *me, conn_t *c, const http_req_t *r, authz_t *a)
{
    clients_load();
    memset(a, 0, sizeof *a);
    a->client_id = msg_str(q, "client_id", "");
    a->client = cJSON_GetObjectItemCaseSensitive(clients, a->client_id);
    if (!*a->client_id || !a->client)
        return "This app is not registered here (unknown client_id). Remove it in the app and add the RemoDroid connector again.";
    cJSON *uris = cJSON_GetObjectItemCaseSensitive(a->client, "redirect_uris"), *u;
    a->redirect_uri = msg_str(q, "redirect_uri", NULL);
    if (!a->redirect_uri && cJSON_GetArraySize(uris) == 1)
        a->redirect_uri = cJSON_GetArrayItem(uris, 0)->valuestring;
    int known = 0;
    cJSON_ArrayForEach(u, uris) known |= a->redirect_uri && cJSON_IsString(u) && !strcmp(u->valuestring, a->redirect_uri);
    if (!known)
        return "The app asked to return to an address it did not register (redirect_uri).";
    a->state = msg_str(q, "state", NULL);
    a->challenge = msg_str(q, "code_challenge", "");
    if (strcmp(msg_str(q, "response_type", ""), "code"))
        a->redirect_err = "unsupported_response_type";
    else if (strlen(a->challenge) < 43 || strlen(a->challenge) > 128 || strcmp(msg_str(q, "code_challenge_method", ""), "S256"))
        a->redirect_err = "invalid_request"; /* PKCE with S256 is required */
    /* scope: "ui screenshot control" (any subset); none asked = all */
    int want[3] = { 0 }, any = 0;
    static const char *ALL[] = { "ui", "screenshot", "control" };
    char sc[200];
    snprintf(sc, sizeof sc, "%s", msg_str(q, "scope", ""));
    for (char *t = strtok(sc, " ,+"); t; t = strtok(NULL, " ,+"))
        for (int i = 0; i < 3; i++)
            if (!strcmp(t, ALL[i]))
                want[i] = any = 1;
    for (int i = 0; i < 3; i++) {
        if (!any || want[i]) {
            size_t l = strlen(a->scopes);
            snprintf(a->scopes + l, sizeof a->scopes - l, "%s%s", l ? "," : "", ALL[i]);
        }
    }
    /* resource: the MCP URL the app wants, https://host/mcp/<user>/<device> (RFC 8707) */
    const char *res = msg_str(q, "resource", "");
    char origin[256];
    mcp_origin(c, r, origin, sizeof origin);
    snprintf(a->user, sizeof a->user, "%s", me);
    const char *p = strstr(res, "/mcp/");
    char ru[64], rd[64];
    if (*res && p && mcp_path(p, strlen(p), ru, sizeof ru, rd, sizeof rd)) {
        if (strcmp(ru, me))
            return "This connector is for another RemoDroid user. Log out, then log in as that user.";
        if (strcmp(rd, "devices")) {
            if (!hub_device(rd))
                return "The phone in this connector’s URL is not on this server any more.";
            snprintf(a->device, sizeof a->device, "%s", rd);
        }
    }
    return NULL;
}

static void url_add(rd_buf_t *b, const char *key, const char *val)
{
    static const char HEX[] = "0123456789ABCDEF";
    rd_buf_printf(b, "%c%s=", memchr(rd_buf_data(b), '?', rd_buf_avail(b)) ? '&' : '?', key);
    for (const unsigned char *p = (const unsigned char *)val; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("-._~", *p))
            rd_buf_append(b, p, 1);
        else
            rd_buf_printf(b, "%%%c%c", HEX[*p >> 4], HEX[*p & 15]);
    }
}

/* The redirect back to the app: code or error, state, iss (RFC 9207). */
static char *back_url(const authz_t *a, const char *code, const char *err, const char *origin)
{
    rd_buf_t b;
    rd_buf_init(&b);
    rd_buf_append(&b, a->redirect_uri, strlen(a->redirect_uri));
    if (code)
        url_add(&b, "code", code);
    else
        url_add(&b, "error", err);
    if (a->state)
        url_add(&b, "state", a->state);
    url_add(&b, "iss", origin);
    rd_buf_append(&b, "", 1);
    char *s = rd_strdup((const char *)rd_buf_data(&b));
    rd_buf_free(&b);
    return s;
}

static void authorize_api(conn_t *c, const http_req_t *r, const char *me, int approve)
{
    cJSON *body = NULL, *q;
    if (approve) {
        if (!http_origin_ok(r)) {
            http_error(c, 403, "forbidden", "origin not allowed");
            return;
        }
        body = cJSON_ParseWithLength((const char *)r->body, r->body_len);
        const char *qs = msg_str(body, "query", "");
        if (*qs == '?')
            qs++;
        q = msg_query_json(qs, strlen(qs));
    } else {
        q = msg_query_json(r->query, r->query_len);
    }
    authz_t a;
    const char *bad = authz_check(q, me, c, r, &a);
    char origin[256];
    mcp_origin(c, r, origin, sizeof origin);
    if (bad) {
        http_error(c, 400, "invalid_request", bad);
    } else if (!approve) {
        cJSON *b = cJSON_CreateObject();
        cJSON_AddStringToObject(b, "client", msg_str(a.client, "name", "An app"));
        const char *ru = a.redirect_uri, *hs = strstr(ru, "://");
        char host[200];
        snprintf(host, sizeof host, "%.*s", hs ? (int)strcspn(hs + 3, "/?:") : (int)strcspn(ru, ":"), hs ? hs + 3 : ru);
        cJSON_AddStringToObject(b, "redirect_host", host);
        cJSON_AddStringToObject(b, "user", me);
        cJSON_AddStringToObject(b, "scopes", a.scopes);
        device_t *d = *a.device ? hub_device(a.device) : NULL;
        if (d) {
            cJSON *dj = cJSON_AddObjectToObject(b, "device");
            cJSON_AddStringToObject(dj, "id", d->id);
            cJSON_AddStringToObject(dj, "name", d->name);
        }
        if (a.redirect_err)
            cJSON_AddStringToObject(b, "error", a.redirect_err);
        http_json(c, 200, b, NULL);
    } else {
        int allow = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(body, "allow"));
        char *url = NULL;
        if (a.redirect_err || !allow) {
            url = back_url(&a, NULL, a.redirect_err ? a.redirect_err : "access_denied", origin);
        } else {
            /* the user may narrow the scopes on the page, never widen them */
            char sc[64] = "";
            const char *pick = msg_str(body, "scopes", a.scopes);
            static const char *ALL[] = { "ui", "screenshot", "control" };
            for (int i = 0; i < 3; i++) {
                char w[24];
                snprintf(w, sizeof w, ",%s,", ALL[i]);
                char have[80], want[80];
                snprintf(have, sizeof have, ",%s,", a.scopes);
                snprintf(want, sizeof want, ",%s,", pick);
                if (strstr(have, w) && strstr(want, w)) {
                    size_t l = strlen(sc);
                    snprintf(sc + l, sizeof sc - l, "%s%s", l ? "," : "", ALL[i]);
                }
            }
            code_t *k = &codes[0];
            uint64_t now = rd_mono_ms();
            for (int i = 0; i < MAX_CODES; i++) /* a free or expired slot, else the oldest */
                if (!codes[i].expires || codes[i].expires < now || codes[i].expires < k->expires)
                    k = &codes[i];
            free(k->redirect_uri);
            memset(k, 0, sizeof *k);
            uint8_t rnd[32];
            rd_random(rnd, sizeof rnd);
            rd_hex(rnd, sizeof rnd, k->code);
            snprintf(k->client_id, sizeof k->client_id, "%s", a.client_id);
            snprintf(k->user, sizeof k->user, "%s", me);
            snprintf(k->device, sizeof k->device, "%s", a.device);
            snprintf(k->scopes, sizeof k->scopes, "%s", *sc ? sc : "ui");
            snprintf(k->challenge, sizeof k->challenge, "%s", a.challenge);
            k->redirect_uri = rd_strdup(a.redirect_uri);
            k->expires = now + CODE_TTL_MS;
            url = back_url(&a, k->code, NULL, origin);
            LOGI("oauth: %s allowed %s (%s) on %s", me, msg_str(a.client, "name", "?"), k->scopes, *a.device ? a.device : "every device");
        }
        cJSON *b = cJSON_CreateObject();
        cJSON_AddStringToObject(b, "redirect", url);
        http_json(c, 200, b, NULL);
        free(url);
    }
    cJSON_Delete(q);
    cJSON_Delete(body);
}

int oauth_api(conn_t *c, const http_req_t *r, const char *user)
{
    if (req_is(r, "GET", "/api/v1/oauth/request"))
        authorize_api(c, r, user, 0);
    else if (req_is(r, "POST", "/api/v1/oauth/approve"))
        authorize_api(c, r, user, 1);
    else
        return 0;
    return 1;
}

/* ---------------- token ---------------- */

static void b64url_sha256(const char *s, char out[48])
{
    uint8_t h[32];
    rd_sha256(s, strlen(s), h);
    rd_b64_encode(h, sizeof h, out, 48);
    for (char *p = out; *p; p++)
        *p = *p == '+' ? '-' : *p == '/' ? '_' : *p;
    out[strcspn(out, "=")] = 0;
}

static void do_token(conn_t *c, const http_req_t *r)
{
    size_t tl;
    const char *ct = req_header(r, "Content-Type", &tl);
    cJSON *in = ct && tl >= 16 && !strncasecmp(ct, "application/json", 16)
                    ? cJSON_ParseWithLength((const char *)r->body, r->body_len)
                    : msg_query_json((const char *)r->body, r->body_len);
    const char *grant = msg_str(in, "grant_type", ""), *code = msg_str(in, "code", "");
    code_t *k = NULL;
    uint64_t now = rd_mono_ms();
    for (int i = 0; i < MAX_CODES && *code; i++)
        if (codes[i].expires && !strcmp(codes[i].code, code))
            k = &codes[i];
    char want[48] = "";
    if (k)
        b64url_sha256(msg_str(in, "code_verifier", ""), want);
    const char *ru = msg_str(in, "redirect_uri", NULL);
    if (strcmp(grant, "authorization_code")) {
        oauth_error(c, 400, "unsupported_grant_type", "only authorization_code");
    } else if (!k || k->expires < now || strcmp(k->client_id, msg_str(in, "client_id", k->client_id)) ||
               (ru && strcmp(ru, k->redirect_uri)) || strcmp(want, k->challenge)) {
        if (k) { /* one try per code */
            free(k->redirect_uri);
            memset(k, 0, sizeof *k);
        }
        LOGW("oauth: bad code or verifier from %s", c->ip); /* codes are 256 random bits: nothing to guess */
        oauth_error(c, 400, "invalid_grant", "the code is wrong, used or expired, or the code_verifier does not match");
    } else {
        /* one token per app, user and phone: authorizing again replaces it */
        clients_load();
        const cJSON *cl = cJSON_GetObjectItemCaseSensitive(clients, k->client_id);
        char slug[16] = "", seed[200], name[48], hx[65], tok[96], val[260];
        const char *cn = msg_str(cl, "name", "app");
        for (size_t i = 0, j = 0; cn[i] && j < sizeof slug - 1; i++) {
            char ch = cn[i];
            if (ch >= 'A' && ch <= 'Z')
                ch = (char)(ch + 32);
            if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'))
                slug[j++] = ch;
            else if (j && slug[j - 1] != '-')
                slug[j++] = '-';
        }
        size_t sl = strlen(slug);
        while (sl && slug[sl - 1] == '-')
            slug[--sl] = 0;
        uint8_t h[32];
        snprintf(seed, sizeof seed, "%s|%s|%s", k->client_id, k->user, k->device);
        rd_sha256(seed, strlen(seed), h);
        rd_hex(h, 4, hx);
        snprintf(name, sizeof name, "oauth-%s-%s", *slug ? slug : "app", hx);
        auth_token_new(name, tok, sizeof tok, hx);
        snprintf(val, sizeof val, "%s %s user=%s%s%s", hx, k->scopes, k->user, *k->device ? " device=" : "", k->device);
        char scope_sp[64];
        snprintf(scope_sp, sizeof scope_sp, "%s", k->scopes);
        for (char *p = scope_sp; *p; p++)
            if (*p == ',')
                *p = ' ';
        free(k->redirect_uri);
        memset(k, 0, sizeof *k);
        if (server_conf_set("api_tokens", name, val) < 0) {
            oauth_error(c, 500, "server_error", "could not save the token");
        } else {
            LOGI("oauth: issued token %s", name);
            cJSON *b = cJSON_CreateObject();
            cJSON_AddStringToObject(b, "access_token", tok);
            cJSON_AddStringToObject(b, "token_type", "Bearer");
            cJSON_AddStringToObject(b, "scope", scope_sp);
            http_json(c, 200, b, CORS); /* every answer is Cache-Control: no-store */
        }
    }
    cJSON_Delete(in);
}

/* ---------------- routes (before the login check) ---------------- */

int oauth_handle(conn_t *c, const http_req_t *r)
{
    static const char PR[] = "/.well-known/oauth-protected-resource", AS[] = "/.well-known/oauth-authorization-server";
    int get = r->method_len == 3 && !memcmp(r->method, "GET", 3);
    int options = r->method_len == 7 && !memcmp(r->method, "OPTIONS", 7);
    int pr = r->path_len >= sizeof PR - 1 && !memcmp(r->path, PR, sizeof PR - 1);
    int as = r->path_len >= sizeof AS - 1 && !memcmp(r->path, AS, sizeof AS - 1);
    int reg = req_is(r, "POST", "/oauth/register") || (options && r->path_len == 15 && !memcmp(r->path, "/oauth/register", 15));
    int tok = req_is(r, "POST", "/oauth/token") || (options && r->path_len == 12 && !memcmp(r->path, "/oauth/token", 12));
    if (!pr && !as && !reg && !tok)
        return 0;
    if (options) { /* CORS preflight from browser-based clients */
        http_respond(c, 204, NULL, "", 0, CORS);
    } else if (pr && get) {
        resource_metadata(c, r, r->path + sizeof PR - 1, r->path_len - (sizeof PR - 1));
    } else if (as && get) {
        server_metadata(c, r); /* also at /.well-known/oauth-authorization-server/<path> */
    } else if (reg) {
        do_register(c, r);
    } else if (tok) {
        do_token(c, r);
    } else {
        http_error(c, 405, "bad_request", "method not allowed");
    }
    return 1;
}
