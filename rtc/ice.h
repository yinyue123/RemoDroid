/* ICE agent (RFC 8445, controlling role) with a TURN client (RFC 8656).
 * The phone is always the offerer and the controlling agent. */
#ifndef RTC_ICE_H
#define RTC_ICE_H

#include "cJSON.h"
#include "ev.h"
#include "net.h"

enum { ICE_NEW, ICE_CHECKING, ICE_CONNECTED, ICE_DISCONNECTED, ICE_FAILED, ICE_CLOSED };

typedef struct ice ice_t;

typedef struct {
    /* a trickled local candidate ("candidate:…" without "a="), or NULL when gathering is done */
    void (*on_candidate)(void *ud, const char *cand);
    void (*on_state)(void *ud, int state);
    /* DTLS / RTP / RTCP from the selected pair */
    void (*on_data)(void *ud, const uint8_t *d, size_t n);
} ice_cb_t;

ice_t      *ice_new(struct ev_loop *loop, net_t *net, ice_cb_t cb, void *ud);
void        ice_free(ice_t *ice);
/* WebRTC iceServers JSON array; force_relay = only relayed candidates */
void        ice_set_servers(ice_t *ice, const cJSON *servers, int force_relay);
void        ice_gather(ice_t *ice);
const char *ice_ufrag(const ice_t *ice);
const char *ice_pwd(const ice_t *ice);
void        ice_set_remote(ice_t *ice, const char *ufrag, const char *pwd);
void        ice_add_remote(ice_t *ice, const char *cand); /* "candidate:…" or "a=candidate:…" */
int         ice_send(ice_t *ice, const void *d, size_t n);
int         ice_state(const ice_t *ice);
/* "host" / "srflx" / "relay" / "prflx" of the selected local and remote candidates */
void        ice_selected(const ice_t *ice, const char **local_type, const char **remote_type, int *rtt_ms);
/* The selected pair's addresses ("ip:port", or "" when there is none): this end's own (host) address and the
 * public one a NAT maps it to (srflx), its TURN allocation when the pair goes through it, and the far end's. */
void        ice_selected_addrs(const ice_t *ice, char *host, char *mapped, char *relay, char *remote, size_t cap);

/* net plumbing */
void ice_on_udp(ice_t *ice, nsock_t *s, const struct sockaddr_storage *from, const uint8_t *d, size_t n);
void ice_on_bound(ice_t *ice, nsock_t *s, const struct sockaddr_storage *local);

#endif
