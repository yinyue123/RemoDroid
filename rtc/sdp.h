/* SDP offer (built by the phone) and answer (from Safari) — only the subset
 * WebRTC needs (plan §5.3, §5.5). */
#ifndef RTC_SDP_H
#define RTC_SDP_H

#include <stddef.h>
#include <stdint.h>

enum { CODEC_NONE, CODEC_H265, CODEC_H264_HIGH, CODEC_H264_BASELINE };

/* payload types in our offer */
enum { PT_H265 = 96, PT_H265_RTX = 97, PT_H264H = 98, PT_H264H_RTX = 99, PT_H264B = 100, PT_H264B_RTX = 101, PT_OPUS = 111 };
/* header extension ids in our offer */
enum { EXT_ABS_SEND_TIME = 1, EXT_TWCC = 2, EXT_PLAYOUT_DELAY = 3 };

typedef struct {
    const char *ufrag, *pwd, *fingerprint;
    uint32_t ssrc, rtx_ssrc;
    int offer_h265;
    int offer_audio;   /* add an Opus m=audio section (mid 2) */
    uint32_t audio_ssrc;
    uint64_t session_id;
} sdp_offer_t;

typedef struct {
    char ufrag[64], pwd[128], fingerprint[128];
    int remote_dtls_client; /* answer a=setup:active */
    int video_ok, codec, pt, rtx_pt;
    int nack, pli, fir, twcc, remb;
    int ext_abs, ext_twcc, ext_playout;
    int sctp_ok, sctp_port;
    int audio_ok, audio_pt;   /* the viewer accepted our Opus track */
    size_t max_message;
    char cand[16][256];
    int ncand;
} sdp_answer_t;

char *sdp_build_offer(const sdp_offer_t *o); /* malloc'd */
int   sdp_parse_answer(const char *sdp, sdp_answer_t *a);
const char *codec_name(int codec);           /* "h265" / "h264" */

#endif
