#include "cc.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "rd.h"

#define HIST 4096          /* sent packets remembered, by twcc seq */
#define WINDOW 20          /* trendline window (packet groups) */
#define BURST 0.005        /* packets sent within 5 ms form one group */

typedef struct {
    double sent, recv;
    uint32_t bytes;
    uint16_t seq;
    int valid, acked;
} pkt_t;

enum { S_INCREASE, S_HOLD, S_DECREASE };
enum { U_NORMAL, U_OVER, U_UNDER };

struct cc {
    pkt_t hist[HIST];
    double min_bps, max_bps, start_bps;
    double last_twcc, last_congestion; /* when feedback / real congestion was last seen */
    double delay_bps, loss_bps, remb_bps;
    double last_change, last_decrease_bps;
    int state;
    /* trendline */
    double grp_send, grp_recv, prev_send, prev_recv;
    int have_grp, have_prev;
    double acc_delay, smooth_delay;
    double xs[WINDOW], ys[WINDOW];
    int nxy, xy_head;
    double first_recv;
    double threshold, last_update, overuse_since;
    int usage;
    double prev_trend;
    /* acked rate */
    double ack_t[256];
    uint32_t ack_b[256];
    int ack_head, ack_n;
    double acked_bps;
};

cc_t *cc_new(int min_kbps, int start_kbps, int max_kbps)
{
    cc_t *c = calloc(1, sizeof *c);
    c->min_bps = min_kbps * 1000.;
    c->max_bps = max_kbps * 1000.;
    c->delay_bps = c->loss_bps = c->start_bps = start_kbps * 1000.;
    c->threshold = 12.5;
    c->state = S_INCREASE;
    return c;
}

void cc_free(cc_t *c) { free(c); }

void cc_set_limits(cc_t *c, int min_kbps, int max_kbps)
{
    c->min_bps = min_kbps * 1000.;
    c->max_bps = max_kbps * 1000.;
}

void cc_set_start(cc_t *c, int kbps)
{
    c->start_bps = c->delay_bps = c->loss_bps = kbps * 1000.;
    c->last_decrease_bps = c->last_congestion = 0;
}

void cc_on_sent(cc_t *c, uint16_t seq, size_t bytes, double now)
{
    pkt_t *p = &c->hist[seq % HIST];
    p->seq = seq;
    p->sent = now;
    p->bytes = (uint32_t)bytes;
    p->valid = 1;
    p->acked = 0;
}

static void acked_rate(cc_t *c, double now, uint32_t bytes)
{
    int i = (c->ack_head + c->ack_n) % 256;
    if (c->ack_n == 256) {
        c->ack_head = (c->ack_head + 1) % 256;
        i = (c->ack_head + 255) % 256;
    } else {
        c->ack_n++;
    }
    c->ack_t[i] = now;
    c->ack_b[i] = bytes;
    double total = 0, oldest = now;
    for (int k = 0; k < c->ack_n; k++) {
        int j = (c->ack_head + k) % 256;
        if (now - c->ack_t[j] <= 0.5) {
            total += c->ack_b[j];
            if (c->ack_t[j] < oldest)
                oldest = c->ack_t[j];
        }
    }
    double span = RD_MAX(now - oldest, 0.1);
    c->acked_bps = total * 8 / span;
}

/* Linear regression slope of accumulated delay over arrival time. */
static double trend_slope(const cc_t *c)
{
    if (c->nxy < 2)
        return 0;
    double mx = 0, my = 0;
    for (int i = 0; i < c->nxy; i++)
        mx += c->xs[i], my += c->ys[i];
    mx /= c->nxy;
    my /= c->nxy;
    double num = 0, den = 0;
    for (int i = 0; i < c->nxy; i++) {
        num += (c->xs[i] - mx) * (c->ys[i] - my);
        den += (c->xs[i] - mx) * (c->xs[i] - mx);
    }
    return den > 0 ? num / den : 0;
}

static void detect(cc_t *c, double now)
{
    double slope = trend_slope(c);
    double modified = RD_MIN(c->nxy, 60) * slope * 4.0; /* gain */
    double dt = c->last_update ? RD_MIN(now - c->last_update, 0.1) : 0;
    c->last_update = now;
    if (modified > c->threshold) {
        if (!c->overuse_since)
            c->overuse_since = now;
        if (now - c->overuse_since > 0.01 && slope >= c->prev_trend)
            c->usage = U_OVER;
    } else if (modified < -c->threshold) {
        c->overuse_since = 0;
        c->usage = U_UNDER;
    } else {
        c->overuse_since = 0;
        c->usage = U_NORMAL;
    }
    c->prev_trend = slope;
    /* adaptive threshold (gamma) */
    double k = fabs(modified) < c->threshold ? 0.039 : 0.0087;
    if (fabs(modified) - c->threshold < 15)
        c->threshold += k * (fabs(modified) - c->threshold) * dt * 1000;
    c->threshold = RD_MIN(RD_MAX(c->threshold, 6.0), 600.0);
}

static void rate_control(cc_t *c, double now)
{
    double acked = c->acked_bps;
    switch (c->usage) {
    case U_OVER: {
        /* What got through measures the path only while the sender fills it. An idle screen sends a
         * trickle, and a delay blip then would sink the target to it (the floor): the next scroll got
         * 0.3 Mbps and blurred. App-limited, the target steps down from itself and not below the start
         * rate, and the blip isn't congestion (which would cap the target at what's sent, below). */
        int limited = acked < 0.5 * c->delay_bps;
        if (c->state != S_DECREASE || now - c->last_change > 0.2) {
            double target = limited ? RD_MAX(0.85 * c->delay_bps, RD_MIN(c->start_bps, c->delay_bps))
                                    : 0.85 * (acked > 0 ? acked : c->delay_bps);
            if (target < c->delay_bps)
                c->delay_bps = target;
            c->last_decrease_bps = c->delay_bps;
            c->last_change = now;
        }
        if (!limited)
            c->last_congestion = now;
        c->state = S_DECREASE;
        break;
    }
    case U_UNDER:
        c->state = S_HOLD;
        break;
    default:
        if (c->state == S_HOLD || c->state == S_DECREASE) {
            c->state = S_INCREASE;
            c->last_change = now;
        }
        double dt = RD_MIN(now - c->last_change, 1.0);
        if (dt > 0) {
            /* near the last congestion point: additive; far from it: multiplicative 8%/s */
            if (c->last_decrease_bps && c->delay_bps < c->last_decrease_bps * 1.2)
                c->delay_bps += 50000.0 * dt; /* ~50 kbit/s per second */
            else
                c->delay_bps *= pow(1.08, dt);
            c->last_change = now;
        }
        /* Don't run far ahead of what is actually sent, but an idle (app-limited) screen
         * must not drag the target below the start rate unless the path was congested. */
        if (acked > 0) {
            double floor = c->last_congestion && now - c->last_congestion < 30 ? 0 : c->start_bps;
            c->delay_bps = RD_MIN(c->delay_bps, RD_MAX(1.5 * acked + 10000, floor));
        }
    }
    c->delay_bps = RD_MIN(RD_MAX(c->delay_bps, c->min_bps), c->max_bps);
}

static void on_group(cc_t *c, double send, double recv, double now)
{
    if (c->have_prev) {
        double d = (recv - c->prev_recv) - (send - c->prev_send);
        c->acc_delay += d * 1000; /* ms */
        c->smooth_delay = 0.9 * c->smooth_delay + 0.1 * c->acc_delay;
        if (!c->first_recv)
            c->first_recv = recv;
        int i;
        if (c->nxy < WINDOW) {
            i = c->nxy++;
        } else { /* overwrite the oldest sample */
            i = c->xy_head;
            c->xy_head = (c->xy_head + 1) % WINDOW;
        }
        c->xs[i] = (recv - c->first_recv) * 1000;
        c->ys[i] = c->smooth_delay;
        detect(c, now);
        rate_control(c, now);
    }
    c->prev_send = send;
    c->prev_recv = recv;
    c->have_prev = 1;
}

static void on_ack(cc_t *c, uint16_t seq, double recv, double now)
{
    pkt_t *p = &c->hist[seq % HIST];
    if (!p->valid || p->seq != seq || p->acked)
        return;
    p->acked = 1;
    p->recv = recv;
    acked_rate(c, now, p->bytes);
    if (!c->have_grp) {
        c->grp_send = p->sent;
        c->grp_recv = recv;
        c->have_grp = 1;
        return;
    }
    if (p->sent - c->grp_send < BURST) {
        if (recv > c->grp_recv)
            c->grp_recv = recv;
        return;
    }
    on_group(c, c->grp_send, c->grp_recv, now);
    c->grp_send = p->sent;
    c->grp_recv = recv;
}

void cc_on_twcc(cc_t *c, const uint8_t *f, size_t n, double now)
{
    if (n < 8)
        return;
    c->last_twcc = now;
    uint16_t base = rd_get_be16(f), count = rd_get_be16(f + 2);
    int32_t ref = (int32_t)((uint32_t)f[4] << 16 | f[5] << 8 | f[6]);
    if (ref & 0x800000)
        ref -= 0x1000000;
    double t = ref * 0.064; /* reference time, 64 ms units */
    size_t off = 8;
    uint8_t status[2048];
    int ns = 0;
    while (ns < count && off + 2 <= n) {
        uint16_t chunk = rd_get_be16(f + off);
        off += 2;
        if (!(chunk & 0x8000)) { /* run length */
            int sym = (chunk >> 13) & 3, len = chunk & 0x1fff;
            for (int i = 0; i < len && ns < count && ns < 2048; i++)
                status[ns++] = (uint8_t)sym;
        } else if (!(chunk & 0x4000)) { /* 14 one-bit symbols */
            for (int i = 0; i < 14 && ns < count && ns < 2048; i++)
                status[ns++] = (chunk >> (13 - i)) & 1;
        } else { /* 7 two-bit symbols */
            for (int i = 0; i < 7 && ns < count && ns < 2048; i++)
                status[ns++] = (chunk >> (12 - 2 * i)) & 3;
        }
    }
    for (int i = 0; i < ns; i++) {
        int sym = status[i];
        if (sym == 0)
            continue; /* not received */
        double delta;
        if (sym == 1) {
            if (off + 1 > n)
                return;
            delta = f[off] * 0.00025;
            off += 1;
        } else {
            if (off + 2 > n)
                return;
            delta = (int16_t)rd_get_be16(f + off) * 0.00025;
            off += 2;
        }
        t += delta;
        on_ack(c, (uint16_t)(base + i), t, now);
    }
}

void cc_on_loss(cc_t *c, double loss, double now)
{
    if (loss > 0.10) {
        c->loss_bps = RD_MAX(c->loss_bps * (1 - 0.5 * loss), c->min_bps);
        c->last_congestion = now;
    } else if (loss < 0.02)
        c->loss_bps = RD_MIN(c->loss_bps * 1.05 + 1000, c->max_bps);
}

/* Receiver estimates reflect an app-limited incoming rate; with transport-cc
 * feedback available the send-side estimate wins (as in libwebrtc). */
void cc_on_remb(cc_t *c, double bps, double now) { c->remb_bps = c->last_twcc && now - c->last_twcc < 2 ? 0 : bps; }

int cc_target_kbps(const cc_t *c)
{
    double t = RD_MIN(c->delay_bps, c->loss_bps);
    if (c->remb_bps > 0)
        t = RD_MIN(t, c->remb_bps);
    t = RD_MIN(RD_MAX(t, c->min_bps), c->max_bps);
    return (int)(t / 1000);
}

int cc_acked_kbps(const cc_t *c) { return (int)(c->acked_bps / 1000); }

const char *cc_state(const cc_t *c)
{
    return c->state == S_DECREASE ? "decrease" : c->state == S_HOLD ? "hold" : "increase";
}
