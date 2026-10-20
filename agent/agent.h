/* remodroid-agent: runs on the router (plan §4.2). One libev loop drives the
 * built-in ADB client, the UDP relay for the phone, the WebSocket to the
 * server and the local CLI socket. */
#ifndef RD_AGENT_H
#define RD_AGENT_H

#include "adb.h"
#include "cJSON.h"
#include "conf.h"
#include "ev.h"
#include "plat.h"
#include "rd.h"
#include "wsc.h"

typedef struct transport_mgr transport_mgr_t;

typedef struct {
    struct ev_loop *loop;
    const char *conf_path;
    rd_conf_t *conf;
    adb_t *adb;
    transport_mgr_t *tm;
    ev_signal sig_hup, sig_term, sig_int;
    ev_timer status_debounce;
    /* [server] */
    char url[256], device_id[64], device_secret[128], ca_file[256];
    int ping_s, timeout_s, heartbeat_s, reconnect_max_s;
    /* [agent] */
    char files_dir[256], phone_dir[128], sock_path[128];
} agent_t;

extern agent_t A;

const char *agent_conf(const char *sec, const char *key, const char *def);
long        agent_conf_int(const char *sec, const char *key, long def);
int         agent_conf_bool(const char *sec, const char *key, int def);
int         agent_reload(void);
int         agent_reload_apply(void); /* agent_reload + reconnect if the server settings changed */
/* Rewrites one key in agent.ini (keeping comments) and reloads. */
int         agent_conf_set(const char *sec, const char *key, const char *val);
void        agent_event(const char *kind, cJSON *info); /* takes ownership of info */
void        agent_status_changed(void);
cJSON      *agent_status_json(void);
cJSON      *agent_adb_json(void);
/* Last log lines, for `diag`. */
cJSON      *agent_log_tail(int lines);

/* usb.c */
transport_mgr_t *transport_start(struct ev_loop *loop, adb_t *adb, const char *serial_filter, const char *tcp_target);
void             transport_reconnect(transport_mgr_t *m);
const char      *transport_problem(const transport_mgr_t *m); /* e.g. "held by adb (pid 1234)", or NULL */

/* server.c: hello, status and server messages over common/wsc.c */
void server_link_apply(void); /* (re)applies [server] and [keepalive]; first call starts it */

/* phone.c: phone server supervisor, tunnel and UDP relay */
void   phone_init(void);
void   phone_adb_state(int state);
void   phone_on_server_msg(cJSON *m);  /* session.*, signal, cmd from the server */
void   phone_server_lost(void);    /* ends the phone's session: it locks after the grace period */
cJSON *phone_status_json(void);
cJSON *phone_config_json(void);
cJSON *phone_server_log_tail(int lines);
int    phone_run_cmd(const char *cmd, cJSON *args, char *err, size_t errcap);
void   phone_restart_server(void);
void   phone_config_changed(void);

/* api.c: the AI control API on the phone (skill-plan.md), one ADB stream per request.
 * Callbacks: on_head once (status, content type, body length, extra headers or NULL),
 * on_body for each piece, then on_done (ok = 0: cut short). */
typedef struct api_req api_req_t;
typedef struct {
    void (*on_head)(api_req_t *r, int status, const char *type, size_t len, cJSON *headers, void *ud);
    void (*on_body)(api_req_t *r, const uint8_t *data, size_t n, void *ud);
    void (*on_done)(api_req_t *r, int ok, void *ud);
} api_cb_t;
/* Takes ownership of args. NULL when it cannot start (err_code/err say why; no callbacks). */
api_req_t *api_request(const char *op, cJSON *args, api_cb_t cb, void *ud, const char **err_code, const char **err);
void       api_cancel(api_req_t *r); /* no further callbacks */
char      *api_error_body(const char *code, const char *text, int retryable); /* caller frees */
int        api_error_status(const char *code);
void       api_relay(long id, cJSON *d); /* an "api" message from the server */

/* http.c: the local HTTP API ([api] listen, token) */
int http_api_start(void); /* (re)applies the [api] settings; 0 when off or listening */

/* cli.c: local control socket and the client side of the CLI */
int cli_server_start(void);
int cli_client_main(int argc, char **argv);
/* One request to the running agent; returns its ack payload (caller frees) or NULL. */
cJSON *cli_call(const char *type);

/* enroll.c: `remodroid-agent enroll --panel URL --code CODE` */
int enroll_main(int argc, char **argv);

#endif
