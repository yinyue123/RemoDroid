/* Logins (the /login page; Basic auth for routers) + session cookies (plan §4.6). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "crypt_blowfish.h"
#include "picohttpparser.h"
#include "server.h"

struct asess {
    char id[33];
    int device;
    char name[64];
    uint64_t created_ms;
    asess_t *next;
};

typedef struct fail {
    char ip[INET6_ADDRSTRLEN];
    int count;
    uint64_t until_ms, last_ms;
    struct fail *next;
} fail_t;

static asess_t *sessions;
static fail_t *fails;

int auth_check_password(const char *hash, const char *password)
{
    if (!hash || strncmp(hash, "$2", 2) != 0)
        return 0;
    char out[64];
    if (!_crypt_blowfish_rn(password, hash, out, sizeof out))
        return 0;
    size_t n = strlen(hash);
    if (strlen(out) != n)
        return 0;
    unsigned diff = 0; /* constant-time compare */
    for (size_t i = 0; i < n; i++)
        diff |= (unsigned)(out[i] ^ hash[i]);
    return diff == 0;
}

char *auth_hash_password(const char *password)
{
    char rnd[16], setting[32], out[64];
    rd_random(rnd, sizeof rnd);
    /* Cost 10 (~80 ms): the server is single-threaded, and logins are rare thanks to
     * session cookies and the per-IP lockout. */
    if (!_crypt_gensalt_blowfish_rn("$2b$", 10, rnd, sizeof rnd, setting, sizeof setting))
        return NULL;
    if (!_crypt_blowfish_rn(password, setting, out, sizeof out))
        return NULL;
    return rd_strdup(out);
}

static uint64_t session_ttl_ms(void) { return (uint64_t)S.session_days * 86400000ull; }

static asess_t *find_session(const char *id)
{
    for (asess_t *s = sessions; s; s = s->next)
        if (!strcmp(s->id, id))
            return s;
    return NULL;
}

/* Session lifetime is checked against the *current* session_days (§4.7). */
static int session_valid(const asess_t *s) { return rd_now_ms() < s->created_ms + session_ttl_ms(); }

const char *auth_new_session(const char *name, int device)
{
    asess_t *s = calloc(1, sizeof *s);
    uint8_t r[16];
    rd_random(r, sizeof r);
    rd_hex(r, sizeof r, s->id);
    s->device = device;
    snprintf(s->name, sizeof s->name, "%s", name);
    s->created_ms = rd_now_ms();
    s->next = sessions;
    sessions = s;
    LOGI("new %s session for %s", device ? "device" : "user", name);
    return s->id;
}

void auth_drop_session(const char *id)
{
    for (asess_t **pp = &sessions; *pp; pp = &(*pp)->next) {
        if (!strcmp((*pp)->id, id)) {
            asess_t *s = *pp;
            *pp = s->next;
            free(s);
            return;
        }
    }
}

void auth_sweep(void)
{
    for (asess_t **pp = &sessions; *pp;) {
        if (!session_valid(*pp)) {
            asess_t *s = *pp;
            *pp = s->next;
            free(s);
        } else {
            pp = &(*pp)->next;
        }
    }
    uint64_t now = rd_now_ms();
    for (fail_t **pp = &fails; *pp;) {
        if ((*pp)->until_ms < now && now - (*pp)->last_ms > 3600000) {
            fail_t *f = *pp;
            *pp = f->next;
            free(f);
        } else {
            pp = &(*pp)->next;
        }
    }
}

void auth_cookie_header(const char *id, int secure, char *out, size_t cap)
{
    const char *sec = secure ? "; Secure" : ""; /* HTTPS: never sent back over plain HTTP */
    if (id)
        snprintf(out, cap, "Set-Cookie: %s=%s; HttpOnly; SameSite=Strict; Path=/; Max-Age=%llu%s\r\n", S.cookie_name, id,
                 (unsigned long long)session_ttl_ms() / 1000, sec);
    else
        snprintf(out, cap, "Set-Cookie: %s=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0%s\r\n", S.cookie_name, sec);
}

const char *auth_cookie_session(const http_req_t *r, int want_device, char *name_out, size_t name_cap,
                                uint64_t *expires_ms)
{
    size_t vl;
    const char *v = req_header(r, "Cookie", &vl);
    if (!v)
        return NULL;
    size_t cl = strlen(S.cookie_name);
    const char *end = v + vl, *p = v;
    while (p < end) {
        while (p < end && (*p == ' ' || *p == ';'))
            p++;
        const char *eq = memchr(p, '=', (size_t)(end - p));
        if (!eq)
            break;
        const char *ve = memchr(eq, ';', (size_t)(end - eq));
        if (!ve)
            ve = end;
        if ((size_t)(eq - p) == cl && !memcmp(p, S.cookie_name, cl) && ve - eq - 1 == 32) {
            char id[33];
            memcpy(id, eq + 1, 32);
            id[32] = 0;
            asess_t *s = find_session(id);
            if (s && s->device == want_device && session_valid(s)) {
                if (name_out)
                    snprintf(name_out, name_cap, "%s", s->name);
                if (expires_ms)
                    *expires_ms = s->created_ms + session_ttl_ms();
                return s->id;
            }
        }
        p = ve;
    }
    return NULL;
}

static fail_t *fail_entry(const char *ip, int create)
{
    for (fail_t *f = fails; f; f = f->next)
        if (!strcmp(f->ip, ip))
            return f;
    if (!create)
        return NULL;
    fail_t *f = calloc(1, sizeof *f);
    snprintf(f->ip, sizeof f->ip, "%s", ip);
    f->next = fails;
    fails = f;
    return f;
}

/* Sessions live in memory. Across our own re-exec (settings saved in the panel) they go
 * through a 0600 file in the state directory, so nobody is logged out by a port change. */
void auth_save_sessions(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fchmod(fileno(f), 0600);
    for (asess_t *s = sessions; s; s = s->next)
        if (session_valid(s))
            fprintf(f, "%s %d %llu %s\n", s->id, s->device, (unsigned long long)s->created_ms, s->name);
    fclose(f);
}

void auth_load_sessions(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[160];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        asess_t s = { 0 };
        unsigned long long created;
        if (sscanf(line, "%32s %d %llu %63s", s.id, &s.device, &created, s.name) != 4 || strlen(s.id) != 32)
            continue;
        s.created_ms = created;
        asess_t *p = malloc(sizeof *p);
        *p = s;
        p->next = sessions;
        sessions = p;
        n++;
    }
    fclose(f);
    unlink(path);
    if (n)
        LOGI("kept %d login session(s) across the restart", n);
}

const char *user_hash(const char *name) { return rd_conf_get(S.conf, "users", name, NULL); }

void auth_drop_sessions_for(const char *name, int device)
{
    for (asess_t **pp = &sessions; *pp;) {
        asess_t *s = *pp;
        if (s->device == device && !strcmp(s->name, name)) {
            *pp = s->next;
            free(s);
        } else {
            pp = &s->next;
        }
    }
}

int auth_locked(conn_t *c)
{
    fail_t *f = fail_entry(c->ip, 0);
    return f && f->until_ms > rd_now_ms();
}

int auth_fail(conn_t *c)
{
    uint64_t now = rd_now_ms();
    fail_t *f = fail_entry(c->ip, 1);
    f->last_ms = now;
    if (++f->count >= S.max_failed) {
        f->until_ms = now + (uint64_t)S.lockout_sec * 1000;
        f->count = 0;
        LOGW("lockout for %s (%d seconds)", c->ip, S.lockout_sec);
        return 1;
    }
    return 0;
}

/* Checks one name/password pair against [users] (or [devices]), with the per-IP lockout:
 * 1 = good, 0 = wrong (counted), -2 = locked out. */
int auth_password(conn_t *c, int device, const char *name, const char *pass)
{
    fail_t *f = fail_entry(c->ip, 0);
    if (f && f->until_ms > rd_now_ms())
        return -2;
    const char *hash = *name ? rd_conf_get(S.conf, device ? "devices" : "users", name, NULL) : NULL;
    if (!hash || !auth_check_password(hash, pass)) {
        if (!auth_fail(c))
            LOGW("failed %s login from %s", device ? "device" : "user", c->ip);
        return 0;
    }
    if (f)
        f->count = 0;
    return 1;
}

/* Routers only (agent.ini secret on /ws/agent); people log in on the /login page. */
int auth_basic(const http_req_t *r, conn_t *c, int device, char *name_out, size_t name_cap)
{
    size_t vl;
    const char *v = req_header(r, "Authorization", &vl);
    if (!v || vl < 6 || strncasecmp(v, "Basic ", 6) != 0)
        return -1;
    uint8_t dec[256];
    size_t dl = 0;
    const char *b = v + 6;
    size_t bl = vl - 6;
    while (bl && *b == ' ')
        b++, bl--;
    char *colon = NULL;
    if (rd_b64_decode(b, bl, dec, sizeof dec - 1, &dl) == 0) {
        dec[dl] = 0;
        colon = strchr((char *)dec, ':');
    }
    if (!colon) {
        if (auth_locked(c))
            return -2;
        if (!auth_fail(c))
            LOGW("failed %s login from %s", device ? "device" : "user", c->ip);
        return 0;
    }
    *colon = 0;
    int a = auth_password(c, device, (const char *)dec, colon + 1);
    if (a == 1)
        snprintf(name_out, name_cap, "%s", (const char *)dec);
    return a;
}

int auth_bearer(const http_req_t *r, conn_t *c, char *name_out, size_t name_cap, char *scopes_out, size_t scopes_cap)
{
    size_t vl;
    const char *v = req_header(r, "Authorization", &vl);
    if (!v || vl < 7 || strncasecmp(v, "Bearer ", 7) != 0)
        return -1;
    if (auth_locked(c))
        return -2;
    const char *tok = v + 7;
    size_t tl = vl - 7;
    const char *dot = memchr(tok, '.', tl);
    char name[64] = "";
    const char *entry = NULL;
    if (dot && dot > tok && (size_t)(dot - tok) < sizeof name) {
        memcpy(name, tok, (size_t)(dot - tok));
        entry = rd_conf_get(S.conf, "api_tokens", name, NULL);
    }
    uint8_t h[32];
    char hex[65];
    rd_sha256(tok, tl, h);
    rd_hex(h, sizeof h, hex);
    int valid = entry && strlen(entry) >= 64 && (!entry[64] || entry[64] == ' ' || entry[64] == '\t');
    unsigned diff = !valid;
    for (int i = 0; i < 64; i++) /* constant time over the hash */
        diff |= (unsigned char)hex[i] ^ (unsigned char)(valid ? entry[i] : '0');
    if (diff) {
        if (!auth_fail(c))
            LOGW("bad API token from %s", c->ip);
        return 0;
    }
    const char *sc = entry + 64;
    while (*sc == ' ' || *sc == '\t')
        sc++;
    snprintf(scopes_out, scopes_cap, "%s", *sc ? sc : API_SCOPES_ALL);
    snprintf(name_out, name_cap, "token:%s", name);
    return 1;
}

/* POST /login {"user", "password"} from the login page (web/login.html) → the session cookie. */
void auth_login_request(conn_t *c, const http_req_t *r)
{
    cJSON *j = cJSON_ParseWithLength((const char *)r->body, r->body_len);
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "user"));
    const char *pass = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "password"));
    if (!name || !pass || strlen(name) >= 64 || strpbrk(name, " \t\r\n")) {
        http_error(c, 400, "bad_request", "user and password required");
    } else {
        int a = auth_password(c, 0, name, pass);
        if (a == -2) {
            http_error(c, 429, "rate_limited", "too many failed logins, try again later");
        } else if (a != 1) {
            http_error(c, 401, "unauthorized", "wrong user name or password");
        } else {
            char cookie[256];
            auth_cookie_header(auth_new_session(name, 0), c->tls, cookie, sizeof cookie);
            cJSON *b = cJSON_CreateObject();
            cJSON_AddStringToObject(b, "user", name);
            http_json(c, 200, b, cookie);
        }
    }
    cJSON_Delete(j);
}
