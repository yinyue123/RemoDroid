/* ICE agent + TURN client. One UDP socket (tunnelled or direct); pairs are
 * either "direct" (from that socket) or "relay" (through the TURN allocation).
 * A single 20 ms tick paces checks, retransmissions, refreshes and consent. */
#include "ice.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rd.h"
#include "stun.h"

#define MAX_LOCAL 16
#define MAX_REMOTE 32
#define MAX_PAIRS 64
#define MAX_PEERS 16
#define TICK 0.02

enum { C_HOST, C_SRFLX, C_PRFLX, C_RELAY };
static const char *CTYPE[] = { "host", "srflx", "prflx", "relay" };
static const int TYPE_PREF[] = { 126, 100, 110, 0 };

enum { P_WAITING, P_INPROGRESS, P_SUCCEEDED, P_FAILED };
enum { PATH_DIRECT, PATH_RELAY };
enum { T_NONE, T_ALLOC, T_ALLOC_AUTH, T_READY, T_FAILED };

typedef struct {
    int type;
    struct sockaddr_storage addr, raddr;
    uint32_t prio;
} lcand_t;

typedef struct {
    int type;
    struct sockaddr_storage addr;
    uint32_t prio;
} rcand_t;

typedef struct {
    int path, remote, state, tries, nominating, nominated;
    uint64_t prio;
    uint8_t tid[12];
    ev_tstamp next_tx, sent_at;
    int rtt_ms;
} pair_t;

typedef struct {
    struct sockaddr_storage addr;
    int perm, binding, bound;
    uint16_t channel;
    uint8_t perm_tid[12], bind_tid[12];
    ev_tstamp perm_at, bind_at, perm_tx, bind_tx;
    int perm_tries, bind_tries;
} peer_t;

typedef struct {
    int state, tries;
    struct sockaddr_storage server;
    char user[256], pass[256], realm[128], nonce[256];
    uint8_t key[16], tid[12], refresh_tid[12];
    ev_tstamp next_tx, refresh_at, refresh_tx;
    int refreshing;
    peer_t peers[MAX_PEERS];
    int npeers;
} turn_t;

struct ice {
    struct ev_loop *loop;
    net_t *net;
    ice_cb_t cb;
    void *ud;
    nsock_t *sock;
    char ufrag[9], pwd[25], rufrag[64], rpwd[128];
    uint64_t tiebreak;
    lcand_t local[MAX_LOCAL];
    int nlocal;
    rcand_t remote[MAX_REMOTE];
    int nremote;
    pair_t pairs[MAX_PAIRS];
    int npairs;
    int selected, state, force_relay;
    turn_t turn;
    struct sockaddr_storage stun;
    int have_stun, stun_state, stun_tries; /* stun_state: 0 none, 1 pending, 2 done */
    uint8_t stun_tid[12];
    ev_tstamp stun_tx, gather_deadline, nominate_at, consent_rx, consent_tx, checking_since;
    int gathering, gather_done;
    ev_timer tick;
    uint8_t consent_tid[12];
};

static void set_state(ice_t *ice, int st)
{
    if (ice->state == st)
        return;
    ice->state = st;
    static const char *names[] = { "new", "checking", "connected", "disconnected", "failed", "closed" };
    LOGI("ice: %s", names[st]);
    if (ice->cb.on_state)
        ice->cb.on_state(ice->ud, st);
}

static ev_tstamp now(ice_t *ice) { return ev_now(ice->loop); }

static void rand_str(char *out, size_t n)
{
    static const char a[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+/";
    uint8_t r[64];
    rd_random(r, n);
    for (size_t i = 0; i < n; i++)
        out[i] = a[r[i] & 63];
    out[n] = 0;
}

/* ---------------- sending ---------------- */

static int send_direct(ice_t *ice, const struct sockaddr_storage *to, const void *d, size_t n)
{
    return ice->sock ? net_udp_send(ice->sock, to, d, n) : -1;
}

static peer_t *turn_peer(ice_t *ice, const struct sockaddr_storage *a, int create)
{
    turn_t *t = &ice->turn;
    for (int i = 0; i < t->npeers; i++)
        if (addr_equal(&t->peers[i].addr, a))
            return &t->peers[i];
    if (!create || t->npeers == MAX_PEERS)
        return NULL;
    peer_t *p = &t->peers[t->npeers];
    memset(p, 0, sizeof *p);
    p->addr = *a;
    p->channel = (uint16_t)(0x4000 + t->npeers);
    t->npeers++;
    return p;
}

static void turn_auth(ice_t *ice, stun_msg_t *m)
{
    turn_t *t = &ice->turn;
    stun_add(m, STUN_A_USERNAME, t->user, strlen(t->user));
    stun_add(m, STUN_A_REALM, t->realm, strlen(t->realm));
    stun_add(m, STUN_A_NONCE, t->nonce, strlen(t->nonce));
    stun_add_integrity(m, t->key, 16);
}

static int send_relay(ice_t *ice, const struct sockaddr_storage *to, const void *d, size_t n)
{
    turn_t *t = &ice->turn;
    if (t->state != T_READY)
        return -1;
    peer_t *p = turn_peer(ice, to, 0);
    if (p && p->bound && addr_equal(&p->addr, to)) {
        uint8_t buf[1600];
        if (n + 4 > sizeof buf)
            return -1;
        rd_put_be16(buf, p->channel);
        rd_put_be16(buf + 2, (uint16_t)n);
        memcpy(buf + 4, d, n);
        return send_direct(ice, &t->server, buf, n + 4);
    }
    stun_msg_t m;
    stun_new(&m, STUN_SEND | STUN_INDICATION, NULL);
    stun_add_xaddr(&m, STUN_A_XOR_PEER_ADDRESS, to);
    stun_add(&m, STUN_A_DATA, d, n);
    return send_direct(ice, &t->server, m.buf, m.len);
}

static int send_path(ice_t *ice, int path, const struct sockaddr_storage *to, const void *d, size_t n)
{
    return path == PATH_RELAY ? send_relay(ice, to, d, n) : send_direct(ice, to, d, n);
}

/* ---------------- candidates ---------------- */

static uint32_t cand_prio(int type, int local_pref) { return (uint32_t)TYPE_PREF[type] << 24 | (uint32_t)local_pref << 8 | 255; }

static uint32_t foundation(const lcand_t *c)
{
    char s[80];
    addr_str(&c->addr, s, sizeof s);
    return stun_crc32((const uint8_t *)s, strlen(s)) ^ (uint32_t)c->type;
}

static void trickle(ice_t *ice, const lcand_t *c)
{
    char ip[INET6_ADDRSTRLEN], line[256], raddr[64] = "";
    if (c->addr.ss_family == AF_INET)
        inet_ntop(AF_INET, &((const struct sockaddr_in *)&c->addr)->sin_addr, ip, sizeof ip);
    else
        inet_ntop(AF_INET6, &((const struct sockaddr_in6 *)&c->addr)->sin6_addr, ip, sizeof ip);
    if (c->type != C_HOST) {
        char rip[INET6_ADDRSTRLEN] = "0.0.0.0";
        if (c->raddr.ss_family == AF_INET)
            inet_ntop(AF_INET, &((const struct sockaddr_in *)&c->raddr)->sin_addr, rip, sizeof rip);
        snprintf(raddr, sizeof raddr, " raddr %s rport %d", rip, c->raddr.ss_family ? addr_port(&c->raddr) : 0);
    }
    snprintf(line, sizeof line, "candidate:%u 1 udp %u %s %d typ %s%s", foundation(c), c->prio, ip, addr_port(&c->addr),
             CTYPE[c->type], raddr);
    LOGI("ice: local %s", line);
    if (ice->cb.on_candidate)
        ice->cb.on_candidate(ice->ud, line);
}

static void add_pairs_for_remote(ice_t *ice, int ri);

static int path_local_prio(ice_t *ice, int path, uint32_t *prio)
{
    uint32_t best = 0;
    int found = 0;
    for (int i = 0; i < ice->nlocal; i++) {
        const lcand_t *c = &ice->local[i];
        int p = c->type == C_RELAY ? PATH_RELAY : PATH_DIRECT;
        if (p == path && c->prio >= best) {
            best = c->prio;
            found = 1;
        }
    }
    *prio = best;
    return found;
}

static void add_local(ice_t *ice, int type, const struct sockaddr_storage *a, const struct sockaddr_storage *raddr)
{
    if (ice->force_relay && type != C_RELAY)
        return;
    for (int i = 0; i < ice->nlocal; i++)
        if (addr_equal(&ice->local[i].addr, a))
            return;
    if (ice->nlocal == MAX_LOCAL)
        return;
    lcand_t *c = &ice->local[ice->nlocal];
    memset(c, 0, sizeof *c);
    c->type = type;
    c->addr = *a;
    if (raddr)
        c->raddr = *raddr;
    c->prio = cand_prio(type, 65535 - ice->nlocal);
    ice->nlocal++;
    trickle(ice, c);
    for (int r = 0; r < ice->nremote; r++)
        add_pairs_for_remote(ice, r);
}

static void add_pair(ice_t *ice, int path, int ri)
{
    for (int i = 0; i < ice->npairs; i++)
        if (ice->pairs[i].path == path && ice->pairs[i].remote == ri)
            return;
    uint32_t lp;
    if (!path_local_prio(ice, path, &lp) || ice->npairs == MAX_PAIRS)
        return;
    if (ice->remote[ri].addr.ss_family != AF_INET)
        return; /* our socket is IPv4 */
    pair_t *p = &ice->pairs[ice->npairs++];
    memset(p, 0, sizeof *p);
    p->path = path;
    p->remote = ri;
    uint64_t g = lp, d = ice->remote[ri].prio;
    p->prio = ((uint64_t)RD_MIN(g, d) << 32) + 2 * RD_MAX(g, d) + (g > d ? 1 : 0);
    p->next_tx = now(ice);
    if (path == PATH_RELAY)
        turn_peer(ice, &ice->remote[ri].addr, 1);
    char s[64];
    addr_str(&ice->remote[ri].addr, s, sizeof s);
    LOGD("ice: pair %s -> %s (%s)", path == PATH_RELAY ? "relay" : "direct", s, CTYPE[ice->remote[ri].type]);
    if (ice->state == ICE_NEW && ice->rufrag[0]) {
        ice->checking_since = now(ice);
        set_state(ice, ICE_CHECKING);
    }
}

static void add_pairs_for_remote(ice_t *ice, int ri)
{
    if (!ice->force_relay)
        add_pair(ice, PATH_DIRECT, ri);
    if (ice->turn.state == T_READY)
        add_pair(ice, PATH_RELAY, ri);
}

static int find_remote(ice_t *ice, const struct sockaddr_storage *a)
{
    for (int i = 0; i < ice->nremote; i++)
        if (addr_equal(&ice->remote[i].addr, a))
            return i;
    return -1;
}

static int add_remote(ice_t *ice, int type, const struct sockaddr_storage *a, uint32_t prio)
{
    int i = find_remote(ice, a);
    if (i >= 0 || ice->nremote == MAX_REMOTE)
        return i;
    rcand_t *r = &ice->remote[ice->nremote];
    r->type = type;
    r->addr = *a;
    r->prio = prio;
    char s[64];
    addr_str(a, s, sizeof s);
    LOGI("ice: remote %s %s", CTYPE[type], s);
    add_pairs_for_remote(ice, ice->nremote);
    return ice->nremote++;
}

void ice_add_remote(ice_t *ice, const char *cand)
{
    if (!strncmp(cand, "a=", 2))
        cand += 2;
    if (strncmp(cand, "candidate:", 10))
        return;
    char fnd[64], proto[16], ip[256], typ[16], ctype[16];
    unsigned comp, prio;
    int port;
    if (sscanf(cand + 10, "%63s %u %15s %u %255s %d %15s %15s", fnd, &comp, proto, &prio, ip, &port, typ, ctype) != 8 ||
        strcmp(typ, "typ") || comp != 1 || !rd_streq_nocase(proto, "udp"))
        return;
    struct sockaddr_storage a;
    if (addr_parse(ip, port, &a) < 0) {
        LOGD("ice: skipping candidate with host name %s (mDNS)", ip);
        return;
    }
    int t = !strcmp(ctype, "host") ? C_HOST : !strcmp(ctype, "srflx") ? C_SRFLX : !strcmp(ctype, "relay") ? C_RELAY : C_PRFLX;
    add_remote(ice, t, &a, prio);
}

/* ---------------- servers / gathering ---------------- */

static int parse_url(const char *url, int *is_turn, struct sockaddr_storage *out)
{
    const char *p;
    if (!strncmp(url, "stun:", 5)) *is_turn = 0, p = url + 5;
    else if (!strncmp(url, "turn:", 5)) *is_turn = 1, p = url + 5;
    else return -1; /* turns: / stuns: (TLS) not supported */
    if (strstr(p, "transport=tcp"))
        return -1;
    char host[256];
    size_t hl = strcspn(p, ":?");
    if (hl >= sizeof host)
        return -1;
    memcpy(host, p, hl);
    host[hl] = 0;
    int port = p[hl] == ':' ? atoi(p + hl + 1) : 3478;
    if (addr_parse(host, port, out) == 0)
        return out->ss_family == AF_INET ? 0 : -1;
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM }, *ai;
    if (getaddrinfo(host, NULL, &hints, &ai))
        return -1;
    memcpy(out, ai->ai_addr, ai->ai_addrlen);
    ((struct sockaddr_in *)out)->sin_port = htons((uint16_t)port);
    freeaddrinfo(ai);
    return 0;
}

void ice_set_servers(ice_t *ice, const cJSON *servers, int force_relay)
{
    ice->force_relay = force_relay;
    const cJSON *srv;
    cJSON_ArrayForEach(srv, servers) {
        const cJSON *urls = cJSON_GetObjectItemCaseSensitive(srv, "urls"), *u;
        const cJSON *user = cJSON_GetObjectItemCaseSensitive(srv, "username");
        const cJSON *cred = cJSON_GetObjectItemCaseSensitive(srv, "credential");
        cJSON *one = NULL;
        if (cJSON_IsString(urls)) { /* "urls" may be a single string */
            one = cJSON_CreateArray();
            cJSON_AddItemToArray(one, cJSON_CreateString(urls->valuestring));
            urls = one;
        }
        cJSON_ArrayForEach(u, urls) {
            int is_turn;
            struct sockaddr_storage a;
            if (!cJSON_IsString(u) || parse_url(u->valuestring, &is_turn, &a) < 0)
                continue;
            if (is_turn && ice->turn.state == T_NONE && cJSON_IsString(user) && cJSON_IsString(cred)) {
                ice->turn.server = a;
                snprintf(ice->turn.user, sizeof ice->turn.user, "%s", user->valuestring);
                snprintf(ice->turn.pass, sizeof ice->turn.pass, "%s", cred->valuestring);
                ice->turn.state = T_ALLOC;
            }
            if (!ice->have_stun) { /* a TURN server also answers STUN Binding */
                ice->stun = a;
                ice->have_stun = 1;
            }
        }
        cJSON_Delete(one);
    }
}

static void maybe_gather_done(ice_t *ice)
{
    if (ice->gather_done)
        return;
    int stun_busy = ice->have_stun && ice->stun_state == 1 && !ice->force_relay;
    int turn_busy = ice->turn.state == T_ALLOC || ice->turn.state == T_ALLOC_AUTH;
    if ((!stun_busy && !turn_busy) || now(ice) > ice->gather_deadline) {
        ice->gather_done = 1;
        LOGI("ice: gathering done (%d local candidates)", ice->nlocal);
        if (ice->cb.on_candidate)
            ice->cb.on_candidate(ice->ud, NULL);
    }
}

static void send_stun_binding(ice_t *ice)
{
    stun_msg_t m;
    stun_new(&m, STUN_BINDING, NULL);
    memcpy(ice->stun_tid, stun_tid(&m), 12);
    stun_add_fingerprint(&m);
    send_direct(ice, &ice->stun, m.buf, m.len);
    ice->stun_tx = now(ice);
}

static void send_allocate(ice_t *ice)
{
    turn_t *t = &ice->turn;
    stun_msg_t m;
    stun_new(&m, STUN_ALLOCATE, NULL);
    memcpy(t->tid, stun_tid(&m), 12);
    stun_add_u32(&m, STUN_A_REQUESTED_TRANSPORT, 17u << 24);
    stun_add_u32(&m, STUN_A_LIFETIME, 600);
    if (t->state == T_ALLOC_AUTH)
        turn_auth(ice, &m);
    send_direct(ice, &t->server, m.buf, m.len);
    t->next_tx = now(ice) + 0.5;
}

void ice_gather(ice_t *ice)
{
    if (ice->gathering)
        return;
    ice->gathering = 1;
    ice->gather_deadline = now(ice) + 4.0;
    ice->sock = net_udp_open(ice->net, AF_INET);
    if (!ice->sock) {
        LOGE("ice: cannot open a UDP socket");
        set_state(ice, ICE_FAILED);
        return;
    }
    net_udp_report(ice->sock);
    if (ice->have_stun && !ice->force_relay) {
        ice->stun_state = 1;
        send_stun_binding(ice);
    }
    if (ice->turn.state == T_ALLOC)
        send_allocate(ice);
}

void ice_on_bound(ice_t *ice, nsock_t *s, const struct sockaddr_storage *local)
{
    if (s != ice->sock || local->ss_family != AF_INET)
        return;
    add_local(ice, C_HOST, local, NULL);
}

/* ---------------- connectivity checks ---------------- */

static void send_check(ice_t *ice, pair_t *p, int nominate)
{
    stun_msg_t m;
    stun_new(&m, STUN_BINDING, NULL);
    memcpy(p->tid, stun_tid(&m), 12);
    char user[200];
    int ul = snprintf(user, sizeof user, "%s:%s", ice->rufrag, ice->ufrag);
    stun_add(&m, STUN_A_USERNAME, user, (size_t)ul);
    stun_add_u32(&m, STUN_A_PRIORITY, cand_prio(C_PRFLX, 65535));
    stun_add_u64(&m, STUN_A_ICE_CONTROLLING, ice->tiebreak);
    if (nominate)
        stun_add(&m, STUN_A_USE_CANDIDATE, NULL, 0);
    stun_add_integrity(&m, ice->rpwd, strlen(ice->rpwd));
    stun_add_fingerprint(&m);
    send_path(ice, p->path, &ice->remote[p->remote].addr, m.buf, m.len);
    p->sent_at = now(ice);
    p->next_tx = now(ice) + RD_MIN(0.1 * (1 << RD_MIN(p->tries, 4)), 1.6);
    p->tries++;
}

static int pair_ready(ice_t *ice, pair_t *p)
{
    if (p->path != PATH_RELAY)
        return 1;
    peer_t *peer = turn_peer(ice, &ice->remote[p->remote].addr, 0);
    return peer && peer->perm;
}

static void nominate(ice_t *ice)
{
    int best = -1;
    for (int i = 0; i < ice->npairs; i++)
        if (ice->pairs[i].state == P_SUCCEEDED && (best < 0 || ice->pairs[i].prio > ice->pairs[best].prio))
            best = i;
    if (best < 0)
        return;
    pair_t *p = &ice->pairs[best];
    p->nominating = 1;
    p->tries = 0;
    char s[64];
    addr_str(&ice->remote[p->remote].addr, s, sizeof s);
    LOGI("ice: nominating %s pair to %s (rtt %d ms)", p->path == PATH_RELAY ? "relay" : "direct", s, p->rtt_ms);
    send_check(ice, p, 1);
}

static void select_pair(ice_t *ice, int i)
{
    ice->selected = i;
    ice->pairs[i].nominated = 1;
    ice->consent_rx = now(ice);
    ice->consent_tx = now(ice) + 4.0;
    pair_t *p = &ice->pairs[i];
    if (p->path == PATH_RELAY) { /* switch to ChannelData framing for lower overhead */
        peer_t *peer = turn_peer(ice, &ice->remote[p->remote].addr, 0);
        if (peer && !peer->bound && !peer->binding) {
            peer->binding = 1;
            peer->bind_tries = 0;
            peer->bind_tx = now(ice);
        }
    }
    set_state(ice, ICE_CONNECTED);
}

static void handle_check_response(ice_t *ice, const stun_view_t *v, int path, const struct sockaddr_storage *from)
{
    for (int i = 0; i < ice->npairs; i++) {
        pair_t *p = &ice->pairs[i];
        if (memcmp(p->tid, v->tid, 12))
            continue;
        if (p->path != path || !addr_equal(&ice->remote[p->remote].addr, from))
            return; /* non-symmetric response */
        if (!stun_check_integrity(v, ice->rpwd, strlen(ice->rpwd)))
            return;
        if (stun_class(v->type) == STUN_ERROR) {
            if (stun_error_code(v) == 487) /* role conflict: we stay controlling with a new tiebreaker */
                rd_random(&ice->tiebreak, sizeof ice->tiebreak);
            p->state = P_WAITING;
            return;
        }
        p->rtt_ms = (int)((now(ice) - p->sent_at) * 1000);
        if (i == ice->selected) {
            ice->consent_rx = now(ice);
            if (ice->state == ICE_DISCONNECTED)
                set_state(ice, ICE_CONNECTED);
            return;
        }
        if (p->state != P_SUCCEEDED) {
            p->state = P_SUCCEEDED;
            if (!ice->nominate_at && ice->selected < 0)
                ice->nominate_at = now(ice) + 0.12; /* wait briefly for a better pair */
        }
        if (p->nominating && ice->selected < 0)
            select_pair(ice, i);
        return;
    }
    if (!memcmp(ice->consent_tid, v->tid, 12))
        ice->consent_rx = now(ice);
}

static void handle_check_request(ice_t *ice, const stun_view_t *v, int path, const struct sockaddr_storage *from)
{
    size_t ul;
    const uint8_t *user = stun_attr(v, STUN_A_USERNAME, &ul);
    size_t ufl = strlen(ice->ufrag);
    if (!user || ul <= ufl || memcmp(user, ice->ufrag, ufl) || user[ufl] != ':' ||
        !stun_check_integrity(v, ice->pwd, strlen(ice->pwd)) || !stun_check_fingerprint(v))
        return;
    stun_msg_t m;
    if (stun_attr(v, STUN_A_ICE_CONTROLLING, NULL)) { /* both controlling: we keep the role */
        stun_new(&m, STUN_BINDING | STUN_ERROR, v->tid);
        uint8_t ec[4] = { 0, 0, 4, 87 };
        stun_add(&m, STUN_A_ERROR_CODE, ec, 4);
    } else {
        stun_new(&m, STUN_BINDING | STUN_SUCCESS, v->tid);
        stun_add_xaddr(&m, STUN_A_XOR_MAPPED_ADDRESS, from);
    }
    stun_add_integrity(&m, ice->pwd, strlen(ice->pwd));
    stun_add_fingerprint(&m);
    send_path(ice, path, from, m.buf, m.len);
    if (stun_attr(v, STUN_A_ICE_CONTROLLING, NULL))
        return;
    int ri = find_remote(ice, from);
    if (ri < 0) {
        uint32_t prio = cand_prio(C_PRFLX, 65535);
        stun_get_u32(v, STUN_A_PRIORITY, &prio);
        ri = add_remote(ice, C_PRFLX, from, prio);
        if (ri < 0)
            return;
    }
    for (int i = 0; i < ice->npairs; i++) { /* triggered check */
        pair_t *p = &ice->pairs[i];
        if (p->remote == ri && p->path == path && p->state != P_SUCCEEDED && p->state != P_INPROGRESS) {
            p->state = P_WAITING;
            p->next_tx = now(ice);
        }
    }
}

static void deliver(ice_t *ice, int path, const struct sockaddr_storage *from, const uint8_t *d, size_t n)
{
    if (stun_is(d, n)) {
        stun_view_t v;
        if (stun_parse(d, n, &v) < 0 || stun_method(v.type) != STUN_BINDING)
            return;
        if (stun_class(v.type) == STUN_REQUEST)
            handle_check_request(ice, &v, path, from);
        else if (stun_class(v.type) == STUN_SUCCESS || stun_class(v.type) == STUN_ERROR)
            handle_check_response(ice, &v, path, from);
        return;
    }
    /* Media and DTLS: accept from any validated pair (the peer may switch). */
    for (int i = 0; i < ice->npairs; i++) {
        pair_t *p = &ice->pairs[i];
        if (p->path == path && (p->state == P_SUCCEEDED || i == ice->selected) && addr_equal(&ice->remote[p->remote].addr, from)) {
            if (ice->cb.on_data)
                ice->cb.on_data(ice->ud, d, n);
            return;
        }
    }
}

/* ---------------- TURN responses ---------------- */

static void turn_response(ice_t *ice, const stun_view_t *v)
{
    turn_t *t = &ice->turn;
    uint16_t method = stun_method(v->type);
    int ok = stun_class(v->type) == STUN_SUCCESS, err = stun_error_code(v);
    if (method == STUN_ALLOCATE && !memcmp(v->tid, t->tid, 12)) {
        if (ok) {
            struct sockaddr_storage relay, mapped;
            uint32_t life = 600;
            stun_get_u32(v, STUN_A_LIFETIME, &life);
            if (stun_get_xaddr(v, STUN_A_XOR_RELAYED_ADDRESS, &relay) < 0) {
                t->state = T_FAILED;
                return;
            }
            t->state = T_READY;
            t->refresh_at = now(ice) + RD_MAX(life, 120u) - 60;
            int have_mapped = stun_get_xaddr(v, STUN_A_XOR_MAPPED_ADDRESS, &mapped) == 0;
            if (have_mapped && ice->stun_state != 2)
                add_local(ice, C_SRFLX, &mapped, NULL);
            add_local(ice, C_RELAY, &relay, have_mapped ? &mapped : NULL);
            for (int r = 0; r < ice->nremote; r++)
                add_pairs_for_remote(ice, r);
            maybe_gather_done(ice);
        } else if ((err == 401 || err == 438) && t->tries < 4) {
            size_t rl, nl;
            const uint8_t *realm = stun_attr(v, STUN_A_REALM, &rl), *nonce = stun_attr(v, STUN_A_NONCE, &nl);
            if (!nonce) {
                t->state = T_FAILED;
                return;
            }
            if (realm)
                snprintf(t->realm, sizeof t->realm, "%.*s", (int)rl, realm);
            snprintf(t->nonce, sizeof t->nonce, "%.*s", (int)nl, nonce);
            stun_lt_key(t->user, t->realm, t->pass, t->key);
            t->state = T_ALLOC_AUTH;
            t->tries++;
            send_allocate(ice);
        } else {
            LOGW("ice: TURN allocation failed (error %d)", err);
            t->state = T_FAILED;
            maybe_gather_done(ice);
        }
        return;
    }
    if (method == STUN_REFRESH && !memcmp(v->tid, t->refresh_tid, 12)) {
        t->refreshing = 0;
        if (ok) {
            uint32_t life = 600;
            stun_get_u32(v, STUN_A_LIFETIME, &life);
            t->refresh_at = now(ice) + RD_MAX(life, 120u) - 60;
        } else if (err == 438) {
            size_t nl;
            const uint8_t *nonce = stun_attr(v, STUN_A_NONCE, &nl);
            if (nonce)
                snprintf(t->nonce, sizeof t->nonce, "%.*s", (int)nl, nonce);
            t->refresh_at = now(ice);
        }
        return;
    }
    for (int i = 0; i < t->npeers; i++) {
        peer_t *p = &t->peers[i];
        int is_perm = method == STUN_CREATE_PERMISSION && !memcmp(v->tid, p->perm_tid, 12);
        int is_bind = method == STUN_CHANNEL_BIND && !memcmp(v->tid, p->bind_tid, 12);
        if (!is_perm && !is_bind)
            continue;
        if (err == 438) {
            size_t nl;
            const uint8_t *nonce = stun_attr(v, STUN_A_NONCE, &nl);
            if (nonce)
                snprintf(t->nonce, sizeof t->nonce, "%.*s", (int)nl, nonce);
            if (is_perm) p->perm_tx = now(ice); else p->bind_tx = now(ice);
            return;
        }
        if (is_perm) {
            p->perm = ok;
            p->perm_at = now(ice);
            if (!ok)
                LOGW("ice: TURN permission refused (error %d)", err);
        } else {
            p->binding = 0;
            p->bound = ok;
            p->bind_at = now(ice);
        }
        return;
    }
}

static void turn_input(ice_t *ice, const uint8_t *d, size_t n)
{
    turn_t *t = &ice->turn;
    if (n >= 4 && (d[0] & 0xC0) == 0x40) { /* ChannelData */
        uint16_t ch = rd_get_be16(d), len = rd_get_be16(d + 2);
        if ((size_t)len + 4 > n)
            return;
        for (int i = 0; i < t->npeers; i++)
            if (t->peers[i].channel == ch && t->peers[i].bound)
                deliver(ice, PATH_RELAY, &t->peers[i].addr, d + 4, len);
        return;
    }
    stun_view_t v;
    if (stun_parse(d, n, &v) < 0)
        return;
    if (v.type == (STUN_DATA | STUN_INDICATION)) {
        struct sockaddr_storage peer;
        size_t dl;
        const uint8_t *data = stun_attr(&v, STUN_A_DATA, &dl);
        if (data && stun_get_xaddr(&v, STUN_A_XOR_PEER_ADDRESS, &peer) == 0)
            deliver(ice, PATH_RELAY, &peer, data, dl);
        return;
    }
    if (stun_method(v.type) == STUN_BINDING && !memcmp(v.tid, ice->stun_tid, 12)) {
        struct sockaddr_storage mapped;
        if (stun_class(v.type) == STUN_SUCCESS && stun_get_xaddr(&v, STUN_A_XOR_MAPPED_ADDRESS, &mapped) == 0) {
            ice->stun_state = 2;
            int dup = 0;
            for (int i = 0; i < ice->nlocal; i++)
                dup |= addr_equal(&ice->local[i].addr, &mapped);
            if (!dup)
                add_local(ice, C_SRFLX, &mapped, NULL);
            maybe_gather_done(ice);
        }
        return;
    }
    turn_response(ice, &v);
}

void ice_on_udp(ice_t *ice, nsock_t *s, const struct sockaddr_storage *from, const uint8_t *d, size_t n)
{
    if (s != ice->sock)
        return;
    if ((ice->turn.state != T_NONE && addr_equal(from, &ice->turn.server)) || (ice->have_stun && addr_equal(from, &ice->stun))) {
        turn_input(ice, d, n);
        return;
    }
    deliver(ice, PATH_DIRECT, from, d, n);
}

/* ---------------- tick ---------------- */

static void turn_tick(ice_t *ice)
{
    turn_t *t = &ice->turn;
    ev_tstamp tn = now(ice);
    if ((t->state == T_ALLOC || t->state == T_ALLOC_AUTH) && tn >= t->next_tx) {
        if (++t->tries > 6) {
            LOGW("ice: no answer from the TURN server");
            t->state = T_FAILED;
            maybe_gather_done(ice);
        } else {
            send_allocate(ice);
        }
    }
    if (t->state != T_READY)
        return;
    if (tn >= t->refresh_at && (!t->refreshing || tn - t->refresh_tx > 1.0)) {
        stun_msg_t m;
        stun_new(&m, STUN_REFRESH, NULL);
        memcpy(t->refresh_tid, stun_tid(&m), 12);
        stun_add_u32(&m, STUN_A_LIFETIME, 600);
        turn_auth(ice, &m);
        send_direct(ice, &t->server, m.buf, m.len);
        t->refreshing = 1;
        t->refresh_tx = tn;
    }
    for (int i = 0; i < t->npeers; i++) {
        peer_t *p = &t->peers[i];
        int need_perm = (!p->perm && p->perm_tries < 6 && tn >= p->perm_tx) || (p->perm && tn - p->perm_at > 240);
        if (need_perm) {
            stun_msg_t m;
            stun_new(&m, STUN_CREATE_PERMISSION, NULL);
            memcpy(p->perm_tid, stun_tid(&m), 12);
            stun_add_xaddr(&m, STUN_A_XOR_PEER_ADDRESS, &p->addr);
            turn_auth(ice, &m);
            send_direct(ice, &t->server, m.buf, m.len);
            p->perm_tx = tn + 0.5;
            if (!p->perm)
                p->perm_tries++;
            else
                p->perm_at = tn;
        }
        int need_bind = p->perm && ((p->binding && tn >= p->bind_tx && p->bind_tries < 6) || (p->bound && tn - p->bind_at > 540));
        if (need_bind) {
            stun_msg_t m;
            stun_new(&m, STUN_CHANNEL_BIND, NULL);
            memcpy(p->bind_tid, stun_tid(&m), 12);
            stun_add_u32(&m, STUN_A_CHANNEL_NUMBER, (uint32_t)p->channel << 16);
            stun_add_xaddr(&m, STUN_A_XOR_PEER_ADDRESS, &p->addr);
            turn_auth(ice, &m);
            send_direct(ice, &t->server, m.buf, m.len);
            p->bind_tx = tn + 0.5;
            p->bind_tries++;
            if (p->bound)
                p->bind_at = tn;
        }
    }
}

static void on_tick(struct ev_loop *loop, ev_timer *w, int revents)
{
    ice_t *ice = w->data;
    ev_tstamp tn = now(ice);
    if (ice->gathering && ice->stun_state == 1 && tn - ice->stun_tx > 0.5) {
        if (++ice->stun_tries > 6)
            ice->stun_state = 3; /* gave up */
        else
            send_stun_binding(ice);
    }
    turn_tick(ice);
    maybe_gather_done(ice);
    if (!ice->rufrag[0])
        return;

    if (ice->selected < 0) {
        /* one check per tick, highest priority first */
        pair_t *next = NULL;
        for (int i = 0; i < ice->npairs; i++) {
            pair_t *p = &ice->pairs[i];
            if ((p->state == P_WAITING || p->state == P_INPROGRESS || (p->nominating && !p->nominated)) &&
                tn >= p->next_tx && pair_ready(ice, p) && (!next || p->prio > next->prio))
                next = p;
        }
        if (next) {
            if (next->tries >= 8 && !next->nominating) {
                next->state = P_FAILED;
            } else if (next->nominating && next->tries >= 8) {
                next->nominating = 0;
                next->state = P_FAILED;
                ice->nominate_at = tn;
            } else {
                if (next->state == P_WAITING)
                    next->state = P_INPROGRESS;
                send_check(ice, next, next->nominating);
            }
        }
        if (ice->nominate_at && tn >= ice->nominate_at) {
            ice->nominate_at = 0;
            nominate(ice);
        }
        int alive = 0;
        for (int i = 0; i < ice->npairs; i++)
            alive |= ice->pairs[i].state != P_FAILED;
        if (ice->state == ICE_CHECKING && ice->gather_done && (!alive || tn - ice->checking_since > 30.)) {
            LOGW("ice: no working candidate pair");
            set_state(ice, ICE_FAILED);
        }
        return;
    }

    /* connected: consent freshness (RFC 7675) */
    pair_t *sel = &ice->pairs[ice->selected];
    if (tn >= ice->consent_tx) {
        stun_msg_t m;
        stun_new(&m, STUN_BINDING, NULL);
        memcpy(ice->consent_tid, stun_tid(&m), 12);
        memcpy(sel->tid, stun_tid(&m), 12);
        char user[200];
        int ul = snprintf(user, sizeof user, "%s:%s", ice->rufrag, ice->ufrag);
        stun_add(&m, STUN_A_USERNAME, user, (size_t)ul);
        stun_add_u32(&m, STUN_A_PRIORITY, cand_prio(C_PRFLX, 65535));
        stun_add_u64(&m, STUN_A_ICE_CONTROLLING, ice->tiebreak);
        stun_add_integrity(&m, ice->rpwd, strlen(ice->rpwd));
        stun_add_fingerprint(&m);
        sel->sent_at = tn;
        send_path(ice, sel->path, &ice->remote[sel->remote].addr, m.buf, m.len);
        uint8_t r;
        rd_random(&r, 1);
        ice->consent_tx = tn + 4.0 + r / 128.0;
    }
    if (tn - ice->consent_rx > 30.)
        set_state(ice, ICE_FAILED);
    else if (tn - ice->consent_rx > 10. && ice->state == ICE_CONNECTED)
        set_state(ice, ICE_DISCONNECTED);
}

/* ---------------- API ---------------- */

ice_t *ice_new(struct ev_loop *loop, net_t *net, ice_cb_t cb, void *ud)
{
    ice_t *ice = calloc(1, sizeof *ice);
    ice->loop = loop;
    ice->net = net;
    ice->cb = cb;
    ice->ud = ud;
    ice->selected = -1;
    rand_str(ice->ufrag, 8);
    rand_str(ice->pwd, 24);
    rd_random(&ice->tiebreak, sizeof ice->tiebreak);
    ev_timer_init(&ice->tick, on_tick, TICK, TICK);
    ice->tick.data = ice;
    ev_timer_start(loop, &ice->tick);
    return ice;
}

void ice_free(ice_t *ice)
{
    if (!ice)
        return;
    ev_timer_stop(ice->loop, &ice->tick);
    if (ice->turn.state == T_READY) { /* release the allocation */
        stun_msg_t m;
        stun_new(&m, STUN_REFRESH, NULL);
        stun_add_u32(&m, STUN_A_LIFETIME, 0);
        turn_auth(ice, &m);
        send_direct(ice, &ice->turn.server, m.buf, m.len);
    }
    net_udp_close(ice->sock);
    free(ice);
}

const char *ice_ufrag(const ice_t *ice) { return ice->ufrag; }
const char *ice_pwd(const ice_t *ice) { return ice->pwd; }
int ice_state(const ice_t *ice) { return ice->state; }

void ice_set_remote(ice_t *ice, const char *ufrag, const char *pwd)
{
    snprintf(ice->rufrag, sizeof ice->rufrag, "%s", ufrag);
    snprintf(ice->rpwd, sizeof ice->rpwd, "%s", pwd);
    if (ice->npairs && ice->state == ICE_NEW) {
        ice->checking_since = now(ice);
        set_state(ice, ICE_CHECKING);
    }
}

int ice_send(ice_t *ice, const void *d, size_t n)
{
    if (ice->selected < 0)
        return -1;
    pair_t *p = &ice->pairs[ice->selected];
    return send_path(ice, p->path, &ice->remote[p->remote].addr, d, n);
}

void ice_selected(const ice_t *ice, const char **lt, const char **rt, int *rtt)
{
    if (ice->selected < 0) {
        *lt = *rt = "none";
        *rtt = 0;
        return;
    }
    const pair_t *p = &ice->pairs[ice->selected];
    *lt = p->path == PATH_RELAY ? "relay" : "host";
    *rt = CTYPE[ice->remote[p->remote].type];
    *rtt = p->rtt_ms;
}
