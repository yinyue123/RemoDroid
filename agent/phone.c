/* Phone side of the agent: installs and supervises the phone server over ADB,
 * carries the tunnel (plan §4.2) and relays the phone's UDP packets. */
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "agent.h"
#include "msg.h"
#include "tunnel.h"

#define SERVER_CLASS "io.remodroid.Server"
#define SOCKET_NAME "remodroid"
#define LINK_BACKLOG_MAX (1024 * 1024)
#define LOG_LINES 500

enum { SV_STOPPED, SV_STARTING, SV_RUNNING, SV_CRASHED };
static const char *SV_NAMES[] = { "stopped", "starting", "running", "crashed" };

typedef struct {
    int fd, af;
    ev_io io;
} usock_t;

static struct {
    int adb_state;
    int sv;                      /* SV_* */
    int link_up;                 /* tunnel open */
    int ready;                   /* phone sent "ready" */
    int missing_files_warned;
    adb_stream_t *server_stream; /* long-running app_process shell */
    adb_stream_t *tunnel;
    int tunnel_tries;
    ev_timer retry, ping;
    ev_tstamp last_pong;
    double restart_delay;
    /* tcp link mode (development) */
    char tcp_link[128];
    int tfd;
    ev_io trio, twio;
    rd_buf_t tout;
    /* tunnel parsing */
    rd_buf_t tin;
    usock_t socks[256];
    /* info */
    char model[64], android[16], serial[64], abi[32];
    int sdk;
    cJSON *phone_status;         /* fields reported by the phone */
    cJSON *ready_info;
    /* server log ring */
    char *log[LOG_LINES];
    int log_head, log_n;
    rd_buf_t log_partial;
} P;

static void start_server(void);
static void link_down(const char *why);
static void schedule_restart(const char *why);

/* ---------------- server log ring ---------------- */

static void log_line(const char *s, size_t n)
{
    char *l = malloc(n + 1);
    memcpy(l, s, n);
    l[n] = 0;
    LOGD("phone: %s", l);
    int idx = (P.log_head + P.log_n) % LOG_LINES;
    if (P.log_n == LOG_LINES) {
        free(P.log[P.log_head]);
        P.log_head = (P.log_head + 1) % LOG_LINES;
        idx = (P.log_head + P.log_n - 1) % LOG_LINES;
    } else {
        P.log_n++;
    }
    P.log[idx] = l;
}

static void server_output(const uint8_t *d, size_t n, void *ud)
{
    rd_buf_append(&P.log_partial, d, n);
    for (;;) {
        uint8_t *p = rd_buf_data(&P.log_partial);
        size_t av = rd_buf_avail(&P.log_partial);
        uint8_t *nl = memchr(p, '\n', av);
        if (!nl) {
            if (av > 4096) {
                log_line((char *)p, av);
                rd_buf_consume(&P.log_partial, av);
            }
            return;
        }
        size_t len = (size_t)(nl - p);
        if (len && p[len - 1] == '\r')
            len--;
        log_line((char *)p, len);
        rd_buf_consume(&P.log_partial, (size_t)(nl - p) + 1);
    }
}

cJSON *phone_server_log_tail(int lines)
{
    cJSON *arr = cJSON_CreateArray();
    int n = RD_MIN(lines, P.log_n);
    for (int i = P.log_n - n; i < P.log_n; i++)
        cJSON_AddItemToArray(arr, cJSON_CreateString(P.log[(P.log_head + i) % LOG_LINES]));
    return arr;
}

/* ---------------- status ---------------- */

static void set_sv(int sv)
{
    if (P.sv == sv)
        return;
    P.sv = sv;
    LOGI("phone server: %s", SV_NAMES[sv]);
    agent_status_changed();
}

cJSON *phone_status_json(void)
{
    cJSON *j = cJSON_CreateObject();
    int attached = P.adb_state != ADB_OFFLINE || P.link_up;
    cJSON_AddBoolToObject(j, "attached", attached);
    const char *problem = attached ? NULL : transport_problem(A.tm);
    if (problem)
        cJSON_AddStringToObject(j, "problem", problem);
    if (P.serial[0])
        cJSON_AddStringToObject(j, "serial", P.serial);
    if (P.model[0])
        cJSON_AddStringToObject(j, "model", P.model);
    if (P.android[0])
        cJSON_AddStringToObject(j, "android", P.android);
    if (P.sdk)
        cJSON_AddNumberToObject(j, "sdk", P.sdk);
    cJSON_AddStringToObject(j, "server", SV_NAMES[P.sv]);
    cJSON_AddStringToObject(j, "adb_state", adb_state_name(P.adb_state));
    if (P.ready_info) {
        cJSON *c = cJSON_GetObjectItemCaseSensitive(P.ready_info, "codecs");
        if (c)
            cJSON_AddItemToObject(j, "codecs", cJSON_Duplicate(c, 1));
    }
    for (cJSON *it = P.phone_status ? P.phone_status->child : NULL; it; it = it->next) {
        cJSON_DeleteItemFromObjectCaseSensitive(j, it->string);
        cJSON_AddItemToObject(j, it->string, cJSON_Duplicate(it, 1));
    }
    return j;
}

/* ---------------- link send ---------------- */

static size_t link_backlog(void)
{
    if (P.tcp_link[0])
        return rd_buf_avail(&P.tout);
    return adb_backlog(P.tunnel);
}

static void tcp_link_flush(void);

static int link_send_buf(rd_buf_t *b)
{
    if (!P.link_up)
        return -1;
    if (P.tcp_link[0]) {
        rd_buf_append(&P.tout, rd_buf_data(b), rd_buf_avail(b));
        tcp_link_flush();
        return 0;
    }
    return adb_write(P.tunnel, rd_buf_data(b), rd_buf_avail(b));
}

static void send_ctrl(cJSON *m)
{
    char *s = msg_print(m);
    cJSON_Delete(m);
    rd_buf_t b;
    rd_buf_init(&b);
    if (tun_build(&b, TUN_CTRL, 0, 0, NULL, 0, s, strlen(s)) == 0)
        link_send_buf(&b);
    else
        LOGW("phone: control message too large");
    rd_buf_free(&b);
    free(s);
}

static void send_simple(uint8_t type)
{
    rd_buf_t b;
    rd_buf_init(&b);
    tun_build(&b, type, 0, 0, NULL, 0, NULL, 0);
    link_send_buf(&b);
    rd_buf_free(&b);
}

/* ---------------- UDP relay ---------------- */

static void usock_close(int id)
{
    usock_t *u = &P.socks[id];
    if (u->fd <= 0)
        return;
    ev_io_stop(A.loop, &u->io);
    close(u->fd);
    u->fd = 0;
}

static void on_udp(struct ev_loop *loop, ev_io *w, int revents)
{
    int id = (int)(intptr_t)w->data;
    usock_t *u = &P.socks[id];
    uint8_t buf[2048];
    for (int i = 0; i < 64; i++) {
        struct sockaddr_storage ss;
        socklen_t sl = sizeof ss;
        ssize_t n = recvfrom(u->fd, buf, sizeof buf, 0, (struct sockaddr *)&ss, &sl);
        if (n < 0)
            return;
        if (link_backlog() > LINK_BACKLOG_MAX)
            continue; /* the phone isn't keeping up: drop rather than add latency */
        rd_buf_t b;
        rd_buf_init(&b);
        if (ss.ss_family == AF_INET6) {
            struct sockaddr_in6 *a = (struct sockaddr_in6 *)&ss;
            if (IN6_IS_ADDR_V4MAPPED(&a->sin6_addr))
                tun_build(&b, TUN_UDP_RECV, (uint8_t)id, 4, a->sin6_addr.s6_addr + 12, ntohs(a->sin6_port), buf, (size_t)n);
            else
                tun_build(&b, TUN_UDP_RECV, (uint8_t)id, 6, a->sin6_addr.s6_addr, ntohs(a->sin6_port), buf, (size_t)n);
        } else {
            struct sockaddr_in *a = (struct sockaddr_in *)&ss;
            tun_build(&b, TUN_UDP_RECV, (uint8_t)id, 4, (uint8_t *)&a->sin_addr, ntohs(a->sin_port), buf, (size_t)n);
        }
        link_send_buf(&b);
        rd_buf_free(&b);
    }
}

/* Tell the phone this socket's LAN addresses so ICE can offer host candidates
 * (lets an iPhone on the home network connect directly). */
static void report_bound(int id, int fd, int af)
{
    struct sockaddr_storage me;
    socklen_t ml = sizeof me;
    if (getsockname(fd, (struct sockaddr *)&me, &ml) < 0)
        return;
    uint16_t port = me.ss_family == AF_INET ? ntohs(((struct sockaddr_in *)&me)->sin_port)
                                            : ntohs(((struct sockaddr_in6 *)&me)->sin6_port);
    struct ifaddrs *ifs;
    if (getifaddrs(&ifs) < 0)
        return;
    for (struct ifaddrs *i = ifs; i; i = i->ifa_next) {
        if (!i->ifa_addr || (i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_UP))
            continue;
        rd_buf_t b;
        rd_buf_init(&b);
        if (i->ifa_addr->sa_family == AF_INET) {
            tun_build(&b, TUN_UDP_BOUND, (uint8_t)id, 4, (uint8_t *)&((struct sockaddr_in *)i->ifa_addr)->sin_addr, port,
                      NULL, 0);
        } else if (i->ifa_addr->sa_family == AF_INET6 && af == AF_INET6) {
            struct sockaddr_in6 *x = (struct sockaddr_in6 *)i->ifa_addr;
            if (!IN6_IS_ADDR_LINKLOCAL(&x->sin6_addr))
                tun_build(&b, TUN_UDP_BOUND, (uint8_t)id, 6, x->sin6_addr.s6_addr, port, NULL, 0);
        }
        if (rd_buf_avail(&b))
            link_send_buf(&b);
        rd_buf_free(&b);
    }
    freeifaddrs(ifs);
}

static void udp_open(const tun_frame_t *f)
{
    usock_close(f->sock_id);
    int af = f->af == 6 ? AF_INET6 : AF_INET;
    int fd = socket(af, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        LOGW("relay: socket: %s", strerror(errno));
        return;
    }
    if (af == AF_INET6) {
        int off = 0;
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);
    }
    int sz = 512 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof sz);
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz);
    struct sockaddr_storage any; /* bind now so the port is known for UDP_BOUND */
    memset(&any, 0, sizeof any);
    any.ss_family = (sa_family_t)af;
    bind(fd, (struct sockaddr *)&any, af == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in));
    usock_t *u = &P.socks[f->sock_id];
    u->fd = fd;
    u->af = af;
    ev_io_init(&u->io, on_udp, fd, EV_READ);
    u->io.data = (void *)(intptr_t)f->sock_id;
    ev_io_start(A.loop, &u->io);
    LOGD("relay: opened UDP socket %u (IPv%d)", f->sock_id, f->af == 6 ? 6 : 4);
    report_bound(f->sock_id, fd, af);
}

static void udp_send(const tun_frame_t *f)
{
    usock_t *u = &P.socks[f->sock_id];
    if (u->fd <= 0)
        return;
    struct sockaddr_storage ss;
    socklen_t sl;
    memset(&ss, 0, sizeof ss);
    if (f->af == 4 && u->af == AF_INET) {
        struct sockaddr_in *a = (struct sockaddr_in *)&ss;
        a->sin_family = AF_INET;
        memcpy(&a->sin_addr, f->addr, 4);
        a->sin_port = htons(f->port);
        sl = sizeof *a;
    } else {
        struct sockaddr_in6 *a = (struct sockaddr_in6 *)&ss;
        a->sin6_family = AF_INET6;
        if (f->af == 4) { /* IPv4 on a dual-stack socket */
            a->sin6_addr.s6_addr[10] = a->sin6_addr.s6_addr[11] = 0xff;
            memcpy(a->sin6_addr.s6_addr + 12, f->addr, 4);
        } else {
            memcpy(&a->sin6_addr, f->addr, 16);
        }
        a->sin6_port = htons(f->port);
        sl = sizeof *a;
    }
    /* UDP: if the socket buffer is full the packet is dropped, as it would be on the wire. */
    sendto(u->fd, f->payload, f->plen, MSG_DONTWAIT, (struct sockaddr *)&ss, sl);
}

/* ---------------- tunnel control ---------------- */

static cJSON *typed_value(const char *v)
{
    if (rd_streq_nocase(v, "true") || rd_streq_nocase(v, "yes") || rd_streq_nocase(v, "on"))
        return cJSON_CreateTrue();
    if (rd_streq_nocase(v, "false") || rd_streq_nocase(v, "no") || rd_streq_nocase(v, "off"))
        return cJSON_CreateFalse();
    char *end;
    double d = strtod(v, &end);
    if (*v && !*end)
        return cJSON_CreateNumber(d);
    return cJSON_CreateString(v);
}

static void add_kv(const char *key, const char *val, void *ud)
{
    cJSON_DeleteItemFromObjectCaseSensitive(ud, key);
    cJSON_AddItemToObject(ud, key, typed_value(val));
}

static void send_config(void)
{
    cJSON *m = msg_new("config", 0, NULL);
    cJSON *d = msg_d(m);
    static const char *secs[] = { "phone", "video", "files" };
    for (size_t i = 0; i < RD_ARRAY_LEN(secs); i++)
        rd_conf_each(A.conf, secs[i], add_kv, cJSON_AddObjectToObject(d, secs[i]));
    send_ctrl(m);
}

void phone_config_changed(void)
{
    if (P.link_up)
        send_config();
}

static void handle_ctrl(const uint8_t *p, size_t n)
{
    cJSON *m = cJSON_ParseWithLength((const char *)p, n);
    if (!cJSON_IsObject(m)) {
        cJSON_Delete(m);
        return;
    }
    const char *t = msg_type(m);
    LOGT("phone <- %.*s", (int)RD_MIN(n, 300), p);
    if (!strcmp(t, "ready")) {
        cJSON_Delete(P.ready_info);
        P.ready_info = cJSON_Duplicate(msg_d(m), 1);
        P.ready = 1;
        P.restart_delay = 0;
        set_sv(SV_RUNNING);
        LOGI("phone server ready (version %s, Android %s)", msg_str(P.ready_info, "version", "?"),
             msg_str(P.ready_info, "android", "?"));
        agent_event("server.started", cJSON_Duplicate(P.ready_info, 1));
    } else if (!strcmp(t, "status")) {
        if (!P.phone_status)
            P.phone_status = cJSON_CreateObject();
        for (cJSON *it = msg_d(m)->child; it; it = it->next) {
            cJSON_DeleteItemFromObjectCaseSensitive(P.phone_status, it->string);
            cJSON_AddItemToObject(P.phone_status, it->string, cJSON_Duplicate(it, 1));
        }
        agent_status_changed();
    } else if (!strcmp(t, "event")) {
        cJSON *d = msg_d(m);
        agent_event(msg_str(d, "kind", "unknown"), cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(d, "info"), 1));
    } else if (!strcmp(t, "signal") || !strcmp(t, "session.ended")) {
        cJSON *f = msg_new(t, 0, msg_sid(m) ? msg_sid(m) : msg_str(msg_d(m), "sid", NULL));
        cJSON_ReplaceItemInObjectCaseSensitive(f, "d", cJSON_Duplicate(msg_d(m), 1));
        wsc_send(f);
    } else if (!strcmp(t, "error")) {
        LOGW("phone error: %s", msg_str(msg_d(m), "msg", "?"));
    }
    cJSON_Delete(m);
}

static void handle_frame(const tun_frame_t *f)
{
    switch (f->type) {
    case TUN_UDP_OPEN: udp_open(f); break;
    case TUN_UDP_SEND: udp_send(f); break;
    case TUN_UDP_CLOSE: usock_close(f->sock_id); break;
    case TUN_PING: send_simple(TUN_PONG); break;
    case TUN_PONG: P.last_pong = ev_now(A.loop); break;
    case TUN_CTRL: handle_ctrl(f->payload, f->plen); break;
    default: LOGD("tunnel: unknown frame type %02x", f->type);
    }
}

static void link_data(const uint8_t *d, size_t n)
{
    rd_buf_append(&P.tin, d, n);
    while (P.link_up) {
        tun_frame_t f;
        int r = tun_parse(rd_buf_data(&P.tin), rd_buf_avail(&P.tin), &f);
        if (r == 0)
            return;
        if (r < 0) {
            link_down("bad tunnel frame");
            schedule_restart("tunnel error");
            return;
        }
        handle_frame(&f);
        rd_buf_consume(&P.tin, (size_t)r);
    }
}

static void link_up(void)
{
    P.link_up = 1;
    P.ready = 0;
    P.last_pong = ev_now(A.loop);
    rd_buf_reset(&P.tin);
    LOGI("tunnel to the phone server is open");
    send_config();
    ev_timer_set(&P.ping, 5., 5.);
    ev_timer_start(A.loop, &P.ping);
    agent_status_changed();
}

static void link_down(const char *why)
{
    if (!P.link_up && !P.tunnel && P.tfd <= 0)
        return;
    if (P.link_up)
        LOGI("tunnel closed: %s", why);
    P.link_up = 0;
    P.ready = 0;
    ev_timer_stop(A.loop, &P.ping);
    if (P.tunnel) {
        adb_stream_t *t = P.tunnel;
        P.tunnel = NULL;
        adb_close(t);
    }
    if (P.tfd > 0) {
        ev_io_stop(A.loop, &P.trio);
        ev_io_stop(A.loop, &P.twio);
        close(P.tfd);
        P.tfd = 0;
        rd_buf_reset(&P.tout);
    }
    for (int i = 0; i < 256; i++)
        usock_close(i);
    agent_status_changed();
}

static void on_ping(struct ev_loop *loop, ev_timer *w, int revents)
{
    if (!P.link_up)
        return;
    if (ev_now(loop) - P.last_pong > 15.) {
        link_down("phone server stopped answering");
        schedule_restart("no answer");
        return;
    }
    send_simple(TUN_PING);
}

/* ---- tunnel over ADB ---- */

static void tunnel_open(adb_stream_t *s, int ok, void *ud)
{
    if (!ok) {
        P.tunnel = NULL;
        if (++P.tunnel_tries > 60) {
            schedule_restart("tunnel never opened");
            return;
        }
        ev_timer_stop(A.loop, &P.retry);
        ev_timer_set(&P.retry, 0.3, 0.);
        ev_timer_start(A.loop, &P.retry);
        return;
    }
    link_up();
}

static void tunnel_data(adb_stream_t *s, const uint8_t *d, size_t n, void *ud) { link_data(d, n); }

static void tunnel_close(adb_stream_t *s, void *ud)
{
    P.tunnel = NULL;
    link_down("closed by the phone");
}

static void open_tunnel(void)
{
    if (P.tunnel || adb_state(A.adb) != ADB_ONLINE)
        return;
    P.tunnel = adb_open(A.adb, "localabstract:" SOCKET_NAME,
                        (adb_stream_cb_t){ tunnel_open, tunnel_data, tunnel_close, NULL }, NULL);
}

/* ---- tunnel over TCP (development: phone server reachable directly) ---- */

static void tcp_link_flush(void)
{
    while (rd_buf_avail(&P.tout)) {
        ssize_t w = write(P.tfd, rd_buf_data(&P.tout), rd_buf_avail(&P.tout));
        if (w < 0) {
            if (errno == EAGAIN || errno == EINTR)
                break;
            link_down(strerror(errno));
            return;
        }
        rd_buf_consume(&P.tout, (size_t)w);
    }
    if (rd_buf_avail(&P.tout))
        ev_io_start(A.loop, &P.twio);
    else
        ev_io_stop(A.loop, &P.twio);
}

static void tcp_link_read(struct ev_loop *loop, ev_io *w, int revents)
{
    uint8_t buf[65536];
    ssize_t n = read(P.tfd, buf, sizeof buf);
    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            return;
        link_down("tcp link closed");
        schedule_restart("tcp link closed");
        return;
    }
    link_data(buf, (size_t)n);
}

static void tcp_link_write(struct ev_loop *loop, ev_io *w, int revents)
{
    if (!P.link_up) {
        int err = 0;
        socklen_t el = sizeof err;
        getsockopt(P.tfd, SOL_SOCKET, SO_ERROR, &err, &el);
        if (err) {
            ev_io_stop(loop, w);
            close(P.tfd);
            P.tfd = 0;
            schedule_restart(strerror(err));
            return;
        }
        ev_io_start(loop, &P.trio);
        link_up();
    }
    tcp_link_flush();
}

static void open_tcp_link(void)
{
    char host[128], port[16];
    struct addrinfo hints = { .ai_socktype = SOCK_STREAM }, *ai;
    if (rd_split_hostport(P.tcp_link, host, sizeof host, port, sizeof port, "27183") < 0 ||
        getaddrinfo(host, port, &hints, &ai)) {
        schedule_restart("bad [phone] link address");
        return;
    }
    P.tfd = socket(ai->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int one = 1;
    setsockopt(P.tfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (connect(P.tfd, ai->ai_addr, ai->ai_addrlen) < 0 && errno != EINPROGRESS) {
        close(P.tfd);
        P.tfd = 0;
        freeaddrinfo(ai);
        schedule_restart("connect failed");
        return;
    }
    freeaddrinfo(ai);
    ev_io_init(&P.trio, tcp_link_read, P.tfd, EV_READ);
    ev_io_init(&P.twio, tcp_link_write, P.tfd, EV_WRITE);
    ev_io_start(A.loop, &P.twio);
    set_sv(SV_STARTING);
}

/* ---------------- supervisor ---------------- */

static void on_retry(struct ev_loop *loop, ev_timer *w, int revents)
{
    if (P.tcp_link[0]) {
        if (!P.link_up && P.tfd <= 0)
            open_tcp_link();
        return;
    }
    if (P.sv == SV_STARTING && !P.link_up)
        open_tunnel();
    else if ((P.sv == SV_STOPPED || P.sv == SV_CRASHED) && adb_state(A.adb) == ADB_ONLINE)
        start_server();
}

static void schedule_restart(const char *why)
{
    P.restart_delay = P.restart_delay ? RD_MIN(P.restart_delay * 2, 30.) : 1.;
    LOGI("phone server: restarting in %.0f s (%s)", P.restart_delay, why);
    if (P.server_stream) {
        adb_stream_t *s = P.server_stream;
        P.server_stream = NULL;
        adb_close(s);
    }
    link_down(why);
    if (P.sv != SV_STOPPED)
        set_sv(SV_CRASHED);
    ev_timer_stop(A.loop, &P.retry);
    ev_timer_set(&P.retry, P.restart_delay, 0.);
    ev_timer_start(A.loop, &P.retry);
}

static void server_exited(int code, const char *out, size_t n, void *ud)
{
    if (!P.server_stream)
        return; /* we closed it ourselves */
    P.server_stream = NULL;
    if (adb_state(A.adb) != ADB_ONLINE)
        return; /* the phone went away; phone_adb_state() handles it */
    LOGW("phone server exited (code %d)", code);
    cJSON *info = cJSON_CreateObject();
    cJSON_AddNumberToObject(info, "code", code);
    agent_event("server.exited", info);
    schedule_restart("exited");
}

static void launch_server(void)
{
    char cmd[1024];
    const char *dir = A.phone_dir;
    snprintf(cmd, sizeof cmd,
             /* The class name must not appear whole in this command: pkill -f also sees the shell's
              * own command line and would kill it (exit 143). "$c.Server" expands only at run time. */
             "c=io.remodroid; pkill -f \"$c.Server\" >/dev/null 2>&1; "
             "CLASSPATH=%s/remodroid-server.jar exec app_process /system/bin \"$c.Server\""
             " socket=" SOCKET_NAME " lib=%s/libremodroid.so log=%s",
             dir, dir, agent_conf("phone", "log_level", "info"));
    P.server_stream = adb_shell(A.adb, cmd, server_output, server_exited, NULL);
    if (!P.server_stream) {
        schedule_restart("cannot start a shell");
        return;
    }
    P.tunnel_tries = 0;
    set_sv(SV_STARTING);
    ev_timer_stop(A.loop, &P.retry);
    ev_timer_set(&P.retry, 0.5, 0.);
    ev_timer_start(A.loop, &P.retry);
}

static int pushes_pending;
static int push_failed;

static void pushed(int code, const char *msg, void *ud)
{
    const char *name = ud;
    if (code < 0) {
        LOGW("push %s failed: %s", name, msg);
        push_failed = 1;
    } else {
        LOGI("push %s: %s", name, code == 1 ? "already up to date" : "done");
    }
    if (--pushes_pending == 0) {
        if (push_failed)
            schedule_restart("push failed");
        else
            launch_server();
    }
}

static void got_props(int code, const char *out, size_t n, void *ud)
{
    char buf[512];
    snprintf(buf, sizeof buf, "%.*s", (int)RD_MIN(n, sizeof buf - 1), out);
    char *save = NULL, *l1 = strtok_r(buf, "\r\n", &save), *l2 = strtok_r(NULL, "\r\n", &save),
         *l3 = strtok_r(NULL, "\r\n", &save), *l4 = strtok_r(NULL, "\r\n", &save);
    snprintf(P.model, sizeof P.model, "%s", l1 ? l1 : "");
    snprintf(P.android, sizeof P.android, "%s", l2 ? l2 : "");
    P.sdk = l3 ? atoi(l3) : 0;
    snprintf(P.abi, sizeof P.abi, "%s", l4 && !strchr(l4, '/') ? l4 : "");
    LOGI("phone: %s, Android %s (SDK %d)", P.model, P.android, P.sdk);
    agent_status_changed();
    if (adb_state(A.adb) != ADB_ONLINE)
        return;

    static const struct { const char *name; unsigned mode; int required; } files[] = {
        { "remodroid-server.jar", 0100644, 1 },
        { "libremodroid.so", 0100755, 0 },
    };
    push_failed = 0;
    pushes_pending = 1; /* guard while queuing */
    for (size_t i = 0; i < RD_ARRAY_LEN(files); i++) {
        char local[512], remote[512];
        struct stat st;
        snprintf(local, sizeof local, "%s/%s/%s", A.files_dir, P.abi, files[i].name); /* per-ABI build first */
        if (!P.abi[0] || stat(local, &st) < 0)
            snprintf(local, sizeof local, "%s/%s", A.files_dir, files[i].name);
        snprintf(remote, sizeof remote, "%s/%s", A.phone_dir, files[i].name);
        if (stat(local, &st) < 0) {
            if (files[i].required) {
                if (!P.missing_files_warned)
                    LOGE("phone files missing: %s not found (set [agent] files_dir)", local);
                P.missing_files_warned = 1;
                pushes_pending = 0;
                set_sv(SV_STOPPED);
                return;
            }
            continue;
        }
        pushes_pending++;
        if (adb_push(A.adb, local, remote, files[i].mode, pushed, (void *)files[i].name) < 0) {
            pushes_pending--;
            push_failed = 1;
        }
    }
    if (--pushes_pending == 0) {
        if (push_failed)
            schedule_restart("push failed");
        else
            launch_server();
    }
}

static void start_server(void)
{
    if (P.tcp_link[0] || adb_state(A.adb) != ADB_ONLINE || P.server_stream)
        return;
    set_sv(SV_STARTING);
    if (!adb_shell(A.adb, "getprop ro.product.model; getprop ro.build.version.release; getprop ro.build.version.sdk; "
                          "getprop ro.product.cpu.abi",
                   NULL, got_props, NULL))
        schedule_restart("cannot run getprop");
}

void phone_restart_server(void)
{
    P.restart_delay = 0;
    if (P.tcp_link[0]) {
        link_down("restart requested");
        schedule_restart("restart requested");
        return;
    }
    if (P.server_stream) {
        adb_stream_t *s = P.server_stream;
        P.server_stream = NULL;
        adb_close(s);
    }
    link_down("restart requested");
    set_sv(SV_STOPPED);
    start_server();
}

void phone_adb_state(int state)
{
    P.adb_state = state;
    snprintf(P.serial, sizeof P.serial, "%s", adb_serial(A.adb));
    if (state == ADB_UNAUTHORIZED) {
        agent_event("adb.unauthorized", NULL);
    } else if (state == ADB_ONLINE) {
        agent_event("adb.authorized", NULL);
        P.restart_delay = 0;
        P.missing_files_warned = 0;
        start_server();
    } else if (state == ADB_OFFLINE) {
        P.server_stream = NULL; /* streams are gone with the transport */
        P.tunnel = NULL;
        link_down("phone disconnected");
        set_sv(SV_STOPPED);
        P.model[0] = P.android[0] = 0;
        P.sdk = 0;
        cJSON_Delete(P.phone_status);
        P.phone_status = NULL;
    }
    agent_status_changed();
}

/* ---------------- server messages and commands ---------------- */

/* The phone may have no DNS of its own (Path A), so turn:/stun: host names
 * are resolved here; TURN credentials don't depend on the host name. */
static void resolve_ice_hosts(cJSON *servers)
{
    cJSON *srv;
    cJSON_ArrayForEach(srv, servers) {
        cJSON *urls = cJSON_GetObjectItemCaseSensitive(srv, "urls"), *u;
        cJSON_ArrayForEach(u, urls) {
            if (!cJSON_IsString(u))
                continue;
            const char *s = u->valuestring, *colon = strchr(s, ':');
            if (!colon || colon[1] == '[')
                continue;
            const char *hs = colon + 1, *he = hs + strcspn(hs, ":?");
            char host[256];
            if ((size_t)(he - hs) >= sizeof host)
                continue;
            memcpy(host, hs, (size_t)(he - hs));
            host[he - hs] = 0;
            struct in_addr tmp;
            if (inet_pton(AF_INET, host, &tmp) == 1)
                continue;
            struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM }, *ai;
            if (getaddrinfo(host, NULL, &hints, &ai)) {
                LOGW("cannot resolve ICE server %s", host);
                continue;
            }
            char ip[INET_ADDRSTRLEN], out[512];
            inet_ntop(AF_INET, &((struct sockaddr_in *)ai->ai_addr)->sin_addr, ip, sizeof ip);
            freeaddrinfo(ai);
            snprintf(out, sizeof out, "%.*s%s%s", (int)(hs - s), s, ip, he);
            cJSON_SetValuestring(u, out);
        }
    }
}

void phone_on_server_msg(cJSON *m)
{
    const char *t = msg_type(m);
    if (!P.link_up || !P.ready) {
        if (!strcmp(t, "session.start")) {
            const char *sid = msg_sid(m) ? msg_sid(m) : msg_str(msg_d(m), "sid", "");
            cJSON *e = msg_new("session.ended", 0, sid);
            cJSON_AddStringToObject(msg_d(e), "sid", sid);
            cJSON_AddStringToObject(msg_d(e), "reason", "phone_unavailable");
            wsc_send(e);
        }
        return;
    }
    cJSON *c = cJSON_Duplicate(m, 1);
    if (!strcmp(t, "session.start"))
        resolve_ice_hosts(cJSON_GetObjectItemCaseSensitive(msg_d(c), "ice_servers"));
    send_ctrl(c);
}

typedef struct {
    const char *api, *sec, *key;
} cfgmap_t;

/* Names used by the web panel (agent.config) → agent.ini keys. */
static const cfgmap_t CFG[] = {
    { "screenOff", "phone", "screen_off_on_connect" }, { "lockOnDisc", "phone", "lock_on_disconnect" },
    { "grace", "phone", "lock_grace_seconds" },        { "clipSync", "phone", "clipboard_sync" },
    { "autoPaste", "phone", "auto_paste" },            { "overheat", "phone", "overheat_c" },
    { "rotation", "phone", "rotation" },               { "route", "phone", "path" },
    { "relay", "phone", "force_relay" },               { "destDir", "files", "upload_dir" },
    { "audio", "phone", "audio" },
    { "mediaScan", "files", "media_scan" },            { "overwrite", "files", "overwrite" },
    { "maxMb", "files", "max_mb" },                    { "codec", "video", "codec" },
    { "kbps", "video", "max_bitrate_kbps" },           { "fps", "video", "max_fps" },
    { "preset", "video", "preset" },                   { "mode", "video", "mode" },
};

/* The settings the panel can change, under the names it uses (agent.config.get). */
cJSON *phone_config_json(void)
{
    cJSON *j = cJSON_CreateObject();
    for (size_t i = 0; i < RD_ARRAY_LEN(CFG); i++) {
        const char *v = agent_conf(CFG[i].sec, CFG[i].key, NULL);
        if (v)
            cJSON_AddItemToObject(j, CFG[i].api, typed_value(v));
    }
    return j;
}

static int apply_config(cJSON *args, char *err, size_t errcap)
{
    int changed = 0;
    for (cJSON *it = args ? args->child : NULL; it; it = it->next) {
        const char *sec = NULL, *key = NULL;
        char sbuf[64];
        for (size_t i = 0; i < RD_ARRAY_LEN(CFG); i++)
            if (!strcmp(CFG[i].api, it->string))
                sec = CFG[i].sec, key = CFG[i].key;
        const char *dot = strchr(it->string, '.');
        if (!sec && dot && (size_t)(dot - it->string) < sizeof sbuf) {
            memcpy(sbuf, it->string, (size_t)(dot - it->string));
            sbuf[dot - it->string] = 0;
            if (!strcmp(sbuf, "phone") || !strcmp(sbuf, "video") || !strcmp(sbuf, "files"))
                sec = sbuf, key = dot + 1;
        }
        if (!sec) {
            snprintf(err, errcap, "unknown setting: %s", it->string);
            return -1;
        }
        char val[256];
        if (cJSON_IsBool(it))
            snprintf(val, sizeof val, "%s", cJSON_IsTrue(it) ? "true" : "false");
        else if (cJSON_IsNumber(it))
            snprintf(val, sizeof val, "%g", it->valuedouble);
        else if (cJSON_IsString(it))
            snprintf(val, sizeof val, "%s", it->valuestring);
        else
            continue;
        if (agent_conf_set(sec, key, val) < 0) {
            snprintf(err, errcap, "cannot write %s", A.conf_path);
            return -1;
        }
        changed = 1;
    }
    if (changed)
        agent_reload_apply();
    return 0;
}

static void shell_quiet(const char *cmd)
{
    if (adb_state(A.adb) == ADB_ONLINE)
        adb_shell(A.adb, cmd, NULL, NULL, NULL);
}

int phone_run_cmd(const char *cmd, cJSON *args, char *err, size_t errcap)
{
    int online = adb_state(A.adb) == ADB_ONLINE;
    if (!strcmp(cmd, "server.restart")) {
        phone_restart_server();
    } else if (!strcmp(cmd, "phone.wake") || !strcmp(cmd, "phone.sleep")) {
        int wake = !strcmp(cmd, "phone.wake");
        if (P.link_up && P.ready) {
            cJSON *m = msg_new("cmd", 0, NULL);
            cJSON_AddStringToObject(msg_d(m), "cmd", cmd);
            send_ctrl(m);
        } else if (online) {
            shell_quiet(wake ? "input keyevent KEYCODE_WAKEUP" : "input keyevent KEYCODE_SLEEP");
        } else {
            snprintf(err, errcap, "the phone is not connected");
            return -1;
        }
    } else if (!strcmp(cmd, "phone.reboot")) {
        if (!online) {
            snprintf(err, errcap, "the phone is not connected");
            return -1;
        }
        const char *mode = msg_str(args, "mode", "normal");
        char c[64];
        snprintf(c, sizeof c, "reboot %s", !strcmp(mode, "recovery") || !strcmp(mode, "bootloader") ? mode : "");
        shell_quiet(c);
    } else if (!strcmp(cmd, "adb.reconnect")) {
        transport_reconnect(A.tm);
    } else if (!strcmp(cmd, "agent.reload")) {
        if (agent_reload_apply() < 0) {
            snprintf(err, errcap, "agent.ini has an error; kept the old settings");
            return -1;
        }
    } else if (!strcmp(cmd, "agent.config") || !strcmp(cmd, "quality.default")) {
        return apply_config(args, err, errcap);
    } else {
        return -1;
    }
    return 0;
}

void phone_init(void)
{
    ev_timer_init(&P.retry, on_retry, 0., 0.);
    ev_timer_init(&P.ping, on_ping, 5., 5.);
    const char *link = agent_conf("phone", "link", "adb");
    if (!strncmp(link, "tcp:", 4)) {
        snprintf(P.tcp_link, sizeof P.tcp_link, "%s", link + 4);
        LOGI("phone link: TCP %s (development mode, no ADB supervision)", P.tcp_link);
        ev_timer_set(&P.retry, 0.1, 0.);
        ev_timer_start(A.loop, &P.retry);
    }
}
