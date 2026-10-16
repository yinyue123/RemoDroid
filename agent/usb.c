/* ADB transports: USB through Linux usbfs (no libusb), with netlink hotplug,
 * and plain TCP (adbd in "adb tcpip" mode, or for development). */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <linux/usbdevice_fs.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "agent.h"

#define SYSFS_USB "/sys/bus/usb/devices"
#define MAX_PAYLOAD (256 * 1024)

typedef struct usbt usbt_t;

typedef struct urbx {
    usbt_t *t;
    int is_in;
    uint8_t *buf;
    struct urbx *prev, *next;
    struct usbdevfs_urb urb; /* last: ends in a flexible array (unused for bulk) */
} urbx_t;

struct usbt {
    transport_mgr_t *m;
    int fd, ifnum;
    unsigned char ep_in, ep_out;
    unsigned maxpkt;
    ev_io io;
    char name[64];
    urbx_t *in;          /* the single outstanding IN transfer */
    uint8_t hdr[24];
    uint8_t *payload;
    uint32_t want, got;  /* payload bytes wanted / received so far */
    int phase;           /* 0 = header, 1 = payload */
    urbx_t *outs;        /* outstanding OUT transfers */
};

typedef struct tcpt {
    transport_mgr_t *m;
    int fd, connected;
    ev_io rio, wio;
    rd_buf_t in, out;
} tcpt_t;

struct transport_mgr {
    struct ev_loop *loop;
    adb_t *adb;
    char serial_filter[64];
    char tcp_target[128];
    int nl_fd;
    ev_io nl_io;
    ev_timer rescan;
    usbt_t *usb;
    tcpt_t *tcp;
    int tcp_retry;
    char problem[200]; /* why a phone that is plugged in can't be used, for the panel */
};

/* The process holding a USB device node open, e.g. "adb (pid 1234)". */
static void find_holder(const char *node, char *out, size_t cap)
{
    out[0] = 0;
    DIR *proc = opendir("/proc");
    if (!proc)
        return;
    struct dirent *p;
    while (!out[0] && (p = readdir(proc))) {
        if (p->d_name[0] < '1' || p->d_name[0] > '9' || atoi(p->d_name) == getpid())
            continue;
        char dir[64];
        snprintf(dir, sizeof dir, "/proc/%s/fd", p->d_name);
        DIR *fds = opendir(dir);
        if (!fds)
            continue;
        struct dirent *f;
        while ((f = readdir(fds))) {
            char link[400], target[256];
            snprintf(link, sizeof link, "%s/%s", dir, f->d_name);
            ssize_t n = readlink(link, target, sizeof target - 1);
            if (n <= 0)
                continue;
            target[n] = 0;
            if (strcmp(target, node))
                continue;
            char comm_path[64], comm[64] = "?";
            snprintf(comm_path, sizeof comm_path, "/proc/%s/comm", p->d_name);
            FILE *c = fopen(comm_path, "r");
            if (c) {
                if (fgets(comm, sizeof comm, c))
                    comm[strcspn(comm, "\n")] = 0;
                fclose(c);
            }
            snprintf(out, cap, "%s (pid %s)", comm, p->d_name);
            break;
        }
        closedir(fds);
    }
    closedir(proc);
}

static void set_problem(transport_mgr_t *m, const char *why)
{
    if (!strcmp(m->problem, why))
        return;
    snprintf(m->problem, sizeof m->problem, "%s", why);
    if (*why) {
        cJSON *info = cJSON_CreateObject();
        cJSON_AddStringToObject(info, "problem", why);
        agent_event("phone.busy", info);
    }
    agent_status_changed();
}

const char *transport_problem(const transport_mgr_t *m) { return m && m->problem[0] ? m->problem : NULL; }

/* ---------------- sysfs helpers ---------------- */

static int read_sysfs(const char *dir, const char *file, char *out, size_t cap)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s/%s", SYSFS_USB, dir, file);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, out, cap - 1);
    close(fd);
    if (n < 0)
        return -1;
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == ' '))
        n--;
    out[n] = 0;
    return 0;
}

typedef struct {
    char dev[64], serial[64], speed[16];
    int bus, devnum, ifnum;
    unsigned char ep_in, ep_out;
    unsigned maxpkt;
} usb_found_t;

static int find_endpoints(const char *ifname, usb_found_t *f)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", SYSFS_USB, ifname);
    DIR *d = opendir(p);
    if (!d)
        return -1;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "ep_", 3))
            continue;
        char sub[192], v[32];
        snprintf(sub, sizeof sub, "%s/%s", ifname, e->d_name);
        if (read_sysfs(sub, "type", v, sizeof v) < 0 || strcmp(v, "Bulk"))
            continue;
        unsigned addr = (unsigned)strtoul(e->d_name + 3, NULL, 16);
        if (addr & 0x80) {
            f->ep_in = (unsigned char)addr;
        } else {
            f->ep_out = (unsigned char)addr;
            if (read_sysfs(sub, "wMaxPacketSize", v, sizeof v) == 0)
                f->maxpkt = (unsigned)strtoul(v, NULL, 16);
        }
    }
    closedir(d);
    return f->ep_in && f->ep_out ? 0 : -1;
}

/* Finds the first USB interface with class ff / subclass 42 / protocol 01 (ADB). */
static int scan_usb(const char *serial_filter, usb_found_t *out)
{
    DIR *d = opendir(SYSFS_USB);
    if (!d)
        return -1;
    struct dirent *e;
    int found = -1;
    while (found < 0 && (e = readdir(d))) {
        const char *n = e->d_name;
        if (!strchr(n, ':'))
            continue;
        char cls[8], sub[8], proto[8], num[8];
        if (read_sysfs(n, "bInterfaceClass", cls, sizeof cls) || strcmp(cls, "ff") ||
            read_sysfs(n, "bInterfaceSubClass", sub, sizeof sub) || strcmp(sub, "42") ||
            read_sysfs(n, "bInterfaceProtocol", proto, sizeof proto) || strcmp(proto, "01") ||
            read_sysfs(n, "bInterfaceNumber", num, sizeof num))
            continue;
        usb_found_t f;
        memset(&f, 0, sizeof f);
        size_t dl = (size_t)(strchr(n, ':') - n);
        if (dl >= sizeof f.dev)
            continue;
        memcpy(f.dev, n, dl);
        f.ifnum = (int)strtol(num, NULL, 16);
        char v[32];
        if (read_sysfs(f.dev, "busnum", v, sizeof v))
            continue;
        f.bus = atoi(v);
        if (read_sysfs(f.dev, "devnum", v, sizeof v))
            continue;
        f.devnum = atoi(v);
        read_sysfs(f.dev, "serial", f.serial, sizeof f.serial);
        if (read_sysfs(f.dev, "speed", v, sizeof v) == 0) {
            int sp = atoi(v);
            snprintf(f.speed, sizeof f.speed, "%s",
                     sp >= 5000 ? "super" : sp >= 480 ? "high" : sp >= 12 ? "full" : "low");
        }
        if (serial_filter && *serial_filter && strcmp(serial_filter, f.serial))
            continue;
        if (find_endpoints(n, &f) < 0)
            continue;
        if (!f.maxpkt)
            f.maxpkt = 512;
        *out = f;
        found = 0;
    }
    closedir(d);
    return found;
}

/* ---------------- USB transport ---------------- */

static void usb_close(void *tp);

static int submit(usbt_t *t, urbx_t *u)
{
    if (ioctl(t->fd, USBDEVFS_SUBMITURB, &u->urb) < 0) {
        LOGW("usb: submit failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static int submit_in(usbt_t *t, uint8_t *buf, uint32_t len)
{
    urbx_t *u = t->in;
    memset(&u->urb, 0, sizeof u->urb);
    u->urb.type = USBDEVFS_URB_TYPE_BULK;
    u->urb.endpoint = t->ep_in;
    u->urb.buffer = buf;
    u->urb.buffer_length = (int)len;
    u->urb.usercontext = u;
    return submit(t, u);
}

static int usb_send(void *tp, const uint8_t hdr[24], const uint8_t *payload, size_t n)
{
    usbt_t *t = tp;
    for (int part = 0; part < 2; part++) {
        const uint8_t *src = part ? payload : hdr;
        size_t len = part ? n : 24;
        if (!len)
            continue;
        urbx_t *u = calloc(1, sizeof *u + len);
        u->t = t;
        u->buf = (uint8_t *)(u + 1);
        memcpy(u->buf, src, len);
        u->urb.type = USBDEVFS_URB_TYPE_BULK;
        u->urb.endpoint = t->ep_out;
        u->urb.buffer = u->buf;
        u->urb.buffer_length = (int)len;
        u->urb.usercontext = u;
        if (len % t->maxpkt == 0)
            u->urb.flags = USBDEVFS_URB_ZERO_PACKET;
        if (submit(t, u) < 0) {
            free(u);
            return -1;
        }
        u->next = t->outs;
        if (t->outs)
            t->outs->prev = u;
        t->outs = u;
    }
    return 0;
}

static void usb_in_done(usbt_t *t, struct usbdevfs_urb *urb)
{
    if (t->phase == 0) {
        if (urb->actual_length == 0) { /* stray zero-length packet */
            submit_in(t, t->hdr, 24);
            return;
        }
        if (urb->actual_length != 24) {
            LOGW("usb: short ADB header (%d bytes)", urb->actual_length);
            submit_in(t, t->hdr, 24);
            return;
        }
        t->want = rd_get_le32(t->hdr + 12);
        if (t->want > MAX_PAYLOAD) {
            LOGW("usb: payload too large (%u)", t->want);
            usb_close(t);
            return;
        }
        if (!t->want) {
            adb_on_packet(t->m->adb, t->hdr, NULL, 0);
            if (t->m->usb == t)
                submit_in(t, t->hdr, 24);
            return;
        }
        t->phase = 1;
        t->got = 0;
        submit_in(t, t->payload, t->want);
        return;
    }
    t->got += (uint32_t)urb->actual_length;
    if (t->got < t->want) {
        submit_in(t, t->payload + t->got, t->want - t->got);
        return;
    }
    t->phase = 0;
    adb_on_packet(t->m->adb, t->hdr, t->payload, t->want);
    if (t->m->usb == t)
        submit_in(t, t->hdr, 24);
}

static void usb_on_io(struct ev_loop *loop, ev_io *w, int revents)
{
    usbt_t *t = w->data;
    for (;;) {
        struct usbdevfs_urb *urb = NULL;
        if (ioctl(t->fd, USBDEVFS_REAPURBNDELAY, &urb) < 0) {
            if (errno == EAGAIN)
                return;
            LOGI("usb: device gone (%s)", strerror(errno));
            usb_close(t);
            return;
        }
        urbx_t *u = urb->usercontext;
        if (urb->status < 0 && urb->status != -EREMOTEIO) {
            LOGI("usb: transfer failed (%s)", strerror(-urb->status));
            if (!u->is_in) {
                if (u->prev) u->prev->next = u->next; else t->outs = u->next;
                if (u->next) u->next->prev = u->prev;
                free(u);
            }
            usb_close(t);
            return;
        }
        if (u->is_in) {
            usb_in_done(t, urb);
            if (t->m->usb != t)
                return; /* closed from a callback */
        } else {
            if (u->prev) u->prev->next = u->next; else t->outs = u->next;
            if (u->next) u->next->prev = u->prev;
            free(u);
        }
    }
}

static void usb_close(void *tp)
{
    usbt_t *t = tp;
    transport_mgr_t *m = t->m;
    if (m->usb != t)
        return;
    m->usb = NULL;
    ev_io_stop(m->loop, &t->io);
    ioctl(t->fd, USBDEVFS_RELEASEINTERFACE, &t->ifnum);
    close(t->fd); /* kills outstanding URBs */
    for (urbx_t *u = t->outs, *n; u; u = n) {
        n = u->next;
        free(u);
    }
    free(t->in);
    free(t->payload);
    free(t);
    adb_on_transport_closed(m->adb);
    agent_event("phone.detached", NULL);
    ev_timer_again(m->loop, &m->rescan);
}

static void try_usb(transport_mgr_t *m)
{
    if (m->usb || m->tcp_target[0])
        return;
    usb_found_t f;
    if (scan_usb(m->serial_filter, &f) < 0) {
        set_problem(m, ""); /* no phone at all: nothing to explain */
        return;
    }
    char node[64];
    snprintf(node, sizeof node, "/dev/bus/usb/%03d/%03d", f.bus, f.devnum);
    int fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        char why[200];
        snprintf(why, sizeof why, "cannot open the phone's USB device %s: %s", node, strerror(errno));
        if (strcmp(why, m->problem))
            LOGW("usb: %s", why);
        set_problem(m, why);
        return;
    }
    int ifnum = f.ifnum;
    if (ioctl(fd, USBDEVFS_CLAIMINTERFACE, &ifnum) < 0) {
        int e = errno;
        char holder[100], why[200];
        find_holder(node, holder, sizeof holder);
        if (e == EBUSY) /* only one program can drive the ADB interface; usually a running `adb` server */
            snprintf(why, sizeof why, "the phone is held by another program%s%s; stop it (adb kill-server) and use remodroid-agent shell/devices instead",
                     holder[0] ? ": " : "", holder);
        else
            snprintf(why, sizeof why, "cannot use the phone's USB interface: %s", strerror(e));
        if (strcmp(why, m->problem))
            LOGW("usb: %s", why);
        set_problem(m, why);
        close(fd);
        return;
    }
    set_problem(m, "");
    usbt_t *t = calloc(1, sizeof *t);
    t->m = m;
    t->fd = fd;
    t->ifnum = ifnum;
    t->ep_in = f.ep_in;
    t->ep_out = f.ep_out;
    t->maxpkt = f.maxpkt;
    snprintf(t->name, sizeof t->name, "%s", f.dev);
    t->payload = malloc(MAX_PAYLOAD);
    t->in = calloc(1, sizeof *t->in);
    t->in->t = t;
    t->in->is_in = 1;
    m->usb = t;
    ev_io_init(&t->io, usb_on_io, fd, EV_WRITE);
    t->io.data = t;
    ev_io_start(m->loop, &t->io);
    LOGI("usb: phone %s on %s (%s speed, interface %d, ep %02x/%02x)", f.serial[0] ? f.serial : "?", node, f.speed,
         ifnum, f.ep_in, f.ep_out);
    cJSON *info = cJSON_CreateObject();
    cJSON_AddStringToObject(info, "serial", f.serial);
    agent_event("phone.attached", info);
    adb_transport_t tr = { .send = usb_send, .close = usb_close, .t = t, .kind = "usb" };
    snprintf(tr.speed, sizeof tr.speed, "%s", f.speed);
    if (submit_in(t, t->hdr, 24) < 0) {
        usb_close(t);
        return;
    }
    adb_attach(m->adb, tr, f.serial);
}

/* ---------------- netlink hotplug ---------------- */

static void on_netlink(struct ev_loop *loop, ev_io *w, int revents)
{
    transport_mgr_t *m = w->data;
    char buf[4096];
    for (;;) {
        ssize_t n = recv(m->nl_fd, buf, sizeof buf - 1, 0);
        if (n <= 0)
            return;
        buf[n] = 0;
        int usbdev = 0;
        for (char *p = buf; p < buf + n; p += strlen(p) + 1)
            if (!strcmp(p, "DEVTYPE=usb_device") || !strcmp(p, "DEVTYPE=usb_interface"))
                usbdev = 1;
        if (usbdev) {
            LOGD("usb: hotplug event %s", buf);
            /* Give the kernel a moment to create /dev/bus/usb nodes. */
            ev_timer_stop(loop, &m->rescan);
            ev_timer_set(&m->rescan, 0.5, 30.);
            ev_timer_start(loop, &m->rescan);
        }
    }
}

static void on_rescan(struct ev_loop *loop, ev_timer *w, int revents)
{
    transport_mgr_t *m = w->data;
    try_usb(m);
    /* With netlink events this is only a slow safety net; a busy phone frees up without one. */
    w->repeat = m->nl_fd >= 0 && !m->problem[0] ? 30. : 3.;
    ev_timer_again(loop, w);
}

/* ---------------- TCP transport ---------------- */

static void tcp_close(void *tp);

static void tcp_flush(tcpt_t *t)
{
    while (rd_buf_avail(&t->out)) {
        ssize_t w = write(t->fd, rd_buf_data(&t->out), rd_buf_avail(&t->out));
        if (w < 0) {
            if (errno == EAGAIN || errno == EINTR)
                break;
            tcp_close(t);
            return;
        }
        rd_buf_consume(&t->out, (size_t)w);
    }
    if (rd_buf_avail(&t->out))
        ev_io_start(t->m->loop, &t->wio);
    else
        ev_io_stop(t->m->loop, &t->wio);
}

static int tcp_send(void *tp, const uint8_t hdr[24], const uint8_t *payload, size_t n)
{
    tcpt_t *t = tp;
    rd_buf_append(&t->out, hdr, 24);
    rd_buf_append(&t->out, payload, n);
    if (t->connected)
        tcp_flush(t);
    return 0;
}

static void tcp_close(void *tp)
{
    tcpt_t *t = tp;
    transport_mgr_t *m = t->m;
    if (m->tcp != t)
        return;
    m->tcp = NULL;
    ev_io_stop(m->loop, &t->rio);
    ev_io_stop(m->loop, &t->wio);
    close(t->fd);
    rd_buf_free(&t->in);
    rd_buf_free(&t->out);
    int was = t->connected;
    free(t);
    if (was) {
        adb_on_transport_closed(m->adb);
        agent_event("phone.detached", NULL);
    }
    ev_timer_stop(m->loop, &m->rescan);
    ev_timer_set(&m->rescan, 3., 3.);
    ev_timer_start(m->loop, &m->rescan);
}

static void tcp_on_read(struct ev_loop *loop, ev_io *w, int revents)
{
    tcpt_t *t = w->data;
    transport_mgr_t *m = t->m;
    rd_buf_reserve(&t->in, 65536);
    ssize_t n = read(t->fd, t->in.p + t->in.len, t->in.cap - t->in.len);
    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            return;
        tcp_close(t);
        return;
    }
    t->in.len += (size_t)n;
    while (m->tcp == t && rd_buf_avail(&t->in) >= 24) {
        const uint8_t *p = rd_buf_data(&t->in);
        uint32_t len = rd_get_le32(p + 12);
        if (len > MAX_PAYLOAD) {
            tcp_close(t);
            return;
        }
        if (rd_buf_avail(&t->in) < 24 + (size_t)len)
            break;
        uint8_t h[24];
        memcpy(h, p, 24);
        adb_on_packet(m->adb, h, p + 24, len);
        if (m->tcp != t)
            return;
        rd_buf_consume(&t->in, 24 + (size_t)len);
    }
}

static void tcp_on_write(struct ev_loop *loop, ev_io *w, int revents)
{
    tcpt_t *t = w->data;
    if (!t->connected) {
        int err = 0;
        socklen_t el = sizeof err;
        getsockopt(t->fd, SOL_SOCKET, SO_ERROR, &err, &el);
        if (err) {
            LOGD("adb tcp: connect to %s: %s", t->m->tcp_target, strerror(err));
            tcp_close(t);
            return;
        }
        t->connected = 1;
        LOGI("adb tcp: connected to %s", t->m->tcp_target);
        ev_io_start(loop, &t->rio);
        agent_event("phone.attached", NULL);
        adb_transport_t tr = { .send = tcp_send, .close = tcp_close, .t = t, .kind = "tcp" };
        snprintf(tr.speed, sizeof tr.speed, "network");
        adb_attach(t->m->adb, tr, t->m->tcp_target); /* sends CNXN */
        return;
    }
    tcp_flush(t);
}

static void try_tcp(transport_mgr_t *m)
{
    if (m->tcp || !m->tcp_target[0])
        return;
    char host[128], port[16];
    if (rd_split_hostport(m->tcp_target, host, sizeof host, port, sizeof port, "5555") < 0)
        return;
    struct addrinfo hints = { .ai_socktype = SOCK_STREAM }, *ai;
    if (getaddrinfo(host, port, &hints, &ai))
        return;
    int fd = socket(ai->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0 || (connect(fd, ai->ai_addr, ai->ai_addrlen) < 0 && errno != EINPROGRESS)) {
        if (fd >= 0)
            close(fd);
        freeaddrinfo(ai);
        return;
    }
    freeaddrinfo(ai);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, 1 /* TCP_NODELAY */, &one, sizeof one);
    tcpt_t *t = calloc(1, sizeof *t);
    t->m = m;
    t->fd = fd;
    ev_io_init(&t->rio, tcp_on_read, fd, EV_READ);
    ev_io_init(&t->wio, tcp_on_write, fd, EV_WRITE);
    t->rio.data = t->wio.data = t;
    m->tcp = t;
    ev_io_start(m->loop, &t->wio);
}

static void on_tcp_retry(struct ev_loop *loop, ev_timer *w, int revents) { try_tcp(w->data); }

/* ---------------- manager ---------------- */

transport_mgr_t *transport_start(struct ev_loop *loop, adb_t *adb, const char *serial_filter, const char *tcp_target)
{
    transport_mgr_t *m = calloc(1, sizeof *m);
    m->loop = loop;
    m->adb = adb;
    m->nl_fd = -1;
    snprintf(m->serial_filter, sizeof m->serial_filter, "%s", serial_filter ? serial_filter : "");
    snprintf(m->tcp_target, sizeof m->tcp_target, "%s", tcp_target ? tcp_target : "");
    if (m->tcp_target[0]) {
        ev_timer_init(&m->rescan, on_tcp_retry, 0., 3.);
        m->rescan.data = m;
        ev_timer_start(loop, &m->rescan);
        try_tcp(m);
        return m;
    }
    m->nl_fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
    struct sockaddr_nl sa = { .nl_family = AF_NETLINK, .nl_groups = 1 };
    if (m->nl_fd >= 0 && bind(m->nl_fd, (struct sockaddr *)&sa, sizeof sa) == 0) {
        ev_io_init(&m->nl_io, on_netlink, m->nl_fd, EV_READ);
        m->nl_io.data = m;
        ev_io_start(loop, &m->nl_io);
    } else {
        LOGW("usb: no netlink hotplug events (%s); rescanning every 3 s", strerror(errno));
        if (m->nl_fd >= 0)
            close(m->nl_fd);
        m->nl_fd = -1;
    }
    ev_timer_init(&m->rescan, on_rescan, 0., 30.);
    m->rescan.data = m;
    ev_timer_start(loop, &m->rescan);
    return m;
}

void transport_reconnect(transport_mgr_t *m)
{
    if (m->usb)
        usb_close(m->usb);
    else if (m->tcp)
        tcp_close(m->tcp);
    else if (m->tcp_target[0])
        try_tcp(m);
    else
        try_usb(m);
}
