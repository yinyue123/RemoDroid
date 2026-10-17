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
    int file_fd;       /* static file being sent (sendfile(), or read into out on TLS) */
    off_t file_off, file_end;
    ws_asm_t wsa;
    void *ssl;         /* TLS state on HTTPS connections (tls.c), else NULL */
    int tls;           /* accepted on the HTTPS listener */
    int ssl_rd_wants_w, ssl_wr_wants_r; /* TLS read blocked on writability / write on readability */
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
    int fd, tls;
    ev_io io;
} listener_t;

typedef struct {
    struct ev_loop *loop;
    rd_conf_t *conf;
    const char *conf_path;
    listener_t listeners[2]; /* [http] listen, [tls] listen */
    int nlisteners;
    ev_timer ping_timer, sweep_timer;
    ev_signal sig_hup, sig_term, sig_int;
    conn_t *conns;
    device_t **devices;  /* allocated one by one: enrollment adds and removes them at runtime */
    int ndevices;
    int setup;           /* no [users] yet: only the setup wizard is served */
    char state_dir[256]; /* [server] state_dir: setup token, ACME files */
    char **argv;         /* for the restart after the wizard */
    /* settings */
    int session_days, max_failed, lockout_sec, ping_s, timeout_s, heartbeat_s;
    char cookie_name[64];
    char *web_root, *turn_urls, *turn_secret, *origins;
    int turn_ttl_min;
    /* [tls] */
    char *tls_listen, *tls_cert, *tls_key;
    int tls_port;      /* for redirects */
    int tls_redirect;  /* plain_http = redirect */
} server_t;

extern server_t S;

/* http.c */
void    conn_send(conn_t *c, const void *data, size_t n);
void    conn_close(conn_t *c);
void    http_respond(conn_t *c, int status, const char *ctype, const void *body, size_t n, const char *extra_headers);
void    http_json(conn_t *c, int status, cJSON *body, const char *extra_headers); /* frees body */
void    http_error(conn_t *c, int status, const char *code, const char *text);
void    ws_send_json(conn_t *c, cJSON *m); /* frees m */
int     server_listen(const char *addr, int tls);
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
void        http_serve_static(conn_t *c, const http_req_t *r, const char *extra);

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
void        auth_cookie_header(const char *id, int secure, char *out, size_t cap);
void        auth_sweep(void);
void        auth_drop_sessions_for(const char *name, int device);
void        auth_save_sessions(const char *path); /* before our own re-exec */
void        auth_load_sessions(const char *path); /* at startup; removes the file */
/* Counts a failed attempt from c's IP; 1 when the IP is locked out (shared with logins). */
int         auth_fail(conn_t *c);
int         auth_locked(conn_t *c);
const char *user_hash(const char *name);

/* hub.c */
void      hub_init_devices(void);
device_t *hub_add_device(const char *id, const char *name, const char *hash);
void      hub_remove_device(device_t *d);
void      hub_rename_device(device_t *d, const char *name);
void      hub_device_changed(device_t *d); /* device.update to every viewer */
device_t *hub_device(const char *id);
cJSON    *hub_device_json(device_t *d);
void      hub_viewer_open(conn_t *c);
void      hub_agent_open(conn_t *c);
void      hub_on_message(conn_t *c, cJSON *m);
void      hub_conn_closed(conn_t *c);
/* REST: forwards a request to the agent and answers c when the agent replies. */
void      hub_forward_cmd(conn_t *c, device_t *d, const char *type, cJSON *d_payload);

/* tls.c: HTTPS on the [tls] listener (mbedTLS) */
enum { TLS_AGAIN = -2 };
int     tls_load(const char *cert_file, const char *key_file); /* (re)loads; 0 ok, keeps the old one on error */
int     tls_ready(void);                                       /* a certificate is loaded */
int     tls_conn_start(conn_t *c);                             /* -1: no certificate yet */
/* > 0 bytes, 0 = closed, TLS_AGAIN = wait (sets c->ssl_rd_wants_w / c->ssl_wr_wants_r), -1 = error */
ssize_t tls_read(conn_t *c, void *buf, size_t n);
ssize_t tls_write(conn_t *c, const void *buf, size_t n);
void    tls_conn_free(conn_t *c);

/* acme.c: automatic certificates ([acme] domains) */
int  acme_start(void);    /* loads an existing certificate and schedules issuance/renewal */
void acme_reload(void);   /* after SIGHUP */
/* http-01: copies the key authorization for token; returns 0 if the token is pending */
int  acme_challenge(const char *token, size_t token_len, char *out, size_t cap);

/* main.c */
int  server_conf_set(const char *sec, const char *key, const char *val); /* writes server.ini, reloads */
void server_restart_soon(void);                                          /* re-exec with the new settings */

/* setup.c: first-run wizard (web/wizard.html) and its API */
void setup_init(void);          /* setup mode: create the token and say where to go */
void setup_request(conn_t *c, const http_req_t *r); /* every request while S.setup */
void setup_api(conn_t *c, const http_req_t *r, const char *user); /* /api/v1/setup after setup */
void settings_api(conn_t *c, const http_req_t *r, const char *user); /* /api/v1/settings: the panel's Server tab */

/* enroll.c: adding routers with one-time codes (web/agent.html, remodroid-agent enroll) */
void enroll_public(conn_t *c, const http_req_t *r);               /* POST /api/v1/enroll (no login) */
int  enroll_api(conn_t *c, const http_req_t *r, const char *user); /* 1 if handled */

/* turn.c: built-in TURN relay ([turn] listen); 0 when disabled or started */
int turn_start(void);

/* api.c */
void api_handle(conn_t *c, const http_req_t *r, const char *user, uint64_t expires_ms, const char *sess_id);

#endif
