#include "dtls.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include "rd.h"

static mbedtls_entropy_context entropy;
static mbedtls_ctr_drbg_context drbg;
static mbedtls_pk_context key;
static mbedtls_x509_crt cert;
static char fingerprint[100];
static int ready;

static const mbedtls_ssl_srtp_profile PROFILES[] = { MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80, MBEDTLS_TLS_SRTP_UNSET };

struct dtls {
    struct ev_loop *loop;
    dtls_cb_t cb;
    void *ud;
    int is_client, connected, closed, fp_ok;
    char remote_fp[100];
    mbedtls_ssl_config conf;
    mbedtls_ssl_context ssl;
    const uint8_t *in;      /* the datagram being processed */
    size_t in_len;
    ev_timer timer;
    ev_tstamp int_at, fin_at; /* mbedtls timer: intermediate / final deadlines (0 = cancelled) */
    unsigned char master[48], randbytes[64];
    mbedtls_tls_prf_types prf;
    int have_keys;
};

static void fp_of(const unsigned char *der, size_t n, char *out)
{
    uint8_t h[32];
    mbedtls_sha256(der, n, h, 0);
    for (int i = 0; i < 32; i++)
        sprintf(out + i * 3, "%02X%s", h[i], i < 31 ? ":" : "");
}

int dtls_global_init(void)
{
    if (ready)
        return 0;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_pk_init(&key);
    mbedtls_x509_crt_init(&cert);
    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const unsigned char *)"remodroid-dtls", 14) ||
        mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) ||
        mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), mbedtls_ctr_drbg_random, &drbg))
        return -1;

    mbedtls_x509write_cert w;
    mbedtls_x509write_crt_init(&w);
    mbedtls_x509write_crt_set_version(&w, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&w, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&w, &key);
    mbedtls_x509write_crt_set_issuer_key(&w, &key);
    mbedtls_x509write_crt_set_subject_name(&w, "CN=remodroid");
    mbedtls_x509write_crt_set_issuer_name(&w, "CN=remodroid");
    unsigned char serial[8];
    rd_random(serial, sizeof serial);
    serial[0] &= 0x7f;
    mbedtls_x509write_crt_set_serial_raw(&w, serial, sizeof serial);
    char nb[16], na[16];
    time_t t = time(NULL) - 86400, t2 = t + 30 * 86400;
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(nb, sizeof nb, "%Y%m%d%H%M%S", &tm);
    gmtime_r(&t2, &tm);
    strftime(na, sizeof na, "%Y%m%d%H%M%S", &tm);
    mbedtls_x509write_crt_set_validity(&w, nb, na);
    unsigned char der[2048];
    int n = mbedtls_x509write_crt_der(&w, der, sizeof der, mbedtls_ctr_drbg_random, &drbg);
    mbedtls_x509write_crt_free(&w);
    if (n <= 0 || mbedtls_x509_crt_parse_der(&cert, der + sizeof der - n, (size_t)n))
        return -1;
    fp_of(der + sizeof der - n, (size_t)n, fingerprint);
    ready = 1;
    return 0;
}

const char *dtls_fingerprint(void) { return fingerprint; }

/* ---------------- BIO + timer ---------------- */

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    dtls_t *d = ctx;
    if (d->cb.send)
        d->cb.send(d->ud, buf, len);
    return (int)len;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    dtls_t *d = ctx;
    if (!d->in)
        return MBEDTLS_ERR_SSL_WANT_READ;
    size_t n = RD_MIN(len, d->in_len);
    memcpy(buf, d->in, n);
    d->in = NULL;
    return (int)n;
}

static void timer_set(void *ctx, uint32_t int_ms, uint32_t fin_ms)
{
    dtls_t *d = ctx;
    ev_timer_stop(d->loop, &d->timer);
    if (!fin_ms) {
        d->int_at = d->fin_at = 0;
        return;
    }
    ev_tstamp now = ev_now(d->loop);
    d->int_at = now + int_ms / 1000.;
    d->fin_at = now + fin_ms / 1000.;
    ev_timer_set(&d->timer, fin_ms / 1000. + 0.001, 0.);
    ev_timer_start(d->loop, &d->timer);
}

static int timer_get(void *ctx)
{
    dtls_t *d = ctx;
    if (!d->fin_at)
        return -1;
    ev_tstamp now = ev_now(d->loop);
    if (now >= d->fin_at)
        return 2;
    if (now >= d->int_at)
        return 1;
    return 0;
}

static void export_keys(void *ctx, mbedtls_ssl_key_export_type type, const unsigned char *secret, size_t secret_len,
                        const unsigned char crand[32], const unsigned char srand[32], mbedtls_tls_prf_types prf)
{
    dtls_t *d = ctx;
    if (type != MBEDTLS_SSL_KEY_EXPORT_TLS12_MASTER_SECRET || secret_len != 48)
        return;
    memcpy(d->master, secret, 48);
    memcpy(d->randbytes, crand, 32);
    memcpy(d->randbytes + 32, srand, 32);
    d->prf = prf;
    d->have_keys = 1;
}

static int verify(void *ctx, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
    dtls_t *d = ctx;
    if (depth != 0)
        return 0;
    char fp[100];
    fp_of(crt->raw.p, crt->raw.len, fp);
    d->fp_ok = rd_streq_nocase(fp, d->remote_fp);
    if (d->fp_ok)
        *flags = 0; /* self-signed by design: the SDP fingerprint is the trust anchor */
    else
        LOGW("dtls: peer certificate fingerprint does not match the SDP");
    return 0;
}

/* ---------------- handshake / data ---------------- */

static void fail(dtls_t *d, int err)
{
    if (d->closed)
        return;
    d->closed = 1;
    char buf[128];
    mbedtls_strerror(err, buf, sizeof buf);
    LOGW("dtls: %s (-0x%04x)", buf, -err);
    if (d->cb.on_closed)
        d->cb.on_closed(d->ud, err);
}

static void finish_handshake(dtls_t *d)
{
    mbedtls_dtls_srtp_info info;
    mbedtls_ssl_get_dtls_srtp_negotiation_result(&d->ssl, &info);
    if (info.MBEDTLS_PRIVATE(chosen_dtls_srtp_profile) != MBEDTLS_TLS_SRTP_AES128_CM_HMAC_SHA1_80 || !d->have_keys || !d->fp_ok) {
        LOGW("dtls: handshake finished without SRTP keys or a verified fingerprint");
        fail(d, MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE);
        return;
    }
    uint8_t keys[60];
    if (mbedtls_ssl_tls_prf(d->prf, d->master, 48, "EXTRACTOR-dtls_srtp", d->randbytes, 64, keys, sizeof keys)) {
        fail(d, MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE);
        return;
    }
    d->connected = 1;
    LOGI("dtls: connected (%s, %s)", d->is_client ? "client" : "server", mbedtls_ssl_get_ciphersuite(&d->ssl));
    if (d->cb.on_connected)
        d->cb.on_connected(d->ud, keys, d->is_client);
}

static void step(dtls_t *d)
{
    if (d->closed)
        return;
    if (!d->connected) {
        int r = mbedtls_ssl_handshake(&d->ssl);
        if (r == 0)
            finish_handshake(d);
        else if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE)
            fail(d, r);
        if (!d->connected)
            return;
    }
    unsigned char buf[16384];
    for (;;) {
        int r = mbedtls_ssl_read(&d->ssl, buf, sizeof buf);
        if (r > 0) {
            if (d->cb.on_data)
                d->cb.on_data(d->ud, buf, (size_t)r);
            if (d->closed)
                return;
            continue;
        }
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
            return;
        if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == 0) {
            LOGI("dtls: closed by the peer");
            d->closed = 1;
            if (d->cb.on_closed)
                d->cb.on_closed(d->ud, 0);
            return;
        }
        if (r == MBEDTLS_ERR_SSL_CLIENT_RECONNECT)
            return;
        fail(d, r);
        return;
    }
}

static void on_timer(struct ev_loop *loop, ev_timer *w, int revents) { step(w->data); }

dtls_t *dtls_new(struct ev_loop *loop, int is_client, const char *remote_fp, dtls_cb_t cb, void *ud)
{
    if (dtls_global_init() < 0)
        return NULL;
    dtls_t *d = calloc(1, sizeof *d);
    d->loop = loop;
    d->is_client = is_client;
    d->cb = cb;
    d->ud = ud;
    snprintf(d->remote_fp, sizeof d->remote_fp, "%s", remote_fp ? remote_fp : "");
    ev_timer_init(&d->timer, on_timer, 1., 0.);
    d->timer.data = d;
    mbedtls_ssl_config_init(&d->conf);
    mbedtls_ssl_init(&d->ssl);
    int r = mbedtls_ssl_config_defaults(&d->conf, is_client ? MBEDTLS_SSL_IS_CLIENT : MBEDTLS_SSL_IS_SERVER,
                                        MBEDTLS_SSL_TRANSPORT_DATAGRAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (r)
        goto err;
    mbedtls_ssl_conf_rng(&d->conf, mbedtls_ctr_drbg_random, &drbg);
    mbedtls_ssl_conf_authmode(&d->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&d->conf, &cert, NULL); /* placeholder; trust comes from verify() */
    mbedtls_ssl_conf_verify(&d->conf, verify, d);
    if ((r = mbedtls_ssl_conf_own_cert(&d->conf, &cert, &key)))
        goto err;
    if ((r = mbedtls_ssl_conf_dtls_srtp_protection_profiles(&d->conf, PROFILES)))
        goto err;
    mbedtls_ssl_conf_srtp_mki_value_supported(&d->conf, MBEDTLS_SSL_DTLS_SRTP_MKI_UNSUPPORTED);
    mbedtls_ssl_conf_min_tls_version(&d->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&d->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_handshake_timeout(&d->conf, 400, 8000);
    if (!is_client)
        mbedtls_ssl_conf_dtls_cookies(&d->conf, NULL, NULL, NULL); /* ICE already proved reachability */
    if ((r = mbedtls_ssl_setup(&d->ssl, &d->conf)))
        goto err;
    mbedtls_ssl_set_bio(&d->ssl, d, bio_send, bio_recv, NULL);
    mbedtls_ssl_set_timer_cb(&d->ssl, d, timer_set, timer_get);
    mbedtls_ssl_set_export_keys_cb(&d->ssl, export_keys, d);
    mbedtls_ssl_set_mtu(&d->ssl, 1200);
    return d;
err:
    LOGE("dtls: setup failed (-0x%04x)", -r);
    dtls_free(d);
    return NULL;
}

void dtls_start(dtls_t *d) { step(d); }

void dtls_input(dtls_t *d, const uint8_t *data, size_t n)
{
    d->in = data;
    d->in_len = n;
    step(d);
    d->in = NULL;
}

int dtls_write(dtls_t *d, const uint8_t *data, size_t n)
{
    if (!d->connected || d->closed)
        return -1;
    int r = mbedtls_ssl_write(&d->ssl, data, n);
    return r < 0 ? -1 : 0;
}

int dtls_connected(const dtls_t *d) { return d && d->connected && !d->closed; }

void dtls_free(dtls_t *d)
{
    if (!d)
        return;
    if (d->connected && !d->closed)
        mbedtls_ssl_close_notify(&d->ssl);
    ev_timer_stop(d->loop, &d->timer);
    mbedtls_ssl_free(&d->ssl);
    mbedtls_ssl_config_free(&d->conf);
    free(d);
}
