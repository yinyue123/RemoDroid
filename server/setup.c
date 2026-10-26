/* First-run setup wizard (web/wizard.html) and its API.
 *
 * A fresh install has no [users]: the server is in setup mode and serves only
 * the wizard, guarded by a one-time token printed in the log and written to
 * <state_dir>/setup-token (install_server.sh prints the link). The wizard picks
 * the ports, domain (automatic HTTPS), admin account and STUN/TURN; saving
 * writes server.ini and the server re-execs itself on the new ports.
 *
 * After setup, the panel's Server tab (web/panel.html) edits one setting at a
 * time through /api/v1/settings: a PATCH carries only what changed, and the
 * server restarts only when a listener has to move (ports, domain, TURN). */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    char tlu[65] = ""; /* the fixed TURN login's name ([turn] users = name:password, …); never its password */
    sscanf(rd_conf_get(S.conf, "turn", "users", ""), " %64[^:, ]", tlu);
    cJSON_AddStringToObject(j, "turn_login", tlu);
    cJSON_AddStringToObject(j, "ice_extra", rd_conf_get(S.conf, "ice", "extra", ""));
    size_t hl = 0;
    const char *h = r ? req_header(r, "Host", &hl) : NULL;
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

cJSON *setup_current(const char *user) { return current(NULL, user); }

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

/* Writes the wizard's form to server.ini; user = the admin it is applied as (NULL during
 * setup). NULL when saved, else what is wrong (*status: 400 the form, 500 the file). The
 * new settings take effect when the server restarts. */
const char *setup_save(cJSON *in, const char *user, int *status)
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
    *status = 400;
    if (err)
        return err;

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
    *status = 500;
    if (!ok)
        return "could not write the configuration file; check its permissions";
    LOGI("setup: settings saved by %s (web port %d%s%s)", user ? user : "the setup wizard", http_port,
         *domain ? ", domain " : "", domain);
    if (!user) {
        char path[400];
        token_path(path, sizeof path);
        unlink(path);
    }
    return NULL;
}

static void apply(conn_t *c, cJSON *in, const char *user)
{
    int status;
    const char *err = setup_save(in, user, &status);
    if (err) {
        http_error(c, status, status == 500 ? "io_error" : "bad_request", err);
        return;
    }
    cJSON *b = cJSON_CreateObject();
    cJSON_AddBoolToObject(b, "ok", 1);
    cJSON_AddNumberToObject(b, "restart_ms", 1500);
    http_json(c, 200, b, NULL);
    server_restart_soon();
}

/* ---------------- the panel's Server tab: one setting at a time ---------------- */

/* [ice] extra = "url [user credential], url, …" (hub.c: extra_ice) */
typedef struct {
    char url[256], user[129], cred[129];
} ice_entry_t;

static int parse_extra(const char *line, ice_entry_t *out, int max)
{
    char *list = rd_strdup(line), *save = NULL;
    int n = 0;
    for (char *e = strtok_r(list, ",", &save); e && n < max; e = strtok_r(NULL, ",", &save)) {
        char *s2 = NULL, *url = strtok_r(e, " \t", &s2), *u = strtok_r(NULL, " \t", &s2), *pw = strtok_r(NULL, " \t", &s2);
        if (!url)
            continue;
        memset(&out[n], 0, sizeof out[n]);
        snprintf(out[n].url, sizeof out[n].url, "%s", url);
        if (u && pw) {
            snprintf(out[n].user, sizeof out[n].user, "%s", u);
            snprintf(out[n].cred, sizeof out[n].cred, "%s", pw);
        }
        n++;
    }
    free(list);
    return n;
}

static int ice_token_ok(const char *s, size_t max)
{
    size_t n = strlen(s);
    if (n > max)
        return 0;
    for (const char *p = s; *p; p++)
        if (*p <= ' ' || *p == ',' || *p == 127)
            return 0;
    return 1;
}

static cJSON *settings_get(const http_req_t *r, const char *user)
{
    cJSON *j = current(r, user);
    cJSON_DeleteItemFromObjectCaseSensitive(j, "ice_extra");
    cJSON *arr = cJSON_AddArrayToObject(j, "ice_servers");
    ice_entry_t e[32];
    int n = parse_extra(rd_conf_get(S.conf, "ice", "extra", ""), e, 32);
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "url", e[i].url);
        cJSON_AddStringToObject(o, "username", e[i].user);
        cJSON_AddBoolToObject(o, "has_credential", e[i].cred[0] != 0); /* never sent back */
        cJSON_AddItemToArray(arr, o);
    }
    /* this server's own STUN/TURN, as handed to phones; the panel lists them with the others */
    cJSON *bi = cJSON_AddArrayToObject(j, "builtin_ice");
    if (*rd_conf_get(S.conf, "turn", "listen", "")) {
        char *list = rd_strdup(rd_conf_get(S.conf, "turn", "urls", "")), *save = NULL;
        for (char *u = strtok_r(list, ", ", &save); u; u = strtok_r(NULL, ", ", &save))
            cJSON_AddItemToArray(bi, cJSON_CreateString(u));
        free(list);
    }
    cJSON_AddBoolToObject(j, "https", tls_ready());
    return j;
}

static int set_if_changed(const char *sec, const char *key, const char *val, int *changed)
{
    const char *cur = rd_conf_get(S.conf, sec, key, NULL);
    if ((!val && !cur) || (val && cur && !strcmp(val, cur)))
        return 0;
    *changed = 1;
    return server_conf_set(sec, key, val);
}

/* PATCH /api/v1/settings: only the fields present change; the rest stays as it is. */
static void settings_patch(conn_t *c, cJSON *in, const char *user)
{
#define HAS(k) (cJSON_GetObjectItemCaseSensitive(in, k) != NULL)
    int http_port = listen_port(rd_conf_get(S.conf, "http", "listen", ""), 80);
    int tls_port = listen_port(rd_conf_get(S.conf, "tls", "listen", ""), 443);
    const char *tl = rd_conf_get(S.conf, "turn", "listen", "");
    int turn_on = *tl != 0, turn_port = listen_port(tl, 3478);
    int redirect = strcmp(rd_conf_get(S.conf, "tls", "plain_http", "redirect"), "allow") != 0;
    char domain[256], email[256], pub[64];
    snprintf(domain, sizeof domain, "%s", rd_conf_get(S.conf, "acme", "domains", ""));
    domain[strcspn(domain, ", ")] = 0;
    snprintf(email, sizeof email, "%s", rd_conf_get(S.conf, "acme", "email", ""));
    snprintf(pub, sizeof pub, "%s", rd_conf_get(S.conf, "turn", "public_ip", ""));

    if (HAS("http_port")) http_port = (int)msg_num(in, "http_port", 0);
    if (HAS("tls_port")) tls_port = (int)msg_num(in, "tls_port", 0);
    if (HAS("turn_port")) turn_port = (int)msg_num(in, "turn_port", 0);
    if (HAS("turn_enabled")) turn_on = msg_bool(in, "turn_enabled", 1);
    if (HAS("redirect_http")) redirect = msg_bool(in, "redirect_http", 1);
    if (HAS("domain")) snprintf(domain, sizeof domain, "%s", msg_str(in, "domain", ""));
    if (HAS("email")) snprintf(email, sizeof email, "%s", msg_str(in, "email", ""));
    if (HAS("public_ip")) snprintf(pub, sizeof pub, "%s", msg_str(in, "public_ip", ""));
    const char *uname = msg_str(in, "username", user), *pw = msg_str(in, "password", "");
    int account = HAS("username") || HAS("password");

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
    else if (HAS("password") && strlen(pw) < 8)
        err = "the password needs at least 8 characters";
    else if (turn_on && !valid_port(turn_port))
        err = "the STUN/TURN port must be 1-65535";
    else if (turn_on && !valid_ipv4(pub))
        err = "the public IPv4 address is needed for STUN/TURN (e.g. 203.0.113.10)";
    if (err) {
        http_error(c, 400, "bad_request", err);
        return;
    }
    const char *old_hash = rd_conf_get(S.conf, "users", user, NULL);
    if (account) { /* changing the login needs the current password, not just a session */
        if (auth_locked(c)) {
            http_error(c, 429, "rate_limited", "too many wrong passwords, try again later");
            return;
        }
        if (!auth_check_password(old_hash, msg_str(in, "current_password", ""))) {
            auth_fail(c);
            http_error(c, 403, "wrong_password", "the current password is wrong");
            return;
        }
        if (strcmp(uname, user) && rd_conf_get(S.conf, "users", uname, NULL)) {
            http_error(c, 409, "bad_request", "that username is taken");
            return;
        }
    }

    /* the built-in TURN's fixed login, for other servers' viewers and agents: {username, password};
     * an empty username removes it, an empty password keeps the one this name has */
    char tlogin[300] = "";
    cJSON *tl_in = cJSON_GetObjectItemCaseSensitive(in, "turn_login");
    if (tl_in) {
        const char *lu = msg_str(tl_in, "username", ""), *lp = msg_str(tl_in, "password", "");
        char oldp[160] = "", oldu[65] = "";
        sscanf(rd_conf_get(S.conf, "turn", "users", ""), " %64[^:, ]:%159[^, ]", oldu, oldp);
        if (*lu && !*lp && !strcmp(lu, oldu))
            lp = oldp;
        if (*lu && (!ice_token_ok(lu, 64) || strchr(lu, ':') || !*lp || !ice_token_ok(lp, 128))) {
            http_error(c, 400, "bad_request", "the TURN login needs a name (no spaces, commas or colons) and a password (no spaces or commas)");
            return;
        }
        if (*lu)
            snprintf(tlogin, sizeof tlogin, "%s:%s", lu, lp);
    }

    /* the ICE list: entries without a credential keep the one they had ("was" = their old URL) */
    char extra[2048] = "";
    cJSON *list = cJSON_GetObjectItemCaseSensitive(in, "ice_servers");
    if (list) {
        if (!cJSON_IsArray(list) || cJSON_GetArraySize(list) > 32) {
            http_error(c, 400, "bad_request", "ice_servers must be a list of up to 32 servers");
            return;
        }
        ice_entry_t old[32];
        int nold = parse_extra(rd_conf_get(S.conf, "ice", "extra", ""), old, 32);
        size_t o = 0;
        cJSON *it;
        cJSON_ArrayForEach(it, list) {
            const char *url = msg_str(it, "url", ""), *u = msg_str(it, "username", ""), *cr = msg_str(it, "credential", "");
            const char *was = msg_str(it, "was", url);
            if ((strncmp(url, "stun:", 5) && strncmp(url, "turn:", 5) && strncmp(url, "turns:", 6)) || strlen(url) < 7 ||
                !ice_token_ok(url, 250) || !ice_token_ok(u, 128) || !ice_token_ok(cr, 128)) {
                http_error(c, 400, "bad_request", "a STUN/TURN server looks wrong (e.g. stun:203.0.113.10:3478)");
                return;
            }
            if (*u && !*cr)
                for (int k = 0; k < nold; k++)
                    if (!strcmp(old[k].url, was) && !strcmp(old[k].user, u))
                        cr = old[k].cred;
            int w = snprintf(extra + o, sizeof extra - o, "%s%s%s%s%s%s", o ? ", " : "", url, *u && *cr ? " " : "",
                             *u && *cr ? u : "", *u && *cr ? " " : "", *u && *cr ? cr : "");
            if (w < 0 || (size_t)w >= sizeof extra - o) {
                http_error(c, 400, "bad_request", "too many STUN/TURN servers");
                return;
            }
            o += (size_t)w;
        }
    }

    /* which of STUN and TURN this server hands out: kept across port/address changes, both when first
     * turned on; "builtin_ice" (the URLs left after removing some from the list) narrows it, and
     * removing both turns the built-in server off */
    const char *cur_urls = rd_conf_get(S.conf, "turn", "urls", "");
    int want_stun = !*tl || !*cur_urls || strstr(cur_urls, "stun:") != NULL;
    int want_turn = !*tl || !*cur_urls || strstr(cur_urls, "turn:") != NULL;
    cJSON *bl = cJSON_GetObjectItemCaseSensitive(in, "builtin_ice");
    if (bl) {
        if (!cJSON_IsArray(bl)) {
            http_error(c, 400, "bad_request", "builtin_ice must be a list of URLs");
            return;
        }
        want_stun = want_turn = 0;
        cJSON *it;
        cJSON_ArrayForEach(it, bl) {
            const char *u = cJSON_IsString(it) ? it->valuestring : "";
            want_stun |= !strncmp(u, "stun:", 5);
            want_turn |= !strncmp(u, "turn:", 5);
        }
        if (!want_stun && !want_turn)
            turn_on = 0;
    }

    int ok = 1, restart = 0, changed = 0;
    int web = HAS("http_port") || HAS("domain") || HAS("email") || HAS("tls_port") || HAS("redirect_http");
    int turn = HAS("turn_enabled") || HAS("turn_port") || HAS("public_ip") || HAS("domain") || bl; /* urls name the domain */
    char v[300];
    if (HAS("http_port")) {
        snprintf(v, sizeof v, "0.0.0.0:%d", http_port);
        ok &= set_if_changed("http", "listen", v, &restart) == 0;
    }
    if (!web) {
    } else if (*domain) {
        ok &= set_if_changed("acme", "domains", domain, &restart) == 0;
        ok &= set_if_changed("acme", "email", *email ? email : NULL, &restart) == 0;
        snprintf(v, sizeof v, "0.0.0.0:%d", tls_port);
        ok &= set_if_changed("tls", "listen", v, &restart) == 0;
        ok &= set_if_changed("tls", "plain_http", redirect ? "redirect" : "allow", &restart) == 0;
    } else {
        ok &= set_if_changed("acme", "domains", NULL, &restart) == 0;
        if (!*rd_conf_get(S.conf, "tls", "cert", ""))
            ok &= set_if_changed("tls", "listen", NULL, &restart) == 0;
    }
    if (!turn) {
    } else if (turn_on) {
        snprintf(v, sizeof v, "0.0.0.0:%d", turn_port);
        if (!*tl || listen_port(tl, 0) != turn_port) /* keep a hand-set listen address */
            ok &= set_if_changed("turn", "listen", v, &restart) == 0;
        ok &= set_if_changed("turn", "public_ip", pub, &restart) == 0;
        const char *h = *domain ? domain : pub;
        char su[140] = "", tu[150] = "";
        if (want_stun)
            snprintf(su, sizeof su, "stun:%s:%d", h, turn_port);
        if (want_turn)
            snprintf(tu, sizeof tu, "turn:%s:%d?transport=udp", h, turn_port);
        snprintf(v, sizeof v, "%s%s%s", su, *su && *tu ? ", " : "", tu);
        int urls_changed = 0; /* read for every new session: no restart */
        ok &= set_if_changed("turn", "urls", v, &urls_changed) == 0;
        if (urls_changed) {
            free(S.turn_urls);
            S.turn_urls = rd_strdup(v);
            changed = 1;
        }
        if (!*rd_conf_get(S.conf, "turn", "shared_secret", "")) {
            uint8_t r[24];
            char s[40];
            rd_random(r, sizeof r);
            rd_b64_encode(r, sizeof r, s, sizeof s);
            for (char *p = s; *p; p++)
                *p = *p == '+' ? '-' : *p == '/' ? '_' : *p;
            ok &= set_if_changed("turn", "shared_secret", s, &restart) == 0;
        }
    } else {
        ok &= set_if_changed("turn", "listen", NULL, &restart) == 0;
        ok &= set_if_changed("turn", "urls", NULL, &restart) == 0;
    }
    if (list) /* read for every new session: no restart */
        ok &= set_if_changed("ice", "extra", *extra ? extra : NULL, &changed) == 0;
    if (tl_in) /* read by the TURN server for every allocation: no restart */
        ok &= set_if_changed("turn", "users", *tlogin ? tlogin : NULL, &changed) == 0;
    int relogin = 0;
    if (account) {
        char *hash = *pw ? auth_hash_password(pw) : rd_strdup(old_hash);
        ok &= hash && set_if_changed("users", uname, hash, &changed) == 0;
        free(hash);
        if (strcmp(uname, user)) {
            ok &= set_if_changed("users", user, NULL, &changed) == 0;
            auth_drop_sessions_for(user, 0);
            relogin = 1;
        }
    }
    if (!ok) {
        http_error(c, 500, "io_error", "could not write the configuration file; check its permissions");
        return;
    }
    LOGI("settings: changed by %s%s", user, restart ? " (restarting)" : "");
    cJSON *b = settings_get(NULL, relogin ? uname : user);
    cJSON_AddBoolToObject(b, "ok", 1);
    cJSON_AddNumberToObject(b, "restart_ms", restart ? 1500 : 0);
    cJSON_AddBoolToObject(b, "relogin", relogin);
    http_json(c, 200, b, NULL);
    if (restart)
        server_restart_soon();
#undef HAS
}

void settings_api(conn_t *c, const http_req_t *r, const char *user)
{
    if (req_is(r, "GET", "/api/v1/settings")) {
        http_json(c, 200, settings_get(r, user), NULL);
    } else if (req_is(r, "PATCH", "/api/v1/settings") || req_is(r, "POST", "/api/v1/settings")) {
        cJSON *in = cJSON_ParseWithLength((const char *)r->body, r->body_len);
        if (!cJSON_IsObject(in))
            http_error(c, 400, "bad_request", "expected a JSON object");
        else
            settings_patch(c, in, user);
        cJSON_Delete(in);
    } else {
        http_error(c, 404, "not_found", "no such endpoint");
    }
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
