/* Cloudflare TURN (server.ini [cloudflare_turn]: key_id, api_token, enabled). Cloudflare Realtime's TURN
 * service (1,000 GB a month free) hands out short-lived credentials only, from its API. The call is
 * blocking HTTPS, so it runs on a detached worker thread, like acme.c and notify.c: credentials for 24
 * hours, asked for again after 12, and kept here. hub.c adds them to every session's ICE servers (and
 * so to the Relay sheet's choices); a session never waits for Cloudflare. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "httpc.h"
#include "msg.h"
#include "server.h"

#define TTL_S 86400        /* how long Cloudflare's credentials last */
#define REFRESH_S (12 * 3600)
#define RETRY_S 60         /* after a failed call */

static rd_mutex_t mu;
static int inited, busy;
static char *ice;          /* Cloudflare's iceServers (a JSON array), or NULL */
static uint64_t got_at, tried_at;
static char for_key[128];  /* the key ID ice belongs to */
static char status[200];   /* for the panel: "" = not asked yet */
static ev_timer timer;

typedef struct {
    char key[128], token[256], ca[256];
} job_t;

static void set_status(const char *s)
{
    snprintf(status, sizeof status, "%s", s);
}

static void *worker(void *arg)
{
    job_t *j = arg;
    char *out = NULL, st[200] = "";
    httpc_t *h = httpc_new(j->ca[0] ? j->ca : NULL, "remodroid-server/" RD_VERSION);
    if (!h) {
        snprintf(st, sizeof st, "couldn't start an HTTPS client");
    } else {
        char auth[300], url[400];
        snprintf(auth, sizeof auth, "Bearer %s", j->token);
        httpc_set_auth(h, auth);
        for (int older = 0; older < 2 && !out && !st[0]; older++) {
            /* generate-ice-servers: a list; the older generate: one server object */
            snprintf(url, sizeof url, "https://rtc.live.cloudflare.com/v1/turn/keys/%s/credentials/%s", j->key,
                     older ? "generate" : "generate-ice-servers");
            httpc_resp_t r;
            char err[256] = "";
            if (httpc_request(h, "POST", url, "application/json", "{\"ttl\":86400}", &r, err, sizeof err) < 0) {
                snprintf(st, sizeof st, "%s", err);
                break;
            }
            if (r.status == 404 && !older) {
                httpc_resp_free(&r);
                continue;
            }
            if (r.status / 100 != 2) {
                snprintf(st, sizeof st, r.status == 401 || r.status == 403 || r.status == 404
                                            ? "Cloudflare refused the TURN key ID / API token (HTTP %d)"
                                            : "Cloudflare answered HTTP %d",
                         r.status);
            } else {
                cJSON *b = cJSON_Parse(r.body), *s = cJSON_DetachItemFromObjectCaseSensitive(b, "iceServers");
                if (cJSON_IsObject(s)) {
                    cJSON *a = cJSON_CreateArray();
                    cJSON_AddItemToArray(a, s);
                    s = a;
                }
                if (cJSON_IsArray(s) && cJSON_GetArraySize(s))
                    out = cJSON_PrintUnformatted(s);
                else
                    snprintf(st, sizeof st, "Cloudflare's answer had no TURN servers");
                cJSON_Delete(s);
                cJSON_Delete(b);
            }
            httpc_resp_free(&r);
        }
        httpc_free(h);
    }
    rd_mutex_lock(&mu);
    if (out && !strcmp(for_key, j->key)) {
        free(ice);
        ice = out;
        out = NULL;
        got_at = rd_now_ms();
        set_status("working");
        LOGI("cloudflare turn: credentials for the next 24 hours");
    } else if (!out) {
        set_status(st);
        LOGW("cloudflare turn: %s", st);
    }
    busy = 0;
    rd_mutex_unlock(&mu);
    free(out);
    memset(j->token, 0, sizeof j->token);
    free(j);
    return NULL;
}

static int key_ok(const char *k)
{
    if (!*k || strlen(k) >= sizeof for_key)
        return 0;
    for (; *k; k++)
        if (!((*k >= 'a' && *k <= 'z') || (*k >= 'A' && *k <= 'Z') || (*k >= '0' && *k <= '9') || *k == '-' || *k == '_'))
            return 0;
    return 1;
}

int cfturn_enabled(void)
{
    return rd_conf_bool(S.conf, "cloudflare_turn", "enabled", 0) && *rd_conf_get(S.conf, "cloudflare_turn", "key_id", "") &&
           *rd_conf_get(S.conf, "cloudflare_turn", "api_token", "");
}

/* On the loop: ask Cloudflare when there are no credentials for this key, they are 12 hours old, or the
 * settings changed (force); not again within a minute of a failure. */
void cfturn_refresh(int force)
{
    if (!inited)
        return;
    const char *key = rd_conf_get(S.conf, "cloudflare_turn", "key_id", "");
    const char *tok = rd_conf_get(S.conf, "cloudflare_turn", "api_token", "");
    uint64_t now = rd_now_ms();
    rd_mutex_lock(&mu);
    if (!cfturn_enabled()) {
        free(ice);
        ice = NULL;
        for_key[0] = 0;
        set_status("");
        rd_mutex_unlock(&mu);
        return;
    }
    if (strcmp(for_key, key)) { /* another key: what we had is no use */
        free(ice);
        ice = NULL;
        force = 1;
    }
    int due = force || !ice || now - got_at > REFRESH_S * 1000ULL;
    if (busy || !due || (!force && tried_at && now - tried_at < RETRY_S * 1000ULL)) {
        rd_mutex_unlock(&mu);
        return;
    }
    if (!key_ok(key) || strpbrk(tok, "\r\n")) {
        set_status("the TURN key ID or API token looks wrong");
        snprintf(for_key, sizeof for_key, "%.127s", key);
        rd_mutex_unlock(&mu);
        return;
    }
    snprintf(for_key, sizeof for_key, "%s", key);
    tried_at = now;
    busy = 1;
    if (!ice)
        set_status("asking Cloudflare…");
    rd_mutex_unlock(&mu);
    job_t *j = calloc(1, sizeof *j);
    snprintf(j->key, sizeof j->key, "%s", key);
    snprintf(j->token, sizeof j->token, "%s", tok);
    snprintf(j->ca, sizeof j->ca, "%s", rd_conf_get(S.conf, "cloudflare_turn", "ca_file", ""));
    if (rd_thread_detached(worker, j) != 0) {
        rd_mutex_lock(&mu);
        busy = 0;
        set_status("couldn't start a thread");
        rd_mutex_unlock(&mu);
        free(j);
    }
}

/* Appends Cloudflare's servers to arr while their credentials are good (an hour to spare). */
void cfturn_ice(cJSON *arr)
{
    if (!inited)
        return;
    cfturn_refresh(0);
    rd_mutex_lock(&mu);
    if (ice && rd_now_ms() - got_at < (TTL_S - 3600) * 1000ULL) {
        cJSON *a = cJSON_Parse(ice), *s;
        while (a && (s = cJSON_DetachItemFromArray(a, 0)))
            cJSON_AddItemToArray(arr, s);
        cJSON_Delete(a);
    }
    rd_mutex_unlock(&mu);
}

void cfturn_status(char *out, size_t cap)
{
    if (!inited) {
        snprintf(out, cap, "%s", "");
        return;
    }
    rd_mutex_lock(&mu);
    snprintf(out, cap, "%s", status);
    rd_mutex_unlock(&mu);
}

static void on_timer(struct ev_loop *loop, ev_timer *w, int revents) { cfturn_refresh(0); }

void cfturn_init(void)
{
    rd_mutex_init(&mu);
    inited = 1;
    ev_timer_init(&timer, on_timer, 1., 60.);
    ev_timer_start(S.loop, &timer);
}
