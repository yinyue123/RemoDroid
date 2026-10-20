/* REST API (plan Appendix A.3). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "msg.h"
#include "server.h"

static const char *ADMIN_CMDS[] = {
    "server.restart", "phone.wake", "phone.sleep", "phone.reboot", "adb.reconnect", "agent.reload",
    "quality.default", "agent.config", "agent.config.get",
};

/* Matches "/api/v1/devices/{id}[/suffix]"; copies id; returns suffix ("" if none) or NULL. */
static const char *device_path(const http_req_t *r, char *id, size_t cap)
{
    static const char pre[] = "/api/v1/devices/";
    size_t pl = sizeof pre - 1;
    if (r->path_len <= pl || memcmp(r->path, pre, pl))
        return NULL;
    const char *s = r->path + pl, *end = r->path + r->path_len;
    const char *slash = memchr(s, '/', (size_t)(end - s));
    size_t il = (size_t)((slash ? slash : end) - s);
    if (!il || il >= cap)
        return NULL;
    memcpy(id, s, il);
    id[il] = 0;
    static char suffix[64];
    size_t sl = slash ? (size_t)(end - slash) : 0;
    if (sl >= sizeof suffix)
        return NULL;
    memcpy(suffix, slash ? slash : "", sl);
    suffix[sl] = 0;
    return suffix;
}

static int is_method(const http_req_t *r, const char *m)
{
    return r->method_len == strlen(m) && !memcmp(r->method, m, r->method_len);
}

static int has_scope(const char *scopes, const char *want)
{
    size_t wl = strlen(want);
    for (const char *p = scopes; p && *p;) {
        const char *e = strchr(p, ',');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l == wl && !memcmp(p, want, l))
            return 1;
        p = e ? e + 1 : NULL;
    }
    return 0;
}

/* AI control (skill-plan.md §8): relayed to the agent, which asks the phone. scopes NULL = a
 * logged-in user (everything). Returns 1 when the suffix is an AI route. */
static int ai_route(conn_t *c, const http_req_t *r, device_t *d, const char *suffix, const char *scopes)
{
    const char *op = NULL, *scope = NULL;
    if (!strcmp(suffix, "/capabilities") && is_method(r, "GET"))
        op = "capabilities";
    else if (!strcmp(suffix, "/ui") && is_method(r, "GET"))
        op = "ui", scope = "ui";
    else if (!strcmp(suffix, "/screenshot") && is_method(r, "GET"))
        op = "screenshot", scope = "screenshot";
    else if (!strcmp(suffix, "/actions") && is_method(r, "POST"))
        op = "action", scope = "control";
    if (!op)
        return 0;
    if (scopes && scope && !has_scope(scopes, scope)) {
        http_error(c, 403, "forbidden", "this API token lacks the scope for this endpoint");
        return 1;
    }
    cJSON *args;
    if (is_method(r, "POST")) {
        args = cJSON_ParseWithLength((const char *)r->body, r->body_len);
        if (!cJSON_IsObject(args)) {
            cJSON_Delete(args);
            http_error(c, 400, "bad_request", "the body must be a JSON object");
            return 1;
        }
    } else {
        args = msg_query_json(r->query, r->query_len);
    }
    hub_forward_api(c, d, op, args);
    return 1;
}

/* Requests with an API token: the device list and the AI routes, nothing that changes settings. */
void api_token_handle(conn_t *c, const http_req_t *r, const char *who, const char *scopes)
{
    LOGD("%s: %.*s %.*s", who, (int)r->method_len, r->method, (int)r->path_len, r->path);
    if (req_is(r, "GET", "/api/v1/devices")) {
        cJSON *b = cJSON_CreateObject();
        cJSON *arr = cJSON_AddArrayToObject(b, "devices");
        for (int i = 0; i < S.ndevices; i++)
            cJSON_AddItemToArray(arr, hub_device_json(S.devices[i]));
        http_json(c, 200, b, NULL);
        return;
    }
    char id[64];
    const char *suffix = device_path(r, id, sizeof id);
    device_t *d = suffix ? hub_device(id) : NULL;
    if (!d) {
        http_error(c, 404, "not_found", suffix ? "unknown device" : "no such endpoint for API tokens");
        return;
    }
    if (!*suffix && is_method(r, "GET"))
        http_json(c, 200, hub_device_json(d), NULL);
    else if (!ai_route(c, r, d, suffix, scopes))
        http_error(c, 404, "not_found", "no such endpoint for API tokens");
}

void api_handle(conn_t *c, const http_req_t *r, const char *user, uint64_t expires_ms, const char *sess_id)
{
    if (r->path_len == 13 && !memcmp(r->path, "/api/v1/setup", 13)) {
        setup_api(c, r, user);
        return;
    }
    if (r->path_len == 16 && !memcmp(r->path, "/api/v1/settings", 16)) {
        settings_api(c, r, user);
        return;
    }
    if (enroll_api(c, r, user))
        return;
    if (req_is(r, "GET", "/api/v1/me")) {
        cJSON *b = cJSON_CreateObject();
        cJSON_AddStringToObject(b, "user", user);
        cJSON_AddNumberToObject(b, "session_expires", (double)expires_ms);
        cJSON_AddStringToObject(b, "version", RD_VERSION);
        http_json(c, 200, b, NULL);
        return;
    }
    if (req_is(r, "POST", "/api/v1/logout")) {
        char cookie[256];
        auth_drop_session(sess_id);
        auth_cookie_header(NULL, c->tls, cookie, sizeof cookie);
        http_respond(c, 204, NULL, "", 0, cookie);
        return;
    }
    if (req_is(r, "GET", "/api/v1/devices")) {
        cJSON *b = cJSON_CreateObject();
        cJSON *arr = cJSON_AddArrayToObject(b, "devices");
        for (int i = 0; i < S.ndevices; i++)
            cJSON_AddItemToArray(arr, hub_device_json(S.devices[i]));
        http_json(c, 200, b, NULL);
        return;
    }

    char id[64];
    const char *suffix = device_path(r, id, sizeof id);
    device_t *d = suffix ? hub_device(id) : NULL;
    if (!suffix) {
        http_error(c, 404, "not_found", "no such endpoint");
        return;
    }
    if (!d) {
        http_error(c, 404, "not_found", "unknown device");
        return;
    }

    if (!*suffix && is_method(r, "GET")) {
        http_json(c, 200, hub_device_json(d), NULL);
    } else if (!*suffix && is_method(r, "PATCH")) { /* rename, Bark URL, pause Bark */
        cJSON *body = cJSON_ParseWithLength((const char *)r->body, r->body_len);
        const char *name = msg_str(body, "name", NULL);
        const char *url = msg_str(body, "bark_url", NULL); /* "" = use the server's [notify] bark_url */
        cJSON *bark = cJSON_GetObjectItemCaseSensitive(body, "bark");
        if (!name && !url && !cJSON_IsBool(bark)) {
            http_error(c, 400, "bad_request", "expected name, bark_url or bark");
        } else if (name && (!*name || strlen(name) > 60 || strpbrk(name, "\r\n=;[]"))) {
            http_error(c, 400, "bad_request", "a name of 1-60 characters, without = ; [ ]");
        } else if (url && *url && ((strncmp(url, "https://", 8) && strncmp(url, "http://", 7)) || strlen(url) > 500 || strpbrk(url, " \t\r\n;[]"))) {
            http_error(c, 400, "bad_request", "a Bark URL starts with https:// (e.g. https://api.day.app/<key>)");
        } else if (cJSON_IsBool(bark) && !url && notify_bark_state(d) < 0) {
            http_error(c, 409, "no_bark", "no Bark URL is set for this router");
        } else if ((name && server_conf_set("device_names", d->id, name) < 0) ||
                   (url && server_conf_set("device_notify", d->id, *url ? url : NULL) < 0) ||
                   (cJSON_IsBool(bark) && server_conf_set("bark_paused", d->id, cJSON_IsTrue(bark) ? NULL : "true") < 0)) {
            http_error(c, 500, "io_error", "could not write the configuration file");
        } else {
            if (url)
                LOGI("%s: Bark URL for %s %s", user, d->id, *url ? "set" : "cleared"); /* never the key itself */
            if (cJSON_IsBool(bark))
                LOGI("%s: Bark push %s for %s", user, cJSON_IsTrue(bark) ? "resumed" : "paused", d->id);
            if (name)
                hub_rename_device(d, name);
            else
                hub_device_changed(d);
            http_json(c, 200, hub_device_json(d), NULL);
        }
        cJSON_Delete(body);
    } else if (!*suffix && is_method(r, "DELETE")) { /* forget the router; it must enroll again */
        LOGI("%s: removed router %s", user, d->id);
        if (server_conf_set("devices", d->id, NULL) < 0 || server_conf_set("device_names", d->id, NULL) < 0 ||
            server_conf_set("device_notify", d->id, NULL) < 0 || server_conf_set("bark_paused", d->id, NULL) < 0) {
            http_error(c, 500, "io_error", "could not write the configuration file");
            return;
        }
        auth_drop_sessions_for(d->id, 1);
        hub_remove_device(d);
        http_respond(c, 204, NULL, "", 0, NULL);
    } else if (!strcmp(suffix, "/adb") && is_method(r, "GET")) {
        cJSON *adb = d->status ? cJSON_GetObjectItemCaseSensitive(d->status, "adb") : NULL;
        if (!d->agent) {
            http_error(c, 409, "offline", "the router agent is offline");
        } else if (cJSON_IsObject(adb)) {
            http_json(c, 200, cJSON_Duplicate(adb, 1), NULL);
        } else {
            cJSON *b = cJSON_CreateObject();
            cJSON_AddStringToObject(b, "usb", "none");
            http_json(c, 200, b, NULL);
        }
    } else if (!strcmp(suffix, "/cmd") && is_method(r, "POST")) {
        cJSON *body = cJSON_ParseWithLength((const char *)r->body, r->body_len);
        const char *cmd = msg_str(body, "cmd", NULL);
        int known = 0;
        for (size_t i = 0; cmd && i < RD_ARRAY_LEN(ADMIN_CMDS); i++)
            known |= !strcmp(cmd, ADMIN_CMDS[i]);
        if (!known) {
            cJSON_Delete(body);
            http_error(c, 400, "bad_request", "unknown or missing \"cmd\"");
            return;
        }
        LOGI("%s: cmd %s on %s", user, cmd, d->id);
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "cmd", cmd);
        cJSON *args = cJSON_GetObjectItemCaseSensitive(body, "args");
        cJSON_AddItemToObject(p, "args", cJSON_IsObject(args) ? cJSON_Duplicate(args, 1) : cJSON_CreateObject());
        cJSON_Delete(body);
        hub_forward_cmd(c, d, "cmd", p);
    } else if (ai_route(c, r, d, suffix, NULL)) {
        /* answered by the agent */
    } else if (!strcmp(suffix, "/diag") && is_method(r, "GET")) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddNumberToObject(p, "lines", RD_MAX(1, RD_MIN(1000, req_query_int(r, "lines", 200))));
        hub_forward_cmd(c, d, "diag", p);
    } else {
        http_error(c, 404, "not_found", "no such endpoint");
    }
}
