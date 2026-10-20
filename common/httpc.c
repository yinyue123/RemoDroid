#include "httpc.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <fcntl.h>
#include <strings.h>
#include <sys/time.h>
#else
#define strncasecmp _strnicmp
#endif

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include "cas.h"
#include "picohttpparser.h"
#include "plat.h"
#include "rd.h"

#define MAX_BODY (1024 * 1024)

struct httpc {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509_crt ca;
    mbedtls_ssl_config conf;
    int tls_ready;
    char ca_file[512], ua[64];
};

static int seterr(char *err, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int seterr(char *err, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
    return -1;
}

static const char *mbed_err(int r)
{
    static __thread char buf[160];
    mbedtls_strerror(r, buf, sizeof buf);
    return buf;
}

int httpc_rng(void *h, unsigned char *out, size_t n) { return mbedtls_ctr_drbg_random(&((httpc_t *)h)->drbg, out, n); }

httpc_t *httpc_new(const char *ca_file, const char *user_agent)
{
    httpc_t *h = calloc(1, sizeof *h);
    mbedtls_entropy_init(&h->entropy);
    mbedtls_ctr_drbg_init(&h->drbg);
    mbedtls_x509_crt_init(&h->ca);
    mbedtls_ssl_config_init(&h->conf);
    snprintf(h->ca_file, sizeof h->ca_file, "%s", ca_file ? ca_file : "");
    snprintf(h->ua, sizeof h->ua, "%s", user_agent ? user_agent : "remodroid");
    if (mbedtls_ctr_drbg_seed(&h->drbg, mbedtls_entropy_func, &h->entropy, (const unsigned char *)"rd-httpc", 8)) {
        httpc_free(h);
        return NULL;
    }
    return h;
}

void httpc_free(httpc_t *h)
{
    if (!h)
        return;
    mbedtls_ssl_config_free(&h->conf);
    mbedtls_x509_crt_free(&h->ca);
    mbedtls_ctr_drbg_free(&h->drbg);
    mbedtls_entropy_free(&h->entropy);
    free(h);
}

void httpc_resp_free(httpc_resp_t *r)
{
    free(r->body);
    memset(r, 0, sizeof *r);
}

/* Trusted roots and the TLS client config, on the first https:// request. */
static int tls_init(httpc_t *h, char *err, size_t cap)
{
    if (h->tls_ready)
        return 0;
    if (rd_load_cas(&h->ca, h->ca_file, NULL, 0, err, cap) < 0)
        return -1;
    int r = mbedtls_ssl_config_defaults(&h->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT);
    if (r)
        return seterr(err, cap, "TLS config: %s", mbed_err(r));
    mbedtls_ssl_conf_authmode(&h->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&h->conf, &h->ca, NULL);
    mbedtls_ssl_conf_rng(&h->conf, httpc_rng, h);
    h->tls_ready = 1;
    return 0;
}

static int bsend(void *ctx, const unsigned char *b, size_t n)
{
    ssize_t r = send(*(int *)ctx, (const char *)b, n, MSG_NOSIGNAL);
    return r < 0 ? MBEDTLS_ERR_NET_SEND_FAILED : (int)r;
}

static int brecv(void *ctx, unsigned char *b, size_t n)
{
    ssize_t r = recv(*(int *)ctx, (char *)b, n, 0);
    return r < 0 ? MBEDTLS_ERR_NET_RECV_FAILED : (int)r; /* blocking, with SO_RCVTIMEO */
}

static void hdr_copy(const struct phr_header *h, char *out, size_t cap)
{
    size_t n = RD_MIN(h->value_len, cap - 1);
    memcpy(out, h->value, n);
    out[n] = 0;
}

typedef struct {
    int fd;
    mbedtls_ssl_context *ssl;
} io_t;

/* Headers plus a Content-Length body, or a chunked body's last chunk, are in. */
static int response_complete(const char *method, const uint8_t *p, size_t n)
{
    int minor, status;
    const char *msg;
    size_t ml, nh = 40;
    struct phr_header hd[40];
    int hl = phr_parse_response((const char *)p, n, &minor, &status, &msg, &ml, hd, &nh, 0);
    if (hl < 0)
        return 0;
    if (!strcmp(method, "HEAD") || status == 204 || status == 304)
        return 1;
    for (size_t i = 0; i < nh; i++) {
        if (hd[i].name_len == 14 && !strncasecmp(hd[i].name, "Content-Length", 14))
            return n - (size_t)hl >= (size_t)atol(hd[i].value);
        if (hd[i].name_len == 17 && !strncasecmp(hd[i].name, "Transfer-Encoding", 17))
            return n - (size_t)hl >= 5 && !memcmp(p + n - 5, "0\r\n\r\n", 5);
    }
    return 0; /* no length: read to EOF */
}

static int io_write(io_t *io, const void *d, size_t n)
{
    for (size_t off = 0; off < n;) {
        int k = io->ssl ? mbedtls_ssl_write(io->ssl, (const unsigned char *)d + off, n - off)
                        : (int)send(io->fd, (const char *)d + off, n - off, MSG_NOSIGNAL);
        if (k <= 0)
            return -1;
        off += (size_t)k;
    }
    return 0;
}

static int io_read(io_t *io, void *d, size_t n)
{
    return io->ssl ? mbedtls_ssl_read(io->ssl, d, n) : (int)recv(io->fd, (char *)d, n, 0);
}

int httpc_request(httpc_t *h, const char *method, const char *url, const char *ctype, const char *body,
                  httpc_resp_t *r, char *err, size_t errcap)
{
    memset(r, 0, sizeof *r);
    int tls;
    const char *hs;
    if (!strncmp(url, "https://", 8))
        tls = 1, hs = url + 8;
    else if (!strncmp(url, "http://", 7))
        tls = 0, hs = url + 7;
    else
        return seterr(err, errcap, "not an http(s) URL: %.200s", url);
    if (tls && tls_init(h, err, errcap) < 0)
        return -1;
    const char *slash = strchr(hs, '/');
    const char *path = slash ? slash : "/";
    char hp[300], host[256], port[16];
    size_t hl = slash ? (size_t)(slash - hs) : strlen(hs);
    if (hl >= sizeof hp)
        return seterr(err, errcap, "URL too long");
    memcpy(hp, hs, hl);
    hp[hl] = 0;
    if (rd_split_hostport(hp, host, sizeof host, port, sizeof port, tls ? "443" : "80") < 0)
        return seterr(err, errcap, "bad URL %.200s", url);

    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *ai, *p;
    rd_net_init();
    int e = getaddrinfo(host, port, &hints, &ai);
    if (e)
        return seterr(err, errcap, "%s: %s", host, gai_strerror(e));
    int fd = -1, cerr = 0;
    for (p = ai; p; p = p->ai_next) { /* every address, e.g. IPv4 after a failed IPv6 */
#ifdef _WIN32
        fd = (int)socket(p->ai_family, SOCK_STREAM, 0);
        if (fd == (int)INVALID_SOCKET) {
            fd = -1;
            continue;
        }
        DWORD tv = 20000;
#else
        fd = socket(p->ai_family, SOCK_STREAM, 0);
        if (fd < 0)
            continue;
        fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); /* macOS: no MSG_NOSIGNAL */
#endif
        struct timeval tv = { 20, 0 };
#endif
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof tv);
        if (connect(fd, p->ai_addr, (socklen_t)p->ai_addrlen) == 0)
            break;
        cerr = rd_sock_error();
        rd_sock_close(fd);
        fd = -1;
    }
    freeaddrinfo(ai);
    if (fd < 0)
        return seterr(err, errcap, "cannot connect to %s:%s: %s", host, port, rd_sock_strerror(cerr));

    int ret = -1, k;
    rd_buf_t in;
    rd_buf_init(&in);
    mbedtls_ssl_context ssl;
    mbedtls_ssl_init(&ssl);
    io_t io = { fd, tls ? &ssl : NULL };
    if (tls) {
        if ((k = mbedtls_ssl_setup(&ssl, &h->conf)) || (k = mbedtls_ssl_set_hostname(&ssl, host))) {
            seterr(err, errcap, "TLS setup: %s", mbed_err(k));
            goto out;
        }
        mbedtls_ssl_set_bio(&ssl, &fd, bsend, brecv, NULL);
        if ((k = mbedtls_ssl_handshake(&ssl))) {
            uint32_t flags = mbedtls_ssl_get_verify_result(&ssl);
            if (flags && flags != (uint32_t)-1) {
                char info[200];
                mbedtls_x509_crt_verify_info(info, sizeof info, "", flags);
                info[strcspn(info, "\n")] = 0;
                seterr(err, errcap, "%s: certificate not trusted: %s", host, info);
            } else {
                seterr(err, errcap, "%s: TLS handshake failed: %s", host, mbed_err(k));
            }
            goto out;
        }
    }
    char req[2048];
    size_t blen = body ? strlen(body) : 0;
    char ct[128] = "";
    if (ctype)
        snprintf(ct, sizeof ct, "Content-Type: %s\r\n", ctype);
    int n = snprintf(req, sizeof req,
                     "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\nAccept: */*\r\nConnection: close\r\n"
                     "%sContent-Length: %zu\r\n\r\n",
                     method, path, hp, h->ua, ct, blen);
    if ((size_t)n >= sizeof req) {
        seterr(err, errcap, "request too long");
        goto out;
    }
    if (io_write(&io, req, (size_t)n) < 0 || (blen && io_write(&io, body, blen) < 0)) {
        seterr(err, errcap, "%s: write failed", host);
        goto out;
    }
    for (;;) { /* until the body is complete, or the server closes (Connection: close) */
        rd_buf_reserve(&in, 16384);
        k = io_read(&io, in.p + in.len, in.cap - in.len);
        if (k <= 0)
            break; /* close_notify, EOF, or a reset after the body (checked below) */
        in.len += (size_t)k;
        if (in.len > MAX_BODY) {
            seterr(err, errcap, "%s: response too large", host);
            goto out;
        }
        if (response_complete(method, in.p, in.len))
            break;
    }
    int minor;
    const char *msg;
    size_t msg_len, nh = 40;
    struct phr_header hd[40];
    int hlen = phr_parse_response((const char *)in.p, in.len, &minor, &r->status, &msg, &msg_len, hd, &nh, 0);
    if (hlen < 0) {
        seterr(err, errcap, "%s: no complete HTTP response", host);
        goto out;
    }
    int chunked = 0;
    long clen = -1;
    for (size_t i = 0; i < nh; i++) {
        if (hd[i].name_len == 8 && !strncasecmp(hd[i].name, "Location", 8))
            hdr_copy(&hd[i], r->location, sizeof r->location);
        else if (hd[i].name_len == 12 && !strncasecmp(hd[i].name, "Replay-Nonce", 12))
            hdr_copy(&hd[i], r->nonce, sizeof r->nonce);
        else if (hd[i].name_len == 11 && !strncasecmp(hd[i].name, "Retry-After", 11))
            r->retry_after = atoi(hd[i].value);
        else if (hd[i].name_len == 14 && !strncasecmp(hd[i].name, "Content-Length", 14))
            clen = atol(hd[i].value);
        else if (hd[i].name_len == 17 && !strncasecmp(hd[i].name, "Transfer-Encoding", 17) &&
                 hd[i].value_len >= 7 && !strncasecmp(hd[i].value, "chunked", 7))
            chunked = 1;
    }
    size_t blen2 = in.len - (size_t)hlen;
    if (!chunked && clen >= 0 && strcmp(method, "HEAD")) {
        if (blen2 < (size_t)clen) {
            seterr(err, errcap, "%s: response cut short", host);
            goto out;
        }
        blen2 = (size_t)clen;
    }
    r->body = malloc(blen2 + 1);
    memcpy(r->body, in.p + hlen, blen2);
    if (chunked) {
        struct phr_chunked_decoder dec = { 0 };
        if (phr_decode_chunked(&dec, r->body, &blen2) == -1) {
            seterr(err, errcap, "%s: bad chunked body", host);
            goto out;
        }
    }
    r->body[blen2] = 0;
    r->len = blen2;
    ret = 0;
out:
    if (tls)
        mbedtls_ssl_close_notify(&ssl);
    mbedtls_ssl_free(&ssl);
    rd_sock_close(fd);
    rd_buf_free(&in);
    if (ret < 0)
        httpc_resp_free(r);
    return ret;
}
