#include "sdp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "plat.h"
#include "rd.h"

const char *codec_name(int c) { return c == CODEC_H265 ? "h265" : c ? "h264" : "none"; }

static void fb_lines(rd_buf_t *b, int pt)
{
    rd_buf_printf(b,
                  "a=rtcp-fb:%d goog-remb\r\na=rtcp-fb:%d transport-cc\r\na=rtcp-fb:%d ccm fir\r\n"
                  "a=rtcp-fb:%d nack\r\na=rtcp-fb:%d nack pli\r\n",
                  pt, pt, pt, pt, pt);
}

/* We offer a=setup:active, so the phone is always the DTLS client. Browsers
 * send a fragmented ClientHello (large post-quantum key shares) that mbedTLS
 * cannot reassemble as a DTLS server; as a client we only receive the
 * browser's ServerHello flight, which mbedTLS reassembles fine. */
char *sdp_build_offer(const sdp_offer_t *o)
{
    rd_buf_t b;
    rd_buf_init(&b);
    rd_buf_printf(&b,
                  "v=0\r\no=- %llu 2 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\n"
                  "a=group:BUNDLE 0 1%s\r\na=extmap-allow-mixed\r\na=msid-semantic: WMS remodroid\r\n",
                  (unsigned long long)o->session_id, o->offer_audio ? " 2" : "");
    const char *pts = o->offer_h265 ? "96 97 98 99 100 101" : "98 99 100 101";
    rd_buf_printf(&b,
                  "m=video 9 UDP/TLS/RTP/SAVPF %s\r\nc=IN IP4 0.0.0.0\r\na=rtcp:9 IN IP4 0.0.0.0\r\n"
                  "a=ice-ufrag:%s\r\na=ice-pwd:%s\r\na=ice-options:trickle\r\n"
                  "a=fingerprint:sha-256 %s\r\na=setup:active\r\na=mid:0\r\n"
                  "a=extmap:%d http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time\r\n"
                  "a=extmap:%d http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01\r\n"
                  "a=extmap:%d http://www.webrtc.org/experiments/rtp-hdrext/playout-delay\r\n"
                  "a=sendonly\r\na=msid:remodroid video0\r\na=rtcp-mux\r\na=rtcp-rsize\r\n",
                  pts, o->ufrag, o->pwd, o->fingerprint, EXT_ABS_SEND_TIME, EXT_TWCC, EXT_PLAYOUT_DELAY);
    if (o->offer_h265) {
        rd_buf_printf(&b, "a=rtpmap:%d H265/90000\r\n", PT_H265);
        fb_lines(&b, PT_H265);
        rd_buf_printf(&b, "a=fmtp:%d level-id=123;profile-id=1;tier-flag=0;tx-mode=SRST\r\n", PT_H265);
        rd_buf_printf(&b, "a=rtpmap:%d rtx/90000\r\na=fmtp:%d apt=%d\r\n", PT_H265_RTX, PT_H265_RTX, PT_H265);
    }
    rd_buf_printf(&b, "a=rtpmap:%d H264/90000\r\n", PT_H264H);
    fb_lines(&b, PT_H264H);
    rd_buf_printf(&b, "a=fmtp:%d level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=640c2a\r\n", PT_H264H);
    rd_buf_printf(&b, "a=rtpmap:%d rtx/90000\r\na=fmtp:%d apt=%d\r\n", PT_H264H_RTX, PT_H264H_RTX, PT_H264H);
    rd_buf_printf(&b, "a=rtpmap:%d H264/90000\r\n", PT_H264B);
    fb_lines(&b, PT_H264B);
    rd_buf_printf(&b, "a=fmtp:%d level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e02a\r\n", PT_H264B);
    rd_buf_printf(&b, "a=rtpmap:%d rtx/90000\r\na=fmtp:%d apt=%d\r\n", PT_H264B_RTX, PT_H264B_RTX, PT_H264B);
    rd_buf_printf(&b,
                  "a=ssrc-group:FID %u %u\r\na=ssrc:%u cname:remodroid\r\na=ssrc:%u msid:remodroid video0\r\n"
                  "a=ssrc:%u cname:remodroid\r\na=ssrc:%u msid:remodroid video0\r\n",
                  o->ssrc, o->rtx_ssrc, o->ssrc, o->ssrc, o->rtx_ssrc, o->rtx_ssrc);
    rd_buf_printf(&b,
                  "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\nc=IN IP4 0.0.0.0\r\n"
                  "a=ice-ufrag:%s\r\na=ice-pwd:%s\r\na=ice-options:trickle\r\n"
                  "a=fingerprint:sha-256 %s\r\na=setup:active\r\na=mid:1\r\n"
                  "a=sctp-port:5000\r\na=max-message-size:262144\r\n",
                  o->ufrag, o->pwd, o->fingerprint);
    /* The phone's sound (AudioCapture.java): Opus, 48 kHz stereo, one 20 ms frame per packet. Same
     * stream id as the video, so the browser plays both from one MediaStream. */
    if (o->offer_audio)
        rd_buf_printf(&b,
                      "m=audio 9 UDP/TLS/RTP/SAVPF %d\r\nc=IN IP4 0.0.0.0\r\na=rtcp:9 IN IP4 0.0.0.0\r\n"
                      "a=ice-ufrag:%s\r\na=ice-pwd:%s\r\na=ice-options:trickle\r\n"
                      "a=fingerprint:sha-256 %s\r\na=setup:active\r\na=mid:2\r\n"
                      "a=sendonly\r\na=msid:remodroid audio0\r\na=rtcp-mux\r\n"
                      "a=rtpmap:%d opus/48000/2\r\na=fmtp:%d minptime=10;useinbandfec=1;stereo=1;sprop-stereo=1\r\n"
                      "a=ssrc:%u cname:remodroid\r\na=ssrc:%u msid:remodroid audio0\r\n",
                      PT_OPUS, o->ufrag, o->pwd, o->fingerprint, PT_OPUS, PT_OPUS, o->audio_ssrc, o->audio_ssrc);
    rd_buf_append(&b, "", 1);
    return (char *)b.p;
}

/* ---------------- answer ---------------- */

typedef struct {
    int pt;
    char name[16];
    int apt;
} ptinfo_t;

static int has_pt(const int *pts, int n, int pt)
{
    for (int i = 0; i < n; i++)
        if (pts[i] == pt)
            return 1;
    return 0;
}

int sdp_parse_answer(const char *sdp, sdp_answer_t *a)
{
    memset(a, 0, sizeof *a);
    a->sctp_port = 5000;
    a->max_message = 65536;
    enum { M_NONE, M_VIDEO, M_APP, M_AUDIO, M_OTHER } media = M_NONE;
    int vpts[32], nvpts = 0;
    ptinfo_t info[32];
    int ninfo = 0;
    struct { int pt; char fb[32]; } fbs[64];
    int nfb = 0;
    char *copy = rd_strdup(sdp), *save = NULL;
    for (char *l = strtok_r(copy, "\r\n", &save); l; l = strtok_r(NULL, "\r\n", &save)) {
        if (!strncmp(l, "m=", 2)) {
            char kind[16];
            int port = 0;
            sscanf(l + 2, "%15s %d", kind, &port);
            if (!strcmp(kind, "video")) {
                media = M_VIDEO;
                a->video_ok = port != 0;
                char *p = l;
                for (int f = 0; f < 3 && p; f++) /* skip "m=video PORT PROTO" */
                    p = strchr(p + 1, ' ');
                while (p && nvpts < 32) {
                    vpts[nvpts++] = atoi(p + 1);
                    p = strchr(p + 1, ' ');
                }
            } else if (!strcmp(kind, "audio")) {
                media = M_AUDIO;
                a->audio_ok = port != 0;
            } else if (!strcmp(kind, "application")) {
                media = M_APP;
                a->sctp_ok = port != 0 && strstr(l, "SCTP");
            } else {
                media = M_OTHER;
            }
            continue;
        }
        if (strncmp(l, "a=", 2))
            continue;
        const char *v = l + 2;
        if (!strncmp(v, "ice-ufrag:", 10) && !a->ufrag[0])
            snprintf(a->ufrag, sizeof a->ufrag, "%s", v + 10);
        else if (!strncmp(v, "ice-pwd:", 8) && !a->pwd[0])
            snprintf(a->pwd, sizeof a->pwd, "%s", v + 8);
        else if (!strncasecmp(v, "fingerprint:sha-256 ", 20) && !a->fingerprint[0])
            snprintf(a->fingerprint, sizeof a->fingerprint, "%s", v + 20);
        else if (!strncmp(v, "setup:", 6))
            a->remote_dtls_client = !strcmp(v + 6, "active");
        else if (!strncmp(v, "candidate:", 10) && a->ncand < 16)
            snprintf(a->cand[a->ncand++], sizeof a->cand[0], "%s", v);
        else if (media == M_APP && !strncmp(v, "sctp-port:", 10))
            a->sctp_port = atoi(v + 10);
        else if (media == M_APP && !strncmp(v, "max-message-size:", 17))
            a->max_message = (size_t)strtoul(v + 17, NULL, 10);
        else if (media == M_AUDIO && !strncmp(v, "rtpmap:", 7) && (strstr(v, " opus/48000") || strstr(v, " OPUS/48000")))
            a->audio_pt = atoi(v + 7);
        else if (media == M_AUDIO && (!strcmp(v, "inactive") || !strcmp(v, "sendonly")))
            a->audio_ok = 0;
        else if (media == M_VIDEO && !strncmp(v, "rtpmap:", 7) && ninfo < 32) {
            ptinfo_t *pi = &info[ninfo++];
            memset(pi, 0, sizeof *pi);
            sscanf(v + 7, "%d %15[^/]", &pi->pt, pi->name);
        } else if (media == M_VIDEO && !strncmp(v, "fmtp:", 5)) {
            int pt = atoi(v + 5);
            const char *apt = strstr(v, "apt=");
            for (int i = 0; apt && i < ninfo; i++)
                if (info[i].pt == pt)
                    info[i].apt = atoi(apt + 4);
        } else if (media == M_VIDEO && !strncmp(v, "rtcp-fb:", 8) && nfb < 64) {
            const char *sp = strchr(v + 8, ' ');
            if (sp) {
                fbs[nfb].pt = v[8] == '*' ? -1 : atoi(v + 8);
                snprintf(fbs[nfb].fb, sizeof fbs[nfb].fb, "%s", sp + 1);
                nfb++;
            }
        } else if (media == M_VIDEO && !strncmp(v, "extmap:", 7)) {
            int id = atoi(v + 7);
            if (strstr(v, "abs-send-time")) a->ext_abs = id;
            else if (strstr(v, "transport-wide-cc")) a->ext_twcc = id;
            else if (strstr(v, "playout-delay")) a->ext_playout = id;
        }
    }
    free(copy);
    if (!a->ufrag[0] || !a->pwd[0] || !a->fingerprint[0])
        return -1;
    /* our preference order */
    static const struct { int pt, codec; const char *name; } pref[] = {
        { PT_H265, CODEC_H265, "H265" }, { PT_H264H, CODEC_H264_HIGH, "H264" }, { PT_H264B, CODEC_H264_BASELINE, "H264" },
    };
    for (size_t i = 0; a->video_ok && !a->codec && i < RD_ARRAY_LEN(pref); i++) {
        if (!has_pt(vpts, nvpts, pref[i].pt))
            continue;
        for (int k = 0; k < ninfo; k++)
            if (info[k].pt == pref[i].pt && !strcasecmp(info[k].name, pref[i].name))
                a->codec = pref[i].codec, a->pt = pref[i].pt;
    }
    if (a->video_ok && !a->codec)
        a->video_ok = 0;
    if (!a->audio_pt)
        a->audio_ok = 0;
    for (int k = 0; a->codec && k < ninfo; k++)
        if (!strcasecmp(info[k].name, "rtx") && info[k].apt == a->pt && has_pt(vpts, nvpts, info[k].pt))
            a->rtx_pt = info[k].pt;
    for (int i = 0; i < nfb; i++) {
        if (fbs[i].pt != -1 && fbs[i].pt != a->pt)
            continue;
        if (!strcmp(fbs[i].fb, "nack")) a->nack = 1;
        else if (!strcmp(fbs[i].fb, "nack pli")) a->pli = 1;
        else if (!strcmp(fbs[i].fb, "ccm fir")) a->fir = 1;
        else if (!strcmp(fbs[i].fb, "transport-cc")) a->twcc = 1;
        else if (!strcmp(fbs[i].fb, "goog-remb")) a->remb = 1;
    }
    return 0;
}
