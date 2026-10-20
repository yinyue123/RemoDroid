#include "stun.h"

#include <stdio.h>
#include <string.h>

#include <mbedtls/md.h>
#include <mbedtls/md5.h>

#include "rd.h"

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static void set_len(stun_msg_t *m, size_t body) { put16(m->buf + 2, (uint16_t)body); }

void stun_new(stun_msg_t *m, uint16_t type, const uint8_t tid[12])
{
    put16(m->buf, type);
    put16(m->buf + 2, 0);
    rd_put_be32(m->buf + 4, STUN_MAGIC);
    if (tid)
        memcpy(m->buf + 8, tid, 12);
    else
        rd_random(m->buf + 8, 12);
    m->len = 20;
}

const uint8_t *stun_tid(const stun_msg_t *m) { return m->buf + 8; }

void stun_add(stun_msg_t *m, uint16_t attr, const void *v, size_t n)
{
    size_t pad = (4 - (n & 3)) & 3;
    if (m->len + 4 + n + pad > sizeof m->buf)
        return;
    put16(m->buf + m->len, attr);
    put16(m->buf + m->len + 2, (uint16_t)n);
    if (n)
        memcpy(m->buf + m->len + 4, v, n);
    memset(m->buf + m->len + 4 + n, 0, pad);
    m->len += 4 + n + pad;
    set_len(m, m->len - 20);
}

void stun_add_u32(stun_msg_t *m, uint16_t attr, uint32_t v)
{
    uint8_t b[4];
    rd_put_be32(b, v);
    stun_add(m, attr, b, 4);
}

void stun_add_u64(stun_msg_t *m, uint16_t attr, uint64_t v)
{
    uint8_t b[8];
    rd_put_be32(b, (uint32_t)(v >> 32));
    rd_put_be32(b + 4, (uint32_t)v);
    stun_add(m, attr, b, 8);
}

void stun_add_xaddr(stun_msg_t *m, uint16_t attr, const struct sockaddr_storage *a)
{
    uint8_t b[20];
    b[0] = 0;
    if (a->ss_family == AF_INET) {
        const struct sockaddr_in *s = (const struct sockaddr_in *)a;
        b[1] = 1;
        put16(b + 2, (uint16_t)(ntohs(s->sin_port) ^ (STUN_MAGIC >> 16)));
        uint32_t ip = ntohl(s->sin_addr.s_addr) ^ STUN_MAGIC;
        rd_put_be32(b + 4, ip);
        stun_add(m, attr, b, 8);
    } else {
        const struct sockaddr_in6 *s = (const struct sockaddr_in6 *)a;
        b[1] = 2;
        put16(b + 2, (uint16_t)(ntohs(s->sin6_port) ^ (STUN_MAGIC >> 16)));
        uint8_t x[16];
        rd_put_be32(x, STUN_MAGIC);
        memcpy(x + 4, m->buf + 8, 12);
        for (int i = 0; i < 16; i++)
            b[4 + i] = s->sin6_addr.s6_addr[i] ^ x[i];
        stun_add(m, attr, b, 20);
    }
}

void stun_add_integrity(stun_msg_t *m, const void *key, size_t keylen)
{
    /* Length must already count the MESSAGE-INTEGRITY attribute (24 bytes). */
    set_len(m, m->len - 20 + 24);
    uint8_t mac[20];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), key, keylen, m->buf, m->len, mac);
    stun_add(m, STUN_A_MESSAGE_INTEGRITY, mac, 20);
}

static uint32_t crc_table[256];
static int crc_ready;

uint32_t stun_crc32(const uint8_t *d, size_t n)
{
    if (!crc_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
        crc_ready = 1;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
        c = crc_table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void stun_add_fingerprint(stun_msg_t *m)
{
    set_len(m, m->len - 20 + 8);
    stun_add_u32(m, STUN_A_FINGERPRINT, stun_crc32(m->buf, m->len) ^ 0x5354554Eu);
}

int stun_is(const uint8_t *d, size_t n)
{
    return n >= 20 && (d[0] & 0xC0) == 0 && rd_get_be32(d + 4) == STUN_MAGIC && (get16(d + 2) & 3) == 0;
}

int stun_parse(const uint8_t *d, size_t n, stun_view_t *v)
{
    if (!stun_is(d, n))
        return -1;
    size_t body = get16(d + 2);
    if (20 + body > n)
        return -1;
    size_t off = 20;
    while (off + 4 <= 20 + body) { /* validate TLVs */
        size_t al = get16(d + off + 2);
        off += 4 + ((al + 3) & ~(size_t)3);
    }
    if (off != 20 + body)
        return -1;
    v->type = get16(d);
    v->msg = d;
    v->len = 20 + body;
    v->tid = d + 8;
    return 0;
}

static const uint8_t *find_attr(const stun_view_t *v, uint16_t attr, size_t *len, size_t *at)
{
    size_t off = 20;
    while (off + 4 <= v->len) {
        uint16_t t = get16(v->msg + off), al = get16(v->msg + off + 2);
        if (t == attr) {
            if (len)
                *len = al;
            if (at)
                *at = off;
            return v->msg + off + 4;
        }
        off += 4 + ((al + 3u) & ~3u);
    }
    return NULL;
}

const uint8_t *stun_attr(const stun_view_t *v, uint16_t attr, size_t *len) { return find_attr(v, attr, len, NULL); }

int stun_get_xaddr(const stun_view_t *v, uint16_t attr, struct sockaddr_storage *out)
{
    size_t n;
    const uint8_t *p = stun_attr(v, attr, &n);
    if (!p || n < 8)
        return -1;
    memset(out, 0, sizeof *out);
    uint16_t port = get16(p + 2) ^ (uint16_t)(STUN_MAGIC >> 16);
    int xored = attr != STUN_A_MAPPED_ADDRESS;
    if (!xored)
        port = get16(p + 2);
    if (p[1] == 1) {
        struct sockaddr_in *s = (struct sockaddr_in *)out;
        s->sin_family = AF_INET;
        s->sin_port = htons(port);
        uint32_t ip = rd_get_be32(p + 4);
        if (xored)
            ip ^= STUN_MAGIC;
        s->sin_addr.s_addr = htonl(ip);
        return 0;
    }
    if (p[1] == 2 && n >= 20) {
        struct sockaddr_in6 *s = (struct sockaddr_in6 *)out;
        s->sin6_family = AF_INET6;
        s->sin6_port = htons(port);
        uint8_t x[16];
        rd_put_be32(x, STUN_MAGIC);
        memcpy(x + 4, v->tid, 12);
        for (int i = 0; i < 16; i++)
            s->sin6_addr.s6_addr[i] = xored ? p[4 + i] ^ x[i] : p[4 + i];
        return 0;
    }
    return -1;
}

int stun_get_u32(const stun_view_t *v, uint16_t attr, uint32_t *out)
{
    size_t n;
    const uint8_t *p = stun_attr(v, attr, &n);
    if (!p || n < 4)
        return -1;
    *out = rd_get_be32(p);
    return 0;
}

int stun_check_integrity(const stun_view_t *v, const void *key, size_t keylen)
{
    size_t n, at;
    const uint8_t *mi = find_attr(v, STUN_A_MESSAGE_INTEGRITY, &n, &at);
    if (!mi || n != 20)
        return 0;
    uint8_t tmp[1500];
    if (at > sizeof tmp)
        return 0;
    memcpy(tmp, v->msg, at);
    put16(tmp + 2, (uint16_t)(at - 20 + 24));
    uint8_t mac[20];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), key, keylen, tmp, at, mac);
    unsigned diff = 0;
    for (int i = 0; i < 20; i++)
        diff |= mac[i] ^ mi[i];
    return diff == 0;
}

int stun_check_fingerprint(const stun_view_t *v)
{
    size_t n, at;
    const uint8_t *fp = find_attr(v, STUN_A_FINGERPRINT, &n, &at);
    if (!fp)
        return 1; /* optional */
    if (n != 4)
        return 0;
    uint8_t tmp[1500];
    if (at > sizeof tmp)
        return 0;
    memcpy(tmp, v->msg, at);
    put16(tmp + 2, (uint16_t)(at - 20 + 8));
    return (stun_crc32(tmp, at) ^ 0x5354554Eu) == rd_get_be32(fp);
}

int stun_error_code(const stun_view_t *v)
{
    size_t n;
    const uint8_t *p = stun_attr(v, STUN_A_ERROR_CODE, &n);
    if (!p || n < 4)
        return 0;
    return (p[2] & 7) * 100 + p[3];
}

void stun_lt_key(const char *user, const char *realm, const char *pass, uint8_t out[16])
{
    char buf[512];
    int n = snprintf(buf, sizeof buf, "%s:%s:%s", user, realm, pass);
    mbedtls_md5((const unsigned char *)buf, (size_t)n, out);
}

/* ---------------- sockaddr helpers ---------------- */

int addr_equal(const struct sockaddr_storage *a, const struct sockaddr_storage *b)
{
    if (a->ss_family != b->ss_family)
        return 0;
    if (a->ss_family == AF_INET) {
        const struct sockaddr_in *x = (const void *)a, *y = (const void *)b;
        return x->sin_port == y->sin_port && x->sin_addr.s_addr == y->sin_addr.s_addr;
    }
    const struct sockaddr_in6 *x = (const void *)a, *y = (const void *)b;
    return x->sin6_port == y->sin6_port && !memcmp(&x->sin6_addr, &y->sin6_addr, 16);
}

int addr_port(const struct sockaddr_storage *a)
{
    return a->ss_family == AF_INET ? ntohs(((const struct sockaddr_in *)a)->sin_port)
                                   : ntohs(((const struct sockaddr_in6 *)a)->sin6_port);
}

socklen_t addr_len(const struct sockaddr_storage *a)
{
    return a->ss_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
}

void addr_str(const struct sockaddr_storage *a, char *out, size_t cap)
{
    char ip[INET6_ADDRSTRLEN] = "?";
    if (a->ss_family == AF_INET) {
        inet_ntop(AF_INET, &((const struct sockaddr_in *)a)->sin_addr, ip, sizeof ip);
        snprintf(out, cap, "%s:%d", ip, addr_port(a));
    } else {
        inet_ntop(AF_INET6, &((const struct sockaddr_in6 *)a)->sin6_addr, ip, sizeof ip);
        snprintf(out, cap, "[%s]:%d", ip, addr_port(a));
    }
}

int addr_parse(const char *ip, int port, struct sockaddr_storage *out)
{
    memset(out, 0, sizeof *out);
    struct sockaddr_in *s4 = (struct sockaddr_in *)out;
    struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)out;
    if (inet_pton(AF_INET, ip, &s4->sin_addr) == 1) {
        s4->sin_family = AF_INET;
        s4->sin_port = htons((uint16_t)port);
        return 0;
    }
    if (inet_pton(AF_INET6, ip, &s6->sin6_addr) == 1) {
        s6->sin6_family = AF_INET6;
        s6->sin6_port = htons((uint16_t)port);
        return 0;
    }
    return -1;
}
