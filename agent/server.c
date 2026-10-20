/* The agent's side of the server connection (common/wsc.c): hello and status,
 * and dispatching what the server sends (commands, diag, AI API, and the
 * session/signaling messages that go on to the phone). */
#include <stdio.h>
#include <string.h>

#include "agent.h"
#include "msg.h"

static void on_open(void *ud)
{
    cJSON *m = msg_new("hello", 0, NULL);
    cJSON *d = msg_d(m);
    cJSON_AddNumberToObject(d, "proto", RD_PROTO);
    cJSON_AddStringToObject(d, "version", RD_VERSION);
    cJSON_AddStringToObject(d, "arch", rd_machine());
    cJSON_AddStringToObject(d, "platform", "android");
    cJSON_AddItemToObject(d, "phone", phone_status_json());
    wsc_send(m);
}

static cJSON *status(void *ud) { return agent_status_json(); }

static void on_lost(void *ud) { phone_server_lost(); }

static void on_message(void *ud, cJSON *m)
{
    const char *t = msg_type(m);
    long id = msg_id(m);
    if (!strcmp(t, "cmd")) {
        cJSON *d = msg_d(m);
        const char *cmd = msg_str(d, "cmd", "");
        char err[200] = "";
        LOGI("server command: %s", cmd);
        if (!strcmp(cmd, "agent.config.get")) {
            wsc_send(msg_ack(id, phone_config_json()));
            return;
        }
        int r = phone_run_cmd(cmd, cJSON_GetObjectItemCaseSensitive(d, "args"), err, sizeof err);
        if (r == 0) {
            cJSON *res = cJSON_CreateObject();
            cJSON_AddStringToObject(res, "cmd", cmd);
            wsc_send(msg_ack(id, res));
        } else if (r > 0) {
            /* the phone answers later (forwarded with the same id) */
        } else {
            wsc_send(msg_error(id, err[0] ? "offline" : "unsupported", err[0] ? err : cmd));
        }
    } else if (!strcmp(t, "diag")) {
        int lines = (int)msg_num(msg_d(m), "lines", 200);
        cJSON *res = cJSON_CreateObject();
        cJSON_AddItemToObject(res, "agent_log", agent_log_tail(lines));
        cJSON_AddItemToObject(res, "server_stderr", phone_server_log_tail(lines));
        wsc_send(msg_ack(id, res));
    } else if (!strcmp(t, "api")) {
        api_relay(id, msg_d(m));
    } else if (!strncmp(t, "session.", 8) || !strcmp(t, "signal")) {
        phone_on_server_msg(m);
    } else if (!strcmp(t, "error")) {
        LOGW("server error: %s", msg_str(msg_d(m), "msg", "?"));
    } else if (strcmp(t, "ack")) {
        LOGD("ignoring server message %s", t);
    }
}

/* [server] and [keepalive] from agent.ini; reconnects when the server or credentials changed. */
void server_link_apply(void)
{
    static int inited;
    if (!inited) {
        wsc_init(A.loop, (wsc_cb_t){ on_open, on_message, on_lost, status, NULL }, NULL);
        inited = 1;
    }
    wsc_conf_t c;
    memset(&c, 0, sizeof c);
    snprintf(c.url, sizeof c.url, "%s", A.url);
    snprintf(c.device_id, sizeof c.device_id, "%s", A.device_id);
    snprintf(c.device_secret, sizeof c.device_secret, "%s", A.device_secret);
    snprintf(c.ca_file, sizeof c.ca_file, "%s", A.ca_file);
    snprintf(c.user_agent, sizeof c.user_agent, "remodroid-agent/%s", RD_VERSION);
    c.ping_s = A.ping_s;
    c.timeout_s = A.timeout_s;
    c.heartbeat_s = A.heartbeat_s;
    c.reconnect_max_s = A.reconnect_max_s;
    wsc_configure(&c);
}
