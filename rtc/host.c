/* Runs the WebRTC stack on its own thread (rd_rtc.h). Calls from other threads
 * are queued and the loop is woken with ev_async; a stats snapshot is kept
 * for readers on other threads. */
#include "rd_rtc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rd.h"

enum { C_CTRL, C_CTRL_IN, C_DC, C_VIDEO, C_END, C_MAXKBPS, C_DIRECT, C_DC_OPEN, C_AUDIO, C_CALL, C_STOP };

typedef struct cmd {
    int type;
    char *str, *label;
    uint8_t *data;
    size_t len;
    int64_t pts;
    int flag, num;
    void (*fn)(void *);
    void *arg;
    struct cmd *next;
} cmd_t;

static struct {
    rd_rtc_cb_t cb;
    void *ud;
    struct ev_loop *loop;
    rtc_t *rtc;
    rd_thread_t thread;
    rd_mutex_t mu;
    rd_cond_t started_cv;
    int inited, started, ok, running;
    ev_async wake;
    ev_timer stats_timer;
    cmd_t *head, *tail;
    size_t queued_video;
    char stats[512];
    size_t dc_buffered;
    rtc_opts_t opts;
    char listen[128];
} H;

/* ---------------- loop thread ---------------- */

static void run_cmd(cmd_t *c)
{
    switch (c->type) {
    case C_CTRL: rtc_send_ctrl(H.rtc, c->str); break;
    case C_CTRL_IN: rtc_ctrl_in(H.rtc, c->str, strlen(c->str)); break;
    case C_DC: rtc_dc_send(H.rtc, c->label, c->flag, c->data, c->len); break;
    case C_DC_OPEN: rtc_dc_open(H.rtc, c->label); break;
    case C_VIDEO: rtc_video(H.rtc, c->data, c->len, c->pts, c->flag); break;
    case C_AUDIO: rtc_audio(H.rtc, c->data, c->len, c->pts); break;
    case C_END: rtc_end_session(H.rtc, c->str); break;
    case C_MAXKBPS: rtc_set_max_kbps(H.rtc, c->num); break;
    case C_DIRECT: rtc_set_direct(H.rtc, c->flag); break;
    case C_CALL: c->fn(c->arg); break;
    case C_STOP: ev_break(H.loop, EVBREAK_ALL); break;
    }
}

static void on_wake(struct ev_loop *loop, ev_async *w, int revents)
{
    rd_mutex_lock(&H.mu);
    cmd_t *c = H.head;
    H.head = H.tail = NULL;
    H.queued_video = 0;
    rd_mutex_unlock(&H.mu);
    while (c) {
        cmd_t *n = c->next;
        run_cmd(c);
        free(c->str);
        free(c->label);
        free(c->data);
        free(c);
        c = n;
    }
}

static void on_stats(struct ev_loop *loop, ev_timer *w, int revents)
{
    rtc_stats_t s;
    rtc_get_stats(H.rtc, &s);
    char buf[512];
    snprintf(buf, sizeof buf,
             "{\"connected\":%s,\"target_kbps\":%d,\"acked_kbps\":%d,\"rtt_ms\":%d,\"loss_pct\":%.1f,"
             "\"packets\":%llu,\"bytes\":%llu,\"retransmits\":%llu,\"nacks\":%llu,\"plis\":%llu,"
             "\"local\":\"%s\",\"remote\":\"%s\",\"cc\":\"%s\",\"audio_packets\":%llu}",
             s.connected ? "true" : "false", s.target_kbps, s.acked_kbps, s.rtt_ms, s.loss * 100,
             (unsigned long long)s.packets, (unsigned long long)s.bytes, (unsigned long long)s.retransmits,
             (unsigned long long)s.nacks, (unsigned long long)s.plis, s.local_type, s.remote_type, s.cc_state,
             (unsigned long long)s.audio_packets);
    size_t buffered = rtc_dc_buffered(H.rtc);
    rd_mutex_lock(&H.mu);
    memcpy(H.stats, buf, sizeof buf);
    H.dc_buffered = buffered;
    rd_mutex_unlock(&H.mu);
}

static void *thread_main(void *arg)
{
    if (H.cb.on_thread)
        H.cb.on_thread(H.ud, 1);
    H.loop = ev_loop_new(EVFLAG_AUTO);
    H.rtc = H.loop ? rtc_new(H.loop, &H.opts, H.cb.rtc, H.ud) : NULL;
    if (H.rtc) {
        ev_async_init(&H.wake, on_wake);
        ev_async_start(H.loop, &H.wake);
        ev_timer_init(&H.stats_timer, on_stats, 0.5, 0.5);
        ev_timer_start(H.loop, &H.stats_timer);
        if (H.cb.on_loop)
            H.cb.on_loop(H.ud, H.loop);
    }
    rd_mutex_lock(&H.mu);
    H.ok = H.rtc != NULL;
    H.started = 1;
    rd_cond_signal(&H.started_cv);
    rd_mutex_unlock(&H.mu);
    if (H.rtc) {
        ev_run(H.loop, 0);
        rtc_free(H.rtc);
        H.rtc = NULL;
        on_wake(H.loop, &H.wake, 0); /* free what is still queued */
    }
    if (H.loop)
        ev_loop_destroy(H.loop);
    H.loop = NULL;
    if (H.cb.on_thread)
        H.cb.on_thread(H.ud, 0);
    return NULL;
}

static void enqueue(cmd_t *c)
{
    rd_mutex_lock(&H.mu);
    if (!H.running || !H.loop) {
        rd_mutex_unlock(&H.mu);
        free(c->str);
        free(c->label);
        free(c->data);
        free(c);
        return;
    }
    if (c->type == C_VIDEO) {
        /* If the loop falls behind, drop delta frames rather than queue latency. */
        if (H.queued_video > 4 * 1024 * 1024 && !c->flag) {
            rd_mutex_unlock(&H.mu);
            free(c->data);
            free(c);
            return;
        }
        H.queued_video += c->len;
    }
    if (H.tail)
        H.tail->next = c;
    else
        H.head = c;
    H.tail = c;
    ev_async_send(H.loop, &H.wake);
    rd_mutex_unlock(&H.mu);
}

static cmd_t *cmd_new(int type)
{
    cmd_t *c = calloc(1, sizeof *c);
    c->type = type;
    return c;
}

/* ---------------- API ---------------- */

int rd_rtc_start(const rd_rtc_opts_t *o, const rd_rtc_cb_t *cb, void *ud)
{
    if (!H.inited) {
        rd_mutex_init(&H.mu);
        rd_cond_init(&H.started_cv);
        H.inited = 1;
    }
    if (H.running)
        return H.ok ? 0 : -1;
    rd_log_init(o->log_name ? o->log_name : "rtc", o->log_level);
    snprintf(H.listen, sizeof H.listen, "%s", o->listen ? o->listen : "embed");
    H.opts = (rtc_opts_t){ .listen = H.listen, .direct = o->direct, .offer_h265 = o->offer_h265,
                           .offer_audio = o->offer_audio, .max_kbps = o->max_kbps };
    H.cb = *cb;
    H.ud = ud;
    H.started = H.ok = 0;
    H.stats[0] = 0;
    if (rd_thread_start(&H.thread, thread_main, NULL) < 0)
        return -1;
    rd_mutex_lock(&H.mu);
    while (!H.started)
        rd_cond_wait(&H.started_cv, &H.mu);
    H.running = H.ok;
    rd_mutex_unlock(&H.mu);
    if (!H.ok) {
        rd_thread_join(H.thread);
        return -1;
    }
    return 0;
}

int rd_rtc_running(void) { return H.running; }

void rd_rtc_stop(void)
{
    if (!H.running)
        return;
    enqueue(cmd_new(C_STOP));
    rd_mutex_lock(&H.mu);
    H.running = 0; /* no more commands */
    rd_mutex_unlock(&H.mu);
    rd_thread_join(H.thread);
}

static void enqueue_str(int type, const char *s)
{
    cmd_t *c = cmd_new(type);
    c->str = rd_strdup(s ? s : "");
    enqueue(c);
}

void rd_rtc_send_ctrl(const char *json) { enqueue_str(C_CTRL, json); }
void rd_rtc_ctrl_in(const char *json) { enqueue_str(C_CTRL_IN, json); }
void rd_rtc_end_session(const char *reason) { enqueue_str(C_END, reason ? reason : "ended"); }

void rd_rtc_dc_send(const char *label, int binary, const void *d, size_t n)
{
    cmd_t *c = cmd_new(C_DC);
    c->label = rd_strdup(label);
    c->flag = binary;
    c->len = n;
    c->data = malloc(n ? n : 1);
    if (n)
        memcpy(c->data, d, n);
    enqueue(c);
}

void rd_rtc_dc_open(const char *label)
{
    cmd_t *c = cmd_new(C_DC_OPEN);
    c->label = rd_strdup(label);
    enqueue(c);
}

void rd_rtc_video(const void *prefix, size_t pl, const void *au, size_t n, int64_t pts_us, int key)
{
    if (!n)
        return;
    cmd_t *c = cmd_new(C_VIDEO);
    c->len = (prefix ? pl : 0) + n;
    c->data = malloc(c->len);
    if (prefix && pl)
        memcpy(c->data, prefix, pl);
    memcpy(c->data + (prefix ? pl : 0), au, n);
    c->pts = pts_us;
    c->flag = key;
    enqueue(c);
}

void rd_rtc_audio(const void *opus, size_t n, int64_t pts_us)
{
    if (!n)
        return;
    cmd_t *c = cmd_new(C_AUDIO);
    c->len = n;
    c->data = malloc(n);
    memcpy(c->data, opus, n);
    c->pts = pts_us;
    enqueue(c);
}

void rd_rtc_set_max_kbps(int kbps)
{
    cmd_t *c = cmd_new(C_MAXKBPS);
    c->num = kbps;
    enqueue(c);
}

void rd_rtc_set_direct(int direct)
{
    cmd_t *c = cmd_new(C_DIRECT);
    c->flag = direct;
    enqueue(c);
}

void rd_rtc_call(void (*fn)(void *), void *arg)
{
    cmd_t *c = cmd_new(C_CALL);
    c->fn = fn;
    c->arg = arg;
    enqueue(c);
}

size_t rd_rtc_stats(char *buf, size_t cap)
{
    if (!cap)
        return 0;
    if (!H.inited) {
        snprintf(buf, cap, "{}");
        return 2;
    }
    rd_mutex_lock(&H.mu);
    int n = snprintf(buf, cap, "%s", H.stats[0] ? H.stats : "{}");
    rd_mutex_unlock(&H.mu);
    return n < 0 ? 0 : RD_MIN((size_t)n, cap - 1);
}

size_t rd_rtc_dc_buffered(void)
{
    if (!H.inited)
        return 0;
    rd_mutex_lock(&H.mu);
    size_t b = H.dc_buffered;
    rd_mutex_unlock(&H.mu);
    return b;
}

rtc_t *rd_rtc_stack(void) { return H.rtc; }
