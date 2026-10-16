#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/un.h>
#include <unistd.h>

#include "rd.h"
#include "tunnel.h"

#define MAX_BACKLOG (4 * 1024 * 1024)

struct nsock {
    net_t *n;
    int id;          /* tunnel socket id (1..255) */
    int fd;          /* direct mode: real socket */
    int af, tunnel;
    ev_io io;
    nsock_t *next;
};

struct net {
    struct ev_loop *loop;
    net_cb_t cb;
    void *ud;
    int direct;
    int lfd;          /* listening socket */
    ev_io lio;
    int cfd;          /* the agent's connection */
    ev_io rio, wio;
    rd_buf_t in, out;
    nsock_t *socks;
    uint8_t used[256];
};

/* ---------------- tunnel connection ---------------- */

static void conn_close(net_t *n, const char *why)
{
    if (n->cfd < 0)
        return;
    LOGI("tunnel: agent disconnected (%s)", why);
    ev_io_stop(n->loop, &n->rio);
    ev_io_stop(n->loop, &n->wio);
    close(n->cfd);
    n->cfd = -1;
    rd_buf_reset(&n->in);
    rd_buf_reset(&n->out);
    if (n->cb.on_link)
        n->cb.on_link(n->ud, 0);
}

static void flush(net_t *n)
{
    while (rd_buf_avail(&n->out)) {
        ssize_t w = write(n->cfd, rd_buf_data(&n->out), rd_buf_avail(&n->out));
        if (w < 0) {
            if (errno == EAGAIN || errno == EINTR)
                break;
            conn_close(n, strerror(errno));
            return;
        }
        rd_buf_consume(&n->out, (size_t)w);
    }
    if (rd_buf_avail(&n->out))
        ev_io_start(n->loop, &n->wio);
    else
        ev_io_stop(n->loop, &n->wio);
}

static int send_frame(net_t *n, uint8_t type, uint8_t id, const struct sockaddr_storage *a, const void *p, size_t len)
{
    if (n->cfd < 0)
        return -1;
    uint8_t af = 0, addr[16];
    uint16_t port = 0;
    if (a && a->ss_family == AF_INET) {
        af = 4;
        memcpy(addr, &((const struct sockaddr_in *)a)->sin_addr, 4);
        port = ntohs(((const struct sockaddr_in *)a)->sin_port);
    } else if (a && a->ss_family == AF_INET6) {
        af = 6;
        memcpy(addr, &((const struct sockaddr_in6 *)a)->sin6_addr, 16);
        port = ntohs(((const struct sockaddr_in6 *)a)->sin6_port);
    }
    if (rd_buf_avail(&n->out) > MAX_BACKLOG && type == TUN_UDP_SEND)
        return -1; /* drop media instead of growing latency */
    if (tun_build(&n->out, type, id, af, addr, port, p, len) < 0)
        return -1;
    flush(n);
    return 0;
}

static nsock_t *find_tunnel_sock(net_t *n, int id)
{
    for (nsock_t *s = n->socks; s; s = s->next)
        if (s->tunnel && s->id == id)
            return s;
    return NULL;
}

static void frame_addr(const tun_frame_t *f, struct sockaddr_storage *a)
{
    memset(a, 0, sizeof *a);
    if (f->af == 4) {
        struct sockaddr_in *s = (struct sockaddr_in *)a;
        s->sin_family = AF_INET;
        memcpy(&s->sin_addr, f->addr, 4);
        s->sin_port = htons(f->port);
    } else {
        struct sockaddr_in6 *s = (struct sockaddr_in6 *)a;
        s->sin6_family = AF_INET6;
        memcpy(&s->sin6_addr, f->addr, 16);
        s->sin6_port = htons(f->port);
    }
}

static void on_frame(net_t *n, const tun_frame_t *f)
{
    struct sockaddr_storage a;
    nsock_t *s;
    switch (f->type) {
    case TUN_UDP_RECV:
        if ((s = find_tunnel_sock(n, f->sock_id)) && f->af && n->cb.on_udp) {
            frame_addr(f, &a);
            n->cb.on_udp(n->ud, s, &a, f->payload, f->plen);
        }
        break;
    case TUN_UDP_BOUND:
        if ((s = find_tunnel_sock(n, f->sock_id)) && f->af && n->cb.on_bound) {
            frame_addr(f, &a);
            n->cb.on_bound(n->ud, s, &a);
        }
        break;
    case TUN_PING:
        send_frame(n, TUN_PONG, 0, NULL, NULL, 0);
        break;
    case TUN_CTRL:
        if (n->cb.on_ctrl)
            n->cb.on_ctrl(n->ud, f->payload, f->plen);
        break;
    default:
        break;
    }
}

static void on_conn_read(struct ev_loop *loop, ev_io *w, int revents)
{
    net_t *n = w->data;
    rd_buf_reserve(&n->in, 65536);
    ssize_t r = read(n->cfd, n->in.p + n->in.len, n->in.cap - n->in.len);
    if (r <= 0) {
        if (r < 0 && (errno == EAGAIN || errno == EINTR))
            return;
        conn_close(n, r == 0 ? "closed" : strerror(errno));
        return;
    }
    n->in.len += (size_t)r;
    while (n->cfd >= 0) {
        tun_frame_t f;
        int c = tun_parse(rd_buf_data(&n->in), rd_buf_avail(&n->in), &f);
        if (c == 0)
            break;
        if (c < 0) {
            conn_close(n, "bad frame");
            return;
        }
        on_frame(n, &f);
        if (n->cfd < 0)
            return;
        rd_buf_consume(&n->in, (size_t)c);
    }
}

static void on_conn_write(struct ev_loop *loop, ev_io *w, int revents) { flush(w->data); }

static void reopen_tunnel_socks(net_t *n)
{
    for (nsock_t *s = n->socks; s; s = s->next) {
        if (!s->tunnel)
            continue;
        struct sockaddr_storage any;
        memset(&any, 0, sizeof any);
        any.ss_family = (sa_family_t)s->af;
        send_frame(n, TUN_UDP_OPEN, (uint8_t)s->id, &any, NULL, 0);
    }
}

static void on_accept(struct ev_loop *loop, ev_io *w, int revents)
{
    net_t *n = w->data;
    int fd = accept4(n->lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0)
        return;
    if (n->cfd >= 0)
        conn_close(n, "replaced by a new connection");
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); /* fails harmlessly on unix sockets */
    int sz = 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz);
    n->cfd = fd;
    ev_io_init(&n->rio, on_conn_read, fd, EV_READ);
    ev_io_init(&n->wio, on_conn_write, fd, EV_WRITE);
    n->rio.data = n->wio.data = n;
    ev_io_start(loop, &n->rio);
    LOGI("tunnel: agent connected");
    reopen_tunnel_socks(n);
    if (n->cb.on_link)
        n->cb.on_link(n->ud, 1);
}

static int listen_on(const char *spec)
{
    int fd = -1;
    if (!strncmp(spec, "abstract:", 9)) {
        struct sockaddr_un sa = { .sun_family = AF_UNIX };
        size_t nl = strlen(spec + 9);
        if (nl + 1 > sizeof sa.sun_path)
            return -1;
        memcpy(sa.sun_path + 1, spec + 9, nl); /* leading NUL: Linux abstract namespace */
        fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0 || bind(fd, (struct sockaddr *)&sa, (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + nl)) < 0)
            goto fail;
    } else if (!strncmp(spec, "tcp:", 4)) {
        char host[128], port[16];
        struct addrinfo hints = { .ai_socktype = SOCK_STREAM, .ai_flags = AI_PASSIVE }, *ai;
        if (rd_split_hostport(spec + 4, host, sizeof host, port, sizeof port, "27183") < 0 ||
            getaddrinfo(host, port, &hints, &ai))
            return -1;
        fd = socket(ai->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        int r = fd < 0 ? -1 : bind(fd, ai->ai_addr, ai->ai_addrlen);
        freeaddrinfo(ai);
        if (r < 0)
            goto fail;
    } else {
        return -1;
    }
    if (listen(fd, 4) < 0)
        goto fail;
    return fd;
fail:
    LOGE("tunnel: cannot listen on %s: %s", spec, strerror(errno));
    if (fd >= 0)
        close(fd);
    return -1;
}

net_t *net_new(struct ev_loop *loop, const char *listen_spec, int direct, net_cb_t cb, void *ud)
{
    net_t *n = calloc(1, sizeof *n);
    n->loop = loop;
    n->cb = cb;
    n->ud = ud;
    n->direct = direct;
    n->cfd = -1;
    n->lfd = listen_on(listen_spec);
    if (n->lfd < 0) {
        free(n);
        return NULL;
    }
    ev_io_init(&n->lio, on_accept, n->lfd, EV_READ);
    n->lio.data = n;
    ev_io_start(loop, &n->lio);
    LOGI("tunnel: listening on %s", listen_spec);
    return n;
}

void net_set_direct(net_t *n, int direct) { n->direct = direct; }
int net_link_up(const net_t *n) { return n->cfd >= 0; }
size_t net_backlog(const net_t *n) { return rd_buf_avail(&n->out); }

int net_ctrl_send(net_t *n, const char *json, size_t len) { return send_frame(n, TUN_CTRL, 0, NULL, json, len); }

/* ---------------- UDP sockets ---------------- */

static void on_udp_read(struct ev_loop *loop, ev_io *w, int revents)
{
    nsock_t *s = w->data;
    uint8_t buf[2048];
    for (int i = 0; i < 64; i++) {
        struct sockaddr_storage from;
        socklen_t fl = sizeof from;
        ssize_t r = recvfrom(s->fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (r < 0)
            return;
        if (from.ss_family == AF_INET6 && IN6_IS_ADDR_V4MAPPED(&((struct sockaddr_in6 *)&from)->sin6_addr)) {
            struct sockaddr_in6 v6 = *(struct sockaddr_in6 *)&from;
            struct sockaddr_in *v4 = (struct sockaddr_in *)&from;
            memset(&from, 0, sizeof from);
            v4->sin_family = AF_INET;
            v4->sin_port = v6.sin6_port;
            memcpy(&v4->sin_addr, v6.sin6_addr.s6_addr + 12, 4);
        }
        if (s->n->cb.on_udp)
            s->n->cb.on_udp(s->n->ud, s, &from, buf, (size_t)r);
    }
}

/* Path B: host candidates are the phone's own interface addresses. */
static void report_local_addrs(nsock_t *s)
{
    struct sockaddr_storage me;
    socklen_t ml = sizeof me;
    if (getsockname(s->fd, (struct sockaddr *)&me, &ml) < 0)
        return;
    int port = me.ss_family == AF_INET ? ntohs(((struct sockaddr_in *)&me)->sin_port)
                                       : ntohs(((struct sockaddr_in6 *)&me)->sin6_port);
    struct ifaddrs *ifs, *i;
    if (getifaddrs(&ifs) < 0)
        return;
    for (i = ifs; i; i = i->ifa_next) {
        if (!i->ifa_addr || (i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_UP))
            continue;
        struct sockaddr_storage a;
        memset(&a, 0, sizeof a);
        if (i->ifa_addr->sa_family == AF_INET && s->af == AF_INET) {
            memcpy(&a, i->ifa_addr, sizeof(struct sockaddr_in));
            ((struct sockaddr_in *)&a)->sin_port = htons((uint16_t)port);
        } else if (i->ifa_addr->sa_family == AF_INET6 && s->af == AF_INET6) {
            struct sockaddr_in6 *x = (struct sockaddr_in6 *)i->ifa_addr;
            if (IN6_IS_ADDR_LINKLOCAL(&x->sin6_addr))
                continue;
            memcpy(&a, x, sizeof *x);
            ((struct sockaddr_in6 *)&a)->sin6_port = htons((uint16_t)port);
        } else {
            continue;
        }
        if (s->n->cb.on_bound)
            s->n->cb.on_bound(s->n->ud, s, &a);
    }
    freeifaddrs(ifs);
}

nsock_t *net_udp_open(net_t *n, int af)
{
    nsock_t *s = calloc(1, sizeof *s);
    s->n = n;
    s->af = af;
    s->fd = -1;
    if (!n->direct) {
        int id = 0;
        for (int i = 1; i < 256 && !id; i++)
            if (!n->used[i])
                id = i;
        if (!id) {
            free(s);
            return NULL;
        }
        n->used[id] = 1;
        s->id = id;
        s->tunnel = 1;
        s->next = n->socks;
        n->socks = s;
        struct sockaddr_storage any;
        memset(&any, 0, sizeof any);
        any.ss_family = (sa_family_t)af;
        send_frame(n, TUN_UDP_OPEN, (uint8_t)id, &any, NULL, 0);
        return s;
    }
    s->fd = socket(af, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (s->fd < 0) {
        free(s);
        return NULL;
    }
    int sz = 1024 * 1024;
    setsockopt(s->fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz);
    setsockopt(s->fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof sz);
    ev_io_init(&s->io, on_udp_read, s->fd, EV_READ);
    s->io.data = s;
    ev_io_start(n->loop, &s->io);
    struct sockaddr_storage any;
    memset(&any, 0, sizeof any);
    any.ss_family = (sa_family_t)af;
    bind(s->fd, (struct sockaddr *)&any, af == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6));
    s->next = n->socks;
    n->socks = s;
    return s;
}

void net_udp_report(nsock_t *s)
{
    if (!s->tunnel)
        report_local_addrs(s); /* tunnel sockets get UDP_BOUND frames from the agent instead */
}

int net_udp_send(nsock_t *s, const struct sockaddr_storage *to, const void *d, size_t len)
{
    if (s->tunnel)
        return send_frame(s->n, TUN_UDP_SEND, (uint8_t)s->id, to, d, len);
    socklen_t tl = to->ss_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
    return sendto(s->fd, d, len, MSG_DONTWAIT, (const struct sockaddr *)to, tl) < 0 ? -1 : 0;
}

void net_udp_close(nsock_t *s)
{
    if (!s)
        return;
    net_t *n = s->n;
    for (nsock_t **pp = &n->socks; *pp; pp = &(*pp)->next) {
        if (*pp == s) {
            *pp = s->next;
            break;
        }
    }
    if (s->tunnel) {
        send_frame(n, TUN_UDP_CLOSE, (uint8_t)s->id, NULL, NULL, 0);
        n->used[s->id] = 0;
    } else {
        ev_io_stop(n->loop, &s->io);
        close(s->fd);
    }
    free(s);
}

int net_udp_is_tunnel(const nsock_t *s) { return s->tunnel; }
