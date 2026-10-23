/* The Rockchip MPP encoder (browser/agent/video_mpp.c) on this machine, without a display: synthetic
 * BGRA frames (colour bars and a moving square) in, H.265 or H.264 out, with the rate it keeps. Run on
 * the OECT host first, then in its container as the agent's user:
 *
 *   mpptest [-c h264|h265] [-w 880] [-h 1904] [-n 300] [-k 4000] [-o out.h265]
 *
 * Writes the Annex-B stream to -o for ffplay/ffprobe: the bars must be, left to right, white, yellow,
 * cyan, green, magenta, red, blue, black (a wrong channel order swaps red and blue). With MPP's own
 * headers available (-DHAVE_MPP_HEADERS, include path to its inc/) it also checks mpp_api.h's copies. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "desk.h"
#include "mppenc.h"

#ifdef HAVE_MPP_HEADERS
#include "mpp_api.h"
#include "rk_mpi.h"
#include "rk_venc_cmd.h"
_Static_assert(sizeof(RdMppApi) == sizeof(MppApi), "MppApi size");
_Static_assert(offsetof(RdMppApi, encode_put_frame) == offsetof(MppApi, encode_put_frame), "put_frame");
_Static_assert(offsetof(RdMppApi, encode_get_packet) == offsetof(MppApi, encode_get_packet), "get_packet");
_Static_assert(offsetof(RdMppApi, control) == offsetof(MppApi, control), "control");
#define SAME(name) _Static_assert(RD##name == (name), #name)
SAME(MPP_CTX_ENC);
SAME(MPP_VIDEO_CodingAVC);
SAME(MPP_VIDEO_CodingHEVC);
SAME(MPP_FMT_YUV420SP);
SAME(MPP_FMT_BGRA8888);
SAME(MPP_BUFFER_TYPE_DRM);
SAME(MPP_BUFFER_FLAGS_CACHABLE);
SAME(MPP_BUFFER_INTERNAL);
SAME(MPP_SET_OUTPUT_TIMEOUT);
SAME(MPP_ENC_SET_CFG);
SAME(MPP_ENC_GET_CFG);
SAME(MPP_ENC_SET_IDR_FRAME);
SAME(MPP_ENC_SET_SEI_CFG);
SAME(MPP_ENC_SET_HEADER_MODE);
SAME(MPP_POLL_BLOCK);
SAME(MPP_ENC_HEADER_MODE_EACH_IDR);
SAME(MPP_ENC_SEI_MODE_DISABLE);
SAME(MPP_ENC_RC_MODE_CBR);
#endif

int rd_log_level = RD_LOG_INFO;
void rd_log(int level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    int codec = VC_H265, w = 880, h = 1904, frames = 300, kbps = 4000;
    const char *out = NULL;
    for (int o; (o = getopt(argc, argv, "c:w:h:n:k:o:")) != -1;)
        switch (o) {
        case 'c': codec = strcmp(optarg, "h264") ? VC_H265 : VC_H264; break;
        case 'w': w = atoi(optarg); break;
        case 'h': h = atoi(optarg); break;
        case 'n': frames = atoi(optarg); break;
        case 'k': kbps = atoi(optarg); break;
        case 'o': out = optarg; break;
        default: fprintf(stderr, "usage: mpptest [-c h264|h265] [-w W] [-h H] [-n FRAMES] [-k KBPS] [-o FILE]\n"); return 2;
        }
    if (!mppenc_caps()) {
        fprintf(stderr, "no Rockchip MPP here (/dev/mpp_service, librockchip_mpp.so.1)\n");
        return 1;
    }
    char err[200] = "";
    mppenc_t *e = mppenc_open(codec, 0, w, h, 30, kbps, err, sizeof err);
    if (!e) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    FILE *f = out ? fopen(out, "wb") : NULL;
    int stride = w * 4;
    uint8_t *bgra = malloc((size_t)stride * h);
    static const uint8_t bars[8][3] = { /* B, G, R */
        { 255, 255, 255 }, { 0, 255, 255 }, { 255, 255, 0 }, { 0, 255, 0 },
        { 255, 0, 255 }, { 0, 0, 255 }, { 255, 0, 0 }, { 0, 0, 0 } };
    size_t bytes = 0;
    int keys = 0;
    double t0 = now_s();
    for (int i = 0; i < frames; i++) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                uint8_t *p = bgra + (size_t)y * stride + x * 4;
                const uint8_t *c = bars[x * 8 / w];
                int sq = (x - i * 3 % w + w) % w < 64 && y > h / 2 && y < h / 2 + 64;
                p[0] = sq ? 128 : c[0], p[1] = sq ? 128 : c[1], p[2] = sq ? 128 : c[2], p[3] = 255;
            }
        double a = now_s();
        size_t n;
        int is_key;
        const uint8_t *au = mppenc_load_bgra(e, bgra, stride) == 0 ? mppenc_frame(e, i % 90 == 0, &n, &is_key) : NULL;
        (void)a;
        if (!au) {
            fprintf(stderr, "frame %d failed\n", i);
            return 1;
        }
        bytes += n;
        keys += is_key;
        if (f)
            fwrite(au, 1, n, f);
    }
    double dt = now_s() - t0;
    printf("%s %dx%d: %d frames in %.2f s (%.1f fps, drawing included), %d keyframes, %.0f kbit/s at 30 fps\n",
           mppenc_name(e), w, h, frames, dt, frames / dt, keys, bytes * 8.0 / frames * 30 / 1000);
    if (f)
        fclose(f);
    mppenc_close(e);
    free(bgra);
    return keys >= frames / 90 ? 0 : 1;
}
