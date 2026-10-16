/* REST API (plan Appendix A.3). */
#include <stdlib.h>
#include <string.h>

#include "msg.h"
#include "server.h"

static const char *ADMIN_CMDS[] = {
    "server.restart", "phone.wake", "phone.sleep", "phone.reboot", "adb.reconnect", "agent.reload",
    "quality.default", "agent.config",
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

void api_handle(conn_t *c, const http_req_t *r, const char *user, uint64_t expires_ms, const char *sess_id)
{
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
        auth_cookie_header(NULL, cookie, sizeof cookie);
        http_respond(c, 204, NULL, "", 0, cookie);
        return;
    }
    if (req_is(r, "GET", "/api/v1/devices")) {
        cJSON *b = cJSON_CreateObject();
        cJSON *arr = cJSON_AddArrayToObject(b, "devices");
        for (int i = 0; i < S.ndevices; i++)
            cJSON_AddItemToArray(arr, hub_device_json(&S.devices[i]));
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
    } else if (!strcmp(suffix, "/diag") && is_method(r, "GET")) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddNumberToObject(p, "lines", RD_MAX(1, RD_MIN(1000, req_query_int(r, "lines", 200))));
        hub_forward_cmd(c, d, "diag", p);
    } else {
        http_error(c, 404, "not_found", "no such endpoint");
    }
}
