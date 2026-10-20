/* The computer's sound: the platform captures what it plays (desk.h dsk_audio_*), this turns it
 * into the WebRTC Opus track the phone sends too (rtc_audio): stereo, 48 kHz, one 20 ms frame per
 * packet. Downmixing and resampling are done here, so a backend hands over whatever the system
 * mixes in (float samples, any rate, any channel count). */
#include <stdlib.h>
#include <string.h>

#include "desk.h"
#include "opus.h"
#include "rd_rtc.h"

#define RATE  48000
#define FRAME (RATE / 50) /* 20 ms */

void (*audio_out)(const void *opus, size_t n, int64_t pts_us) = rd_rtc_audio;

static struct {
    rd_mutex_t mu;
    int init, running;
    OpusEncoder *enc;
    float buf[FRAME * 2];
    int have;           /* frames in buf */
    int in_rate;
    double pos;         /* resampler: where the next output frame falls, in input frames from prev */
    float prev[2];      /* the last input frame */
    int64_t t0;         /* pts of the first frame since (re)start */
    int64_t sent;       /* frames encoded since t0 */
} A;

static void lock(void)
{
    if (!A.init) {
        rd_mutex_init(&A.mu);
        A.init = 1;
    }
    rd_mutex_lock(&A.mu);
}

static int64_t now_us(void) { return (int64_t)rd_mono_ms() * 1000; }

static void flush_frame(void)
{
    int64_t now = now_us(), pts = A.t0 + A.sent * 20000;
    if (now - pts > 200000) { /* the system was silent and sent nothing: skip ahead, keep in time */
        A.t0 += now - pts - 20000;
        pts = A.t0 + A.sent * 20000;
    }
    uint8_t pkt[1200];
    int n = opus_encode_float(A.enc, A.buf, FRAME, pkt, sizeof pkt);
    A.sent++;
    A.have = 0;
    if (n > 0)
        audio_out(pkt, (size_t)n, pts);
}

static void put(float l, float r)
{
    A.buf[A.have * 2] = l;
    A.buf[A.have * 2 + 1] = r;
    if (++A.have == FRAME)
        flush_frame();
}

/* Front left/right, plus the centre and the rear speakers folded in (WAVEFORMATEXTENSIBLE / Core
 * Audio order: FL FR FC LFE BL BR …). */
static void stereo(const float *s, int ch, float *l, float *r)
{
    if (ch == 1) {
        *l = *r = s[0];
        return;
    }
    float L = s[0], R = s[1];
    if (ch >= 3) {
        L += 0.707f * s[2];
        R += 0.707f * s[2];
    }
    if (ch >= 6) {
        L += 0.707f * s[4];
        R += 0.707f * s[5];
    }
    float k = ch >= 6 ? 0.5f : ch >= 3 ? 0.6f : 1.0f;
    *l = L * k;
    *r = R * k;
}

void audio_pcm(const float *pcm, int frames, int channels, int rate)
{
    if (channels < 1 || rate < 8000)
        return;
    lock();
    if (!A.running) {
        rd_mutex_unlock(&A.mu);
        return;
    }
    if (rate != A.in_rate) {
        A.in_rate = rate;
        A.pos = 0;
    }
    if (!A.t0)
        A.t0 = now_us();
    for (int i = 0; i < frames; i++) {
        float l, r;
        stereo(pcm + (size_t)i * channels, channels, &l, &r);
        if (rate == RATE) {
            put(l, r);
        } else { /* linear interpolation between the previous input frame and this one */
            double step = (double)rate / RATE;
            while (A.pos < 1.0) {
                float t = (float)A.pos;
                put(A.prev[0] + (l - A.prev[0]) * t, A.prev[1] + (r - A.prev[1]) * t);
                A.pos += step;
            }
            A.pos -= 1.0;
        }
        A.prev[0] = l;
        A.prev[1] = r;
    }
    rd_mutex_unlock(&A.mu);
}

int audio_available(void) { return dsk_audio_supported(); }

int audio_running(void)
{
    lock();
    int r = A.running;
    rd_mutex_unlock(&A.mu);
    return r;
}

int audio_start(void)
{
    if (audio_running())
        return 0;
    int err = 0;
    OpusEncoder *e = opus_encoder_create(RATE, 2, OPUS_APPLICATION_AUDIO, &err);
    if (!e) {
        LOGW("sound: opus_encoder_create: %d", err);
        return -1;
    }
    opus_encoder_ctl(e, OPUS_SET_BITRATE((int)desk_conf_int("desktop", "audio_kbps", 128) * 1000));
    opus_encoder_ctl(e, OPUS_SET_INBAND_FEC(1)); /* the viewer asked for useinbandfec=1 */
    opus_encoder_ctl(e, OPUS_SET_PACKET_LOSS_PERC(5));
    lock();
    A.enc = e;
    A.have = 0;
    A.in_rate = 0;
    A.pos = 0;
    A.prev[0] = A.prev[1] = 0;
    A.t0 = 0;
    A.sent = 0;
    A.running = 1;
    rd_mutex_unlock(&A.mu);
    if (dsk_audio_start(audio_pcm) < 0) {
        LOGW("sound: the computer's sound can't be captured");
        audio_stop();
        return -1;
    }
    LOGI("sound: on");
    return 0;
}

void audio_stop(void)
{
    if (!audio_running())
        return;
    dsk_audio_stop(); /* no more audio_pcm calls after this */
    lock();
    A.running = 0;
    OpusEncoder *e = A.enc;
    A.enc = NULL;
    rd_mutex_unlock(&A.mu);
    opus_encoder_destroy(e);
    LOGI("sound: off");
}
