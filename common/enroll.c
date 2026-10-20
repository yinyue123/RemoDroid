#include "enroll.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "httpc.h"
#include "msg.h"
#include "plat.h"
#include "rd.h"

int rd_enroll(const char *panel, const char *code, const char *name, const char *platform, const char *ca_file,
              const char *user_agent, rd_enroll_t *out, char *err, size_t errcap)
{
    memset(out, 0, sizeof *out);
    while (*panel == ' ')
        panel++;
    char base[300];
    if (strncmp(panel, "https://", 8) && strncmp(panel, "http://", 7))
        snprintf(base, sizeof base, "https://%s", panel); /* "remo.example.com" */
    else
        snprintf(base, sizeof base, "%s", panel);
    size_t bl = strlen(base);
    while (bl > 8 && (base[bl - 1] == '/' || base[bl - 1] == ' '))
        base[--bl] = 0;
    int tls = !strncmp(base, "https://", 8);
    const char *hostport = base + (tls ? 8 : 7);
    if (!*hostport || strchr(hostport, '/')) {
        snprintf(err, errcap, "the panel address is just the address, e.g. https://remodroid.example.com");
        return -1;
    }
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "code", code);
    cJSON_AddStringToObject(req, "name", name);
    cJSON_AddStringToObject(req, "arch", rd_machine());
    cJSON_AddStringToObject(req, "platform", platform);
    cJSON_AddStringToObject(req, "version", RD_VERSION);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    char url[400];
    snprintf(url, sizeof url, "%s/api/v1/enroll", base);
    httpc_t *h = httpc_new(ca_file ? ca_file : "", user_agent);
    httpc_resp_t r;
    char herr[300] = "out of memory";
    int e = h ? httpc_request(h, "POST", url, "application/json", body, &r, herr, sizeof herr) : -1;
    free(body);
    httpc_free(h);
    if (e < 0) {
        snprintf(err, errcap, "%s", herr);
        return -1;
    }
    cJSON *res = cJSON_Parse(r.body);
    int ret = -1;
    if (r.status != 200) {
        snprintf(err, errcap, "%s", msg_str(res, "msg", r.status == 404 ? "this panel has no enrollment (wrong address?)"
                                                                        : "the panel refused the code"));
    } else if (!*msg_str(res, "device_id", "") || !*msg_str(res, "device_secret", "")) {
        snprintf(err, errcap, "unexpected answer from the panel");
    } else {
        snprintf(out->device_id, sizeof out->device_id, "%s", msg_str(res, "device_id", ""));
        snprintf(out->device_secret, sizeof out->device_secret, "%s", msg_str(res, "device_secret", ""));
        snprintf(out->name, sizeof out->name, "%s", msg_str(res, "name", out->device_id));
        snprintf(out->ws_url, sizeof out->ws_url, "%s://%s%s", tls ? "wss" : "ws", hostport,
                 msg_str(res, "ws_path", "/ws/agent"));
        ret = 0;
    }
    cJSON_Delete(res);
    httpc_resp_free(&r);
    return ret;
}
