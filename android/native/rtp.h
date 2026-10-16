/* RTP video sender: H.265 (RFC 7798) / H.264 (RFC 6184) packetization,
 * header extensions, NACK + RTX retransmission, RTCP SR and feedback. */
#ifndef RTC_RTP_H
#define RTC_RTP_H

#include <stddef.h>
#include <stdint.h>

#include "cc.h"

typedef struct rtp rtp_t;

typedef struct {
    /* plaintext packet out; the caller encrypts (SRTP/SRTCP) and sends it */
    void (*send)(void *ud, const uint8_t *pkt, size_t n, int is_rtcp);
    void (*on_keyframe_request)(void *ud);
} rtp_cb_t;

typedef struct {
    int codec;                 /* CODEC_H265 / CODEC_H264_* (sdp.h) */
    int pt, rtx_pt;            /* rtx_pt 0 = plain retransmission */
    uint32_t ssrc, rtx_ssrc;
    int ext_abs, ext_twcc, ext_playout; /* 0 = not negotiated */
    int nack;
} rtp_conf_t;

typedef struct {
    uint64_t packets, bytes, retransmits, nacks, plis, firs;
    int rtt_ms;
    double loss; /* last fraction lost reported by the viewer */
    uint32_t jitter;
} rtp_stats_t;

rtp_t *rtp_new(rtp_conf_t conf, cc_t *cc, rtp_cb_t cb, void *ud);
void   rtp_free(rtp_t *r);
/* One access unit in Annex-B format; ts90k = RTP timestamp (90 kHz). */
int    rtp_send_frame(rtp_t *r, const uint8_t *annexb, size_t n, uint32_t ts90k, int keyframe, double now);
void   rtp_on_rtcp(rtp_t *r, const uint8_t *pkt, size_t n, double now); /* decrypted compound RTCP */
void   rtp_tick(rtp_t *r, double now);                                  /* call ~every 100 ms (SR each second) */
void   rtp_get_stats(const rtp_t *r, rtp_stats_t *s);

/* Splits Annex-B into NAL units; returns the count (exposed for tests). */
int    annexb_split(const uint8_t *d, size_t n, const uint8_t **nal, size_t *len, int max);

#endif
