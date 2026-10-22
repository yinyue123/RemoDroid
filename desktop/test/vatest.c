/* The VA-API encoder (desktop/linux/video_vaapi.c) on this machine's GPU, without a display: a moving
 * test picture, an IDR on request half way, the stream written as Annex-B for a decoder to check.
 *
 *   vatest [h264|h264b|h265] [frames] [WxH] [out] e.g. vatest h265 300 1280x800 /tmp/out.h265 (h264b: Baseline)
 *   ffmpeg -v error -i /tmp/out.h265 -f null -    a clean decode prints nothing
 *   vatest screen [h264|h265] [seconds] [out]      the agent's own path (dsk_hwvideo_*): this X display,
 *                                                  captured, converted and encoded at 60 fps
 *
 * The screen mode is built with -DVATEST_SCREEN and the capture and colour code (capture_x11.c, yuv.c).
 * make desktop-vatest builds it (build/host/desktop/vatest, with the screen mode). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "desk.h"
#include "vaenc.h"
#ifdef VATEST_SCREEN
#include <X11/Xlib.h>
#endif

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* what video_vaapi.c needs from the rest of the agent */
void rd_log(int level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
uint64_t rd_mono_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
void rd_sleep_ms(int ms)
{
    struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000 };
    nanosleep(&t, NULL);
}
#ifndef VATEST_SCREEN
dsk_capture_t *dsk_capture_open(const dsk_screen_t *s, char *err, size_t cap) { return NULL; }
int dsk_capture_next(dsk_capture_t *c, dsk_frame_t *f, int timeout_ms) { return -1; }
void dsk_capture_close(dsk_capture_t *c) {}
void yuv_from_bgra(const uint8_t *bgra, int sw, int sh, int sstride, uint8_t *y, uint8_t *u, uint8_t *v, int dw, int dh) {}
void yuv_nv12_uv(const uint8_t *u, const uint8_t *v, uint8_t *uv, int w, int h) {}
#else
Display *x11_open(void) { return XOpenDisplay(NULL); } /* sys.c's */
static FILE *screen_out;
static int screen_aus, screen_keys;
static void on_au(void *ud, const uint8_t *au, size_t n, int64_t pts_us, int key)
{
    fwrite(au, 1, n, screen_out);
    screen_aus++;
    screen_keys += key;
}

/* the whole X screen (DISPLAY) for some seconds through dsk_hwvideo, as video.c drives it */
static int screen_mode(int hevc, int seconds, const char *path)
{
    Display *d = XOpenDisplay(NULL);
    if (!d) {
        printf("no X display\n");
        return 1;
    }
    dsk_video_cfg_t c = { .codec = hevc ? VC_H265 : VC_H264, .high = 1, .fps = 60, .kbps = 8000 };
    c.screen.pw = c.screen.w = c.width = DisplayWidth(d, DefaultScreen(d));
    c.screen.ph = c.screen.h = c.height = DisplayHeight(d, DefaultScreen(d));
    XCloseDisplay(d);
    char err[256] = "";
    screen_out = fopen(path, "wb");
    dsk_hwvideo_t *v = dsk_hwvideo_open(&c, on_au, NULL, err, sizeof err);
    if (!v) {
        printf("open: %s\n", err);
        return 1;
    }
    printf("encoder: %s, screen %dx%d\n", dsk_hwvideo_name(v), c.width, c.height);
    double t0 = now_ms();
    int steps = 0;
    while (now_ms() - t0 < seconds * 1000.0) {
        if (steps++ % 120 == 0)
            dsk_hwvideo_keyframe(v);
        if (dsk_hwvideo_step(v, 100) < 0) {
            printf("step failed\n");
            return 1;
        }
    }
    dsk_hwvideo_close(v);
    fclose(screen_out);
    printf("%d frames (%d key) in %d s: %.1f fps -> %s\n", screen_aus, screen_keys, seconds, screen_aus / (double)seconds, path);
    return 0;
}
#endif


/* a gradient with a bright square sweeping across it */
static void picture(uint8_t *nv12, int w, int h, int i)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            nv12[(size_t)y * w + x] = (uint8_t)((x + y + i * 4) & 255);
    int sx = (i * 9) % (w - 96), sy = h / 3;
    for (int y = sy; y < sy + 96; y++)
        memset(nv12 + (size_t)y * w + sx, 235, 96);
    for (int y = 0; y < h / 2; y++)
        for (int x = 0; x < w; x += 2) {
            nv12[(size_t)w * h + (size_t)y * w + x] = (uint8_t)(128 + 60 * (x > w / 2));
            nv12[(size_t)w * h + (size_t)y * w + x + 1] = (uint8_t)(128 + 60 * (y > h / 4));
        }
}

/* the NAL unit types in an access unit, as "SPS PPS IDR" */
static void nals(const uint8_t *p, size_t n, int hevc, char *out, size_t cap)
{
    out[0] = 0;
    for (size_t i = 0; i + 3 < n; i++) {
        if (p[i] || p[i + 1] || p[i + 2] != 1)
            continue;
        int t = hevc ? (p[i + 3] >> 1) & 63 : p[i + 3] & 31;
        const char *name = hevc ? (t == 32 ? "VPS" : t == 33 ? "SPS" : t == 34 ? "PPS" : t == 19 || t == 20 ? "IDR" : t < 10 ? "P" : "other")
                                : (t == 7 ? "SPS" : t == 8 ? "PPS" : t == 5 ? "IDR" : t == 1 ? "P" : "other");
        size_t l = strlen(out);
        snprintf(out + l, cap - l, "%s%s", l ? " " : "", name);
        i += 3;
    }
}

int main(int argc, char **argv)
{
#ifdef VATEST_SCREEN
    if (argc > 1 && !strcmp(argv[1], "screen"))
        return screen_mode(argc > 2 && !strcmp(argv[2], "h265"), argc > 3 ? atoi(argv[3]) : 5, argc > 4 ? argv[4] : "screen.out");
#endif
    int hevc = argc > 1 && !strcmp(argv[1], "h265"), base = argc > 1 && !strcmp(argv[1], "h264b"), frames = argc > 2 ? atoi(argv[2]) : 120, w = 1280, h = 800;
    if (argc > 3)
        sscanf(argv[3], "%dx%d", &w, &h);
    const char *path = argc > 4 ? argv[4] : hevc ? "out.h265" : "out.h264";
    printf("caps: H.264 %s, H.265 %s\n", vaenc_caps() & 1 ? "yes" : "no", vaenc_caps() & 2 ? "yes" : "no");
    char err[256] = "";
    vaenc_t *e = vaenc_open(hevc ? VC_H265 : VC_H264, !base, w, h, 60, 8000, err, sizeof err);
    if (!e) {
        printf("open: %s\n", err);
        return 1;
    }
    printf("encoder: %s\n", vaenc_name(e));
    FILE *f = fopen(path, "wb");
    uint8_t *pic = malloc((size_t)w * h * 3 / 2);
    double worst = 0, sum = 0;
    size_t bytes = 0;
    for (int i = 0; i < frames; i++) {
        picture(pic, w, h, i);
        int key = i == frames / 2;
        double t = now_ms();
        size_t n;
        const uint8_t *au = vaenc_frame(e, pic, key, &n);
        double dt = now_ms() - t;
        if (!au) {
            printf("frame %d: failed\n", i);
            return 1;
        }
        if (i)
            sum += dt, worst = dt > worst ? dt : worst;
        if (i == 0 || i == 1 || key) {
            char types[128];
            nals(au, n, hevc, types, sizeof types);
            printf("frame %d: %zu bytes, %.1f ms: %s\n", i, n, dt, types);
        }
        fwrite(au, 1, n, f);
        bytes += n;
    }
    fclose(f);
    printf("%d frames, %.2f ms average, %.2f ms worst, %.0f kbps at 60 fps -> %s\n", frames, sum / (frames - 1), worst,
           bytes * 8.0 * 60 / frames / 1000, path);
    vaenc_close(e);
    return 0;
}
