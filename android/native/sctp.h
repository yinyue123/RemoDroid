/* WebRTC data channels: SCTP over DTLS (RFC 8831) with DCEP (RFC 8832),
 * using usrsctp without its threads (driven from our event loop). */
#ifndef RTC_SCTP_H
#define RTC_SCTP_H

#include <stddef.h>
#include <stdint.h>

#include "ev.h"

typedef struct sctp sctp_t;

typedef struct {
    void (*send)(void *ud, const uint8_t *d, size_t n); /* SCTP packets out, to DTLS */
    void (*on_open)(void *ud, const char *label);
    void (*on_message)(void *ud, const char *label, int binary, const uint8_t *d, size_t n);
    void (*on_writable)(void *ud);  /* send buffer drained below the threshold */
    void (*on_closed)(void *ud);
} sctp_cb_t;

/* dtls_client decides stream id parity (client even, server odd). */
sctp_t *sctp_new(struct ev_loop *loop, int dtls_client, int local_port, int remote_port, sctp_cb_t cb, void *ud);
void    sctp_input(sctp_t *s, const uint8_t *d, size_t n);
/* Opens an ordered, reliable channel; returns 0 or -1. */
int     sctp_open_channel(sctp_t *s, const char *label);
/* Returns 0, or -1 when the channel is unknown or the send buffer is full. */
int     sctp_send(sctp_t *s, const char *label, int binary, const void *d, size_t n);
size_t  sctp_buffered(const sctp_t *s);
int     sctp_is_open(const sctp_t *s, const char *label);
void    sctp_free(sctp_t *s);

#endif
