/* ACME client (RFC 8555) for automatic certificates, e.g. from Let's Encrypt.
 *
 * [acme] domains = remo.example.com is all that's needed: the server creates an
 * account key, orders a certificate, proves control of each domain with the
 * http-01 challenge (answered by the plain [http] listener on port 80), and
 * renews when a third of the certificate's lifetime is left.
 *
 * The exchange with the CA is blocking HTTPS, so it runs on a worker thread;
 * the event loop only answers challenges (acme_challenge) and loads the result.
 * Account and certificate keys are ECDSA P-256 (JWS alg ES256). Crypto and TLS
 * are mbedTLS; the worker has its own contexts, nothing is shared with the loop. */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <mbedtls/asn1.h>
#include <mbedtls/bignum.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/error.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/x509_csr.h>

#include "httpc.h"
#include "msg.h"
#include "server.h"

#define LETSENCRYPT "https://acme-v02.api.letsencrypt.org/directory"
#define MAX_DOMAINS 16
#define RETRY_MIN 600.          /* first retry after a failure: 10 min, doubling */
#define RETRY_MAX (12 * 3600.)
#define CHECK_EVERY (6 * 3600.) /* renewal check */

/* ---------------- job: settings copied for the worker ---------------- */

typedef struct {
    char directory[512], email[256], dir[512], ca_file[512], profile[64];
    char domains[MAX_DOMAINS][256];
    int ndomains;
    /* result */
    int ok;
    char err[512];
} job_t;

static struct {
    job_t cfg;            /* current settings (event loop) */
    int enabled;
    job_t *running;       /* the worker's job while it runs */
    ev_async done;
    ev_timer timer;
    double backoff;
    rd_mutex_t mu;        /* guards chal[] */
    struct { char token[128], keyauth[300]; } chal[MAX_DOMAINS];
} A = { .mu = RD_MUTEX_INIT };

static void cert_path(const job_t *j, char *out, size_t cap) { snprintf(out, cap, "%s/cert.pem", j->dir); }
static void key_path(const job_t *j, char *out, size_t cap) { snprintf(out, cap, "%s/key.pem", j->dir); }

/* ---------------- challenge table (shared with the event loop) ---------------- */

static void chal_set(const char *token, const char *keyauth)
{
    rd_mutex_lock(&A.mu);
    for (int i = 0; i < MAX_DOMAINS; i++) {
        if (!A.chal[i].token[0] || !strcmp(A.chal[i].token, token)) {
            snprintf(A.chal[i].token, sizeof A.chal[i].token, "%s", token);
            snprintf(A.chal[i].keyauth, sizeof A.chal[i].keyauth, "%s", keyauth);
            break;
        }
    }
    rd_mutex_unlock(&A.mu);
}

static void chal_clear(void)
{
    rd_mutex_lock(&A.mu);
    memset(A.chal, 0, sizeof A.chal);
    rd_mutex_unlock(&A.mu);
}

int acme_challenge(const char *token, size_t len, char *out, size_t cap)
{
    int r = -1;
    rd_mutex_lock(&A.mu);
    for (int i = 0; i < MAX_DOMAINS; i++) {
        if (A.chal[i].token[0] && strlen(A.chal[i].token) == len && !memcmp(A.chal[i].token, token, len)) {
            snprintf(out, cap, "%s", A.chal[i].keyauth);
            r = 0;
            break;
        }
    }
    rd_mutex_unlock(&A.mu);
    if (r == 0)
        LOGI("acme: answered the http-01 challenge %.*s", (int)RD_MIN(len, 16), token);
    return r;
}

/* ---------------- small helpers ---------------- */

static int fail(job_t *j, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static int fail(job_t *j, const char *fmt, ...)
{
    if (!j->err[0]) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(j->err, sizeof j->err, fmt, ap);
        va_end(ap);
    }
    return -1;
}

static const char *mbed_err(int r)
{
    static __thread char buf[160];
    mbedtls_strerror(r, buf, sizeof buf);
    return buf;
}

/* base64url without padding (RFC 7515 §2) */
static char *b64url(const void *d, size_t n)
{
    size_t cap = (n + 2) / 3 * 4 + 1;
    char *s = malloc(cap);
    rd_b64_encode(d, n, s, cap);
    char *w = s;
    for (char *p = s; *p; p++) {
        if (*p == '=')
            break;
        *w++ = *p == '+' ? '-' : *p == '/' ? '_' : *p;
    }
    *w = 0;
    return s;
}

/* Writes via a temporary file and rename(), so a reader never sees half a file. */
static int write_file(const char *path, const void *d, size_t n, mode_t mode)
{
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return -1;
    chmod(tmp, mode);
    int ok = fwrite(d, 1, n, f) == n;
    ok &= fclose(f) == 0;
#ifdef _WIN32 /* rename() won't replace a file there */
    ok = ok && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
#else
    ok = ok && rename(tmp, path) == 0;
#endif
    if (!ok) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* ---------------- HTTPS to the CA (common/httpc.c) ---------------- */

typedef httpc_resp_t resp_t;

typedef struct {
    job_t *j;
    httpc_t *http;              /* also the random generator */
    char nonce[256];
    mbedtls_pk_context key;     /* account key */
    char *jwk;                  /* {"crv":…,"kty":…,"x":…,"y":…} */
    char kid[512];              /* account URL */
} acme_t;

static int rng(void *a, unsigned char *out, size_t n) { return httpc_rng(((acme_t *)a)->http, out, n); }

static int write_key(acme_t *a, const char *path, mbedtls_pk_context *k)
{
    unsigned char pem[1024];
    if (mbedtls_pk_write_key_pem(k, pem, sizeof pem))
        return -1;
    return write_file(path, pem, strlen((char *)pem), 0600);
}

static int new_key(acme_t *a, mbedtls_pk_context *k)
{
    return mbedtls_pk_setup(k, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) ||
           mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(*k), rng, a)
               ? -1
               : 0;
}

static void resp_free(resp_t *r) { httpc_resp_free(r); }

static int https(acme_t *a, const char *method, const char *url, const char *body, resp_t *r)
{
    char err[300];
    if (strncmp(url, "https://", 8))
        return fail(a->j, "not an https URL: %.200s", url);
    if (httpc_request(a->http, method, url, body ? "application/jose+json" : NULL, body, r, err, sizeof err) < 0)
        return fail(a->j, "%s%s", err, strstr(err, "not trusted") || strstr(err, "CA") ? " (set [acme] ca_file?)" : "");
    if (r->nonce[0])
        snprintf(a->nonce, sizeof a->nonce, "%s", r->nonce);
    return 0;
}

/* ---------------- JWS (RFC 7515) with ES256 ---------------- */

static char *make_jwk(mbedtls_pk_context *k)
{
    unsigned char pt[65]; /* 04 | X | Y */
    size_t n;
    if (mbedtls_pk_get_type(k) != MBEDTLS_PK_ECKEY ||
        mbedtls_ecp_write_public_key(mbedtls_pk_ec(*k), MBEDTLS_ECP_PF_UNCOMPRESSED, &n, pt, sizeof pt) || n != 65)
        return NULL;
    char *xs = b64url(pt + 1, 32), *ys = b64url(pt + 33, 32);
    /* members in lexicographic order, no spaces: this exact text is hashed for the thumbprint (RFC 7638) */
    size_t cap = strlen(xs) + strlen(ys) + 64;
    char *out = malloc(cap);
    snprintf(out, cap, "{\"crv\":\"P-256\",\"kty\":\"EC\",\"x\":\"%s\",\"y\":\"%s\"}", xs, ys);
    free(xs);
    free(ys);
    return out;
}

/* ES256 signature: raw r || s, 32 bytes each (mbedTLS gives DER) */
static char *sign_es256(acme_t *a, const char *data)
{
    unsigned char hash[32], der[MBEDTLS_ECDSA_MAX_LEN], raw[64], *p = der;
    size_t dl, len;
    mbedtls_sha256((const unsigned char *)data, strlen(data), hash, 0);
    if (mbedtls_pk_sign(&a->key, MBEDTLS_MD_SHA256, hash, 32, der, sizeof der, &dl, rng, a))
        return NULL;
    mbedtls_mpi r, s;
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    char *out = NULL;
    const unsigned char *end = der + dl;
    if (!mbedtls_asn1_get_tag(&p, end, &len, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) &&
        !mbedtls_asn1_get_mpi(&p, end, &r) && !mbedtls_asn1_get_mpi(&p, end, &s) &&
        !mbedtls_mpi_write_binary(&r, raw, 32) && !mbedtls_mpi_write_binary(&s, raw + 32, 32))
        out = b64url(raw, 64);
    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);
    return out;
}

static int get_nonce(acme_t *a, const char *new_nonce_url)
{
    resp_t r;
    if (https(a, "HEAD", new_nonce_url, NULL, &r) < 0)
        return -1;
    int ok = r.nonce[0] != 0;
    resp_free(&r);
    return ok ? 0 : fail(a->j, "the CA sent no Replay-Nonce");
}

static const char *problem(const resp_t *r, char *buf, size_t cap)
{
    cJSON *j = cJSON_Parse(r->body ? r->body : "");
    snprintf(buf, cap, "HTTP %d: %s %s", r->status, msg_str(j, "type", ""), msg_str(j, "detail", r->body ? r->body : ""));
    cJSON_Delete(j);
    return buf;
}

/* POST a JWS; payload NULL = POST-as-GET (empty payload). use_jwk for newAccount. */
static int post(acme_t *a, const char *url, const char *payload, int use_jwk, resp_t *r)
{
    for (int attempt = 0; attempt < 4; attempt++) {
        char prot[2048];
        if (use_jwk)
            snprintf(prot, sizeof prot, "{\"alg\":\"ES256\",\"jwk\":%s,\"nonce\":\"%s\",\"url\":\"%s\"}", a->jwk, a->nonce, url);
        else
            snprintf(prot, sizeof prot, "{\"alg\":\"ES256\",\"kid\":\"%s\",\"nonce\":\"%s\",\"url\":\"%s\"}", a->kid, a->nonce,
                     url);
        char *p64 = b64url(prot, strlen(prot));
        char *b64 = payload ? b64url(payload, strlen(payload)) : rd_strdup("");
        size_t sl = strlen(p64) + strlen(b64) + 2;
        char *signing = malloc(sl);
        snprintf(signing, sl, "%s.%s", p64, b64);
        char *sig = sign_es256(a, signing);
        free(signing);
        if (!sig) {
            free(p64);
            free(b64);
            return fail(a->j, "signing failed");
        }
        size_t bl = strlen(p64) + strlen(b64) + strlen(sig) + 64;
        char *body = malloc(bl);
        snprintf(body, bl, "{\"protected\":\"%s\",\"payload\":\"%s\",\"signature\":\"%s\"}", p64, b64, sig);
        free(p64);
        free(b64);
        free(sig);
        a->nonce[0] = 0;
        int e = https(a, "POST", url, body, r);
        free(body);
        if (e < 0)
            return -1;
        /* badNonce: retry with the fresh nonce from this response (RFC 8555 §6.5) */
        if (r->status == 400 && r->body && strstr(r->body, "urn:ietf:params:acme:error:badNonce") && a->nonce[0]) {
            LOGD("acme: bad nonce, retrying");
            resp_free(r);
            continue;
        }
        return 0;
    }
    return fail(a->j, "the CA kept rejecting nonces");
}

/* POST-as-GET a JSON resource; with wait, until its status leaves pending/processing. */
static cJSON *fetch(acme_t *a, const char *url, const char *what, int wait_done)
{
    for (int i = 0; i < 40; i++) {
        resp_t r;
        if (post(a, url, NULL, 0, &r) < 0)
            return NULL;
        cJSON *j = r.status == 200 ? cJSON_Parse(r.body) : NULL;
        if (!j) {
            char b[400];
            fail(a->j, "%s: %s", what, problem(&r, b, sizeof b));
            resp_free(&r);
            return NULL;
        }
        const char *st = msg_str(j, "status", "");
        if (!wait_done || (strcmp(st, "pending") && strcmp(st, "processing"))) {
            resp_free(&r);
            return j;
        }
        int wait = r.retry_after > 0 ? RD_MIN(r.retry_after, 10) : 2;
        resp_free(&r);
        cJSON_Delete(j);
        rd_sleep_ms(wait * 1000);
    }
    fail(a->j, "%s: still pending after a long wait", what);
    return NULL;
}

/* ---------------- certificate request ---------------- */

static char *make_csr(acme_t *a, mbedtls_pk_context *k)
{
    job_t *j = a->j;
    mbedtls_x509write_csr csr;
    mbedtls_x509write_csr_init(&csr);
    mbedtls_x509write_csr_set_md_alg(&csr, MBEDTLS_MD_SHA256);
    mbedtls_x509write_csr_set_key(&csr, k);
    mbedtls_x509_san_list san[MAX_DOMAINS];
    memset(san, 0, sizeof san);
    for (int i = 0; i < j->ndomains; i++) {
        san[i].node.type = MBEDTLS_X509_SAN_DNS_NAME;
        san[i].node.san.unstructured_name.p = (unsigned char *)j->domains[i];
        san[i].node.san.unstructured_name.len = strlen(j->domains[i]);
        san[i].next = i + 1 < j->ndomains ? &san[i + 1] : NULL;
    }
    char subject[300];
    snprintf(subject, sizeof subject, "CN=%s", j->domains[0]);
    unsigned char der[4096];
    char *out = NULL;
    int n = -1;
    if (!mbedtls_x509write_csr_set_subject_name(&csr, subject) &&
        !mbedtls_x509write_csr_set_subject_alternative_name(&csr, san))
        n = mbedtls_x509write_csr_der(&csr, der, sizeof der, rng, a); /* written at the end of der */
    if (n > 0)
        out = b64url(der + sizeof der - n, (size_t)n);
    mbedtls_x509write_csr_free(&csr);
    return out;
}

/* ---------------- the RFC 8555 flow ---------------- */

static int authorize(acme_t *a, const char *authz_url, const char *thumb)
{
    cJSON *az = fetch(a, authz_url, "authorization", 0);
    if (!az)
        return -1;
    const char *domain = msg_str(cJSON_GetObjectItemCaseSensitive(az, "identifier"), "value", "?");
    if (!strcmp(msg_str(az, "status", ""), "valid")) { /* authorized recently; nothing to prove */
        cJSON_Delete(az);
        return 0;
    }
    cJSON *ch, *http01 = NULL;
    cJSON_ArrayForEach(ch, cJSON_GetObjectItemCaseSensitive(az, "challenges"))
        if (!strcmp(msg_str(ch, "type", ""), "http-01"))
            http01 = ch;
    if (!http01) {
        cJSON_Delete(az);
        return fail(a->j, "%s: the CA offered no http-01 challenge", domain);
    }
    char token[128], url[512], keyauth[300];
    snprintf(token, sizeof token, "%s", msg_str(http01, "token", ""));
    snprintf(url, sizeof url, "%s", msg_str(http01, "url", ""));
    snprintf(keyauth, sizeof keyauth, "%s.%s", token, thumb);
    chal_set(token, keyauth);
    LOGI("acme: proving control of %s (http://%s/.well-known/acme-challenge/%.12s…)", domain, domain, token);
    resp_t r;
    int e = post(a, url, "{}", 0, &r); /* "ready, go and check" */
    if (e == 0 && r.status != 200) {
        char b[400];
        fail(a->j, "%s: %s", domain, problem(&r, b, sizeof b));
        e = -1;
    }
    resp_free(&r);
    cJSON *done = e == 0 ? fetch(a, authz_url, "authorization", 1) : NULL;
    if (!done) {
        cJSON_Delete(az);
        return -1;
    }
    int ok = !strcmp(msg_str(done, "status", ""), "valid");
    if (!ok) {
        /* the challenge's error explains it, e.g. "connection refused" on port 80 */
        const char *detail = "";
        cJSON_ArrayForEach(ch, cJSON_GetObjectItemCaseSensitive(done, "challenges"))
            if (cJSON_GetObjectItemCaseSensitive(ch, "error"))
                detail = msg_str(cJSON_GetObjectItemCaseSensitive(ch, "error"), "detail", "");
        fail(a->j, "%s: validation failed (%s): %s — is port 80 of this server reachable at http://%s/?", domain,
             msg_str(done, "status", "?"), detail, domain);
    }
    cJSON_Delete(done);
    cJSON_Delete(az);
    return ok ? 0 : -1;
}

static int issue(acme_t *a)
{
    job_t *j = a->j;
    char path[600], b[400];
    int ret = -1;
    resp_t r;
    cJSON *dir = NULL, *order = NULL;
    mbedtls_pk_context ck;
    mbedtls_pk_init(&ck);
    char *csr = NULL;

    /* directory: the CA's endpoint URLs */
    if (https(a, "GET", j->directory, NULL, &r) < 0)
        return -1;
    dir = r.status == 200 ? cJSON_Parse(r.body) : NULL;
    if (!dir) {
        fail(j, "directory %s: %s", j->directory, problem(&r, b, sizeof b));
        resp_free(&r);
        return -1;
    }
    resp_free(&r);
    char new_nonce[512], new_account[512], new_order[512];
    snprintf(new_nonce, sizeof new_nonce, "%s", msg_str(dir, "newNonce", ""));
    snprintf(new_account, sizeof new_account, "%s", msg_str(dir, "newAccount", ""));
    snprintf(new_order, sizeof new_order, "%s", msg_str(dir, "newOrder", ""));
    if (!new_nonce[0] || !new_account[0] || !new_order[0]) {
        fail(j, "directory %s is not an ACME directory", j->directory);
        goto out;
    }

    /* account key, kept so the account (and its rate limits) stays the same */
    snprintf(path, sizeof path, "%s/account.key", j->dir);
    if (access(path, F_OK) == 0) {
        int k = mbedtls_pk_parse_keyfile(&a->key, path, NULL, rng, a);
        if (k) {
            fail(j, "cannot read %s: %s", path, mbed_err(k));
            goto out;
        }
    } else {
        if (new_key(a, &a->key) < 0 || write_key(a, path, &a->key) < 0) {
            fail(j, "cannot create %s: %s", path, strerror(errno));
            goto out;
        }
        LOGI("acme: created account key %s", path);
    }
    a->jwk = make_jwk(&a->key);
    if (!a->jwk) {
        fail(j, "account key is not EC P-256");
        goto out;
    }
    uint8_t tp[32];
    rd_sha256(a->jwk, strlen(a->jwk), tp);
    char *thumb = b64url(tp, 32);

    if (get_nonce(a, new_nonce) < 0)
        goto out_thumb;

    /* account: created, or found again for the same key */
    cJSON *acc = cJSON_CreateObject();
    cJSON_AddTrueToObject(acc, "termsOfServiceAgreed");
    if (j->email[0]) {
        char mail[300];
        snprintf(mail, sizeof mail, "mailto:%s", j->email);
        cJSON_AddItemToObject(acc, "contact", cJSON_CreateStringArray((const char *[]){ mail }, 1));
    }
    char *accs = cJSON_PrintUnformatted(acc);
    cJSON_Delete(acc);
    int e = post(a, new_account, accs, 1, &r);
    free(accs);
    if (e < 0)
        goto out_thumb;
    if ((r.status != 200 && r.status != 201) || !r.location[0]) {
        fail(j, "account: %s", problem(&r, b, sizeof b));
        resp_free(&r);
        goto out_thumb;
    }
    snprintf(a->kid, sizeof a->kid, "%s", r.location);
    resp_free(&r);

    /* order */
    cJSON *o = cJSON_CreateObject();
    cJSON *ids = cJSON_AddArrayToObject(o, "identifiers");
    for (int i = 0; i < j->ndomains; i++) {
        cJSON *id = cJSON_CreateObject();
        cJSON_AddStringToObject(id, "type", "dns");
        cJSON_AddStringToObject(id, "value", j->domains[i]);
        cJSON_AddItemToArray(ids, id);
    }
    if (j->profile[0]) /* draft-ietf-acme-profiles, e.g. Let's Encrypt "shortlived" */
        cJSON_AddStringToObject(o, "profile", j->profile);
    char *os = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    e = post(a, new_order, os, 0, &r);
    free(os);
    if (e < 0)
        goto out_thumb;
    order = r.status == 201 ? cJSON_Parse(r.body) : NULL;
    char order_url[512];
    snprintf(order_url, sizeof order_url, "%s", r.location);
    if (!order || !order_url[0]) {
        fail(j, "new order: %s", problem(&r, b, sizeof b));
        resp_free(&r);
        goto out_thumb;
    }
    resp_free(&r);

    /* authorizations: one http-01 challenge per domain */
    cJSON *au;
    cJSON_ArrayForEach(au, cJSON_GetObjectItemCaseSensitive(order, "authorizations"))
        if (!cJSON_IsString(au) || authorize(a, au->valuestring, thumb) < 0)
            goto out_thumb;

    /* finalize with a CSR for a fresh certificate key */
    csr = new_key(a, &ck) == 0 ? make_csr(a, &ck) : NULL;
    if (!csr) {
        fail(j, "cannot make a certificate request");
        goto out_thumb;
    }
    char *fin_body = malloc(strlen(csr) + 16);
    sprintf(fin_body, "{\"csr\":\"%s\"}", csr);
    e = post(a, msg_str(order, "finalize", ""), fin_body, 0, &r);
    free(fin_body);
    if (e < 0)
        goto out_thumb;
    if (r.status != 200) {
        fail(j, "finalize: %s", problem(&r, b, sizeof b));
        resp_free(&r);
        goto out_thumb;
    }
    resp_free(&r);
    cJSON_Delete(order);
    order = fetch(a, order_url, "order", 1);
    if (!order)
        goto out_thumb;
    if (strcmp(msg_str(order, "status", ""), "valid")) {
        fail(j, "order ended %s", msg_str(order, "status", "?"));
        goto out_thumb;
    }

    /* download the chain (leaf + intermediates) */
    if (post(a, msg_str(order, "certificate", ""), NULL, 0, &r) < 0)
        goto out_thumb;
    if (r.status != 200 || !strstr(r.body, "-----BEGIN CERTIFICATE-----")) {
        fail(j, "certificate download: %s", problem(&r, b, sizeof b));
        resp_free(&r);
        goto out_thumb;
    }
    char cp[600], kp[600];
    cert_path(j, cp, sizeof cp);
    key_path(j, kp, sizeof kp);
    if (write_key(a, kp, &ck) < 0 || write_file(cp, r.body, r.len, 0644) < 0) {
        fail(j, "cannot write %s: %s", cp, strerror(errno));
        resp_free(&r);
        goto out_thumb;
    }
    resp_free(&r);
    ret = 0;
out_thumb:
    free(thumb);
out:
    free(csr);
    mbedtls_pk_free(&ck);
    cJSON_Delete(order);
    cJSON_Delete(dir);
    return ret;
}

static void *worker(void *arg)
{
    job_t *j = arg;
    acme_t *a = calloc(1, sizeof *a);
    a->j = j;
    mbedtls_pk_init(&a->key);
    a->http = httpc_new(j->ca_file, "remodroid-server/" RD_VERSION);
    if (a->http)
        j->ok = issue(a) == 0;
    else
        fail(j, "cannot seed the random generator");
    mbedtls_pk_free(&a->key);
    httpc_free(a->http);
    free(a->jwk);
    free(a);
    chal_clear();
    ev_async_send(S.loop, &A.done); /* the loop reads j after this */
    return NULL;
}

/* ---------------- event loop side ---------------- */

/* 1 = the certificate on disk covers every domain and isn't due for renewal.
 * *renew_in: seconds until renewal is due (a third of the lifetime before expiry). */
static double x509_time(const mbedtls_x509_time *t)
{
    struct tm tm = { .tm_year = t->year - 1900, .tm_mon = t->mon - 1, .tm_mday = t->day,
                     .tm_hour = t->hour, .tm_min = t->min, .tm_sec = t->sec };
    return (double)timegm(&tm);
}

static int cert_good(const job_t *j, double *renew_in)
{
    char cp[600];
    cert_path(j, cp, sizeof cp);
    *renew_in = 0;
    mbedtls_x509_crt x; /* the first certificate in the file is ours */
    mbedtls_x509_crt_init(&x);
    if (access(cp, R_OK) || mbedtls_x509_crt_parse_file(&x, cp) < 0) {
        mbedtls_x509_crt_free(&x);
        return 0;
    }
    int ok = 1;
    for (int i = 0; i < j->ndomains && ok; i++) {
        int found = 0;
        for (const mbedtls_x509_sequence *s = &x.subject_alt_names; s && s->buf.p && !found; s = s->next)
            found = s->buf.tag == (MBEDTLS_ASN1_CONTEXT_SPECIFIC | MBEDTLS_X509_SAN_DNS_NAME) &&
                    s->buf.len == strlen(j->domains[i]) && !strncasecmp((const char *)s->buf.p, j->domains[i], s->buf.len);
        ok = found;
    }
    if (ok) {
        double from = x509_time(&x.valid_from), until = x509_time(&x.valid_to), now = (double)time(NULL);
        *renew_in = until - (until - from) / 3 - now;
        ok = *renew_in > 0;
    }
    mbedtls_x509_crt_free(&x);
    return ok;
}

static void load_current(void)
{
    char cp[600], kp[600];
    cert_path(&A.cfg, cp, sizeof cp);
    key_path(&A.cfg, kp, sizeof kp);
    if (access(cp, R_OK) == 0 && access(kp, R_OK) == 0)
        tls_load(cp, kp);
}

static void check(void)
{
    if (!A.enabled || A.running)
        return;
    double renew_in;
    if (cert_good(&A.cfg, &renew_in)) {
        double next = RD_MIN(renew_in + 1, CHECK_EVERY);
        LOGD("acme: certificate is current; next check in %.0f s", next);
        ev_timer_stop(S.loop, &A.timer);
        ev_timer_set(&A.timer, next, 0.);
        ev_timer_start(S.loop, &A.timer);
        return;
    }
    job_t *j = malloc(sizeof *j);
    *j = A.cfg;
    j->ok = 0;
    j->err[0] = 0;
    if (rd_thread_detached(worker, j) < 0) {
        LOGE("acme: cannot start the worker thread");
        free(j);
    } else {
        A.running = j;
        LOGI("acme: requesting a certificate for %s%s from %s", j->domains[0], j->ndomains > 1 ? " (+more)" : "",
             j->directory);
    }
}

static void on_done(struct ev_loop *loop, ev_async *w, int revents)
{
    job_t *j = A.running;
    if (!j)
        return;
    A.running = NULL;
    double next, renew_in;
    if (j->ok && !cert_good(&A.cfg, &renew_in)) { /* never loop on a certificate we can't use */
        j->ok = 0;
        snprintf(j->err, sizeof j->err, "the issued certificate doesn't match [acme] domains or is already due");
    }
    if (j->ok) {
        LOGI("acme: certificate issued for %s", j->domains[0]);
        load_current();
        A.backoff = 0;
        next = RD_MIN(renew_in + 1, CHECK_EVERY);
    } else {
        A.backoff = A.backoff ? RD_MIN(A.backoff * 2, RETRY_MAX) : RETRY_MIN;
        LOGE("acme: %s (retrying in %.0f min)", j->err[0] ? j->err : "failed", A.backoff / 60);
        next = A.backoff;
    }
    free(j);
    ev_timer_stop(loop, &A.timer);
    ev_timer_set(&A.timer, next, 0.);
    ev_timer_start(loop, &A.timer);
}

static void on_timer(struct ev_loop *loop, ev_timer *w, int revents) { check(); }

static int read_config(void)
{
    job_t *c = &A.cfg;
    memset(c, 0, sizeof *c);
    char list[MAX_DOMAINS * 260];
    snprintf(list, sizeof list, "%s", rd_conf_get(S.conf, "acme", "domains", ""));
    for (char *save = NULL, *d = strtok_r(list, ", ", &save); d && c->ndomains < MAX_DOMAINS;
         d = strtok_r(NULL, ", ", &save)) {
        if (strchr(d, '*') || strchr(d, '/') || strchr(d, ':')) {
            LOGE("[acme] domains: %s is not a plain host name (wildcards need DNS validation)", d);
            return -1;
        }
        snprintf(c->domains[c->ndomains++], sizeof c->domains[0], "%s", d);
    }
    snprintf(c->directory, sizeof c->directory, "%s", rd_conf_get(S.conf, "acme", "directory", LETSENCRYPT));
    snprintf(c->email, sizeof c->email, "%s", rd_conf_get(S.conf, "acme", "email", ""));
#ifdef _WIN32
    char def[300];
    snprintf(def, sizeof def, "%s\\acme", S.state_dir);
#else
    const char *def = "/var/lib/remodroid/acme";
#endif
    snprintf(c->dir, sizeof c->dir, "%s", rd_conf_get(S.conf, "acme", "dir", def));
    snprintf(c->ca_file, sizeof c->ca_file, "%s", rd_conf_get(S.conf, "acme", "ca_file", ""));
    snprintf(c->profile, sizeof c->profile, "%s", rd_conf_get(S.conf, "acme", "profile", ""));
    A.enabled = c->ndomains > 0;
    if (A.enabled && rd_mkdirs(c->dir, 0700) < 0) {
        LOGE("[acme] dir %s: %s", c->dir, strerror(errno));
        return -1;
    }
    return 0;
}

int acme_start(void)
{
    if (read_config() < 0)
        return -1;
    if (!A.enabled)
        return 0;
    ev_async_init(&A.done, on_done);
    ev_async_start(S.loop, &A.done);
    ev_timer_init(&A.timer, on_timer, 0., 0.);
    load_current(); /* serve the existing certificate (even if due) while renewing */
    check();
    return 0;
}

void acme_reload(void)
{
    int was = A.enabled;
    if (read_config() < 0 || !A.enabled)
        return;
    if (!was) {
        ev_async_init(&A.done, on_done);
        ev_async_start(S.loop, &A.done);
        ev_timer_init(&A.timer, on_timer, 0., 0.);
    }
    load_current();
    A.backoff = 0;
    check(); /* domains may have changed */
}
