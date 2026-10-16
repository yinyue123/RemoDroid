/* Tunnel frames between the router agent and the phone server (plan §4.2).
 *
 *   u16 len | u8 type | u8 sock_id | u8 af | addr (0, 4 or 16 bytes) | u16 port | payload
 *
 * `len` counts every byte after the len field. af = 4 or 6 carries an
 * address and port; af = 0 carries neither (CTRL, PING, PONG, UDP_CLOSE). */
#ifndef RD_TUNNEL_H
#define RD_TUNNEL_H

#include "rd.h"

enum {
    TUN_UDP_OPEN = 0x01,
    TUN_UDP_SEND = 0x02, /* phone -> router */
    TUN_UDP_RECV = 0x03, /* router -> phone */
    TUN_UDP_CLOSE = 0x04,
    TUN_UDP_BOUND = 0x05, /* router -> phone: a local address + port of the socket (host candidate) */
    TUN_PING = 0x20,
    TUN_PONG = 0x21,
    TUN_CTRL = 0x30, /* payload: one JSON control message */
};

#define TUN_MAX_FRAME 65535

typedef struct {
    uint8_t type, sock_id, af;
    uint8_t addr[16];
    uint16_t port;
    const uint8_t *payload;
    size_t plen;
} tun_frame_t;

/* Returns bytes consumed (>0), 0 if incomplete, -1 if malformed. */
int tun_parse(const uint8_t *data, size_t len, tun_frame_t *f);
/* Appends an encoded frame. addr may be NULL when af == 0. */
int tun_build(rd_buf_t *out, uint8_t type, uint8_t sock_id, uint8_t af, const uint8_t *addr, uint16_t port,
              const void *payload, size_t plen);

#endif
