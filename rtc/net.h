/* Phone-side network layer: the tunnel listener (the router agent connects to
 * it over ADB) and UDP sockets, either tunnelled through the router (Path A)
 * or real sockets on the phone's own network (Path B). */
#ifndef RTC_NET_H
#define RTC_NET_H

#include <stddef.h>
#include <stdint.h>
#include "plat.h"

#include "ev.h"

typedef struct net net_t;
typedef struct nsock nsock_t;

typedef struct {
    void (*on_udp)(void *ud, nsock_t *s, const struct sockaddr_storage *from, const uint8_t *d, size_t n);
    void (*on_bound)(void *ud, nsock_t *s, const struct sockaddr_storage *local); /* host candidate address */
    void (*on_ctrl)(void *ud, const uint8_t *json, size_t n);                     /* CTRL frame from the agent */
    void (*on_link)(void *ud, int up);                                            /* agent connected / gone */
    void (*on_ctrl_out)(void *ud, const char *json, size_t n);                    /* "embed": CTRL to the embedder */
} net_cb_t;

/* listen: "abstract:NAME", "tcp:HOST:PORT", or "embed" (no tunnel: CTRL goes through
 * on_ctrl_out and net_ctrl_input, UDP is always direct). direct: 1 = Path B (own UDP). */
net_t   *net_new(struct ev_loop *loop, const char *listen, int direct, net_cb_t cb, void *ud);
void     net_set_direct(net_t *n, int direct); /* applies to sockets opened afterwards */
int      net_link_up(const net_t *n);
size_t   net_backlog(const net_t *n);          /* bytes queued towards the agent */
int      net_ctrl_send(net_t *n, const char *json, size_t len);
void     net_ctrl_input(net_t *n, const char *json, size_t len); /* "embed": CTRL from the embedder */
nsock_t *net_udp_open(net_t *n, int af);
/* Direct sockets: reports the phone's interface addresses through on_bound
 * (call after storing the handle). */
void     net_udp_report(nsock_t *s);
int      net_udp_send(nsock_t *s, const struct sockaddr_storage *to, const void *d, size_t len);
void     net_udp_close(nsock_t *s);
int      net_udp_is_tunnel(const nsock_t *s);

#endif
