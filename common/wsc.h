/* WebSocket client to the RemoDroid server, shared by the router agent and the
 * desktop agents (plan §4.5, Appendix A.5): ws:// or wss:// (TLS 1.2 via
 * mbedTLS, certificate checked against [server] ca_file or the system's trusted
 * roots); Basic auth on first connect, then the session cookie; ping/pong
 * keepalive and heartbeat status; reconnect with exponential backoff.
 * One client per process, on one libev loop. */
#ifndef RD_WSC_H
#define RD_WSC_H

#include <stddef.h>

#include "cJSON.h"
#include "ev.h"

typedef struct {
    char url[256], device_id[64], device_secret[128], ca_file[256];
    int ping_s, timeout_s, heartbeat_s, reconnect_max_s;
    char user_agent[64]; /* "remodroid-agent/0.4" */
} wsc_conf_t;

typedef struct {
    void (*on_open)(void *ud);               /* connected: send hello (and status) now */
    void (*on_message)(void *ud, cJSON *m);  /* every server message except welcome */
    void (*on_lost)(void *ud);               /* an open connection went away */
    cJSON *(*status)(void *ud);              /* heartbeat "status" payload (caller owns it) */
    /* optional: connection state for a UI; problem is NULL when fine ("server rejected the device", …) */
    void (*on_state)(void *ud, int online, const char *problem);
} wsc_cb_t;

void wsc_init(struct ev_loop *loop, wsc_cb_t cb, void *ud);
/* (Re)applies the settings; reconnects when the URL, credentials or CA changed. An empty URL
 * disconnects. */
void wsc_configure(const wsc_conf_t *c);
void wsc_restart(void);   /* reconnect now */
void wsc_stop(void);      /* disconnect and stay disconnected until wsc_configure / wsc_restart */
void wsc_send(cJSON *m);  /* frees m; events are kept while disconnected (32), the rest dropped */
void wsc_send_binary(const void *data, size_t n); /* dropped while disconnected */
int  wsc_online(void);
const char *wsc_problem(void); /* the last connection problem, or "" */

#endif
