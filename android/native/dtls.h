/* DTLS 1.2 with the use_srtp extension (RFC 5764), on mbedTLS. */
#ifndef RTC_DTLS_H
#define RTC_DTLS_H

#include <stddef.h>
#include <stdint.h>

#include "ev.h"

typedef struct dtls dtls_t;

typedef struct {
    void (*send)(void *ud, const uint8_t *d, size_t n); /* records out, to ICE */
    /* keys: client_key(16) server_key(16) client_salt(14) server_salt(14) */
    void (*on_connected)(void *ud, const uint8_t keys[60], int is_client);
    void (*on_data)(void *ud, const uint8_t *d, size_t n); /* application data (SCTP) */
    void (*on_closed)(void *ud, int error);
} dtls_cb_t;

/* Generates this process's certificate once; returns 0 on success. */
int         dtls_global_init(void);
/* "AB:CD:…" SHA-256 fingerprint of our certificate */
const char *dtls_fingerprint(void);

/* remote_fp: the peer's a=fingerprint value (sha-256 "AB:CD:…") */
dtls_t *dtls_new(struct ev_loop *loop, int is_client, const char *remote_fp, dtls_cb_t cb, void *ud);
void    dtls_start(dtls_t *d);
void    dtls_input(dtls_t *d, const uint8_t *data, size_t n);
int     dtls_write(dtls_t *d, const uint8_t *data, size_t n);
int     dtls_connected(const dtls_t *d);
void    dtls_free(dtls_t *d);

#endif
