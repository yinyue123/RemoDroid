/* The video pipeline (own thread): the platform's hardware path (capture and
 * encode on the GPU) when it has one, else capture + yuv.c + swenc.c. Encoded
 * access units go straight to the WebRTC host (rd_rtc_video, thread-safe).
 * Keyframes mostly on request (PLI); on a still screen the software path sends
 * nothing new except a refresh every 500 ms. */
#include <stdlib.h>
#include <string.h>

#include "desk.h"
#include "rd_rtc.h"

static struct {
    rd_mutex_t mu;
    int inited;
    rd_thread_t th;
    int running;            /* thread alive */
    volatile int quit;
    volatile int want_key;
    volatile int kbps;      /* latest target */
    video_req_t req;
    dsk_screen_t screen;
    /* reported */
    int w, h;
    double fps;
    const char *encoder;
} V;

static int64_t now_us(void) { return (int64_t)rd_mono_ms() * 1000; }

static void count_frame(void)
{
    static unsigned frames;
    static int64_t t0;
    int64_t t = now_us();
    if (!t0)
        t0 = t;
    frames++;
    if (t - t0 >= 1000000) {
        rd_mutex_lock(&V.mu);
        V.fps = frames * 1e6 / (double)(t - t0);
        rd_mutex_unlock(&V.mu);
        frames = 0;
        t0 = t;
    }
}

static void push_au(void *ud, const uint8_t *au, size_t n, int64_t pts_us, int key)
{
    rd_rtc_video(NULL, 0, au, n, pts_us, key);
    count_frame();
}

static void set_info(int w, int h, const char *enc)
{
    rd_mutex_lock(&V.mu);
    V.w = w;
    V.h = h;
    V.encoder = enc;
    rd_mutex_unlock(&V.mu);
}

/* ---------------- hardware ---------------- */

static int run_hw(void)
{
    dsk_video_cfg_t c = { .screen = V.screen, .codec = V.req.codec, .high = V.req.high, .fps = V.req.fps, .kbps = V.kbps };
    video_fit(V.screen.pw, V.screen.ph, V.req.short_side, 0, &c.width, &c.height);
    char err[200] = "";
    dsk_hwvideo_t *hv = dsk_hwvideo_open(&c, push_au, NULL, err, sizeof err);
    if (!hv) {
        LOGI("video: no hardware encoder (%s); using software", err[0] ? err : "unsupported");
        return -1;
    }
    LOGI("video: %s, %s %dx%d @%d fps, %d kbps", dsk_hwvideo_name(hv), c.codec == VC_H265 ? "H.265" : "H.264", c.width,
         c.height, c.fps, c.kbps);
    set_info(c.width, c.height, dsk_hwvideo_name(hv));
    int kbps = V.kbps, fails = 0;
    dsk_hwvideo_keyframe(hv);
    while (!V.quit) {
        if (V.want_key) {
            V.want_key = 0;
            dsk_hwvideo_keyframe(hv);
        }
        if (V.kbps != kbps) {
            kbps = V.kbps;
            dsk_hwvideo_bitrate(hv, kbps);
        }
        if (dsk_hwvideo_step(hv, 100) < 0) {
            if (++fails >= 3) {
                LOGW("video: the hardware path keeps failing; switching to software");
                dsk_hwvideo_close(hv);
                return -1;
            }
            dsk_hwvideo_close(hv);
            rd_sleep_ms(200);
            hv = dsk_hwvideo_open(&c, push_au, NULL, err, sizeof err);
            if (!hv)
                return -1;
            dsk_hwvideo_keyframe(hv);
        }
    }
    dsk_hwvideo_close(hv);
    return 0;
}

/* ---------------- software ---------------- */

static void run_sw(void)
{
    int w, h;
    /* minih264 needs multiples of 16: the picture is scaled to fill them (< 1.5% stretch) */
    int ss = V.req.short_side;
    if (ss <= 0 || ss > 1080)
        ss = 1080; /* software encoding above 1080p costs more than it shows */
    video_fit(V.screen.pw, V.screen.ph, ss, 1, &w, &h);
    int fps = RD_MIN(V.req.fps > 0 ? V.req.fps : 30, 30);
    char err[200] = "";
    dsk_capture_t *cap = NULL;
    swenc_t *enc = NULL;
    uint8_t *yuv = malloc((size_t)w * h * 3 / 2);
    int kbps = V.kbps;
    int64_t last_sent = 0, frame_us = 1000000 / fps, refine_gap = 0;
    int have_frame = 0, fails = 0, refine = 0;
    set_info(w, h, "software");
    LOGI("video: software H.264 %dx%d @%d fps, %d kbps", w, h, fps, kbps);
    while (!V.quit) {
        if (!cap) {
            cap = dsk_capture_open(&V.screen, err, sizeof err);
            if (!cap) {
                if (fails++ % 20 == 0)
                    LOGW("video: capture: %s", err);
                rd_sleep_ms(250);
                continue;
            }
            fails = 0;
            have_frame = 0;
        }
        if (!enc) {
            enc = swenc_new(w, h, fps, kbps);
            if (!enc) {
                LOGE("video: cannot start the software encoder at %dx%d", w, h);
                break;
            }
            V.want_key = 1;
        }
        int64_t t = now_us();
        /* until the next frame is due; once it is, wait for the screen to change */
        int wait = (int)((last_sent + frame_us - t) / 1000);
        dsk_frame_t f;
        int r = dsk_capture_next(cap, &f, wait > 0 ? wait : (int)(frame_us / 1000));
        if (r < 0) {
            dsk_capture_close(cap);
            cap = NULL;
            continue;
        }
        t = now_us();
        if (r > 0) {
            yuv_from_bgra(f.bgra, f.w, f.h, f.stride, yuv, yuv + (size_t)w * h, yuv + (size_t)w * h * 5 / 4, w, h);
            have_frame = 1;
        }
        if (!have_frame)
            continue;
        int key = V.want_key, qp = 0;
        if (r > 0)
            refine = 36;
        /* encode when the screen changed (at most fps), on request, and as a 500 ms refresh.
         * Moving pictures get what the bitrate allows, which blurs scrolled text; once the
         * screen is still, a few fixed-quality frames sharpen it (QP 36 → 24), no faster than
         * the bitrate carries them. */
        if (!key && r == 0 && refine && t - last_sent >= refine_gap) {
            qp = refine;
            refine = refine > 24 ? refine - 4 : 0;
        } else if (!key && (r == 0 ? t - last_sent < 500000 : t - last_sent < frame_us))
            continue;
        if (V.kbps != kbps) {
            kbps = V.kbps;
            swenc_bitrate(enc, kbps);
        }
        V.want_key = 0;
        size_t n;
        const uint8_t *au = swenc_encode(enc, yuv, yuv + (size_t)w * h, yuv + (size_t)w * h * 5 / 4, key, qp, &n);
        if (au && n)
            rd_rtc_video(NULL, 0, au, n, t, key);
        last_sent = t;
        refine_gap = RD_MAX(100000, (int64_t)n * 8000 / RD_MAX(kbps, 100));
        count_frame();
    }
    if (cap)
        dsk_capture_close(cap);
    swenc_free(enc);
    free(yuv);
}

static void *thread_main(void *arg)
{
    if (V.req.codec == VC_H265) {
        /* the viewer expects H.265, which only the hardware makes: keep trying it */
        while (!V.quit && run_hw() < 0)
            for (int i = 0; i < 10 && !V.quit; i++)
                rd_sleep_ms(100);
        return NULL;
    }
    if (V.req.software || run_hw() < 0) {
        if (!V.quit)
            run_sw();
    }
    return NULL;
}

/* ---------------- API (agent thread) ---------------- */

int video_start(const video_req_t *r)
{
    if (!V.inited) {
        rd_mutex_init(&V.mu);
        V.inited = 1;
    }
    video_stop();
    if (D.screen < 0 || D.screen >= D.nscreens)
        return -1;
    V.req = *r;
    V.screen = D.screens[D.screen];
    V.kbps = r->kbps;
    V.quit = 0;
    V.want_key = 1;
    V.fps = 0;
    if (rd_thread_start(&V.th, thread_main, NULL) < 0)
        return -1;
    V.running = 1;
    return 0;
}

void video_stop(void)
{
    if (!V.running)
        return;
    V.quit = 1;
    rd_thread_join(V.th);
    V.running = 0;
    set_info(0, 0, NULL);
}

void video_keyframe(void) { V.want_key = 1; }

void video_bitrate(int kbps)
{
    if (kbps > 0)
        V.kbps = kbps;
}

void video_info(int *w, int *h, double *fps, const char **encoder)
{
    if (!V.inited) {
        *w = *h = 0;
        *fps = 0;
        *encoder = NULL;
        return;
    }
    rd_mutex_lock(&V.mu);
    *w = V.w;
    *h = V.h;
    *fps = V.fps;
    *encoder = V.encoder;
    rd_mutex_unlock(&V.mu);
}
