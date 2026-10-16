/* remodroid-server: control server on AWS (plan §4.3, Appendix A.3–A.5). */
#ifndef RD_SERVER_H
#define RD_SERVER_H

#include <netinet/in.h>
#include <sys/types.h>

#include "cJSON.h"
#include "conf.h"
#include "ev.h"
#include "rd.h"
#include "ws.h"

enum { CONN_HTTP, CONN_WS };
enum { ROLE_NONE, ROLE_VIEWER, ROLE_AGENT };

typedef struct device device_t;
typedef struct vsession vsession_t;
typedef struct conn conn_t;

struct conn {
    ev_io rio, wio;
    int fd;
    char ip[INET6_ADDRSTRLEN];
    rd_buf_t in, out;
    int kind, role;
    int close_after_write;
    int busy;          /* waiting on an async reply; stop parsing new requests */
    unsigned gen;      /* changes when the conn is freed, to detect stale refs */
    int file_fd;       /* static file being sent with sendfile() */
    off_t file_off, file_end;
    ws_asm_t wsa;
    ev_tstamp last_rx;
    char user[64];     /* viewer user name or agent device id */
    device_t *dev;     /* agent: its device */
    vsession_t *vs;    /* viewer: its active stream session */
    conn_t *prev, *next;
};

struct device {
    char *id, *name, *hash;
    conn_t *agent;
    cJSON *hello, *status;
    uint64_t last_seen, online_since;
    vsession_t *vs;
    char public_ip[INET6_ADDRSTRLEN];
};

struct vsession {
    char sid[24];
    device_t *dev;
    conn_t *viewer;
    uint64_t since;
};

typedef struct {
    struct ev_loop *loop;
    rd_conf_t *conf;
    const char *conf_path;
    int listen_fd;
    ev_io listen_io;
    ev_timer ping_timer, sweep_timer;
    ev_signal sig_hup, sig_term, sig_int;
    conn_t *conns;
    device_t *devices;
    int ndevices;
    /* settings */
    int session_days, max_failed, lockout_min, ping_s, timeout_s, heartbeat_s;
    char cookie_name[64];
    char *web_root, *turn_urls, *turn_secret, *origins;
    int turn_ttl_min;
} server_t;

extern server_t S;

/* http.c */
void    conn_send(conn_t *c, const void *data, size_t n);
void    conn_close(conn_t *c);
void    http_respond(conn_t *c, int status, const char *ctype, const void *body, size_t n, const char *extra_headers);
void    http_json(conn_t *c, int status, cJSON *body, const char *extra_headers); /* frees body */
void    http_error(conn_t *c, int status, const char *code, const char *text);
void    ws_send_json(conn_t *c, cJSON *m); /* frees m */
int     server_listen(const char *addr);
void    server_tick_ping(void);

/* request view passed to handlers */
struct phr_header;
typedef struct {
    const char *method, *path, *query;
    size_t method_len, path_len, query_len;
    struct phr_header *headers;
    size_t nheaders;
    const uint8_t *body;
    size_t body_len;
} http_req_t;

const char *req_header(const http_req_t *r, const char *name, size_t *len);
int         req_is(const http_req_t *r, const char *method, const char *path);
int         req_query_int(const http_req_t *r, const char *key, int def);

/* auth.c */
typedef struct asess asess_t;
int         auth_check_password(const char *hash, const char *password);
char       *auth_hash_password(const char *password); /* malloc'd bcrypt hash */
const char *auth_cookie_session(const http_req_t *r, int want_device, char *name_out, size_t name_cap,
                                uint64_t *expires_ms);
/* Returns 1 ok, 0 bad credentials, -1 no header, -2 locked out. On success
 * name_out gets the user/device name. */
int         auth_basic(const http_req_t *r, conn_t *c, int device, char *name_out, size_t name_cap);
const char *auth_new_session(const char *name, int device); /* returns session id */
void        auth_drop_session(const char *id);
void        auth_cookie_header(const char *id, char *out, size_t cap);
void        auth_sweep(void);
const char *user_hash(const char *name);

/* hub.c */
void      hub_init_devices(void);
device_t *hub_device(const char *id);
cJSON    *hub_device_json(device_t *d);
void      hub_viewer_open(conn_t *c);
void      hub_agent_open(conn_t *c);
void      hub_on_message(conn_t *c, cJSON *m);
void      hub_conn_closed(conn_t *c);
/* REST: forwards a request to the agent and answers c when the agent replies. */
void      hub_forward_cmd(conn_t *c, device_t *d, const char *type, cJSON *d_payload);

/* turn.c: built-in TURN relay ([turn] listen); 0 when disabled or started */
int turn_start(void);

/* api.c */
void api_handle(conn_t *c, const http_req_t *r, const char *user, uint64_t expires_ms, const char *sess_id);

#endif
