#include "ws.h"

#include <string.h>

int ws_parse(uint8_t *data, size_t len, int need_mask, ws_frame_t *f)
{
    if (len < 2)
        return 0;
    int fin = data[0] >> 7, rsv = (data[0] >> 4) & 7, op = data[0] & 15;
    int masked = data[1] >> 7;
    uint64_t plen = data[1] & 127;
    size_t hdr = 2;
    if (rsv || masked != need_mask)
        return -1;
    if (plen == 126) {
        if (len < 4)
            return 0;
        plen = rd_get_be16(data + 2);
        hdr = 4;
    } else if (plen == 127) {
        if (len < 10)
            return 0;
        plen = (uint64_t)rd_get_be32(data + 2) << 32 | rd_get_be32(data + 6);
        hdr = 10;
    }
    if (op >= 8 && (plen > 125 || !fin))
        return -1;
    if (plen > WS_MAX_MESSAGE)
        return -1;
    uint8_t mask[4] = { 0 };
    if (masked) {
        if (len < hdr + 4)
            return 0;
        memcpy(mask, data + hdr, 4);
        hdr += 4;
    }
    if (len < hdr + plen)
        return 0;
    uint8_t *p = data + hdr;
    if (masked)
        for (size_t i = 0; i < plen; i++)
            p[i] ^= mask[i & 3];
    f->fin = fin;
    f->opcode = op;
    f->payload = p;
    f->len = (size_t)plen;
    return (int)(hdr + plen);
}

int ws_build(rd_buf_t *out, int opcode, const void *data, size_t len, int mask)
{
    uint8_t h[14];
    size_t hl = 2;
    h[0] = (uint8_t)(0x80 | opcode);
    if (len < 126) {
        h[1] = (uint8_t)len;
    } else if (len < 65536) {
        h[1] = 126;
        rd_put_be16(h + 2, (uint16_t)len);
        hl = 4;
    } else {
        h[1] = 127;
        rd_put_be32(h + 2, 0);
        rd_put_be32(h + 6, (uint32_t)len);
        hl = 10;
    }
    uint8_t m[4] = { 0 };
    if (mask) {
        h[1] |= 0x80;
        rd_random(m, 4);
        memcpy(h + hl, m, 4);
        hl += 4;
    }
    if (rd_buf_reserve(out, hl + len) < 0)
        return -1;
    rd_buf_append(out, h, hl);
    uint8_t *dst = out->p + out->len;
    rd_buf_append(out, data, len);
    if (mask)
        for (size_t i = 0; i < len; i++)
            dst[i] ^= m[i & 3];
    return 0;
}

void ws_accept_key(const char *client_key, char out[29])
{
    char cat[128];
    uint8_t sha[20];
    size_t n = strlen(client_key);
    if (n > 64)
        n = 64;
    memcpy(cat, client_key, n);
    memcpy(cat + n, WS_GUID, sizeof WS_GUID - 1);
    rd_sha1(cat, n + sizeof WS_GUID - 1, sha);
    rd_b64_encode(sha, 20, out, 29);
}

void ws_client_key(char out[25])
{
    uint8_t r[16];
    rd_random(r, sizeof r);
    rd_b64_encode(r, sizeof r, out, 25);
}

int ws_asm_feed(ws_asm_t *a, const ws_frame_t *f, int *op, const uint8_t **msg, size_t *len)
{
    if (f->opcode >= 8) { /* control frames may interleave fragments */
        *op = f->opcode;
        *msg = f->payload;
        *len = f->len;
        return 1;
    }
    if (f->opcode != WS_OP_CONT) {
        if (a->frag_op)
            return -1; /* new message while a fragmented one is open */
        if (f->fin) {
            *op = f->opcode;
            *msg = f->payload;
            *len = f->len;
            return 1;
        }
        a->frag_op = f->opcode;
        rd_buf_reset(&a->frag);
    } else if (!a->frag_op) {
        return -1;
    }
    if (rd_buf_avail(&a->frag) + f->len > WS_MAX_MESSAGE || rd_buf_append(&a->frag, f->payload, f->len) < 0)
        return -1;
    if (!f->fin)
        return 0;
    *op = a->frag_op;
    *msg = rd_buf_data(&a->frag);
    *len = rd_buf_avail(&a->frag);
    a->frag_op = 0;
    return 1;
}

void ws_asm_free(ws_asm_t *a) { rd_buf_free(&a->frag); }
