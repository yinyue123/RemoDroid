#include "sctp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rd.h"
#include "usrsctp.h"

#define PPID_DCEP 50
#define PPID_STRING 51
#define PPID_BINARY 53
#define PPID_STRING_EMPTY 56
#define PPID_BINARY_EMPTY 57
#define DCEP_OPEN 0x03
#define DCEP_ACK 0x02
#define MAX_CH 16
#define SNDBUF (1024 * 1024)

typedef struct {
    int used, open;
    uint16_t sid;
    char label[64];
} chan_t;

struct sctp {
    struct ev_loop *loop;
    sctp_cb_t cb;
    void *ud;
    struct socket *sock;
    int dtls_client, connected, closed, valid;
    int remote_port;
    uint16_t next_sid;
    chan_t ch[MAX_CH];
};

static int inited;
static int nassoc;
static ev_timer timers;
static ev_tstamp last_tick;

static void debug_printf(const char *fmt, ...)
{
    if (rd_log_level < RD_LOG_TRACE)
        return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    LOGT("sctp: %s", buf);
}

static int conn_output(void *addr, void *buf, size_t len, uint8_t tos, uint8_t set_df)
{
    sctp_t *s = addr;
    if (s && s->valid && s->cb.send)
        s->cb.send(s->ud, buf, len);
    return 0;
}

static void on_timers(struct ev_loop *loop, ev_timer *w, int revents)
{
    ev_tstamp now = ev_now(loop);
    uint32_t ms = (uint32_t)((now - last_tick) * 1000);
    last_tick = now;
    usrsctp_handle_timers(ms ? ms : 1);
}

static void global_init(struct ev_loop *loop)
{
    if (inited)
        return;
    usrsctp_init_nothreads(0, conn_output, debug_printf);
    usrsctp_sysctl_set_sctp_ecn_enable(0);
    usrsctp_sysctl_set_sctp_blackhole(2);
    usrsctp_sysctl_set_sctp_delayed_sack_time_default(20);
    usrsctp_sysctl_set_sctp_heartbeat_interval_default(10000);
    ev_timer_init(&timers, on_timers, 0.01, 0.01);
    last_tick = ev_now(loop);
    inited = 1;
}

static chan_t *by_sid(sctp_t *s, uint16_t sid)
{
    for (int i = 0; i < MAX_CH; i++)
        if (s->ch[i].used && s->ch[i].sid == sid)
            return &s->ch[i];
    return NULL;
}

static chan_t *by_label(sctp_t *s, const char *label)
{
    for (int i = 0; i < MAX_CH; i++)
        if (s->ch[i].used && !strcmp(s->ch[i].label, label))
            return &s->ch[i];
    return NULL;
}

static chan_t *new_chan(sctp_t *s, uint16_t sid, const char *label)
{
    for (int i = 0; i < MAX_CH; i++) {
        if (!s->ch[i].used) {
            chan_t *c = &s->ch[i];
            memset(c, 0, sizeof *c);
            c->used = 1;
            c->sid = sid;
            snprintf(c->label, sizeof c->label, "%s", label);
            return c;
        }
    }
    return NULL;
}

static int raw_send(sctp_t *s, uint16_t sid, uint32_t ppid, const void *d, size_t n)
{
    struct sctp_sendv_spa spa;
    memset(&spa, 0, sizeof spa);
    spa.sendv_flags = SCTP_SEND_SNDINFO_VALID;
    spa.sendv_sndinfo.snd_sid = sid;
    spa.sendv_sndinfo.snd_ppid = htonl(ppid);
    spa.sendv_sndinfo.snd_flags = SCTP_EOR;
    ssize_t r = usrsctp_sendv(s->sock, d, n, NULL, 0, &spa, (socklen_t)sizeof spa, SCTP_SENDV_SPA, 0);
    return r < 0 ? -1 : 0;
}

static void handle_dcep(sctp_t *s, uint16_t sid, const uint8_t *d, size_t n)
{
    if (n >= 12 && d[0] == DCEP_OPEN) {
        uint16_t ll = rd_get_be16(d + 8);
        if (12 + (size_t)ll > n)
            return;
        char label[64];
        snprintf(label, sizeof label, "%.*s", (int)RD_MIN(ll, 63), (const char *)d + 12);
        chan_t *c = by_sid(s, sid);
        if (!c)
            c = new_chan(s, sid, label);
        if (!c)
            return;
        c->open = 1;
        uint8_t ack = DCEP_ACK;
        raw_send(s, sid, PPID_DCEP, &ack, 1);
        LOGI("sctp: peer opened channel '%s' (stream %u)", label, sid);
        if (s->cb.on_open)
            s->cb.on_open(s->ud, c->label);
    } else if (n >= 1 && d[0] == DCEP_ACK) {
        chan_t *c = by_sid(s, sid);
        if (c && !c->open) {
            c->open = 1;
            LOGI("sctp: channel '%s' open (stream %u)", c->label, sid);
            if (s->cb.on_open)
                s->cb.on_open(s->ud, c->label);
        }
    }
}

static void handle_notification(sctp_t *s, const union sctp_notification *n)
{
    if (n->sn_header.sn_type == SCTP_ASSOC_CHANGE) {
        const struct sctp_assoc_change *ac = &n->sn_assoc_change;
        if (ac->sac_state == SCTP_COMM_UP) {
            s->connected = 1;
            LOGI("sctp: association up");
            for (int i = 0; i < MAX_CH; i++) /* channels opened before the association came up */
                if (s->ch[i].used && !s->ch[i].open) {
                    size_t ll = strlen(s->ch[i].label);
                    uint8_t m[12 + 64];
                    memset(m, 0, 12);
                    m[0] = DCEP_OPEN;
                    m[1] = 0x00; /* reliable, ordered */
                    rd_put_be16(m + 8, (uint16_t)ll);
                    memcpy(m + 12, s->ch[i].label, ll);
                    raw_send(s, s->ch[i].sid, PPID_DCEP, m, 12 + ll);
                }
        } else if (ac->sac_state == SCTP_COMM_LOST || ac->sac_state == SCTP_SHUTDOWN_COMP ||
                   ac->sac_state == SCTP_CANT_STR_ASSOC) {
            if (!s->closed) {
                s->closed = 1;
                LOGI("sctp: association closed");
                if (s->cb.on_closed)
                    s->cb.on_closed(s->ud);
            }
        }
    }
}

static int receive_cb(struct socket *sock, union sctp_sockstore addr, void *data, size_t len, struct sctp_rcvinfo rcv,
                      int flags, void *ulp)
{
    sctp_t *s = ulp;
    if (!data)
        return 1;
    if (flags & MSG_NOTIFICATION) {
        handle_notification(s, data);
    } else {
        uint32_t ppid = ntohl(rcv.rcv_ppid);
        uint16_t sid = rcv.rcv_sid;
        if (ppid == PPID_DCEP) {
            handle_dcep(s, sid, data, len);
        } else {
            chan_t *c = by_sid(s, sid);
            if (c && s->cb.on_message) {
                int bin = ppid == PPID_BINARY || ppid == PPID_BINARY_EMPTY;
                int empty = ppid == PPID_STRING_EMPTY || ppid == PPID_BINARY_EMPTY;
                s->cb.on_message(s->ud, c->label, bin, data, empty ? 0 : len);
            }
        }
    }
    free(data);
    return 1;
}

static int send_cb(struct socket *sock, uint32_t sb_free, void *ulp)
{
    sctp_t *s = ulp;
    if (s->cb.on_writable)
        s->cb.on_writable(s->ud);
    return 1;
}

sctp_t *sctp_new(struct ev_loop *loop, int dtls_client, int local_port, int remote_port, sctp_cb_t cb, void *ud)
{
    global_init(loop);
    sctp_t *s = calloc(1, sizeof *s);
    s->loop = loop;
    s->cb = cb;
    s->ud = ud;
    s->dtls_client = dtls_client;
    s->remote_port = remote_port;
    s->next_sid = dtls_client ? 0 : 1;
    s->valid = 1;
    usrsctp_register_address(s);
    s->sock = usrsctp_socket(AF_CONN, SOCK_STREAM, IPPROTO_SCTP, receive_cb, send_cb, SNDBUF / 2, s);
    if (!s->sock) {
        usrsctp_deregister_address(s);
        free(s);
        return NULL;
    }
    if (nassoc++ == 0) {
        last_tick = ev_now(loop);
        ev_timer_start(loop, &timers);
    }
    usrsctp_set_non_blocking(s->sock, 1);
    struct linger lg = { 1, 0 };
    usrsctp_setsockopt(s->sock, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
    int one = 1, sb = SNDBUF;
    usrsctp_setsockopt(s->sock, IPPROTO_SCTP, SCTP_NODELAY, &one, sizeof one);
    usrsctp_setsockopt(s->sock, IPPROTO_SCTP, SCTP_RECVRCVINFO, &one, sizeof one);
    usrsctp_setsockopt(s->sock, SOL_SOCKET, SO_SNDBUF, &sb, sizeof sb);
    struct sctp_assoc_value av = { .assoc_id = SCTP_ALL_ASSOC, .assoc_value = SCTP_ENABLE_RESET_STREAM_REQ };
    usrsctp_setsockopt(s->sock, IPPROTO_SCTP, SCTP_ENABLE_STREAM_RESET, &av, sizeof av);
    struct sctp_initmsg im = { .sinit_num_ostreams = 256, .sinit_max_instreams = 256 };
    usrsctp_setsockopt(s->sock, IPPROTO_SCTP, SCTP_INITMSG, &im, sizeof im);
    uint16_t evs[] = { SCTP_ASSOC_CHANGE, SCTP_STREAM_RESET_EVENT, SCTP_SEND_FAILED_EVENT };
    for (size_t i = 0; i < RD_ARRAY_LEN(evs); i++) {
        struct sctp_event ev = { .se_assoc_id = SCTP_ALL_ASSOC, .se_type = evs[i], .se_on = 1 };
        usrsctp_setsockopt(s->sock, IPPROTO_SCTP, SCTP_EVENT, &ev, sizeof ev);
    }
    struct sockaddr_conn sc;
    memset(&sc, 0, sizeof sc);
    sc.sconn_family = AF_CONN;
    sc.sconn_port = htons((uint16_t)local_port);
    sc.sconn_addr = s;
    if (usrsctp_bind(s->sock, (struct sockaddr *)&sc, sizeof sc) < 0) {
        LOGE("sctp: bind failed: %s", strerror(errno));
        sctp_free(s);
        return NULL;
    }
    sc.sconn_port = htons((uint16_t)remote_port);
    if (usrsctp_connect(s->sock, (struct sockaddr *)&sc, sizeof sc) < 0 && errno != EINPROGRESS) {
        LOGE("sctp: connect failed: %s", strerror(errno));
        sctp_free(s);
        return NULL;
    }
    return s;
}

void sctp_input(sctp_t *s, const uint8_t *d, size_t n) { usrsctp_conninput(s, d, n, 0); }

int sctp_open_channel(sctp_t *s, const char *label)
{
    if (by_label(s, label))
        return 0;
    chan_t *c = new_chan(s, s->next_sid, label);
    if (!c)
        return -1;
    s->next_sid += 2;
    if (s->connected) {
        size_t ll = strlen(label);
        uint8_t m[12 + 64];
        memset(m, 0, 12);
        m[0] = DCEP_OPEN;
        rd_put_be16(m + 8, (uint16_t)ll);
        memcpy(m + 12, label, ll);
        return raw_send(s, c->sid, PPID_DCEP, m, 12 + ll);
    }
    return 0; /* sent once the association is up */
}

int sctp_send(sctp_t *s, const char *label, int binary, const void *d, size_t n)
{
    chan_t *c = by_label(s, label);
    if (!c || !s->connected || s->closed)
        return -1;
    if (!n) {
        uint8_t z = 0;
        return raw_send(s, c->sid, binary ? PPID_BINARY_EMPTY : PPID_STRING_EMPTY, &z, 1);
    }
    return raw_send(s, c->sid, binary ? PPID_BINARY : PPID_STRING, d, n);
}

size_t sctp_buffered(const sctp_t *s)
{
    uint32_t bytes = 0;
    struct sctp_status st;
    socklen_t l = sizeof st;
    memset(&st, 0, sizeof st);
    if (s->sock && usrsctp_getsockopt(s->sock, IPPROTO_SCTP, SCTP_STATUS, &st, &l) == 0)
        bytes = (uint32_t)st.sstat_penddata * 1200; /* chunks pending, approximate bytes */
    return bytes;
}

int sctp_is_open(const sctp_t *s, const char *label)
{
    chan_t *c = by_label((sctp_t *)s, label);
    return c && c->open;
}

void sctp_free(sctp_t *s)
{
    if (!s)
        return;
    s->valid = 0;
    if (s->sock) {
        usrsctp_close(s->sock);
        if (--nassoc == 0)
            ev_timer_stop(s->loop, &timers);
    }
    usrsctp_deregister_address(s);
    free(s);
}
