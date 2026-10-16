/* Small blocking HTTP/1.1 client (http:// and https:// via mbedTLS) for
 * one-off exchanges off the event loop: the server's ACME client (worker
 * thread) and `remodroid-agent enroll`. Not for use on a libev loop. */
#ifndef RD_HTTPC_H
#define RD_HTTPC_H

#include <stddef.h>

typedef struct httpc httpc_t;

typedef struct {
    int status;
    char *body; /* NUL-terminated */
    size_t len;
    char location[512], nonce[256]; /* Location, Replay-Nonce */
    int retry_after;
} httpc_resp_t;

/* ca_file: trusted roots for https:// ("" or NULL = the system bundle, loaded on first use). */
httpc_t *httpc_new(const char *ca_file, const char *user_agent);
void     httpc_free(httpc_t *h);
/* Returns 0 with r filled (any HTTP status), or -1 with err set (DNS, connect, TLS, bad response).
 * ctype: Content-Type of body (NULL = no body). */
int      httpc_request(httpc_t *h, const char *method, const char *url, const char *ctype, const char *body,
                       httpc_resp_t *r, char *err, size_t errcap);
void     httpc_resp_free(httpc_resp_t *r);
/* The client's CTR-DRBG, usable as an mbedTLS f_rng (p_rng = h). */
int      httpc_rng(void *h, unsigned char *out, size_t n);

#endif
