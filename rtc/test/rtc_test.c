/* Host stand-in for the phone server, for interop tests of the native WebRTC
 * stack: serves the tunnel, streams a pre-encoded clip once a session is
 * connected, and echoes data channel messages.
 *
 *   rtc_test --listen tcp:127.0.0.1:27183 --frames clip.frames [--direct 1] [--h265 0] [--audio 1] [--seconds 20]
 *
 * --audio 1 offers the sound track and sends an Opus silence packet every 20 ms.
 *
 * clip.frames: repeated records of u32 BE length, u8 keyframe, Annex-B data. */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "msg.h"
#include "rd.h"
#include "rtc.h"

typedef struct {
    uint8_t *data;
    size_t len;
    int key;
} frame_t;

static struct {
    struct ev_loop *loop;
    rtc_t *rtc;
    frame_t *frames;
    int nframes, next, connected, sent, keyreq, audio, audio_sent;
    int64_t pts, audio_pts;
    ev_timer frame_timer, stop_timer, audio_timer;
    int kbps;
    char codec[16];
} T;

static int load_frames(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    uint8_t h[5];
    while (fread(h, 1, 5, f) == 5) {
        size_t len = rd_get_be32(h);
        T.frames = realloc(T.frames, sizeof *T.frames * (size_t)(T.nframes + 1));
        frame_t *fr = &T.frames[T.nframes++];
        fr->data = malloc(len);
        fr->len = len;
        fr->key = h[4];
        if (fread(fr->data, 1, len, f) != len) {
            fclose(f);
            return -1;
        }
    }
    fclose(f);
    return T.nframes ? 0 : -1;
}

static void on_frame(struct ev_loop *loop, ev_timer *w, int revents)
{
    if (!T.connected)
        return;
    if (T.keyreq) { /* restart at the next keyframe in the clip */
        T.keyreq = 0;
        T.next = 0;
    }
    frame_t *f = &T.frames[T.next];
    if (rtc_video(T.rtc, f->data, f->len, T.pts, f->key) == 0)
        T.sent++;
    T.pts += 33333;
    T.next = (T.next + 1) % T.nframes;
}

static void on_audio(struct ev_loop *loop, ev_timer *w, int revents)
{
    static const uint8_t silence[] = { 0xF8, 0xFF, 0xFE }; /* Opus CELT FB 20 ms, one silent frame */
    if (T.connected && rtc_audio(T.rtc, silence, sizeof silence, T.audio_pts) == 0)
        T.audio_sent++;
    T.audio_pts += 20000;
}

static void on_ctrl(void *ud, const char *json, size_t n) { LOGI("test: ctrl from agent: %.*s", (int)n, json); }

static void on_link(void *ud, int up)
{
    if (up)
        rtc_send_ctrl(T.rtc, "{\"t\":\"ready\",\"d\":{\"proto\":1,\"version\":\"rtc-test\",\"codecs\":[\"h264\"]}}");
}

static void on_session(void *ud, const char *sid, int state, const char *codec, const char *profile, const char *quality,
                       const char *reason, int audio)
{
    LOGI("test: session %s state %d codec %s profile %s reason %s", sid, state, codec ? codec : "-",
         profile ? profile : "-", reason ? reason : "-");
    if (state == RTC_SESSION_CONNECTED) {
        snprintf(T.codec, sizeof T.codec, "%s", codec);
        T.connected = 1;
        T.next = 0;
        ev_timer_start(T.loop, &T.frame_timer);
        if (audio)
            ev_timer_start(T.loop, &T.audio_timer);
    } else if (state == RTC_SESSION_ENDED) {
        T.connected = 0;
        ev_timer_stop(T.loop, &T.frame_timer);
        ev_timer_stop(T.loop, &T.audio_timer);
    }
}

static void on_channel_open(void *ud, const char *label) { LOGI("test: channel %s open", label); }

static void on_message(void *ud, const char *label, int binary, const uint8_t *d, size_t n)
{
    char reply[512];
    if (!strcmp(label, "ctrl") && !binary) {
        int l = n && d[0] == '{' ? snprintf(reply, sizeof reply, "{\"t\":\"echo\",\"d\":%.*s}", (int)RD_MIN(n, 400), d)
                                 : snprintf(reply, sizeof reply, "echo:%.*s", (int)RD_MIN(n, 400), d);
        rtc_dc_send(T.rtc, "ctrl", 0, reply, (size_t)l);
    } else {
        int l = snprintf(reply, sizeof reply, "{\"t\":\"got\",\"d\":{\"label\":\"%s\",\"binary\":%d,\"n\":%zu}}", label,
                         binary, n);
        rtc_dc_send(T.rtc, "ctrl", 0, reply, (size_t)l);
    }
}

static void on_keyframe(void *ud)
{
    LOGI("test: keyframe requested");
    T.keyreq = 1;
}

static void on_bitrate(void *ud, int kbps)
{
    T.kbps = kbps;
    LOGD("test: target bitrate %d kbps", kbps);
}

static void print_stats(void)
{
    rtc_stats_t s;
    rtc_get_stats(T.rtc, &s);
    printf("{\"frames_sent\":%d,\"codec\":\"%s\",\"packets\":%llu,\"bytes\":%llu,\"retransmits\":%llu,"
           "\"plis\":%llu,\"target_kbps\":%d,\"acked_kbps\":%d,\"rtt_ms\":%d,\"local\":\"%s\",\"remote\":\"%s\","
           "\"cc\":\"%s\",\"audio_sent\":%d,\"audio_packets\":%llu}\n",
           T.sent, T.codec, (unsigned long long)s.packets, (unsigned long long)s.bytes,
           (unsigned long long)s.retransmits, (unsigned long long)s.plis, s.target_kbps, s.acked_kbps, s.rtt_ms,
           s.local_type, s.remote_type, s.cc_state, T.audio_sent, (unsigned long long)s.audio_packets);
    fflush(stdout);
}

static void on_stop(struct ev_loop *loop, ev_timer *w, int revents)
{
    print_stats();
    ev_break(loop, EVBREAK_ALL);
}

static void on_usr1(struct ev_loop *loop, ev_signal *w, int revents) { print_stats(); }
static void on_term(struct ev_loop *loop, ev_signal *w, int revents) { on_stop(loop, NULL, 0); }

int main(int argc, char **argv)
{
    const char *listen = "tcp:127.0.0.1:27183", *frames = NULL;
    int direct = 1, h265 = 0, seconds = 30;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--listen")) listen = argv[i + 1];
        else if (!strcmp(argv[i], "--frames")) frames = argv[i + 1];
        else if (!strcmp(argv[i], "--direct")) direct = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--h265")) h265 = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--audio")) T.audio = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--seconds")) seconds = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--log")) rd_log_level = rd_log_parse_level(argv[i + 1], RD_LOG_INFO);
    }
    rd_log_init("phone", rd_log_level);
    if (!frames || load_frames(frames) < 0) {
        fprintf(stderr, "cannot load --frames\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    T.loop = EV_DEFAULT;
    rtc_opts_t o = { .listen = listen, .direct = direct, .offer_h265 = h265, .offer_audio = T.audio, .start_kbps = 1500 };
    rtc_cb_t cb = { on_ctrl, on_link, on_session, on_channel_open, on_message, on_keyframe, on_bitrate, NULL };
    T.rtc = rtc_new(T.loop, &o, cb, NULL);
    if (!T.rtc)
        return 1;
    ev_timer_init(&T.frame_timer, on_frame, 1 / 30., 1 / 30.);
    ev_timer_init(&T.audio_timer, on_audio, 0.02, 0.02);
    ev_timer_init(&T.stop_timer, on_stop, seconds, 0.);
    ev_timer_start(T.loop, &T.stop_timer);
    ev_signal sig_usr1, sig_term;
    ev_signal_init(&sig_usr1, on_usr1, SIGUSR1);
    ev_signal_start(T.loop, &sig_usr1);
    ev_signal_init(&sig_term, on_term, SIGTERM);
    ev_signal_start(T.loop, &sig_term);
    LOGI("test: %d frames loaded, listening on %s (%s)", T.nframes, listen, direct ? "direct UDP" : "tunnel UDP");
    ev_run(T.loop, 0);
    rtc_free(T.rtc);
    return 0;
}
