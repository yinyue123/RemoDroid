/* RemoDroid phone-side WebRTC stack (plan §5): owns the tunnel listener the
 * router agent connects to, runs one session at a time for the viewer, and
 * hands Android-specific work (encoder, input, files…) to the embedder. */
#ifndef RTC_H
#define RTC_H

#include <stddef.h>
#include <stdint.h>

#include "ev.h"

typedef struct rtc rtc_t;

enum { RTC_SESSION_CONNECTING, RTC_SESSION_CONNECTED, RTC_SESSION_ENDED };

typedef struct {
    /* "abstract:remodroid" (phone), "tcp:127.0.0.1:27183" (tests), or "embed" (desktop agents:
     * signaling goes through rtc_ctrl_in / on_ctrl_out, media over this computer's sockets) */
    const char *listen;
    int direct;           /* 1 = Path B: the phone's own UDP sockets */
    int offer_h265;       /* the phone has a usable hardware HEVC encoder */
    int offer_audio;      /* the phone can capture its sound (Opus track) */
    int min_kbps, start_kbps, max_kbps;
} rtc_opts_t;

typedef struct {
    /* CTRL messages from the agent that the stack doesn't handle itself (config, cmd, …) */
    void (*on_ctrl)(void *ud, const char *json, size_t n);
    void (*on_link)(void *ud, int up);
    /* codec: "h265"/"h264", profile: "main"/"high"/"baseline"; quality: the viewer's
     * requested settings as JSON (may be "{}"); audio: the viewer accepted the sound track */
    void (*on_session)(void *ud, const char *sid, int state, const char *codec, const char *profile,
                       const char *quality, const char *reason, int audio);
    void (*on_channel_open)(void *ud, const char *label);
    void (*on_message)(void *ud, const char *label, int binary, const uint8_t *d, size_t n);
    void (*on_keyframe)(void *ud);
    void (*on_bitrate)(void *ud, int kbps);
    /* "embed" only: messages for the server (signal, session.ended, and what rtc_send_ctrl sends) */
    void (*on_ctrl_out)(void *ud, const char *json, size_t n);
} rtc_cb_t;

typedef struct {
    int connected;
    int target_kbps, acked_kbps, rtt_ms;
    double loss;
    uint64_t packets, bytes, retransmits, nacks, plis, audio_packets;
    const char *local_type, *remote_type; /* "host"/"relay", "srflx"/"prflx"/… */
    const char *cc_state;
} rtc_stats_t;

int    rtc_global_init(void);
rtc_t *rtc_new(struct ev_loop *loop, const rtc_opts_t *o, rtc_cb_t cb, void *ud);
void   rtc_free(rtc_t *r);
/* Everything below must be called on the loop's thread (jni.c marshals). */
int    rtc_send_ctrl(rtc_t *r, const char *json);  /* to the agent: ready, status, event */
/* "embed": a message from the server (session.start, signal, session.end); others go to on_ctrl. */
void   rtc_ctrl_in(rtc_t *r, const char *json, size_t n);
/* Bytes waiting in the data channels' send buffer (approximate). */
size_t rtc_dc_buffered(rtc_t *r);
int    rtc_dc_send(rtc_t *r, const char *label, int binary, const void *d, size_t n);
int    rtc_dc_open(rtc_t *r, const char *label);
/* One encoded access unit (Annex-B, with parameter sets before keyframes). */
int    rtc_video(rtc_t *r, const uint8_t *annexb, size_t n, int64_t pts_us, int keyframe);
/* One Opus packet (20 ms) of the phone's sound. */
int    rtc_audio(rtc_t *r, const uint8_t *opus, size_t n, int64_t pts_us);
void   rtc_end_session(rtc_t *r, const char *reason);
void   rtc_get_stats(rtc_t *r, rtc_stats_t *s);
void   rtc_set_max_kbps(rtc_t *r, int max_kbps);
void   rtc_set_min_kbps(rtc_t *r, int min_kbps);   /* <= 0: the default, 300 */
/* The bandwidth estimate starts over from kbps (<= 0: the default, 2500), which is also what an idle
 * sender keeps it at. */
void   rtc_set_estimate(rtc_t *r, int kbps);
void   rtc_set_direct(rtc_t *r, int direct);

#endif
