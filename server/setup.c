/* First-run setup wizard (web/wizard.html) and its API.
 *
 * A fresh install has no [users]: the server is in setup mode and serves only
 * the wizard, guarded by a one-time token printed in the log and written to
 * <state_dir>/setup-token (install_server.sh prints the link). The wizard picks
 * the ports, domain (automatic HTTPS), admin account and STUN/TURN; saving
 * writes server.ini and the server re-execs itself on the new ports. Later the
 * same page edits the settings, behind the normal login. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "msg.h"
#include "server.h"

static char token[32];

static void token_path(char *out, size_t cap) { snprintf(out, cap, "%s/setup-token", S.state_dir); }

static int listen_port(const char *addr, int def)
{
    const char *c = addr ? strrchr(addr, ':') : NULL;
    int p = c ? atoi(c + 1) : 0;
    return p > 0 ? p : def;
}

void setup_init(void)
{
    char path[400];
    token_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (f) {
        if (!fgets(token, sizeof token, f))
            token[0] = 0;
        token[strcspn(token, "\r\n")] = 0;
        fclose(f);
    }
    if (strlen(token) < 12) {
        static const char abc[] = "23456789abcdefghjkmnpqrstuvwxyz";
        uint8_t r[12];
        rd_random(r, sizeof r);
        for (int i = 0; i < 12; i++)
            token[i] = abc[r[i] % (sizeof abc - 1)];
        token[12] = 0;
        if (rd_mkdirs(S.state_dir, 0700) == 0 && (f = fopen(path, "w"))) {
            fchmod(fileno(f), 0600);
            fprintf(f, "%s\n", token);
            fclose(f);
        } else {
            LOGW("setup: cannot write %s: %s", path, strerror(errno));
        }
    }
    int port = listen_port(rd_conf_get(S.conf, "http", "listen", "0.0.0.0:80"), 80);
    LOGW("setup: no admin account yet. Open http://<this server's address>:%d/wizard.html?token=%s", port, token);
}

/* ---------------- reading and writing the settings ---------------- */

static cJSON *current(const http_req_t *r, const char *user)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "platform", "server");
    cJSON_AddStringToObject(j, "version", RD_VERSION);
    cJSON_AddBoolToObject(j, "configured", !S.setup);
    cJSON_AddStringToObject(j, "config_file", S.conf_path);
    cJSON_AddNumberToObject(j, "http_port", listen_port(rd_conf_get(S.conf, "http", "listen", ""), 80));
    char dom[256];
    snprintf(dom, sizeof dom, "%s", rd_conf_get(S.conf, "acme", "domains", ""));
    dom[strcspn(dom, ", ")] = 0;
    cJSON_AddStringToObject(j, "domain", dom);
    cJSON_AddStringToObject(j, "email", rd_conf_get(S.conf, "acme", "email", ""));
    cJSON_AddNumberToObject(j, "tls_port", listen_port(rd_conf_get(S.conf, "tls", "listen", ""), 443));
    cJSON_AddBoolToObject(j, "redirect_http", strcmp(rd_conf_get(S.conf, "tls", "plain_http", "redirect"), "allow") != 0);
    cJSON_AddStringToObject(j, "username", user ? user : "");
    const char *tl = rd_conf_get(S.conf, "turn", "listen", "");
    cJSON_AddBoolToObject(j, "turn_enabled", *tl != 0);
    cJSON_AddNumberToObject(j, "turn_port", listen_port(tl, 3478));
    cJSON_AddStringToObject(j, "public_ip", rd_conf_get(S.conf, "turn", "public_ip", ""));
    cJSON_AddStringToObject(j, "ice_extra", rd_conf_get(S.conf, "ice", "extra", ""));
    size_t hl;
    const char *h = req_header(r, "Host", &hl);
    char host[256] = "";
    if (h && hl < sizeof host) {
        memcpy(host, h, hl);
        host[hl] = 0;
        char *colon = strrchr(host, ':'), *br = strrchr(host, ']');
        if (colon && (!br || colon > br))
            *colon = 0;
    }
    cJSON_AddStringToObject(j, "host_hint", host); /* the address the browser used: a good public IP guess */
    return j;
}

static int valid_port(int p) { return p >= 1 && p <= 65535; }

static int valid_name(const char *s, const char *extra, size_t max)
{
    size_t n = strlen(s);
    if (!n || n > max)
        return 0;
    for (const char *p = s; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr(extra, *p)))
            return 0;
    return 1;
}

static int valid_ipv4(const char *s)
{
    int a, b, c, d;
    char tail;
    return sscanf(s, "%d.%d.%d.%d%c", &a, &b, &c, &d, &tail) == 4 && a >= 0 && a < 256 && b >= 0 && b < 256 &&
           c >= 0 && c < 256 && d >= 0 && d < 256;
}

/* Applies the wizard's form; user = the logged-in admin (NULL during setup). */
static void apply(conn_t *c, cJSON *in, const char *user)
{
    int http_port = (int)msg_num(in, "http_port", 0), tls_port = (int)msg_num(in, "tls_port", 443);
    int turn_port = (int)msg_num(in, "turn_port", 3478), turn_on = msg_bool(in, "turn_enabled", 1);
    const char *domain = msg_str(in, "domain", ""), *email = msg_str(in, "email", "");
    const char *uname = msg_str(in, "username", ""), *pw = msg_str(in, "password", "");
    const char *pub = msg_str(in, "public_ip", ""), *extra = msg_str(in, "ice_extra", "");
    const char *err = NULL;
    if (!valid_port(http_port))
        err = "the web port must be 1-65535";
    else if (*domain && (!valid_name(domain, ".-", 253) || !strchr(domain, '.')))
        err = "the domain looks wrong (e.g. remodroid.example.com)";
    else if (*domain && (!valid_port(tls_port) || tls_port == http_port))
        err = "the HTTPS port must be 1-65535 and differ from the web port";
    else if (*email && (strchr(email, ' ') || !strchr(email, '@') || strlen(email) > 200))
        err = "the email address looks wrong";
    else if (!valid_name(uname, "._-", 32))
        err = "the username may use letters, digits, '.', '_' and '-' (up to 32)";
    else if ((!user || strcmp(uname, user)) && !*pw)
        err = "choose a password";
    else if (*pw && strlen(pw) < 8)
        err = "the password needs at least 8 characters";
    else if (turn_on && !valid_port(turn_port))
        err = "the STUN/TURN port must be 1-65535";
    else if (turn_on && !valid_ipv4(pub))
        err = "the public IPv4 address is needed for STUN/TURN (e.g. 203.0.113.10)";
    else if (strchr(extra, '\n') || strlen(extra) > 1000)
        err = "the extra ICE servers must be one line";
    if (err) {
        http_error(c, 400, "bad_request", err);
        return;
    }

    char v[1100];
    int ok = 1;
    snprintf(v, sizeof v, "0.0.0.0:%d", http_port);
    ok &= server_conf_set("http", "listen", v) == 0;
    if (*domain) {
        ok &= server_conf_set("acme", "domains", domain) == 0;
        ok &= server_conf_set("acme", "email", *email ? email : NULL) == 0;
        snprintf(v, sizeof v, "0.0.0.0:%d", tls_port);
        ok &= server_conf_set("tls", "listen", v) == 0;
        ok &= server_conf_set("tls", "plain_http", msg_bool(in, "redirect_http", 1) ? "redirect" : "allow") == 0;
    } else {
        ok &= server_conf_set("acme", "domains", NULL) == 0;
        if (!*rd_conf_get(S.conf, "tls", "cert", "")) /* keep HTTPS only for a hand-installed certificate */
            ok &= server_conf_set("tls", "listen", NULL) == 0;
    }
    if (*pw) {
        char *hash = auth_hash_password(pw);
        ok &= hash && server_conf_set("users", uname, hash) == 0;
        free(hash);
    }
    if (user && strcmp(uname, user)) { /* renamed: the old account goes */
        ok &= server_conf_set("users", user, NULL) == 0;
        auth_drop_sessions_for(user, 0);
    }
    if (turn_on) {
        snprintf(v, sizeof v, "0.0.0.0:%d", turn_port);
        ok &= server_conf_set("turn", "listen", v) == 0;
        ok &= server_conf_set("turn", "public_ip", pub) == 0;
        const char *h = *domain ? domain : pub;
        snprintf(v, sizeof v, "stun:%s:%d, turn:%s:%d?transport=udp", h, turn_port, h, turn_port);
        ok &= server_conf_set("turn", "urls", v) == 0;
        if (!*rd_conf_get(S.conf, "turn", "shared_secret", "")) {
            uint8_t r[24];
            char s[40];
            rd_random(r, sizeof r);
            rd_b64_encode(r, sizeof r, s, sizeof s);
            for (char *p = s; *p; p++)
                *p = *p == '+' ? '-' : *p == '/' ? '_' : *p;
            ok &= server_conf_set("turn", "shared_secret", s) == 0;
        }
    } else {
        ok &= server_conf_set("turn", "listen", NULL) == 0;
        ok &= server_conf_set("turn", "urls", NULL) == 0;
    }
    ok &= server_conf_set("ice", "extra", *extra ? extra : NULL) == 0;
    if (!ok) {
        http_error(c, 500, "io_error", "could not write the configuration file; check its permissions");
        return;
    }
    LOGI("setup: settings saved by %s (web port %d%s%s)", user ? user : "the setup wizard", http_port,
         *domain ? ", domain " : "", domain);
    if (!user) {
        char path[400];
        token_path(path, sizeof path);
        unlink(path);
    }
    cJSON *b = cJSON_CreateObject();
    cJSON_AddBoolToObject(b, "ok", 1);
    cJSON_AddNumberToObject(b, "restart_ms", 1500);
    http_json(c, 200, b, NULL);
    server_restart_soon();
}

static void handle(conn_t *c, const http_req_t *r, const char *user)
{
    if (req_is(r, "GET", "/api/v1/setup")) {
        http_json(c, 200, current(r, user), NULL);
    } else if (req_is(r, "POST", "/api/v1/setup")) {
        cJSON *in = cJSON_ParseWithLength((const char *)r->body, r->body_len);
        if (!cJSON_IsObject(in))
            http_error(c, 400, "bad_request", "expected a JSON object");
        else
            apply(c, in, user);
        cJSON_Delete(in);
    } else {
        http_error(c, 404, "not_found", "no such endpoint");
    }
}

void setup_api(conn_t *c, const http_req_t *r, const char *user) { handle(c, r, user); }

static int token_ok(conn_t *c, const http_req_t *r)
{
    size_t tl;
    const char *t = req_header(r, "X-Setup-Token", &tl);
    size_t n = strlen(token);
    unsigned diff = !t || tl != n;
    for (size_t i = 0; t && i < n && i < tl; i++)
        diff |= (unsigned)(t[i] ^ token[i]);
    return !diff;
}

void setup_request(conn_t *c, const http_req_t *r)
{
    if (r->path_len >= 13 && !memcmp(r->path, "/api/v1/setup", 13)) {
        if (auth_locked(c)) {
            http_error(c, 429, "rate_limited", "too many wrong setup codes, try again later");
        } else if (!token_ok(c, r)) {
            auth_fail(c);
            http_error(c, 403, "forbidden", "wrong or missing setup code (see the server log or setup-token file)");
        } else {
            handle(c, r, NULL);
        }
        return;
    }
    if ((r->path_len >= 5 && !memcmp(r->path, "/api/", 5)) || (r->path_len >= 4 && !memcmp(r->path, "/ws/", 4))) {
        http_error(c, 503, "setup_required", "finish the setup wizard first (/wizard.html)");
        return;
    }
    if (r->path_len == 1) { /* "/" → the wizard, keeping ?token=… */
        char extra[400];
        snprintf(extra, sizeof extra, "Location: /wizard.html%s%.*s\r\n", r->query_len ? "?" : "", (int)r->query_len,
                 r->query);
        http_respond(c, 302, NULL, "", 0, extra);
        return;
    }
    http_serve_static(c, r, NULL); /* the page and its files hold no secrets */
}
