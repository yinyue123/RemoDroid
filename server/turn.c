/* Built-in TURN relay (RFC 8656, UDP only) so the AWS server needs no coturn.
 * Two kinds of long-term credentials:
 *  - the TURN REST format the control server hands out: username "<expiry>:<user>", password
 *    base64(HMAC-SHA1(shared_secret, username));
 *  - fixed logins, as coturn's user= lines: [turn] users = name:password, … (for another server's
 *    viewers and agents to use this relay, entered there as an ordinary TURN username and password).
 * Also answers plain STUN Binding requests (server-reflexive candidates). */
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "server.h"
#include "stun.h"

#define MAX_PERMS 16
#define MAX_CHANNELS 16
#define PERM_LIFETIME 300
#define CHANNEL_LIFETIME 600
#define NONCE_LIFETIME 3600

typedef struct alloc {
    struct sockaddr_storage client, relay;
    int fd;
    ev_io io;
    double expires;
    uint8_t key[16];
    char user[256];
    struct { struct sockaddr_storage ip; double expires; } perms[MAX_PERMS];
    int nperms;
    struct { uint16_t num; struct sockaddr_storage peer; double expires; } chans[MAX_CHANNELS];
    int nchans;
    uint64_t tx, rx;
    struct alloc *next;
} alloc_t;

static struct {
    int fd;
    ev_io io;
    ev_timer sweep;
    char realm[64], nonce[33];
    double nonce_at;
    int min_port, max_port, next_port, max_allocs, nallocs;
    struct sockaddr_storage public_ip;
    int have_public;
    alloc_t *allocs;
} T;

static double now(void) { return ev_now(S.loop); }

static void new_nonce(void)
{
    uint8_t r[16];
    rd_random(r, sizeof r);
    rd_hex(r, sizeof r, T.nonce);
    T.nonce_at = now();
}

static int same_ip(const struct sockaddr_storage *a, const struct sockaddr_storage *b)
{
    if (a->ss_family != b->ss_family)
        return 0;
    if (a->ss_family == AF_INET)
        return ((const struct sockaddr_in *)a)->sin_addr.s_addr == ((const struct sockaddr_in *)b)->sin_addr.s_addr;
    return !memcmp(&((const struct sockaddr_in6 *)a)->sin6_addr, &((const struct sockaddr_in6 *)b)->sin6_addr, 16);
}

static alloc_t *find_alloc(const struct sockaddr_storage *client)
{
    for (alloc_t *a = T.allocs; a; a = a->next)
        if (addr_equal(&a->client, client))
            return a;
    return NULL;
}

static void free_alloc(alloc_t *a)
{
    for (alloc_t **pp = &T.allocs; *pp; pp = &(*pp)->next) {
        if (*pp == a) {
            *pp = a->next;
            break;
        }
    }
    char c[64];
    addr_str(&a->client, c, sizeof c);
    LOGI("turn: allocation for %s (%s) closed, %llu/%llu bytes relayed", c, a->user, (unsigned long long)a->tx,
         (unsigned long long)a->rx);
    ev_io_stop(S.loop, &a->io);
    close(a->fd);
    free(a);
    T.nallocs--;
}

static void reply(const struct sockaddr_storage *to, stun_msg_t *m) { sendto(T.fd, m->buf, m->len, 0, (const struct sockaddr *)to, addr_len(to)); }

static void error(const struct sockaddr_storage *to, const stun_view_t *v, int code, const char *reason, int auth_hint)
{
    stun_msg_t m;
    stun_new(&m, (uint16_t)(stun_method(v->type) | STUN_ERROR), v->tid);
    uint8_t ec[4 + 64] = { 0, 0, (uint8_t)(code / 100), (uint8_t)(code % 100) };
    size_t rl = strlen(reason);
    memcpy(ec + 4, reason, RD_MIN(rl, 64));
    stun_add(&m, STUN_A_ERROR_CODE, ec, 4 + RD_MIN(rl, 64));
    if (auth_hint) {
        stun_add(&m, STUN_A_REALM, T.realm, strlen(T.realm));
        stun_add(&m, STUN_A_NONCE, T.nonce, strlen(T.nonce));
    }
    stun_add_fingerprint(&m);
    reply(to, &m);
}

/* A fixed login's password from [turn] users = name:password, … (split at the first ':'). */
static int fixed_pass(const char *user, char *pass, size_t cap)
{
    char *list = rd_strdup(rd_conf_get(S.conf, "turn", "users", "")), *save = NULL;
    int found = 0;
    for (char *e = strtok_r(list, ",", &save); e && !found; e = strtok_r(NULL, ",", &save)) {
        e += strspn(e, " \t");
        char *c = strchr(e, ':');
        if (!c || c == e)
            continue;
        *c++ = 0;
        c[strcspn(c, " \t")] = 0;
        if (*c && !strcmp(e, user)) {
            snprintf(pass, cap, "%s", c);
            found = 1;
        }
    }
    free(list);
    return found;
}

/* Long-term credentials: a fixed login, else a TURN REST username. Returns 1 when authenticated. */
static int authenticate(const struct sockaddr_storage *from, const stun_view_t *v, uint8_t key[16], char *user, size_t ucap)
{
    size_t ul, nl;
    const uint8_t *u = stun_attr(v, STUN_A_USERNAME, &ul), *n = stun_attr(v, STUN_A_NONCE, &nl);
    if (!u || !n || !stun_attr(v, STUN_A_MESSAGE_INTEGRITY, NULL)) {
        error(from, v, 401, "Unauthorized", 1);
        return 0;
    }
    if (nl != strlen(T.nonce) || memcmp(n, T.nonce, nl) || now() - T.nonce_at > NONCE_LIFETIME) {
        error(from, v, 438, "Stale Nonce", 1);
        return 0;
    }
    snprintf(user, ucap, "%.*s", (int)RD_MIN(ul, ucap - 1), (const char *)u);
    char pass[160];
    if (!fixed_pass(user, pass, sizeof pass)) {
        unsigned long long expiry = strtoull(user, NULL, 10);
        if (!S.turn_secret || !*S.turn_secret || expiry * 1000 < rd_now_ms()) {
            error(from, v, 401, "Unauthorized", 1);
            return 0;
        }
        uint8_t mac[20];
        rd_hmac_sha1(S.turn_secret, strlen(S.turn_secret), user, strlen(user), mac);
        rd_b64_encode(mac, sizeof mac, pass, sizeof pass);
    }
    stun_lt_key(user, T.realm, pass, key);
    if (!stun_check_integrity(v, key, 16)) {
        error(from, v, 401, "Unauthorized", 1);
        return 0;
    }
    return 1;
}

static int has_perm(alloc_t *a, const struct sockaddr_storage *peer)
{
    for (int i = 0; i < a->nperms; i++)
        if (same_ip(&a->perms[i].ip, peer) && a->perms[i].expires > now())
            return 1;
    return 0;
}

static void add_perm(alloc_t *a, const struct sockaddr_storage *peer)
{
    for (int i = 0; i < a->nperms; i++)
        if (same_ip(&a->perms[i].ip, peer)) {
            a->perms[i].expires = now() + PERM_LIFETIME;
            return;
        }
    if (a->nperms < MAX_PERMS) {
        a->perms[a->nperms].ip = *peer;
        a->perms[a->nperms++].expires = now() + PERM_LIFETIME;
    }
}

/* ---------------- relay sockets ---------------- */

static void on_relay(struct ev_loop *loop, ev_io *w, int revents)
{
    alloc_t *a = w->data;
    uint8_t buf[2048];
    for (int k = 0; k < 64; k++) {
        struct sockaddr_storage peer;
        socklen_t pl = sizeof peer;
        ssize_t n = recvfrom(a->fd, buf + 4, sizeof buf - 4, 0, (struct sockaddr *)&peer, &pl);
        if (n < 0)
            return;
        if (!has_perm(a, &peer))
            continue;
        a->rx += (uint64_t)n;
        int ch = -1;
        for (int i = 0; i < a->nchans; i++)
            if (addr_equal(&a->chans[i].peer, &peer) && a->chans[i].expires > now())
                ch = i;
        if (ch >= 0) { /* ChannelData: 4-byte header */
            rd_put_be16(buf, a->chans[ch].num);
            rd_put_be16(buf + 2, (uint16_t)n);
            sendto(T.fd, buf, (size_t)n + 4, 0, (struct sockaddr *)&a->client, addr_len(&a->client));
        } else if (n <= 1400) {
            stun_msg_t m;
            stun_new(&m, STUN_DATA | STUN_INDICATION, NULL);
            stun_add_xaddr(&m, STUN_A_XOR_PEER_ADDRESS, &peer);
            stun_add(&m, STUN_A_DATA, buf + 4, (size_t)n);
            reply(&a->client, &m);
        }
    }
}

static int open_relay(alloc_t *a)
{
    int span = T.max_port - T.min_port + 1;
    for (int tries = 0; tries < span; tries++) {
        int port = T.min_port + (T.next_port++ % span);
        int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        struct sockaddr_in sin = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
        int sz = 512 * 1024;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof sz);
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz);
        if (fd >= 0 && bind(fd, (struct sockaddr *)&sin, sizeof sin) == 0) {
            a->fd = fd;
            memset(&a->relay, 0, sizeof a->relay);
            if (T.have_public) {
                a->relay = T.public_ip;
            } else { /* the address the client reached us on */
                struct sockaddr_storage me;
                socklen_t ml = sizeof me;
                getsockname(T.fd, (struct sockaddr *)&me, &ml);
                a->relay = me;
            }
            ((struct sockaddr_in *)&a->relay)->sin_port = htons((uint16_t)port);
            ev_io_init(&a->io, on_relay, fd, EV_READ);
            a->io.data = a;
            ev_io_start(S.loop, &a->io);
            return 0;
        }
        if (fd >= 0)
            close(fd);
    }
    return -1;
}

/* ---------------- requests ---------------- */

static void success(const struct sockaddr_storage *to, const stun_view_t *v, stun_msg_t *m, const uint8_t key[16])
{
    stun_add_integrity(m, key, 16);
    stun_add_fingerprint(m);
    reply(to, m);
}

static void handle_request(const struct sockaddr_storage *from, const stun_view_t *v)
{
    uint16_t method = stun_method(v->type);
    stun_msg_t m;
    if (method == STUN_BINDING) {
        stun_new(&m, STUN_BINDING | STUN_SUCCESS, v->tid);
        stun_add_xaddr(&m, STUN_A_XOR_MAPPED_ADDRESS, from);
        stun_add_fingerprint(&m);
        reply(from, &m);
        return;
    }
    uint8_t key[16];
    char user[256];
    if (!authenticate(from, v, key, user, sizeof user))
        return;
    alloc_t *a = find_alloc(from);
    if (method == STUN_ALLOCATE) {
        if (a) {
            if (!memcmp(a->key, key, 16)) { /* retransmitted Allocate: same answer */
                stun_new(&m, STUN_ALLOCATE | STUN_SUCCESS, v->tid);
                stun_add_xaddr(&m, STUN_A_XOR_RELAYED_ADDRESS, &a->relay);
                stun_add_xaddr(&m, STUN_A_XOR_MAPPED_ADDRESS, from);
                stun_add_u32(&m, STUN_A_LIFETIME, (uint32_t)(a->expires - now()));
                success(from, v, &m, key);
            } else {
                error(from, v, 437, "Allocation Mismatch", 0);
            }
            return;
        }
        uint32_t transport = 0;
        if (stun_get_u32(v, STUN_A_REQUESTED_TRANSPORT, &transport) < 0 || (transport >> 24) != 17) {
            error(from, v, 442, "Unsupported Transport Protocol", 0);
            return;
        }
        if (T.nallocs >= T.max_allocs) {
            error(from, v, 486, "Allocation Quota Reached", 0);
            return;
        }
        a = calloc(1, sizeof *a);
        a->client = *from;
        memcpy(a->key, key, 16);
        snprintf(a->user, sizeof a->user, "%s", user);
        if (open_relay(a) < 0) {
            free(a);
            error(from, v, 508, "Insufficient Capacity", 0);
            return;
        }
        uint32_t life = 600;
        stun_get_u32(v, STUN_A_LIFETIME, &life);
        life = RD_MIN(RD_MAX(life, 60u), 3600u);
        a->expires = now() + life;
        a->next = T.allocs;
        T.allocs = a;
        T.nallocs++;
        char c[64], r[64];
        addr_str(from, c, sizeof c);
        addr_str(&a->relay, r, sizeof r);
        LOGI("turn: allocation %s -> relay %s for %s", c, r, user);
        stun_new(&m, STUN_ALLOCATE | STUN_SUCCESS, v->tid);
        stun_add_xaddr(&m, STUN_A_XOR_RELAYED_ADDRESS, &a->relay);
        stun_add_xaddr(&m, STUN_A_XOR_MAPPED_ADDRESS, from);
        stun_add_u32(&m, STUN_A_LIFETIME, life);
        success(from, v, &m, key);
        return;
    }
    if (!a) {
        error(from, v, 437, "Allocation Mismatch", 0);
        return;
    }
    if (method == STUN_REFRESH) {
        uint32_t life = 600;
        stun_get_u32(v, STUN_A_LIFETIME, &life);
        stun_new(&m, STUN_REFRESH | STUN_SUCCESS, v->tid);
        stun_add_u32(&m, STUN_A_LIFETIME, life ? RD_MIN(RD_MAX(life, 60u), 3600u) : 0);
        success(from, v, &m, key);
        if (!life)
            free_alloc(a);
        else
            a->expires = now() + RD_MIN(RD_MAX(life, 60u), 3600u);
        return;
    }
    if (method == STUN_CREATE_PERMISSION) {
        struct sockaddr_storage peer;
        if (stun_get_xaddr(v, STUN_A_XOR_PEER_ADDRESS, &peer) < 0) {
            error(from, v, 400, "Bad Request", 0);
            return;
        }
        add_perm(a, &peer);
        stun_new(&m, STUN_CREATE_PERMISSION | STUN_SUCCESS, v->tid);
        success(from, v, &m, key);
        return;
    }
    if (method == STUN_CHANNEL_BIND) {
        struct sockaddr_storage peer;
        uint32_t chv = 0;
        if (stun_get_xaddr(v, STUN_A_XOR_PEER_ADDRESS, &peer) < 0 || stun_get_u32(v, STUN_A_CHANNEL_NUMBER, &chv) < 0 ||
            (chv >> 16) < 0x4000 || (chv >> 16) > 0x4FFF) {
            error(from, v, 400, "Bad Request", 0);
            return;
        }
        uint16_t num = (uint16_t)(chv >> 16);
        int slot = -1;
        for (int i = 0; i < a->nchans; i++)
            if (a->chans[i].num == num || addr_equal(&a->chans[i].peer, &peer))
                slot = i;
        if (slot >= 0 && (a->chans[slot].num != num || !addr_equal(&a->chans[slot].peer, &peer))) {
            error(from, v, 400, "Bad Request", 0);
            return;
        }
        if (slot < 0) {
            if (a->nchans == MAX_CHANNELS) {
                error(from, v, 508, "Insufficient Capacity", 0);
                return;
            }
            slot = a->nchans++;
        }
        a->chans[slot].num = num;
        a->chans[slot].peer = peer;
        a->chans[slot].expires = now() + CHANNEL_LIFETIME;
        add_perm(a, &peer);
        stun_new(&m, STUN_CHANNEL_BIND | STUN_SUCCESS, v->tid);
        success(from, v, &m, key);
        return;
    }
    error(from, v, 400, "Bad Request", 0);
}

static void on_packet(const struct sockaddr_storage *from, const uint8_t *d, size_t n)
{
    if (n >= 4 && (d[0] & 0xC0) == 0x40) { /* ChannelData from the client */
        alloc_t *a = find_alloc(from);
        uint16_t num = rd_get_be16(d), len = rd_get_be16(d + 2);
        if (!a || (size_t)len + 4 > n)
            return;
        for (int i = 0; i < a->nchans; i++) {
            if (a->chans[i].num == num && a->chans[i].expires > now()) {
                sendto(a->fd, d + 4, len, 0, (struct sockaddr *)&a->chans[i].peer, addr_len(&a->chans[i].peer));
                a->tx += len;
                return;
            }
        }
        return;
    }
    stun_view_t v;
    if (stun_parse(d, n, &v) < 0)
        return;
    if (stun_class(v.type) == STUN_REQUEST) {
        handle_request(from, &v);
    } else if (v.type == (STUN_SEND | STUN_INDICATION)) {
        alloc_t *a = find_alloc(from);
        struct sockaddr_storage peer;
        size_t dl;
        const uint8_t *data = stun_attr(&v, STUN_A_DATA, &dl);
        if (a && data && stun_get_xaddr(&v, STUN_A_XOR_PEER_ADDRESS, &peer) == 0 && has_perm(a, &peer)) {
            sendto(a->fd, data, dl, 0, (struct sockaddr *)&peer, addr_len(&peer));
            a->tx += dl;
        }
    }
}

static void on_read(struct ev_loop *loop, ev_io *w, int revents)
{
    uint8_t buf[2048];
    for (int k = 0; k < 128; k++) {
        struct sockaddr_storage from;
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(T.fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (n < 0)
            return;
        on_packet(&from, buf, (size_t)n);
    }
}

static void on_sweep(struct ev_loop *loop, ev_timer *w, int revents)
{
    for (alloc_t *a = T.allocs, *n; a; a = n) {
        n = a->next;
        if (a->expires < now())
            free_alloc(a);
    }
    if (now() - T.nonce_at > NONCE_LIFETIME / 2)
        new_nonce(); /* old nonces then get 438 Stale Nonce and clients retry */
}

int turn_start(void)
{
    const char *listen = rd_conf_get(S.conf, "turn", "listen", "");
    if (!*listen)
        return 0;
    char host[128], port[16];
    if (rd_split_hostport(listen, host, sizeof host, port, sizeof port, "3478") < 0) {
        LOGE("turn: bad listen address %s", listen);
        return -1;
    }
    snprintf(T.realm, sizeof T.realm, "%s", rd_conf_get(S.conf, "turn", "realm", "remodroid"));
    T.min_port = (int)rd_conf_int(S.conf, "turn", "relay_min_port", 49152);
    T.max_port = (int)rd_conf_int(S.conf, "turn", "relay_max_port", 49252);
    T.max_allocs = (int)rd_conf_int(S.conf, "turn", "max_allocations", 32);
    const char *pub = rd_conf_get(S.conf, "turn", "public_ip", "");
    T.have_public = *pub && addr_parse(pub, 0, &T.public_ip) == 0;
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM, .ai_flags = AI_PASSIVE }, *ai;
    if (getaddrinfo(strcmp(host, "*") ? host : NULL, port, &hints, &ai)) {
        LOGE("turn: cannot resolve %s", listen);
        return -1;
    }
    T.fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int sz = 1024 * 1024;
    setsockopt(T.fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof sz);
    setsockopt(T.fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz);
    if (T.fd < 0 || bind(T.fd, ai->ai_addr, ai->ai_addrlen) < 0) {
        LOGE("turn: cannot listen on %s: %s", listen, strerror(errno));
        freeaddrinfo(ai);
        return -1;
    }
    freeaddrinfo(ai);
    new_nonce();
    ev_io_init(&T.io, on_read, T.fd, EV_READ);
    ev_io_start(S.loop, &T.io);
    ev_timer_init(&T.sweep, on_sweep, 5., 5.);
    ev_timer_start(S.loop, &T.sweep);
    LOGI("turn: relay on udp %s, ports %d-%d%s%s", listen, T.min_port, T.max_port, T.have_public ? ", public IP " : "",
         T.have_public ? pub : "");
    if ((!S.turn_secret || !*S.turn_secret) && !*rd_conf_get(S.conf, "turn", "users", ""))
        LOGW("turn: [turn] shared_secret and users are empty; allocations will be refused");
    return 0;
}
