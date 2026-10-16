#include "tunnel.h"

#include <string.h>

static size_t addr_len(uint8_t af) { return af == 4 ? 4 : af == 6 ? 16 : 0; }

int tun_parse(const uint8_t *data, size_t len, tun_frame_t *f)
{
    if (len < 2)
        return 0;
    size_t flen = rd_get_be16(data);
    if (flen < 3)
        return -1;
    if (len < 2 + flen)
        return 0;
    const uint8_t *p = data + 2;
    f->type = p[0];
    f->sock_id = p[1];
    f->af = p[2];
    if (f->af != 0 && f->af != 4 && f->af != 6)
        return -1;
    size_t al = addr_len(f->af), hdr = 3 + al + (al ? 2 : 0);
    if (flen < hdr)
        return -1;
    memset(f->addr, 0, sizeof f->addr);
    memcpy(f->addr, p + 3, al);
    f->port = al ? rd_get_be16(p + 3 + al) : 0;
    f->payload = p + hdr;
    f->plen = flen - hdr;
    return (int)(2 + flen);
}

int tun_build(rd_buf_t *out, uint8_t type, uint8_t sock_id, uint8_t af, const uint8_t *addr, uint16_t port,
              const void *payload, size_t plen)
{
    size_t al = addr_len(af), hdr = 3 + al + (al ? 2 : 0);
    if (hdr + plen > TUN_MAX_FRAME)
        return -1;
    uint8_t h[2 + 3 + 16 + 2];
    rd_put_be16(h, (uint16_t)(hdr + plen));
    h[2] = type;
    h[3] = sock_id;
    h[4] = af;
    if (al) {
        memcpy(h + 5, addr, al);
        rd_put_be16(h + 5 + al, port);
    }
    if (rd_buf_reserve(out, 2 + hdr + plen) < 0)
        return -1;
    rd_buf_append(out, h, 2 + hdr);
    return rd_buf_append(out, payload, plen);
}
