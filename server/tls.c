/* HTTPS for the [tls] listener, on mbedTLS (TLS 1.2). The certificate comes
 * from [tls] cert/key or from the ACME client (acme.c). A renewed certificate
 * gets a new config; connections already open keep a reference to the old one
 * until they close. */
#include <stdlib.h>
#include <string.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include "server.h"

typedef struct certset {
    mbedtls_x509_crt chain;
    mbedtls_pk_context key;
    mbedtls_ssl_config conf;
    int refs;
} certset_t;

typedef struct {
    mbedtls_ssl_context ssl;
    certset_t *cs;
    conn_t *c;
    size_t wlen; /* length of a write to repeat after WANT_WRITE */
} tconn_t;

static struct {
    int ready;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    certset_t *cur;
} G;

static const char *tls_err(int r)
{
    static char buf[160];
    mbedtls_strerror(r, buf, sizeof buf);
    return buf;
}

static void certset_unref(certset_t *cs)
{
    if (!cs || --cs->refs > 0)
        return;
    mbedtls_ssl_config_free(&cs->conf);
    mbedtls_pk_free(&cs->key);
    mbedtls_x509_crt_free(&cs->chain);
    free(cs);
}

/* The first DNS name of a certificate (newer CA profiles leave the subject empty). */
static void cert_name(const mbedtls_x509_crt *x, char *out, size_t cap)
{
    for (const mbedtls_x509_sequence *s = &x->subject_alt_names; s && s->buf.p; s = s->next) {
        if (s->buf.tag == (MBEDTLS_ASN1_CONTEXT_SPECIFIC | MBEDTLS_X509_SAN_DNS_NAME)) {
            snprintf(out, cap, "%.*s", (int)s->buf.len, (const char *)s->buf.p);
            return;
        }
    }
    if (mbedtls_x509_dn_gets(out, cap, &x->subject) < 0)
        snprintf(out, cap, "?");
}

int tls_load(const char *cert_file, const char *key_file)
{
    if (!G.ready) {
        mbedtls_entropy_init(&G.entropy);
        mbedtls_ctr_drbg_init(&G.drbg);
        if (mbedtls_ctr_drbg_seed(&G.drbg, mbedtls_entropy_func, &G.entropy, (const unsigned char *)"rd-https", 8)) {
            LOGE("tls: cannot seed the random generator");
            return -1;
        }
        G.ready = 1;
    }
    certset_t *cs = calloc(1, sizeof *cs);
    cs->refs = 1;
    mbedtls_x509_crt_init(&cs->chain);
    mbedtls_pk_init(&cs->key);
    mbedtls_ssl_config_init(&cs->conf);
    int r = mbedtls_x509_crt_parse_file(&cs->chain, cert_file);
    if (r != 0) {
        LOGE("tls: cannot load %s: %s", cert_file, r < 0 ? tls_err(r) : "some certificates did not parse");
        goto fail;
    }
    if ((r = mbedtls_pk_parse_keyfile(&cs->key, key_file, NULL, mbedtls_ctr_drbg_random, &G.drbg)) ||
        (r = mbedtls_pk_check_pair(&cs->chain.pk, &cs->key, mbedtls_ctr_drbg_random, &G.drbg))) {
        LOGE("tls: cannot use %s with %s: %s", key_file, cert_file, tls_err(r));
        goto fail;
    }
    if ((r = mbedtls_ssl_config_defaults(&cs->conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT)) ||
        (r = mbedtls_ssl_conf_own_cert(&cs->conf, &cs->chain, &cs->key))) {
        LOGE("tls: %s", tls_err(r));
        goto fail;
    }
    mbedtls_ssl_conf_rng(&cs->conf, mbedtls_ctr_drbg_random, &G.drbg);

    char name[256];
    cert_name(&cs->chain, name, sizeof name);
    const mbedtls_x509_time *t = &cs->chain.valid_to;
    certset_unref(G.cur); /* open connections hold their own reference */
    G.cur = cs;
    LOGI("tls: certificate %s, valid until %04d-%02d-%02d %02d:%02d UTC", name, t->year, t->mon, t->day, t->hour, t->min);
    return 0;
fail:
    certset_unref(cs);
    return -1;
}

int tls_ready(void) { return G.cur != NULL; }

static int bio_send(void *ctx, const unsigned char *b, size_t n)
{
    tconn_t *t = ctx;
    ssize_t r = send(t->c->fd, (const void *)b, (int)RD_MIN(n, (size_t)1 << 30), MSG_NOSIGNAL);
    if (r < 0)
        return rd_sock_again(rd_sock_error()) ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
    return (int)r;
}

static int bio_recv(void *ctx, unsigned char *b, size_t n)
{
    tconn_t *t = ctx;
    ssize_t r = recv(t->c->fd, (void *)b, (int)RD_MIN(n, (size_t)1 << 30), 0);
    if (r < 0)
        return rd_sock_again(rd_sock_error()) ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
    return (int)r; /* 0 = EOF */
}

int tls_conn_start(conn_t *c)
{
    if (!G.cur)
        return -1;
    tconn_t *t = calloc(1, sizeof *t);
    t->c = c;
    t->cs = G.cur;
    mbedtls_ssl_init(&t->ssl);
    if (mbedtls_ssl_setup(&t->ssl, &t->cs->conf)) {
        mbedtls_ssl_free(&t->ssl);
        free(t);
        return -1;
    }
    t->cs->refs++;
    mbedtls_ssl_set_bio(&t->ssl, t, bio_send, bio_recv, NULL);
    c->ssl = t;
    return 0;
}

/* Maps an mbedTLS result; TLS_AGAIN tells the caller which readiness to wait for. */
static ssize_t io_result(conn_t *c, int r, int reading)
{
    switch (r) {
    case MBEDTLS_ERR_SSL_WANT_READ:
        if (!reading)
            c->ssl_wr_wants_r = 1;
        return TLS_AGAIN;
    case MBEDTLS_ERR_SSL_WANT_WRITE:
        if (reading)
            c->ssl_rd_wants_w = 1;
        return TLS_AGAIN;
    case 0:
    case MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY:
    case MBEDTLS_ERR_SSL_CONN_EOF:
        return 0;
    case MBEDTLS_ERR_NET_RECV_FAILED:
    case MBEDTLS_ERR_NET_SEND_FAILED:
        LOGD("%s: tls: connection reset", c->ip);
        return -1;
    default:
        /* handshake failures are routine (scanners, old clients) */
        LOGD("%s: tls: %s", c->ip, tls_err(r));
        return -1;
    }
}

ssize_t tls_read(conn_t *c, void *buf, size_t n)
{
    tconn_t *t = c->ssl;
    c->ssl_rd_wants_w = 0;
    int r = mbedtls_ssl_read(&t->ssl, buf, n); /* runs the handshake first */
    return r > 0 ? r : io_result(c, r, 1);
}

ssize_t tls_write(conn_t *c, const void *buf, size_t n)
{
    tconn_t *t = c->ssl;
    c->ssl_wr_wants_r = 0;
    /* after WANT_WRITE, mbedTLS must be called again with the same length */
    size_t len = t->wlen ? t->wlen : RD_MIN(n, (size_t)16384);
    int r = mbedtls_ssl_write(&t->ssl, buf, len);
    if (r == MBEDTLS_ERR_SSL_WANT_WRITE || r == MBEDTLS_ERR_SSL_WANT_READ) {
        t->wlen = len;
        return io_result(c, r, 0);
    }
    t->wlen = 0;
    return r > 0 ? r : io_result(c, r ? r : MBEDTLS_ERR_NET_SEND_FAILED, 0);
}

void tls_conn_free(conn_t *c)
{
    tconn_t *t = c->ssl;
    if (!t)
        return;
    if (mbedtls_ssl_is_handshake_over(&t->ssl))
        mbedtls_ssl_close_notify(&t->ssl); /* one try; the socket closes right after */
    mbedtls_ssl_free(&t->ssl);
    certset_unref(t->cs);
    free(t);
    c->ssl = NULL;
}
