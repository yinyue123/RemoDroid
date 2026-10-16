/* ADB protocol core (AOSP system/core/adb/protocol.txt): CNXN/AUTH handshake,
 * streams with OKAY flow control, shell (v1/v2) and sync (STAT/SEND/RECV). */
#include "adb.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <mbedtls/bignum.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>

#define A_SYNC 0x434e5953
#define A_CNXN 0x4e584e43
#define A_OPEN 0x4e45504f
#define A_OKAY 0x59414b4f
#define A_CLSE 0x45534c43
#define A_WRTE 0x45545257
#define A_AUTH 0x48545541
#define A_STLS 0x534c5453

#define A_VERSION 0x01000001
#define HOST_MAX_PAYLOAD (256 * 1024)
#define AUTH_TOKEN 1
#define AUTH_SIGNATURE 2
#define AUTH_RSAPUBLICKEY 3

struct adb_stream {
    adb_t *a;
    uint32_t local_id, remote_id;
    int open, dead, waiting_okay, user_closed;
    rd_buf_t outq;
    adb_stream_cb_t cb;
    void *ud;
    adb_stream_t *next;
};

struct adb {
    struct ev_loop *loop;
    adb_callbacks_t cb;
    void *ud;
    int state;
    adb_transport_t tr;
    int attached;
    char serial[64];
    char banner[1024];
    uint32_t max_payload;
    int sent_signature;
    mbedtls_pk_context key;
    int have_key;
    char pubkey[1024];
    adb_stream_t *streams;
    uint32_t next_id;
    int dispatching;
    uint64_t tx, rx;
};

static mbedtls_entropy_context entropy;
static mbedtls_ctr_drbg_context drbg;
static int drbg_ready;

static int rng_init(void)
{
    if (drbg_ready)
        return 0;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const unsigned char *)"remodroid-adb", 13))
        return -1;
    drbg_ready = 1;
    return 0;
}

const char *adb_state_name(int st)
{
    switch (st) {
    case ADB_CONNECTING: return "waiting";
    case ADB_UNAUTHORIZED: return "unauthorized";
    case ADB_ONLINE: return "authorized";
    default: return "offline";
    }
}

/* ---------------- RSA key ---------------- */

/* Android's RSAPublicKey struct (mincrypt), base64-encoded, followed by " user". */
static int build_pubkey(adb_t *a)
{
    mbedtls_rsa_context *rsa = mbedtls_pk_rsa(a->key);
    mbedtls_mpi N, E, R, RR;
    mbedtls_mpi_init(&N); mbedtls_mpi_init(&E); mbedtls_mpi_init(&R); mbedtls_mpi_init(&RR);
    int ret = -1;
    if (mbedtls_rsa_export(rsa, &N, NULL, NULL, NULL, &E) || mbedtls_mpi_bitlen(&N) != 2048)
        goto out;
    uint8_t blob[4 + 4 + 256 + 256 + 4];
    rd_put_le32(blob, 64);
    if (mbedtls_mpi_write_binary_le(&N, blob + 8, 256))
        goto out;
    uint32_t n0 = rd_get_le32(blob + 8), inv = n0;
    for (int i = 0; i < 5; i++)
        inv *= 2 - n0 * inv; /* Newton: inv = n0^-1 mod 2^32 */
    rd_put_le32(blob + 4, (uint32_t)(0u - inv));
    if (mbedtls_mpi_lset(&R, 1) || mbedtls_mpi_shift_l(&R, 4096) || mbedtls_mpi_mod_mpi(&RR, &R, &N) ||
        mbedtls_mpi_write_binary_le(&RR, blob + 264, 256))
        goto out;
    uint8_t eb[4];
    if (mbedtls_mpi_write_binary_le(&E, eb, sizeof eb))
        goto out;
    rd_put_le32(blob + 520, rd_get_le32(eb));
    size_t n = rd_b64_encode(blob, sizeof blob, a->pubkey, sizeof a->pubkey - 32);
    if (!n)
        goto out;
    snprintf(a->pubkey + n, sizeof a->pubkey - n, " remodroid@router");
    ret = 0;
out:
    mbedtls_mpi_free(&N); mbedtls_mpi_free(&E); mbedtls_mpi_free(&R); mbedtls_mpi_free(&RR);
    return ret;
}

static int load_or_create_key(adb_t *a, const char *path)
{
    if (rng_init() < 0)
        return -1;
    mbedtls_pk_init(&a->key);
    if (mbedtls_pk_parse_keyfile(&a->key, path, NULL, mbedtls_ctr_drbg_random, &drbg) == 0 &&
        mbedtls_pk_get_type(&a->key) == MBEDTLS_PK_RSA) {
        a->have_key = 1;
        return build_pubkey(a);
    }
    mbedtls_pk_free(&a->key);
    mbedtls_pk_init(&a->key);
    LOGI("adb: generating a new RSA-2048 key at %s (this can take a while on a router)", path);
    if (mbedtls_pk_setup(&a->key, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) ||
        mbedtls_rsa_gen_key(mbedtls_pk_rsa(a->key), mbedtls_ctr_drbg_random, &drbg, 2048, 65537)) {
        LOGE("adb: key generation failed");
        return -1;
    }
    unsigned char pem[4096];
    if (mbedtls_pk_write_key_pem(&a->key, pem, sizeof pem) == 0) {
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd < 0 || write(fd, pem, strlen((char *)pem)) < 0)
            LOGW("adb: cannot save the key to %s: %s (a new one is made next start)", path, strerror(errno));
        if (fd >= 0)
            close(fd);
    }
    a->have_key = 1;
    return build_pubkey(a);
}

/* ---------------- packets ---------------- */

static void send_packet(adb_t *a, uint32_t cmd, uint32_t arg0, uint32_t arg1, const void *data, size_t n)
{
    if (!a->attached)
        return;
    uint8_t h[24];
    rd_put_le32(h, cmd);
    rd_put_le32(h + 4, arg0);
    rd_put_le32(h + 8, arg1);
    rd_put_le32(h + 12, (uint32_t)n);
    rd_put_le32(h + 16, 0); /* data checksum: optional since A_VERSION 0x01000001 */
    rd_put_le32(h + 20, cmd ^ 0xffffffffu);
    a->tx += 24 + n;
    LOGT("adb -> %.4s %08x %08x len=%zu", (const char *)h, arg0, arg1, n);
    if (a->tr.send(a->tr.t, h, data, n) < 0)
        LOGW("adb: transport send failed");
}

static void set_state(adb_t *a, int st)
{
    if (a->state == st)
        return;
    a->state = st;
    LOGI("adb: %s", adb_state_name(st));
    if (a->cb.on_state)
        a->cb.on_state(a, st, a->ud);
}

adb_t *adb_new(struct ev_loop *loop, const char *keypath, adb_callbacks_t cb, void *ud)
{
    adb_t *a = calloc(1, sizeof *a);
    a->loop = loop;
    a->cb = cb;
    a->ud = ud;
    a->next_id = 1;
    if (load_or_create_key(a, keypath) < 0) {
        LOGE("adb: no usable RSA key; ADB authentication will fail");
    }
    return a;
}

void adb_attach(adb_t *a, adb_transport_t tr, const char *serial)
{
    a->tr = tr;
    a->attached = 1;
    a->sent_signature = 0;
    a->banner[0] = 0;
    a->max_payload = 4096;
    a->tx = a->rx = 0;
    snprintf(a->serial, sizeof a->serial, "%s", serial ? serial : "");
    set_state(a, ADB_CONNECTING);
    static const char banner[] = "host::features=shell_v2,cmd,stat_v2";
    send_packet(a, A_CNXN, A_VERSION, HOST_MAX_PAYLOAD, banner, sizeof banner);
}

static void stream_free(adb_stream_t *s)
{
    if (s->cb.on_free)
        s->cb.on_free(s->ud);
    rd_buf_free(&s->outq);
    free(s);
}

static void reap_streams(adb_t *a)
{
    for (adb_stream_t **pp = &a->streams; *pp;) {
        if ((*pp)->dead) {
            adb_stream_t *s = *pp;
            *pp = s->next;
            stream_free(s);
        } else {
            pp = &(*pp)->next;
        }
    }
}

static void drop_all_streams(adb_t *a)
{
    a->dispatching++;
    for (adb_stream_t *s = a->streams; s; s = s->next) {
        if (s->dead)
            continue;
        s->dead = 1;
        if (!s->user_closed) {
            if (!s->open && s->cb.on_open)
                s->cb.on_open(s, 0, s->ud);
            else if (s->cb.on_close)
                s->cb.on_close(s, s->ud);
        }
    }
    a->dispatching--;
    reap_streams(a);
}

void adb_on_transport_closed(adb_t *a)
{
    if (!a->attached)
        return;
    a->attached = 0;
    /* Go offline first so callbacks can't open streams on a dead transport. */
    int was = a->state;
    a->state = ADB_OFFLINE;
    drop_all_streams(a);
    if (was != ADB_OFFLINE) {
        LOGI("adb: %s", adb_state_name(ADB_OFFLINE));
        if (a->cb.on_state)
            a->cb.on_state(a, ADB_OFFLINE, a->ud);
    }
}

void adb_disconnect(adb_t *a)
{
    if (!a->attached)
        return;
    a->tr.close(a->tr.t); /* the transport calls adb_on_transport_closed */
}

static adb_stream_t *find_stream(adb_t *a, uint32_t local_id)
{
    for (adb_stream_t *s = a->streams; s; s = s->next)
        if (s->local_id == local_id && !s->dead)
            return s;
    return NULL;
}

static void pump(adb_stream_t *s)
{
    adb_t *a = s->a;
    if (!s->open || s->waiting_okay || s->dead || !rd_buf_avail(&s->outq) || !a->attached)
        return;
    size_t n = RD_MIN(rd_buf_avail(&s->outq), a->max_payload);
    send_packet(a, A_WRTE, s->local_id, s->remote_id, rd_buf_data(&s->outq), n);
    rd_buf_consume(&s->outq, n);
    s->waiting_okay = 1;
}

static void handle_auth(adb_t *a, uint32_t type, const uint8_t *data, size_t n)
{
    if (type != AUTH_TOKEN || !a->have_key)
        return;
    if (!a->sent_signature) {
        unsigned char sig[256];
        if (n != 20 ||
            mbedtls_rsa_pkcs1_sign(mbedtls_pk_rsa(a->key), mbedtls_ctr_drbg_random, &drbg, MBEDTLS_MD_SHA1, 20, data, sig)) {
            LOGW("adb: cannot sign the auth token (len %zu)", n);
            return;
        }
        a->sent_signature = 1;
        send_packet(a, A_AUTH, AUTH_SIGNATURE, 0, sig, sizeof sig);
    } else {
        /* Our key isn't trusted yet: offer it; the phone shows "Allow USB debugging?". */
        LOGI("adb: phone does not know this router's key; accept the prompt on the phone");
        send_packet(a, A_AUTH, AUTH_RSAPUBLICKEY, 0, a->pubkey, strlen(a->pubkey) + 1);
        set_state(a, ADB_UNAUTHORIZED);
    }
}

void adb_on_packet(adb_t *a, const uint8_t h[24], const uint8_t *data, size_t n)
{
    uint32_t cmd = rd_get_le32(h), arg0 = rd_get_le32(h + 4), arg1 = rd_get_le32(h + 8);
    if (rd_get_le32(h + 20) != (cmd ^ 0xffffffffu)) {
        LOGW("adb: bad packet magic, dropping the connection");
        adb_disconnect(a);
        return;
    }
    a->rx += 24 + n;
    LOGT("adb <- %.4s %08x %08x len=%zu", (const char *)h, arg0, arg1, n);
    a->dispatching++;
    switch (cmd) {
    case A_CNXN: {
        size_t bl = RD_MIN(n, sizeof a->banner - 1);
        memcpy(a->banner, data, bl);
        a->banner[bl] = 0;
        a->max_payload = RD_MIN(arg1 ? arg1 : 4096, HOST_MAX_PAYLOAD);
        LOGI("adb: connected (%s, max payload %u)", a->banner, a->max_payload);
        set_state(a, ADB_ONLINE);
        break;
    }
    case A_AUTH:
        handle_auth(a, arg0, data, n);
        break;
    case A_STLS:
        LOGW("adb: phone requested TLS (wireless debugging pairing), which is not supported; use USB");
        break;
    case A_OKAY: {
        adb_stream_t *s = find_stream(a, arg1);
        if (!s)
            break;
        if (!s->open) {
            s->open = 1;
            s->remote_id = arg0;
            if (s->cb.on_open && !s->user_closed)
                s->cb.on_open(s, 1, s->ud);
        } else {
            s->waiting_okay = 0;
        }
        pump(s);
        break;
    }
    case A_WRTE: {
        adb_stream_t *s = find_stream(a, arg1);
        if (!s) {
            send_packet(a, A_CLSE, 0, arg0, NULL, 0);
            break;
        }
        send_packet(a, A_OKAY, s->local_id, arg0, NULL, 0);
        if (s->cb.on_data && !s->user_closed)
            s->cb.on_data(s, data, n, s->ud);
        break;
    }
    case A_CLSE: {
        adb_stream_t *s = find_stream(a, arg1);
        if (!s)
            break;
        s->dead = 1;
        if (!s->user_closed) {
            if (!s->open && s->cb.on_open)
                s->cb.on_open(s, 0, s->ud);
            else if (s->cb.on_close)
                s->cb.on_close(s, s->ud);
        }
        break;
    }
    default:
        LOGD("adb: ignoring command %08x", cmd);
    }
    a->dispatching--;
    if (!a->dispatching)
        reap_streams(a);
}

/* ---------------- streams ---------------- */

adb_stream_t *adb_open(adb_t *a, const char *service, adb_stream_cb_t cb, void *ud)
{
    if (a->state != ADB_ONLINE)
        return NULL;
    adb_stream_t *s = calloc(1, sizeof *s);
    s->a = a;
    s->local_id = a->next_id++;
    if (!a->next_id)
        a->next_id = 1;
    s->cb = cb;
    s->ud = ud;
    s->next = a->streams;
    a->streams = s;
    LOGD("adb: open %s (#%u)", service, s->local_id);
    send_packet(a, A_OPEN, s->local_id, 0, service, strlen(service) + 1);
    return s;
}

int adb_write(adb_stream_t *s, const void *data, size_t n)
{
    if (!s || s->dead || s->user_closed)
        return -1;
    if (rd_buf_append(&s->outq, data, n) < 0)
        return -1;
    pump(s);
    return 0;
}

size_t adb_backlog(const adb_stream_t *s) { return s ? rd_buf_avail(&s->outq) : 0; }

void adb_close(adb_stream_t *s)
{
    if (!s || s->user_closed)
        return;
    adb_t *a = s->a;
    s->user_closed = 1;
    if (!s->dead) {
        if (s->open)
            send_packet(a, A_CLSE, s->local_id, s->remote_id, NULL, 0);
        s->dead = 1;
    }
    if (!a->dispatching)
        reap_streams(a);
}

int adb_state(const adb_t *a) { return a->state; }
const char *adb_serial(const adb_t *a) { return a->serial; }
uint32_t adb_max_payload(const adb_t *a) { return a->max_payload; }
const adb_transport_t *adb_transport(const adb_t *a) { return a->attached ? &a->tr : NULL; }

void adb_stats(const adb_t *a, uint64_t *tx, uint64_t *rx, int *streams)
{
    *tx = a->tx;
    *rx = a->rx;
    int n = 0;
    for (adb_stream_t *s = a->streams; s; s = s->next)
        n += !s->dead;
    *streams = n;
}

/* Banner: "device::ro.product.name=x;ro.product.model=y;features=a,b" */
const char *adb_banner_prop(const adb_t *a, const char *key, char *out, size_t cap)
{
    const char *p = strstr(a->banner, "::");
    if (!p || !cap)
        return NULL;
    p += 2;
    size_t kl = strlen(key);
    while (*p) {
        const char *e = strchr(p, ';');
        if (!e)
            e = p + strlen(p);
        if ((size_t)(e - p) > kl && !strncmp(p, key, kl) && p[kl] == '=') {
            size_t vl = RD_MIN((size_t)(e - p) - kl - 1, cap - 1);
            memcpy(out, p + kl + 1, vl);
            out[vl] = 0;
            return out;
        }
        p = *e ? e + 1 : e;
    }
    return NULL;
}

int adb_has_feature(const adb_t *a, const char *feature)
{
    char f[512];
    if (!adb_banner_prop(a, "features", f, sizeof f))
        return 0;
    size_t fl = strlen(feature);
    for (char *p = f; p && *p;) {
        char *c = strchr(p, ',');
        size_t l = c ? (size_t)(c - p) : strlen(p);
        if (l == fl && !strncmp(p, feature, fl))
            return 1;
        p = c ? c + 1 : NULL;
    }
    return 0;
}

/* ---------------- shell ---------------- */

typedef struct {
    int v2, code, done;
    rd_buf_t raw, out;
    adb_out_cb out_cb;
    adb_done_cb done_cb;
    void *ud;
} shell_t;

static void shell_emit(shell_t *sh, const uint8_t *d, size_t n)
{
    if (sh->out.len < 1024 * 1024)
        rd_buf_append(&sh->out, d, n);
    if (sh->out_cb)
        sh->out_cb(d, n, sh->ud);
}

static void shell_data(adb_stream_t *s, const uint8_t *d, size_t n, void *ud)
{
    shell_t *sh = ud;
    if (!sh->v2) {
        shell_emit(sh, d, n);
        return;
    }
    rd_buf_append(&sh->raw, d, n);
    while (rd_buf_avail(&sh->raw) >= 5) {
        const uint8_t *p = rd_buf_data(&sh->raw);
        uint32_t len = rd_get_le32(p + 1);
        if (rd_buf_avail(&sh->raw) < 5 + (size_t)len)
            break;
        if (p[0] == 1 || p[0] == 2)
            shell_emit(sh, p + 5, len);
        else if (p[0] == 3 && len >= 1)
            sh->code = p[5];
        rd_buf_consume(&sh->raw, 5 + len);
    }
}

static void shell_finish(shell_t *sh)
{
    if (sh->done)
        return;
    sh->done = 1;
    if (sh->done_cb)
        sh->done_cb(sh->code, (const char *)rd_buf_data(&sh->out), rd_buf_avail(&sh->out), sh->ud);
}

static void shell_free(void *ud)
{
    shell_t *sh = ud;
    rd_buf_free(&sh->raw);
    rd_buf_free(&sh->out);
    free(sh);
}

static void shell_open(adb_stream_t *s, int ok, void *ud)
{
    if (!ok)
        shell_finish(ud);
}

static void shell_close(adb_stream_t *s, void *ud) { shell_finish(ud); }

adb_stream_t *adb_shell(adb_t *a, const char *cmd, adb_out_cb out_cb, adb_done_cb done_cb, void *ud)
{
    shell_t *sh = calloc(1, sizeof *sh);
    sh->v2 = adb_has_feature(a, "shell_v2");
    sh->code = -1;
    sh->out_cb = out_cb;
    sh->done_cb = done_cb;
    sh->ud = ud;
    size_t n = strlen(cmd) + 32;
    char *svc = malloc(n);
    snprintf(svc, n, "%s%s", sh->v2 ? "shell,v2,raw:" : "shell:", cmd);
    adb_stream_t *s = adb_open(a, svc, (adb_stream_cb_t){ shell_open, shell_data, shell_close, shell_free }, sh);
    free(svc);
    if (!s) {
        free(sh);
        return NULL;
    }
    return s;
}

/* ---------------- sync: push / pull ---------------- */

enum { SY_STAT, SY_SEND, SY_RECV, SY_DONE };

typedef struct {
    adb_stream_t *s;
    int phase, fd;
    char local[512], remote[512];
    unsigned mode;
    struct stat st;
    rd_buf_t in;
    uint32_t recv_left; /* bytes left in the current DATA chunk (pull) */
    adb_file_cb cb;
    void *ud;
    int finished;
} sync_t;

static void sync_req(adb_stream_t *s, const char *id, const void *data, uint32_t n)
{
    uint8_t h[8];
    memcpy(h, id, 4);
    rd_put_le32(h + 4, n);
    adb_write(s, h, 8);
    if (n)
        adb_write(s, data, n);
}

static void sync_finish(sync_t *y, int code, const char *msg)
{
    if (y->finished)
        return;
    y->finished = 1;
    if (y->fd >= 0)
        close(y->fd);
    y->fd = -1;
    if (y->phase == SY_RECV && code < 0)
        unlink(y->local);
    if (y->s && code >= 0)
        sync_req(y->s, "QUIT", NULL, 0);
    if (y->cb)
        y->cb(code, msg, y->ud);
    adb_stream_t *s = y->s;
    y->s = NULL;
    adb_close(s); /* y is freed by sync_free when the stream goes away */
}

static void sync_free(void *ud)
{
    sync_t *y = ud;
    if (!y->finished && y->cb)
        y->cb(-1, "cancelled", y->ud);
    if (y->fd >= 0)
        close(y->fd);
    rd_buf_free(&y->in);
    free(y);
}

static void sync_send_file(sync_t *y)
{
    char spec[600];
    int sl = snprintf(spec, sizeof spec, "%s,%u", y->remote, y->mode);
    sync_req(y->s, "SEND", spec, (uint32_t)sl);
    uint8_t buf[64 * 1024];
    for (;;) {
        ssize_t r = read(y->fd, buf, sizeof buf);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0) {
            sync_finish(y, -1, strerror(errno));
            return;
        }
        if (r == 0)
            break;
        sync_req(y->s, "DATA", buf, (uint32_t)r);
    }
    uint8_t done[8];
    memcpy(done, "DONE", 4);
    rd_put_le32(done + 4, (uint32_t)y->st.st_mtime);
    adb_write(y->s, done, 8);
    y->phase = SY_SEND;
}

static void sync_data(adb_stream_t *s, const uint8_t *d, size_t n, void *ud)
{
    sync_t *y = ud;
    rd_buf_append(&y->in, d, n);
    while (!y->finished) {
        size_t av = rd_buf_avail(&y->in);
        const uint8_t *p = rd_buf_data(&y->in);
        if (y->phase == SY_RECV && y->recv_left) {
            size_t take = RD_MIN(av, y->recv_left);
            if (!take)
                return;
            if (write(y->fd, p, take) != (ssize_t)take) {
                sync_finish(y, -1, strerror(errno));
                return;
            }
            y->recv_left -= (uint32_t)take;
            rd_buf_consume(&y->in, take);
            continue;
        }
        if (av < 8)
            return;
        if (y->phase == SY_STAT) {
            if (av < 16)
                return;
            if (memcmp(p, "STAT", 4)) {
                sync_finish(y, -1, "unexpected STAT reply");
                return;
            }
            uint32_t mode = rd_get_le32(p + 4), size = rd_get_le32(p + 8), mtime = rd_get_le32(p + 12);
            rd_buf_consume(&y->in, 16);
            if (mode && size == (uint32_t)y->st.st_size && mtime == (uint32_t)y->st.st_mtime) {
                sync_finish(y, 1, "unchanged");
                return;
            }
            sync_send_file(y);
            continue;
        }
        uint32_t len = rd_get_le32(p + 4);
        if (!memcmp(p, "FAIL", 4)) {
            if (av < 8 + (size_t)len)
                return;
            char msg[256];
            snprintf(msg, sizeof msg, "%.*s", (int)RD_MIN(len, 200), (const char *)p + 8);
            sync_finish(y, -1, msg);
            return;
        }
        if (y->phase == SY_SEND && !memcmp(p, "OKAY", 4)) {
            sync_finish(y, 0, "pushed");
            return;
        }
        if (y->phase == SY_RECV && !memcmp(p, "DATA", 4)) {
            y->recv_left = len;
            rd_buf_consume(&y->in, 8);
            continue;
        }
        if (y->phase == SY_RECV && !memcmp(p, "DONE", 4)) {
            sync_finish(y, 0, "pulled");
            return;
        }
        sync_finish(y, -1, "unexpected sync reply");
        return;
    }
}

static void sync_open(adb_stream_t *s, int ok, void *ud)
{
    sync_t *y = ud;
    if (!ok) {
        y->s = NULL;
        sync_finish(y, -1, "sync service refused");
        return;
    }
    if (y->phase == SY_STAT)
        sync_req(s, "STAT", y->remote, (uint32_t)strlen(y->remote));
    else
        sync_req(s, "RECV", y->remote, (uint32_t)strlen(y->remote));
}

static void sync_closed(adb_stream_t *s, void *ud)
{
    sync_t *y = ud;
    y->s = NULL;
    sync_finish(y, -1, "connection closed");
}

static sync_t *sync_new(adb_t *a, int phase, const char *local, const char *remote, adb_file_cb cb, void *ud)
{
    sync_t *y = calloc(1, sizeof *y);
    y->phase = phase;
    y->fd = -1;
    snprintf(y->local, sizeof y->local, "%s", local);
    snprintf(y->remote, sizeof y->remote, "%s", remote);
    y->cb = cb;
    y->ud = ud;
    return y;
}

int adb_push(adb_t *a, const char *local, const char *remote, unsigned mode, adb_file_cb cb, void *ud)
{
    sync_t *y = sync_new(a, SY_STAT, local, remote, cb, ud);
    y->mode = mode;
    y->fd = open(local, O_RDONLY | O_CLOEXEC);
    if (y->fd < 0 || fstat(y->fd, &y->st) < 0) {
        LOGW("adb push: %s: %s", local, strerror(errno));
        if (y->fd >= 0)
            close(y->fd);
        free(y);
        return -1;
    }
    y->s = adb_open(a, "sync:", (adb_stream_cb_t){ sync_open, sync_data, sync_closed, sync_free }, y);
    if (!y->s) {
        close(y->fd);
        free(y);
        return -1;
    }
    return 0;
}

int adb_pull(adb_t *a, const char *remote, const char *local, adb_file_cb cb, void *ud)
{
    sync_t *y = sync_new(a, SY_RECV, local, remote, cb, ud);
    y->fd = open(local, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (y->fd < 0) {
        free(y);
        return -1;
    }
    y->s = adb_open(a, "sync:", (adb_stream_cb_t){ sync_open, sync_data, sync_closed, sync_free }, y);
    if (!y->s) {
        close(y->fd);
        unlink(local);
        free(y);
        return -1;
    }
    return 0;
}
