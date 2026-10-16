#include "rd.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#if !defined(__ANDROID__) || __ANDROID_API__ >= 28
#include <sys/random.h>
#endif

#include <mbedtls/base64.h>
#include <mbedtls/md.h>
#include <mbedtls/sha1.h>
#include <mbedtls/sha256.h>

/* ---------------- logging ---------------- */

int rd_log_level = RD_LOG_INFO;
static const char *log_prog = "remodroid";
static void (*log_hook)(const char *line);

void rd_log_init(const char *prog, int level)
{
    log_prog = prog;
    rd_log_level = level;
}

void rd_log_set_hook(void (*hook)(const char *line)) { log_hook = hook; }

int rd_log_parse_level(const char *s, int def)
{
    static const char *names[] = { "error", "warn", "info", "debug", "trace" };
    if (!s || !*s)
        return def;
    for (size_t i = 0; i < RD_ARRAY_LEN(names); i++)
        if (rd_streq_nocase(s, names[i]))
            return (int)i;
    return def;
}

void rd_log(int level, const char *fmt, ...)
{
    static const char tag[] = "EWIDT";
    if (level > rd_log_level)
        return;
    char line[1024];
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    int n = snprintf(line, sizeof line, "%02d:%02d:%02d.%03ld %c %s: ", tm.tm_hour, tm.tm_min, tm.tm_sec,
                     ts.tv_nsec / 1000000, tag[level], log_prog);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof line - (size_t)n, fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s\n", line);
    if (log_hook)
        log_hook(line);
}

/* ---------------- buffer ---------------- */

void rd_buf_init(rd_buf_t *b) { memset(b, 0, sizeof *b); }

void rd_buf_free(rd_buf_t *b)
{
    free(b->p);
    memset(b, 0, sizeof *b);
}

void rd_buf_reset(rd_buf_t *b) { b->len = b->off = 0; }

int rd_buf_reserve(rd_buf_t *b, size_t extra)
{
    if (b->off && b->off == b->len)
        b->off = b->len = 0;
    if (b->len + extra <= b->cap)
        return 0;
    if (b->off) { /* compact before growing */
        memmove(b->p, b->p + b->off, b->len - b->off);
        b->len -= b->off;
        b->off = 0;
        if (b->len + extra <= b->cap)
            return 0;
    }
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + extra)
        cap *= 2;
    uint8_t *p = realloc(b->p, cap);
    if (!p)
        return -1;
    b->p = p;
    b->cap = cap;
    return 0;
}

int rd_buf_append(rd_buf_t *b, const void *data, size_t n)
{
    if (rd_buf_reserve(b, n) < 0)
        return -1;
    if (n)
        memcpy(b->p + b->len, data, n);
    b->len += n;
    return 0;
}

int rd_buf_printf(rd_buf_t *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0 || rd_buf_reserve(b, (size_t)n + 1) < 0)
        return -1;
    va_start(ap, fmt);
    vsnprintf((char *)b->p + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
    return n;
}

void rd_buf_consume(rd_buf_t *b, size_t n)
{
    b->off += n;
    if (b->off >= b->len)
        b->off = b->len = 0;
}

/* ---------------- misc ---------------- */

uint64_t rd_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

uint64_t rd_mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static ssize_t sys_getrandom(void *p, size_t n)
{
#if defined(__ANDROID__) && __ANDROID_API__ < 28
    return syscall(__NR_getrandom, p, n, 0); /* bionic only wraps it from API 28 */
#else
    return getrandom(p, n, 0);
#endif
}

int rd_random(void *out, size_t n)
{
    uint8_t *p = out;
    while (n) {
        ssize_t r = sys_getrandom(p, n);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC); /* very old kernels */
            if (fd < 0)
                return -1;
            r = read(fd, p, n);
            close(fd);
            if (r <= 0)
                return -1;
        }
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

void rd_hex(const uint8_t *in, size_t n, char *out)
{
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = d[in[i] >> 4];
        out[2 * i + 1] = d[in[i] & 15];
    }
    out[2 * n] = 0;
}

int rd_set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0)
        return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

char *rd_strdup(const char *s)
{
    if (!s)
        return NULL;
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

int rd_streq_nocase(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        a++, b++;
    }
    return *a == *b;
}

size_t rd_b64_encode(const void *in, size_t n, char *out, size_t outcap)
{
    size_t olen = 0;
    if (mbedtls_base64_encode((unsigned char *)out, outcap, &olen, in, n) != 0)
        return 0;
    return olen;
}

int rd_b64_decode(const char *in, size_t n, uint8_t *out, size_t outcap, size_t *outlen)
{
    return mbedtls_base64_decode(out, outcap, outlen, (const unsigned char *)in, n) == 0 ? 0 : -1;
}

void rd_sha1(const void *data, size_t n, uint8_t out[20]) { mbedtls_sha1(data, n, out); }

void rd_sha256(const void *data, size_t n, uint8_t out[32]) { mbedtls_sha256(data, n, out, 0); }

void rd_hmac_sha1(const void *key, size_t keylen, const void *data, size_t n, uint8_t out[20])
{
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), key, keylen, data, n, out);
}

uint16_t rd_get_be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
uint32_t rd_get_be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
void rd_put_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
void rd_put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
uint32_t rd_get_le32(const uint8_t *p) { return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }
void rd_put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

int rd_split_hostport(const char *in, char *host, size_t hostcap, char *port, size_t portcap, const char *defport)
{
    const char *h = in, *he, *p = NULL;
    if (*in == '[') {
        h = in + 1;
        he = strchr(h, ']');
        if (!he)
            return -1;
        if (he[1] == ':')
            p = he + 2;
    } else {
        he = strrchr(in, ':');
        if (he && strchr(in, ':') == he) {
            p = he + 1;
        } else {
            he = in + strlen(in);
        }
    }
    size_t hl = (size_t)(he - h);
    if (hl == 0 || hl >= hostcap)
        return -1;
    memcpy(host, h, hl);
    host[hl] = 0;
    const char *ps = p && *p ? p : defport;
    if (!ps || strlen(ps) >= portcap)
        return -1;
    strcpy(port, ps);
    return 0;
}
