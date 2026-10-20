#include "rtp.h"

#include <stdlib.h>
#include <string.h>

#include "rd.h"
#include "sdp.h"

#define MAX_PAYLOAD 1100   /* leaves room for SRTP, TURN ChannelData and IPv6 */
#define HIST 1024          /* sent packets kept for NACK */
#define MAX_PKT 1300

typedef struct {
    uint16_t seq;
    int valid;
    size_t len;
    double sent;
    uint8_t data[MAX_PKT];
} hist_t;

struct rtp {
    rtp_conf_t c;
    cc_t *cc;
    rtp_cb_t cb;
    void *ud;
    uint16_t seq, rtx_seq, twcc_seq;
    uint32_t last_ts;
    double last_ts_time, last_sr;
    hist_t *hist;
    rtp_stats_t st;
    uint32_t sr_ntp_mid; /* middle 32 bits of the last SR's NTP time */
    double sr_sent_at;
};

/* ---------------- helpers ---------------- */

static uint64_t ntp_now(void)
{
    uint64_t ms = rd_now_ms();
    uint64_t sec = ms / 1000 + 2208988800ull;
    uint64_t frac = ((ms % 1000) << 32) / 1000;
    return sec << 32 | frac;
}

int annexb_split(const uint8_t *d, size_t n, const uint8_t **nal, size_t *len, int max)
{
    int count = 0;
    size_t i = 0, start = 0;
    int have = 0;
    while (i + 3 <= n) {
        if (d[i] == 0 && d[i + 1] == 0 && (d[i + 2] == 1 || (i + 4 <= n && d[i + 2] == 0 && d[i + 3] == 1))) {
            size_t sc = d[i + 2] == 1 ? 3 : 4;
            if (have && count < max) {
                size_t end = i;
                while (end > start && d[end - 1] == 0) /* trailing zero bytes */
                    end--;
                nal[count] = d + start;
                len[count++] = end - start;
            }
            i += sc;
            start = i;
            have = 1;
            continue;
        }
        i++;
    }
    if (have && start < n && count < max) {
        nal[count] = d + start;
        len[count++] = n - start;
    } else if (!have && n && count < max) { /* no start code: one raw NAL */
        nal[count] = d;
        len[count++] = n;
    }
    return count;
}

rtp_t *rtp_new(rtp_conf_t conf, cc_t *cc, rtp_cb_t cb, void *ud)
{
    rtp_t *r = calloc(1, sizeof *r);
    r->c = conf;
    r->cc = cc;
    r->cb = cb;
    r->ud = ud;
    rd_random(&r->seq, sizeof r->seq);
    rd_random(&r->rtx_seq, sizeof r->rtx_seq);
    r->hist = calloc(HIST, sizeof *r->hist);
    return r;
}

void rtp_free(rtp_t *r)
{
    if (!r)
        return;
    free(r->hist);
    free(r);
}

/* Header + one-byte extensions. Returns header length; *twcc_at = offset of the
 * transport-cc sequence (so retransmissions can rewrite it), 0 if absent. */
static size_t build_header(rtp_t *r, uint8_t *p, int pt, uint16_t seq, uint32_t ts, uint32_t ssrc, int marker,
                           int playout, size_t *twcc_at)
{
    p[0] = 0x80;
    p[1] = (uint8_t)((marker ? 0x80 : 0) | pt);
    rd_put_be16(p + 2, seq);
    rd_put_be32(p + 4, ts);
    rd_put_be32(p + 8, ssrc);
    *twcc_at = 0;
    if (!r->c.ext_abs && !r->c.ext_twcc && !(playout && r->c.ext_playout))
        return 12;
    p[0] |= 0x10;
    size_t o = 16;
    if (r->c.ext_abs) { /* placeholder; filled at send time */
        p[o++] = (uint8_t)(r->c.ext_abs << 4 | 2);
        p[o++] = 0; p[o++] = 0; p[o++] = 0;
    }
    if (r->c.ext_twcc) {
        p[o++] = (uint8_t)(r->c.ext_twcc << 4 | 1);
        *twcc_at = o;
        p[o++] = 0; p[o++] = 0;
    }
    if (playout && r->c.ext_playout) { /* min = max = 0: render as soon as possible */
        p[o++] = (uint8_t)(r->c.ext_playout << 4 | 2);
        p[o++] = 0; p[o++] = 0; p[o++] = 0;
    }
    while ((o - 16) & 3)
        p[o++] = 0;
    p[12] = 0xBE;
    p[13] = 0xDE;
    rd_put_be16(p + 14, (uint16_t)((o - 16) / 4));
    return o;
}

/* Stamps abs-send-time / transport-cc and hands the packet to the transport. */
static void transmit(rtp_t *r, uint8_t *p, size_t n, size_t twcc_at, double now)
{
    if (p[0] & 0x10) {
        size_t ext_end = 16 + (size_t)rd_get_be16(p + 14) * 4;
        for (size_t o = 16; o < ext_end;) {
            if (!p[o]) { o++; continue; }
            int id = p[o] >> 4, len = (p[o] & 15) + 1;
            if (id == r->c.ext_abs && len == 3) {
                uint32_t v = (uint32_t)((uint64_t)(now * (1 << 18)) & 0xFFFFFF);
                p[o + 1] = (uint8_t)(v >> 16); p[o + 2] = (uint8_t)(v >> 8); p[o + 3] = (uint8_t)v;
            }
            o += 1 + (size_t)len;
        }
    }
    if (twcc_at) {
        uint16_t s = r->twcc_seq++;
        rd_put_be16(p + twcc_at, s);
        if (r->cc)
            cc_on_sent(r->cc, s, n + 10, now);
    }
    r->st.packets++;
    r->st.bytes += n;
    r->cb.send(r->ud, p, n, 0);
}

static void send_packet(rtp_t *r, const uint8_t *pl1, size_t l1, const uint8_t *pl2, size_t l2, uint32_t ts, int marker,
                        int playout, double now)
{
    hist_t *h = &r->hist[r->seq % HIST];
    size_t twcc_at;
    size_t hl = build_header(r, h->data, r->c.pt, r->seq, ts, r->c.ssrc, marker, playout, &twcc_at);
    if (hl + l1 + l2 > MAX_PKT)
        return;
    memcpy(h->data + hl, pl1, l1);
    if (l2)
        memcpy(h->data + hl + l1, pl2, l2);
    h->len = hl + l1 + l2;
    h->seq = r->seq;
    h->valid = 1;
    h->sent = now;
    uint8_t out[MAX_PKT];
    memcpy(out, h->data, h->len);
    transmit(r, out, h->len, twcc_at, now);
    r->seq++;
}

/* ---------------- packetizers ---------------- */

static void send_h264_nal(rtp_t *r, const uint8_t *nal, size_t n, uint32_t ts, int last, int playout, double now)
{
    if (n <= MAX_PAYLOAD) {
        send_packet(r, nal, n, NULL, 0, ts, last, playout, now);
        return;
    }
    uint8_t ind = (nal[0] & 0xE0) | 28, type = nal[0] & 0x1F; /* FU-A */
    const uint8_t *p = nal + 1;
    size_t left = n - 1;
    int first = 1;
    while (left) {
        size_t chunk = RD_MIN(left, MAX_PAYLOAD - 2);
        uint8_t hdr[2] = { ind, (uint8_t)((first ? 0x80 : 0) | (chunk == left ? 0x40 : 0) | type) };
        send_packet(r, hdr, 2, p, chunk, ts, last && chunk == left, playout, now);
        p += chunk;
        left -= chunk;
        first = 0;
    }
}

static void send_h265_nal(rtp_t *r, const uint8_t *nal, size_t n, uint32_t ts, int last, int playout, double now)
{
    if (n < 3)
        return;
    if (n <= MAX_PAYLOAD) {
        send_packet(r, nal, n, NULL, 0, ts, last, playout, now);
        return;
    }
    int type = (nal[0] >> 1) & 0x3F; /* FU, type 49 */
    uint8_t ph0 = (uint8_t)((nal[0] & 0x81) | (49 << 1)), ph1 = nal[1];
    const uint8_t *p = nal + 2;
    size_t left = n - 2;
    int first = 1;
    while (left) {
        size_t chunk = RD_MIN(left, MAX_PAYLOAD - 3);
        uint8_t hdr[3] = { ph0, ph1, (uint8_t)((first ? 0x80 : 0) | (chunk == left ? 0x40 : 0) | type) };
        send_packet(r, hdr, 3, p, chunk, ts, last && chunk == left, playout, now);
        p += chunk;
        left -= chunk;
        first = 0;
    }
}

int rtp_send_frame(rtp_t *r, const uint8_t *d, size_t n, uint32_t ts, int keyframe, double now)
{
    const uint8_t *nal[64];
    size_t len[64];
    int count = annexb_split(d, n, nal, len, 64);
    int playout = keyframe; /* playout-delay on keyframes is enough for the receiver to latch it */
    for (int i = 0; i < count; i++) {
        if (!len[i])
            continue;
        if (r->c.codec == CODEC_H265) {
            int t = (nal[i][0] >> 1) & 0x3F;
            if (t == 35 || t == 39 || t == 40) /* AUD, SEI: not needed */
                continue;
            send_h265_nal(r, nal[i], len[i], ts, i == count - 1, playout, now);
        } else {
            int t = nal[i][0] & 0x1F;
            if (t == 9 || t == 6) /* AUD, SEI */
                continue;
            send_h264_nal(r, nal[i], len[i], ts, i == count - 1, playout, now);
        }
    }
    r->last_ts = ts;
    r->last_ts_time = now;
    return 0;
}

/* ---------------- RTCP ---------------- */

static void retransmit(rtp_t *r, uint16_t seq, double now)
{
    hist_t *h = &r->hist[seq % HIST];
    if (!h->valid || h->seq != seq || now - h->sent > 1.0)
        return;
    uint8_t out[MAX_PKT + 2];
    size_t hl = 12 + ((h->data[0] & 0x10) ? 4 + (size_t)rd_get_be16(h->data + 14) * 4 : 0);
    size_t twcc_at = 0;
    if (h->data[0] & 0x10) { /* locate the transport-cc element to rewrite */
        for (size_t o = 16; o < hl;) {
            if (!h->data[o]) { o++; continue; }
            int id = h->data[o] >> 4, l = (h->data[o] & 15) + 1;
            if (id == r->c.ext_twcc)
                twcc_at = o + 1;
            o += 1 + (size_t)l;
        }
    }
    r->st.retransmits++;
    if (!r->c.rtx_pt) { /* plain resend */
        memcpy(out, h->data, h->len);
        transmit(r, out, h->len, twcc_at, now);
        return;
    }
    if (h->len + 2 > sizeof out)
        return;
    memcpy(out, h->data, hl);
    out[1] = (uint8_t)((out[1] & 0x80) | r->c.rtx_pt);
    rd_put_be16(out + 2, r->rtx_seq++);
    rd_put_be32(out + 8, r->c.rtx_ssrc);
    rd_put_be16(out + hl, seq); /* original sequence number */
    memcpy(out + hl + 2, h->data + hl, h->len - hl);
    transmit(r, out, h->len + 2, twcc_at, now);
}

static void handle_rr_block(rtp_t *r, const uint8_t *b, double now)
{
    if (rd_get_be32(b) != r->c.ssrc)
        return;
    r->st.loss = b[4] / 256.0;
    r->st.jitter = rd_get_be32(b + 12);
    uint32_t lsr = rd_get_be32(b + 16), dlsr = rd_get_be32(b + 20);
    if (lsr) {
        uint64_t ntp = ntp_now();
        uint32_t mid = (uint32_t)(ntp >> 16);
        uint32_t rtt = mid - lsr - dlsr; /* 1/65536 s */
        if (rtt < 65536u * 5)
            r->st.rtt_ms = (int)((uint64_t)rtt * 1000 / 65536);
    }
    if (r->cc)
        cc_on_loss(r->cc, r->st.loss, now);
}

void rtp_on_rtcp(rtp_t *r, const uint8_t *p, size_t n, double now)
{
    size_t off = 0;
    while (off + 4 <= n) {
        const uint8_t *h = p + off;
        if ((h[0] >> 6) != 2)
            return;
        int count = h[0] & 0x1F, pt = h[1];
        size_t len = ((size_t)rd_get_be16(h + 2) + 1) * 4;
        if (off + len > n)
            return;
        if (pt == 201 && len >= 8) { /* RR */
            for (int i = 0; i < count && 8 + (size_t)(i + 1) * 24 <= len; i++)
                handle_rr_block(r, h + 8 + i * 24, now);
        } else if (pt == 200 && len >= 28) { /* SR (unlikely from a receive-only viewer) */
            for (int i = 0; i < count && 28 + (size_t)(i + 1) * 24 <= len; i++)
                handle_rr_block(r, h + 28 + i * 24, now);
        } else if (pt == 205 && len >= 12) { /* RTPFB */
            if (count == 1) { /* generic NACK */
                for (size_t o = 12; o + 4 <= len; o += 4) {
                    uint16_t pid = rd_get_be16(h + o), blp = rd_get_be16(h + o + 2);
                    r->st.nacks++;
                    retransmit(r, pid, now);
                    for (int b = 0; b < 16; b++)
                        if (blp & (1 << b))
                            retransmit(r, (uint16_t)(pid + b + 1), now);
                }
            } else if (count == 15 && r->cc) { /* transport-cc */
                cc_on_twcc(r->cc, h + 12, len - 12, now);
            }
        } else if (pt == 206 && len >= 12) { /* PSFB */
            if (count == 1) { /* PLI */
                r->st.plis++;
                if (r->cb.on_keyframe_request)
                    r->cb.on_keyframe_request(r->ud);
            } else if (count == 4) { /* FIR */
                r->st.firs++;
                if (r->cb.on_keyframe_request)
                    r->cb.on_keyframe_request(r->ud);
            } else if (count == 15 && len >= 20 && !memcmp(h + 12, "REMB", 4) && r->cc) {
                int exp = h[17] >> 2;
                uint32_t mant = ((uint32_t)(h[17] & 3) << 16) | (uint32_t)h[18] << 8 | h[19];
                cc_on_remb(r->cc, (double)mant * (double)(1ull << exp), now);
            }
        }
        off += len;
    }
}

void rtp_tick(rtp_t *r, double now)
{
    if (now - r->last_sr < 1.0 || !r->st.packets)
        return;
    r->last_sr = now;
    uint8_t p[64];
    uint64_t ntp = ntp_now();
    uint32_t ts = r->last_ts + (uint32_t)((now - r->last_ts_time) * 90000);
    p[0] = 0x80; /* SR, no report blocks */
    p[1] = 200;
    rd_put_be16(p + 2, 6);
    rd_put_be32(p + 4, r->c.ssrc);
    rd_put_be32(p + 8, (uint32_t)(ntp >> 32));
    rd_put_be32(p + 12, (uint32_t)ntp);
    rd_put_be32(p + 16, ts);
    rd_put_be32(p + 20, (uint32_t)r->st.packets);
    rd_put_be32(p + 24, (uint32_t)r->st.bytes);
    /* SDES with CNAME */
    static const char cname[] = "remodroid";
    size_t sl = 4 + 4 + 2 + sizeof cname - 1 + 1;
    size_t padded = (sl + 3) & ~(size_t)3;
    uint8_t *s = p + 28;
    memset(s, 0, padded);
    s[0] = 0x81;
    s[1] = 202;
    rd_put_be16(s + 2, (uint16_t)(padded / 4 - 1));
    rd_put_be32(s + 4, r->c.ssrc);
    s[8] = 1;
    s[9] = sizeof cname - 1;
    memcpy(s + 10, cname, sizeof cname - 1);
    r->sr_ntp_mid = (uint32_t)(ntp >> 16);
    r->cb.send(r->ud, p, 28 + padded, 1);
}

void rtp_get_stats(const rtp_t *r, rtp_stats_t *s) { *s = r->st; }
