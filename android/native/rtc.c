#include "rtc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "cc.h"
#include "dtls.h"
#include "ice.h"
#include "msg.h"
#include "net.h"
#include "rd.h"
#include "rtp.h"
#include "sctp.h"
#include "sdp.h"
#include "srtp.h"

typedef struct session {
    struct rtc *r;
    char sid[64];
    char *quality;
    ice_t *ice;
    dtls_t *dtls;
    sctp_t *sctp;
    srtp_t srtp_out, srtp_in;
    int srtp_ready;
    rtp_t *rtp;
    cc_t *cc;
    sdp_answer_t ans;
    int have_answer, connected, dying;
    uint32_t ssrc, rtx_ssrc;
    int64_t first_pts;
    uint32_t ts_base;
    int have_pts;
    int last_kbps;
    uint32_t audio_ssrc, audio_ts_base;
    uint16_t audio_seq;
    int64_t audio_first_pts;
    int audio_have_pts;
    uint64_t audio_packets;
    double last_kf_req;
    ev_tstamp started;
    struct session *next_dead;
} session_t;

struct rtc {
    struct ev_loop *loop;
    rtc_opts_t o;
    rtc_cb_t cb;
    void *ud;
    net_t *net;
    session_t *s;
    session_t *dead;
    ev_timer tick, reaper;
    int force_relay;
};

static void end_session(rtc_t *r, const char *reason, int notify);

int rtc_global_init(void)
{
    static int done;
    if (done)
        return 0;
    if (srtp_init() != srtp_err_status_ok || dtls_global_init() < 0)
        return -1;
    done = 1;
    return 0;
}

/* ---------------- signaling to the agent ---------------- */

static void send_msg(rtc_t *r, cJSON *m)
{
    char *s = msg_print(m);
    cJSON_Delete(m);
    net_ctrl_send(r->net, s, strlen(s));
    free(s);
}

int rtc_send_ctrl(rtc_t *r, const char *json) { return net_ctrl_send(r->net, json, strlen(json)); }

static void send_signal(session_t *s, cJSON *d)
{
    cJSON *m = msg_new("signal", 0, s->sid);
    cJSON_ReplaceItemInObjectCaseSensitive(m, "d", d);
    send_msg(s->r, m);
}

/* ---------------- media path ---------------- */

static void ice_candidate(void *ud, const char *cand)
{
    session_t *s = ud;
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "type", "candidate");
    if (cand) {
        cJSON_AddStringToObject(d, "candidate", cand);
        cJSON_AddStringToObject(d, "sdpMid", "0");
        cJSON_AddNumberToObject(d, "sdpMLineIndex", 0);
    } else {
        cJSON_AddNullToObject(d, "candidate"); /* end of candidates */
    }
    send_signal(s, d);
}

static void rtp_out(void *ud, const uint8_t *pkt, size_t n, int is_rtcp)
{
    session_t *s = ud;
    if (!s->srtp_ready || s->dying)
        return;
    uint8_t buf[1600];
    if (n + SRTP_MAX_TRAILER_LEN + 4 > sizeof buf)
        return;
    memcpy(buf, pkt, n);
    int len = (int)n;
    srtp_err_status_t e = is_rtcp ? srtp_protect_rtcp(s->srtp_out, buf, &len) : srtp_protect(s->srtp_out, buf, &len);
    if (e != srtp_err_status_ok) {
        LOGD("srtp: protect failed (%d)", e);
        return;
    }
    ice_send(s->ice, buf, (size_t)len);
}

static void keyframe_request(void *ud)
{
    session_t *s = ud;
    ev_tstamp now = ev_now(s->r->loop);
    if (now - s->last_kf_req < 0.3)
        return;
    s->last_kf_req = now;
    if (s->r->cb.on_keyframe)
        s->r->cb.on_keyframe(s->r->ud);
}

static void sctp_out(void *ud, const uint8_t *d, size_t n)
{
    session_t *s = ud;
    if (s->dtls && !s->dying)
        dtls_write(s->dtls, d, n);
}

static void sctp_open(void *ud, const char *label)
{
    session_t *s = ud;
    if (s->r->cb.on_channel_open)
        s->r->cb.on_channel_open(s->r->ud, label);
}

static void sctp_msg(void *ud, const char *label, int bin, const uint8_t *d, size_t n)
{
    session_t *s = ud;
    if (s->r->cb.on_message)
        s->r->cb.on_message(s->r->ud, label, bin, d, n);
}

static void sctp_closed(void *ud) { end_session(((session_t *)ud)->r, "datachannel_closed", 1); }

static void dtls_out(void *ud, const uint8_t *d, size_t n)
{
    session_t *s = ud;
    if (!s->dying)
        ice_send(s->ice, d, n);
}

static int make_srtp(srtp_t *out, const uint8_t *key, const uint8_t *salt, int outbound)
{
    srtp_policy_t p;
    memset(&p, 0, sizeof p);
    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&p.rtp);
    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&p.rtcp);
    p.ssrc.type = outbound ? ssrc_any_outbound : ssrc_any_inbound;
    uint8_t k[30];
    memcpy(k, key, 16);
    memcpy(k + 16, salt, 14);
    p.key = k;
    p.window_size = 1024;
    p.allow_repeat_tx = 1; /* plain NACK resends repeat sequence numbers */
    return srtp_create(out, &p) == srtp_err_status_ok ? 0 : -1;
}

static void dtls_up(void *ud, const uint8_t keys[60], int is_client)
{
    session_t *s = ud;
    rtc_t *r = s->r;
    const uint8_t *ck = keys, *sk = keys + 16, *cs = keys + 32, *ss = keys + 46;
    if (make_srtp(&s->srtp_out, is_client ? ck : sk, is_client ? cs : ss, 1) < 0 ||
        make_srtp(&s->srtp_in, is_client ? sk : ck, is_client ? ss : cs, 0) < 0) {
        end_session(r, "srtp_failed", 1);
        return;
    }
    s->srtp_ready = 1;
    if (s->ans.video_ok) {
        s->cc = cc_new(r->o.min_kbps, r->o.start_kbps, r->o.max_kbps);
        rtp_conf_t c = {
            .codec = s->ans.codec, .pt = s->ans.pt, .rtx_pt = s->ans.rtx_pt, .ssrc = s->ssrc, .rtx_ssrc = s->rtx_ssrc,
            .ext_abs = s->ans.ext_abs, .ext_twcc = s->ans.twcc ? s->ans.ext_twcc : 0, .ext_playout = s->ans.ext_playout,
            .nack = s->ans.nack,
        };
        s->rtp = rtp_new(c, s->cc, (rtp_cb_t){ rtp_out, keyframe_request }, s);
        s->last_kbps = r->o.start_kbps;
    }
    if (s->ans.sctp_ok) {
        s->sctp = sctp_new(r->loop, is_client, 5000, s->ans.sctp_port,
                           (sctp_cb_t){ sctp_out, sctp_open, sctp_msg, NULL, sctp_closed }, s);
        if (s->sctp) {
            sctp_open_channel(s->sctp, "ctrl");
            sctp_open_channel(s->sctp, "input");
            sctp_open_channel(s->sctp, "file");
        }
    }
    s->connected = 1;
    const char *profile = s->ans.codec == CODEC_H265 ? "main" : s->ans.codec == CODEC_H264_HIGH ? "high" : "baseline";
    LOGI("session %s connected: %s (%s), data channels %s, audio %s", s->sid, codec_name(s->ans.codec), profile,
         s->sctp ? "on" : "off", s->ans.audio_ok ? "on" : "off");
    if (r->cb.on_session)
        r->cb.on_session(r->ud, s->sid, RTC_SESSION_CONNECTED, codec_name(s->ans.codec), profile, s->quality, NULL,
                         s->ans.audio_ok);
    if (r->cb.on_bitrate)
        r->cb.on_bitrate(r->ud, r->o.start_kbps);
}

static void dtls_data(void *ud, const uint8_t *d, size_t n)
{
    session_t *s = ud;
    if (s->sctp)
        sctp_input(s->sctp, d, n);
}

static void dtls_closed(void *ud, int err) { end_session(((session_t *)ud)->r, err ? "dtls_failed" : "dtls_closed", 1); }

static void ice_data(void *ud, const uint8_t *d, size_t n)
{
    session_t *s = ud;
    if (!n || s->dying)
        return;
    if (d[0] >= 20 && d[0] <= 63) {
        if (s->dtls)
            dtls_input(s->dtls, d, n);
    } else if (d[0] >= 128 && d[0] <= 191 && n >= 8 && d[1] >= 192 && d[1] <= 223) {
        if (!s->srtp_ready)
            return;
        uint8_t buf[1600];
        if (n > sizeof buf)
            return;
        memcpy(buf, d, n);
        int len = (int)n;
        if (srtp_unprotect_rtcp(s->srtp_in, buf, &len) == srtp_err_status_ok && s->rtp)
            rtp_on_rtcp(s->rtp, buf, (size_t)len, ev_now(s->r->loop));
    }
}

static void ice_state_cb(void *ud, int st)
{
    session_t *s = ud;
    if (st == ICE_CONNECTED && !s->dtls && s->have_answer) {
        int is_client = !s->ans.remote_dtls_client;
        s->dtls = dtls_new(s->r->loop, is_client, s->ans.fingerprint, (dtls_cb_t){ dtls_out, dtls_up, dtls_data, dtls_closed }, s);
        if (!s->dtls) {
            end_session(s->r, "dtls_failed", 1);
            return;
        }
        if (is_client)
            dtls_start(s->dtls);
    } else if (st == ICE_FAILED) {
        end_session(s->r, "ice_failed", 1);
    }
}

/* ---------------- sessions ---------------- */

static void free_session(session_t *s)
{
    sctp_free(s->sctp);
    dtls_free(s->dtls);
    if (s->srtp_ready) {
        srtp_dealloc(s->srtp_out);
        srtp_dealloc(s->srtp_in);
    }
    rtp_free(s->rtp);
    cc_free(s->cc);
    ice_free(s->ice);
    free(s->quality);
    free(s);
}

static void on_reap(struct ev_loop *loop, ev_timer *w, int revents)
{
    rtc_t *r = w->data;
    while (r->dead) {
        session_t *s = r->dead;
        r->dead = s->next_dead;
        free_session(s);
    }
}

static void end_session(rtc_t *r, const char *reason, int notify)
{
    session_t *s = r->s;
    if (!s || s->dying)
        return;
    s->dying = 1;
    r->s = NULL;
    LOGI("session %s ended: %s", s->sid, reason);
    if (notify && net_link_up(r->net)) {
        cJSON *m = msg_new("session.ended", 0, s->sid);
        cJSON_AddStringToObject(msg_d(m), "sid", s->sid);
        cJSON_AddStringToObject(msg_d(m), "reason", reason);
        send_msg(r, m);
    }
    if (r->cb.on_session)
        r->cb.on_session(r->ud, s->sid, RTC_SESSION_ENDED, NULL, NULL, NULL, reason, 0);
    /* freed from the loop, never from inside ICE/DTLS/SCTP callbacks */
    s->next_dead = r->dead;
    r->dead = s;
    ev_timer_stop(r->loop, &r->reaper);
    ev_timer_set(&r->reaper, 0., 0.);
    ev_timer_start(r->loop, &r->reaper);
}

void rtc_end_session(rtc_t *r, const char *reason) { end_session(r, reason ? reason : "ended", 1); }

static void start_session(rtc_t *r, const char *sid, cJSON *d)
{
    if (r->s)
        end_session(r, "replaced", 0);
    session_t *s = calloc(1, sizeof *s);
    s->r = r;
    snprintf(s->sid, sizeof s->sid, "%s", sid);
    cJSON *q = cJSON_GetObjectItemCaseSensitive(d, "quality");
    s->quality = q ? cJSON_PrintUnformatted(q) : rd_strdup("{}");
    s->started = ev_now(r->loop);
    rd_random(&s->ssrc, 4);
    rd_random(&s->rtx_ssrc, 4);
    rd_random(&s->audio_ssrc, 4);
    r->s = s;
    s->ice = ice_new(r->loop, r->net, (ice_cb_t){ ice_candidate, ice_state_cb, ice_data }, s);
    int relay = r->force_relay || msg_bool(d, "force_relay", 0);
    ice_set_servers(s->ice, cJSON_GetObjectItemCaseSensitive(d, "ice_servers"), relay);
    uint64_t sess_id;
    rd_random(&sess_id, sizeof sess_id);
    sdp_offer_t o = { .ufrag = ice_ufrag(s->ice), .pwd = ice_pwd(s->ice), .fingerprint = dtls_fingerprint(),
                      .ssrc = s->ssrc, .rtx_ssrc = s->rtx_ssrc, .offer_h265 = r->o.offer_h265,
                      .offer_audio = r->o.offer_audio, .audio_ssrc = s->audio_ssrc,
                      .session_id = sess_id >> 2 };
    char *sdp = sdp_build_offer(&o);
    cJSON *sd = cJSON_CreateObject();
    cJSON_AddStringToObject(sd, "type", "offer");
    cJSON_AddStringToObject(sd, "sdp", sdp);
    free(sdp);
    send_signal(s, sd);
    ice_gather(s->ice);
    LOGI("session %s: offer sent (%s)", sid, r->o.offer_h265 ? "H.265 + H.264" : "H.264");
    if (r->cb.on_session)
        r->cb.on_session(r->ud, s->sid, RTC_SESSION_CONNECTING, NULL, NULL, s->quality, NULL, 0);
}

static void on_signal(rtc_t *r, const char *sid, cJSON *d)
{
    session_t *s = r->s;
    if (!s || !sid || strcmp(sid, s->sid))
        return;
    const char *type = msg_str(d, "type", "");
    if (!strcmp(type, "answer")) {
        if (s->have_answer)
            return;
        if (sdp_parse_answer(msg_str(d, "sdp", ""), &s->ans) < 0) {
            end_session(r, "bad_answer", 1);
            return;
        }
        s->have_answer = 1;
        LOGI("session %s: answer: %s, data channels %s, DTLS %s", s->sid, codec_name(s->ans.codec),
             s->ans.sctp_ok ? "yes" : "no", s->ans.remote_dtls_client ? "server" : "client");
        if (!s->ans.video_ok && !s->ans.sctp_ok) {
            end_session(r, "nothing_negotiated", 1);
            return;
        }
        ice_set_remote(s->ice, s->ans.ufrag, s->ans.pwd);
        for (int i = 0; i < s->ans.ncand; i++)
            ice_add_remote(s->ice, s->ans.cand[i]);
    } else if (!strcmp(type, "candidate")) {
        const char *c = msg_str(d, "candidate", NULL);
        if (c && *c)
            ice_add_remote(s->ice, c);
    }
}

/* ---------------- net callbacks ---------------- */

static void net_udp(void *ud, nsock_t *sock, const struct sockaddr_storage *from, const uint8_t *d, size_t n)
{
    rtc_t *r = ud;
    if (r->s && !r->s->dying)
        ice_on_udp(r->s->ice, sock, from, d, n);
}

static void net_bound(void *ud, nsock_t *sock, const struct sockaddr_storage *local)
{
    rtc_t *r = ud;
    if (r->s && !r->s->dying)
        ice_on_bound(r->s->ice, sock, local);
}

static void net_ctrl(void *ud, const uint8_t *json, size_t n)
{
    rtc_t *r = ud;
    cJSON *m = cJSON_ParseWithLength((const char *)json, n);
    if (!cJSON_IsObject(m)) {
        cJSON_Delete(m);
        return;
    }
    const char *t = msg_type(m);
    const char *sid = msg_sid(m) ? msg_sid(m) : msg_str(msg_d(m), "sid", NULL);
    if (!strcmp(t, "session.start") && sid)
        start_session(r, sid, msg_d(m));
    else if (!strcmp(t, "signal"))
        on_signal(r, sid, msg_d(m));
    else if (!strcmp(t, "session.end")) {
        if (r->s && sid && !strcmp(sid, r->s->sid))
            end_session(r, msg_str(msg_d(m), "reason", "viewer_left"), 0);
    } else if (r->cb.on_ctrl)
        r->cb.on_ctrl(r->ud, (const char *)json, n);
    cJSON_Delete(m);
}

static void net_link(void *ud, int up)
{
    rtc_t *r = ud;
    if (!up && r->s)
        end_session(r, "agent_disconnected", 0);
    if (r->cb.on_link)
        r->cb.on_link(r->ud, up);
}

/* ---------------- periodic ---------------- */

static void on_tick(struct ev_loop *loop, ev_timer *w, int revents)
{
    rtc_t *r = w->data;
    session_t *s = r->s;
    if (!s || s->dying)
        return;
    ev_tstamp now = ev_now(loop);
    if (!s->connected && now - s->started > 30.) {
        end_session(r, "timeout", 1);
        return;
    }
    if (s->rtp)
        rtp_tick(s->rtp, now);
    if (s->cc && r->cb.on_bitrate) {
        int k = cc_target_kbps(s->cc);
        if (abs(k - s->last_kbps) * 20 > s->last_kbps) { /* >5% change */
            s->last_kbps = k;
            r->cb.on_bitrate(r->ud, k);
        }
    }
}

/* ---------------- API ---------------- */

rtc_t *rtc_new(struct ev_loop *loop, const rtc_opts_t *o, rtc_cb_t cb, void *ud)
{
    if (rtc_global_init() < 0) {
        LOGE("rtc: crypto init failed");
        return NULL;
    }
    rtc_t *r = calloc(1, sizeof *r);
    r->loop = loop;
    r->o = *o;
    if (!r->o.min_kbps) r->o.min_kbps = 300;
    if (!r->o.start_kbps) r->o.start_kbps = 2500;
    if (!r->o.max_kbps) r->o.max_kbps = 20000;
    r->cb = cb;
    r->ud = ud;
    r->net = net_new(loop, o->listen, o->direct, (net_cb_t){ net_udp, net_bound, net_ctrl, net_link }, r);
    if (!r->net) {
        free(r);
        return NULL;
    }
    ev_timer_init(&r->tick, on_tick, 0.1, 0.1);
    r->tick.data = r;
    ev_timer_start(loop, &r->tick);
    ev_timer_init(&r->reaper, on_reap, 0., 0.);
    r->reaper.data = r;
    return r;
}

void rtc_free(rtc_t *r)
{
    if (!r)
        return;
    end_session(r, "shutdown", 1);
    on_reap(r->loop, &r->reaper, 0);
    ev_timer_stop(r->loop, &r->tick);
    ev_timer_stop(r->loop, &r->reaper);
    free(r);
}

int rtc_dc_send(rtc_t *r, const char *label, int binary, const void *d, size_t n)
{
    session_t *s = r->s;
    if (!s || s->dying || !s->sctp)
        return -1;
    return sctp_send(s->sctp, label, binary, d, n);
}

int rtc_dc_open(rtc_t *r, const char *label)
{
    session_t *s = r->s;
    return s && s->sctp ? sctp_open_channel(s->sctp, label) : -1;
}

int rtc_video(rtc_t *r, const uint8_t *d, size_t n, int64_t pts_us, int keyframe)
{
    session_t *s = r->s;
    if (!s || s->dying || !s->rtp)
        return -1;
    if (!s->have_pts) {
        if (!keyframe)
            return -1; /* start at a keyframe */
        s->have_pts = 1;
        s->first_pts = pts_us;
        rd_random(&s->ts_base, 4);
    }
    if (!keyframe && net_backlog(r->net) > 1024 * 1024) {
        keyframe_request(s); /* the link is behind: skip until a keyframe */
        return -1;
    }
    uint32_t ts = s->ts_base + (uint32_t)((pts_us - s->first_pts) * 90 / 1000);
    return rtp_send_frame(s->rtp, d, n, ts, keyframe, ev_now(r->loop));
}

/* Opus needs no packetizer: one encoder packet is one RTP payload (RFC 7587). No RTCP SR for it,
 * so the browser plays it as it arrives instead of holding it back for lip sync. */
int rtc_audio(rtc_t *r, const uint8_t *d, size_t n, int64_t pts_us)
{
    session_t *s = r->s;
    if (!s || s->dying || !s->srtp_ready || !s->ans.audio_ok || !n || n > 1200)
        return -1;
    int first = !s->audio_have_pts;
    if (first) {
        s->audio_have_pts = 1;
        s->audio_first_pts = pts_us;
        rd_random(&s->audio_ts_base, 4);
        rd_random(&s->audio_seq, 2);
    }
    uint32_t ts = s->audio_ts_base + (uint32_t)((pts_us - s->audio_first_pts) * 48 / 1000);
    uint8_t pkt[1212];
    pkt[0] = 0x80;
    pkt[1] = (uint8_t)((first ? 0x80 : 0) | (s->ans.audio_pt & 0x7f));
    rd_put_be16(pkt + 2, s->audio_seq++);
    rd_put_be32(pkt + 4, ts);
    rd_put_be32(pkt + 8, s->audio_ssrc);
    memcpy(pkt + 12, d, n);
    rtp_out(s, pkt, n + 12, 0);
    s->audio_packets++;
    return 0;
}

void rtc_get_stats(rtc_t *r, rtc_stats_t *st)
{
    memset(st, 0, sizeof *st);
    session_t *s = r->s;
    st->local_type = st->remote_type = "none";
    st->cc_state = "none";
    if (!s || s->dying)
        return;
    st->connected = s->connected;
    st->audio_packets = s->audio_packets;
    int rtt = 0;
    ice_selected(s->ice, &st->local_type, &st->remote_type, &rtt);
    st->rtt_ms = rtt;
    if (s->rtp) {
        rtp_stats_t rs;
        rtp_get_stats(s->rtp, &rs);
        st->packets = rs.packets;
        st->bytes = rs.bytes;
        st->retransmits = rs.retransmits;
        st->nacks = rs.nacks;
        st->plis = rs.plis + rs.firs;
        st->loss = rs.loss;
        if (rs.rtt_ms)
            st->rtt_ms = rs.rtt_ms;
    }
    if (s->cc) {
        st->target_kbps = cc_target_kbps(s->cc);
        st->acked_kbps = cc_acked_kbps(s->cc);
        st->cc_state = cc_state(s->cc);
    }
}

void rtc_set_max_kbps(rtc_t *r, int max_kbps)
{
    r->o.max_kbps = max_kbps;
    if (r->s && r->s->cc)
        cc_set_limits(r->s->cc, r->o.min_kbps, max_kbps);
}

void rtc_set_direct(rtc_t *r, int direct) { net_set_direct(r->net, direct); }
