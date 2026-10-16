/* remodroid-agent: runs on the router (plan §4.2). One libev loop drives the
 * built-in ADB client, the UDP relay for the phone, the WebSocket to the
 * server and the local CLI socket. */
#ifndef RD_AGENT_H
#define RD_AGENT_H

#include "adb.h"
#include "cJSON.h"
#include "conf.h"
#include "ev.h"
#include "rd.h"

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

/* wsc.c: WebSocket client to the server */
void wsc_start(void);
void wsc_send(cJSON *m); /* frees m; dropped while disconnected */
int  wsc_online(void);
void wsc_restart(void);

/* phone.c: phone server supervisor, tunnel and UDP relay */
void   phone_init(void);
void   phone_adb_state(int state);
void   phone_on_server_msg(cJSON *m);  /* session.*, signal, cmd from the server */
cJSON *phone_status_json(void);
cJSON *phone_config_json(void);
cJSON *phone_server_log_tail(int lines);
int    phone_run_cmd(const char *cmd, cJSON *args, char *err, size_t errcap);
void   phone_restart_server(void);
void   phone_config_changed(void);

/* cli.c: local control socket and the client side of the CLI */
int cli_server_start(void);
int cli_client_main(int argc, char **argv);
/* One request to the running agent; returns its ack payload (caller frees) or NULL. */
cJSON *cli_call(const char *type);

/* enroll.c: `remodroid-agent enroll --panel URL --code CODE` */
int enroll_main(int argc, char **argv);

#endif
