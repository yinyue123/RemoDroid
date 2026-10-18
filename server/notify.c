/* Phone-notification push (notifications feature). A notification captured on the
 * phone arrives here as an "event" of kind "notification" (hub.c, on the libev
 * loop). WebSocket delivery to viewers is handled there; this adds the optional
 * Bark push (https://github.com/Finb/Bark). The Bark call is blocking HTTPS, so
 * it runs on a detached worker thread, like acme.c. The Bark key lives only in
 * server.ini, and notification text is never written to the log. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "httpc.h"
#include "msg.h"
#include "server.h"

typedef struct {
    char url[512], ca[256], pkg[128];
    char *json;
} bark_job_t;

static void *bark_worker(void *arg)
{
    bark_job_t *j = arg;
    httpc_t *h = httpc_new(j->ca[0] ? j->ca : NULL, "remodroid/" RD_VERSION);
    if (h) {
        httpc_resp_t r;
        char err[256] = "";
        if (httpc_request(h, "POST", j->url, "application/json", j->json, &r, err, sizeof err) == 0) {
            if (r.status / 100 == 2)
                LOGI("notify: pushed a %s notification to Bark", j->pkg);
            else
                LOGW("notify: Bark returned HTTP %d for %s", r.status, j->pkg);
            httpc_resp_free(&r);
        } else {
            LOGW("notify: Bark push failed for %s: %s", j->pkg, err);
        }
        httpc_free(h);
    }
    free(j->json);
    free(j);
    return NULL;
}

/* {"title","body","group","level"} for Bark; title/body are the notification's own text. */
static char *bark_body(const char *title, const char *body)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "title", title && *title ? title : "Notification");
    cJSON_AddStringToObject(o, "body", body ? body : "");
    cJSON_AddStringToObject(o, "group", "RemoDroid");
    cJSON_AddStringToObject(o, "level", "active");
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

void notify_on_event(device_t *d, const char *kind, cJSON *info)
{
    if (!kind || strcmp(kind, "notification"))
        return;
    const char *url = rd_conf_get(S.conf, "device_notify", d->id, NULL);
    if (!url)
        url = rd_conf_get(S.conf, "notify", "bark_url", NULL);
    if (!url || !*url)
        return; /* WebSocket push only */
    const char *pkg = msg_str(info, "pkg", "app");
    const char *title = msg_str(info, "title", NULL);
    const char *text = msg_str(info, "text", "");
    char ttl[256];
    snprintf(ttl, sizeof ttl, "%s", title && *title ? title : pkg); /* fall back to the app's package */

    bark_job_t *j = calloc(1, sizeof *j);
    snprintf(j->url, sizeof j->url, "%s", url);
    snprintf(j->ca, sizeof j->ca, "%s", rd_conf_get(S.conf, "notify", "ca_file", ""));
    snprintf(j->pkg, sizeof j->pkg, "%s", pkg);
    j->json = bark_body(ttl, text);

    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &at, bark_worker, j) != 0) {
        LOGE("notify: cannot start the Bark worker thread");
        free(j->json);
        free(j);
    }
    pthread_attr_destroy(&at);
}
