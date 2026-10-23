/* Hardware video on Linux through VA-API: AMD and Intel GPUs (Mesa's radeonsi and iris, Intel's iHD),
 * a Steam Deck among them. libva is loaded with dlopen, so the agent still starts where it isn't
 * installed and video.c falls back to software. X11 capture gives BGRA in memory; yuv.c scales it to
 * NV12 at the encode size, which goes into a VA surface and is encoded as H.264 or H.265 for low delay:
 * P frames only, one reference, constant bitrate, an IDR on request. The parameter sets (SPS, PPS and
 * the H.265 VPS) and every slice header are written here and handed over as packed headers: Mesa's
 * radeonsi writes none itself and takes the NAL unit types from them.
 *
 * The dsk_hwvideo layer at the end prefers Rockchip MPP (browser/agent/video_mpp.c: the OECT's RK3566)
 * when it is there; it takes the captured BGRA as it is. */
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_enc_h264.h>
#include <va/va_enc_hevc.h>

#include "desk.h"
#include "mppenc.h"
#include "vaenc.h"

/* ---------------- libva, loaded at run time ---------------- */

#define VA_FNS(X)                                                                                                     \
    X(vaInitialize) X(vaTerminate) X(vaQueryConfigEntrypoints) X(vaGetConfigAttributes) X(vaCreateConfig)            \
    X(vaDestroyConfig) X(vaCreateSurfaces) X(vaDestroySurfaces) X(vaCreateContext) X(vaDestroyContext)                \
    X(vaCreateBuffer) X(vaDestroyBuffer) X(vaMapBuffer) X(vaUnmapBuffer) X(vaDeriveImage) X(vaCreateImage)            \
    X(vaPutImage) X(vaDestroyImage) X(vaBeginPicture) X(vaRenderPicture) X(vaEndPicture) X(vaSyncSurface)             \
    X(vaErrorStr) X(vaQueryVendorString)
#define VA_PTR(f) static __typeof__(&f) p_##f;
VA_FNS(VA_PTR)
static __typeof__(&vaGetDisplayDRM) p_vaGetDisplayDRM;
static __typeof__(&vaSetInfoCallback) p_vaSetInfoCallback; /* optional: keep libva's own messages out of */
static __typeof__(&vaSetErrorCallback) p_vaSetErrorCallback; /* stderr; the agent logs why it falls back */

static pthread_once_t va_once = PTHREAD_ONCE_INIT;
static int va_ok;

static void va_load(void)
{
    void *va = dlopen("libva.so.2", RTLD_NOW | RTLD_LOCAL);
    void *drm = va ? dlopen("libva-drm.so.2", RTLD_NOW | RTLD_LOCAL) : NULL;
    if (!drm)
        return;
    int ok = 1;
#define VA_SYM(f)                                                                                                     \
    if (!(p_##f = (__typeof__(p_##f))dlsym(va, #f)))                                                                  \
        ok = 0;
    VA_FNS(VA_SYM)
    p_vaGetDisplayDRM = (__typeof__(p_vaGetDisplayDRM))dlsym(drm, "vaGetDisplayDRM");
    p_vaSetInfoCallback = (__typeof__(p_vaSetInfoCallback))dlsym(va, "vaSetInfoCallback");
    p_vaSetErrorCallback = (__typeof__(p_vaSetErrorCallback))dlsym(va, "vaSetErrorCallback");
    va_ok = ok && p_vaGetDisplayDRM;
}

typedef struct {
    int fd;
    VADisplay dpy;
} vadev_t;

/* the first render node whose driver initializes */
static int dev_open(vadev_t *d, char *err, size_t cap)
{
    pthread_once(&va_once, va_load);
    if (!va_ok) {
        snprintf(err, cap, "no VA-API (libva.so.2, libva-drm.so.2)");
        return -1;
    }
    for (int i = 128; i < 136; i++) {
        char path[32];
        snprintf(path, sizeof path, "/dev/dri/renderD%d", i);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;
        VADisplay dpy = p_vaGetDisplayDRM(fd);
        int maj, min;
        if (dpy) {
            if (p_vaSetInfoCallback)
                p_vaSetInfoCallback(dpy, NULL, NULL);
            if (p_vaSetErrorCallback)
                p_vaSetErrorCallback(dpy, NULL, NULL);
            if (p_vaInitialize(dpy, &maj, &min) == VA_STATUS_SUCCESS) {
                d->fd = fd;
                d->dpy = dpy;
                return 0;
            }
            p_vaTerminate(dpy);
        }
        close(fd);
    }
    snprintf(err, cap, "no GPU with a VA-API driver (/dev/dri/renderD*)");
    return -1;
}

static void dev_close(vadev_t *d)
{
    if (d->dpy)
        p_vaTerminate(d->dpy);
    if (d->fd >= 0)
        close(d->fd);
    d->dpy = NULL;
    d->fd = -1;
}

/* the encode entry point for a profile, or 0 */
static VAEntrypoint enc_entry(VADisplay dpy, VAProfile prof)
{
    VAEntrypoint ep[32];
    int n = 0;
    if (p_vaQueryConfigEntrypoints(dpy, prof, ep, &n) != VA_STATUS_SUCCESS)
        return 0;
    VAEntrypoint lp = 0;
    for (int i = 0; i < n && i < 32; i++) {
        if (ep[i] == VAEntrypointEncSlice)
            return ep[i];
        if (ep[i] == VAEntrypointEncSliceLP)
            lp = ep[i];
    }
    return lp;
}

static pthread_once_t caps_once = PTHREAD_ONCE_INIT;
static int caps;

static void caps_probe(void)
{
    vadev_t d = { -1, NULL };
    char err[128];
    if (dev_open(&d, err, sizeof err) < 0)
        return;
    if (enc_entry(d.dpy, VAProfileH264High) || enc_entry(d.dpy, VAProfileH264Main) ||
        enc_entry(d.dpy, VAProfileH264ConstrainedBaseline))
        caps |= 1;
    if (enc_entry(d.dpy, VAProfileHEVCMain))
        caps |= 2;
    dev_close(&d);
}

int vaenc_caps(void)
{
    pthread_once(&caps_once, caps_probe);
    return caps;
}

/* ---------------- the encoder ---------------- */

struct vaenc {
    vadev_t dev;
    int codec, high, w, h, fps, kbps, rc_changed;
    VAProfile profile;
    VAEntrypoint entry;
    unsigned rc_mode;
    VAConfigID cfg;
    VAContextID ctx;
    VASurfaceID src, recon[2];
    int cur;           /* recon[cur] takes the frame being encoded, recon[cur ^ 1] holds the reference */
    VABufferID coded;
    VAImage img;       /* upload through vaPutImage when the surface can't be mapped directly */
    int derive;
    unsigned n;        /* frames since the IDR */
    unsigned idr_id;
    int have_ref;
    int ctb_log2, min_cb_log2, tu_depth_inter, tu_depth_intra; /* H.265 block sizes */
    uint8_t hdr[512], slh[128]; /* packed parameter sets, slice header */
    uint8_t *out;
    size_t out_cap;
    char name[128];
};

static VABufferID param(vaenc_t *e, VABufferType type, const void *data, size_t size)
{
    VABufferID id = VA_INVALID_ID;
    if (p_vaCreateBuffer(e->dev.dpy, e->ctx, type, (unsigned)size, 1, (void *)data, &id) != VA_STATUS_SUCCESS)
        return VA_INVALID_ID;
    return id;
}

static VABufferID misc(vaenc_t *e, VAEncMiscParameterType type, const void *data, size_t size)
{
    uint8_t tmp[sizeof(VAEncMiscParameterBuffer) + 256];
    VAEncMiscParameterBuffer *m = (VAEncMiscParameterBuffer *)tmp;
    memset(tmp, 0, sizeof tmp);
    m->type = type;
    memcpy(m->data, data, size);
    return param(e, VAEncMiscParameterBufferType, tmp, sizeof *m + size);
}

/* ---- rate control: the same for both codecs ---- */

static int rate_buffers(vaenc_t *e, VABufferID *b)
{
    uint32_t bps = (uint32_t)e->kbps * 1000;
    VAEncMiscParameterRateControl rc = { .bits_per_second = bps, .target_percentage = e->rc_mode == VA_RC_CBR ? 100 : 90,
                                         .window_size = 1000 };
    rc.rc_flags.bits.reset = 1;
    rc.rc_flags.bits.disable_frame_skip = 1;
    VAEncMiscParameterFrameRate fr = { .framerate = (uint32_t)e->fps };
    /* half a second of buffer: room for an IDR, little delay */
    VAEncMiscParameterHRD hrd = { .initial_buffer_fullness = bps / 4, .buffer_size = bps / 2 };
    b[0] = misc(e, VAEncMiscParameterTypeRateControl, &rc, sizeof rc);
    b[1] = misc(e, VAEncMiscParameterTypeFrameRate, &fr, sizeof fr);
    b[2] = misc(e, VAEncMiscParameterTypeHRD, &hrd, sizeof hrd);
    return 3;
}

/* ---- packed headers: a bit writer, NAL units with emulation prevention ---- */

typedef struct {
    uint8_t b[256];
    int bits;
} bw_t;

static void put(bw_t *w, uint32_t v, int n)
{
    for (int i = n - 1; i >= 0; i--, w->bits++)
        if (v >> i & 1)
            w->b[w->bits >> 3] |= (uint8_t)(0x80 >> (w->bits & 7));
}
static void ue(bw_t *w, uint32_t v)
{
    int n = 0;
    for (uint32_t t = ++v; t > 1; t >>= 1)
        n++;
    put(w, 0, n);
    put(w, v, n + 1);
}
static void se(bw_t *w, int v) { ue(w, v > 0 ? 2u * (uint32_t)v - 1 : (uint32_t)(-2 * v)); }
static void trailing(bw_t *w)
{
    put(w, 1, 1);
    while (w->bits & 7)
        put(w, 0, 1);
}

/* start code, NAL header, payload with 0x03 after two zeros; returns the bits written to out */
static int nal(uint8_t *out, const uint8_t *hdr, int nh, const bw_t *w)
{
    int o = 0, zeros = 0, full = w->bits / 8;
    out[o++] = 0, out[o++] = 0, out[o++] = 0, out[o++] = 1;
    for (int i = 0; i < nh; i++)
        out[o++] = hdr[i];
    for (int i = 0; i < full; i++) {
        if (zeros == 2 && w->b[i] <= 3)
            out[o++] = 3, zeros = 0;
        zeros = w->b[i] ? 0 : zeros + 1;
        out[o++] = w->b[i];
    }
    int bits = o * 8;
    if (w->bits & 7) /* a slice header that stops mid-byte: the driver goes on from there */
        out[o] = w->b[full], bits += w->bits & 7;
    return bits;
}

static int packed(vaenc_t *e, VABufferID *b, unsigned type, const uint8_t *data, int bits)
{
    VAEncPackedHeaderParameterBuffer ph = { .type = type, .bit_length = (unsigned)bits, .has_emulation_bytes = 1 };
    b[0] = param(e, VAEncPackedHeaderParameterBufferType, &ph, sizeof ph);
    b[1] = param(e, VAEncPackedHeaderDataBufferType, data, (size_t)(bits + 7) / 8);
    return 2;
}

/* H.264 SPS and PPS, as h264_buffers configures the encoder */
static int h264_headers(vaenc_t *e, uint8_t *out, int level)
{
    bw_t w = { { 0 }, 0 };
    int prof = e->profile == VAProfileH264High ? 100 : e->profile == VAProfileH264Main ? 77 : 66;
    put(&w, (uint32_t)prof, 8);
    put(&w, prof == 66 ? 0xC0 : prof == 77 ? 0x40 : 0, 8); /* constraint_set0/1: Constrained Baseline; set1: Main */
    put(&w, (uint32_t)level, 8);
    ue(&w, 0); /* seq_parameter_set_id */
    if (prof == 100) {
        ue(&w, 1); /* 4:2:0 */
        ue(&w, 0), ue(&w, 0); /* 8 bits */
        put(&w, 0, 1), put(&w, 0, 1); /* no transform bypass, no scaling matrices */
    }
    ue(&w, 4);  /* log2_max_frame_num_minus4 */
    ue(&w, 2);  /* pic_order_cnt_type */
    ue(&w, 1);  /* max_num_ref_frames */
    put(&w, 0, 1);
    ue(&w, (uint32_t)(e->w / 16 - 1));
    ue(&w, (uint32_t)(e->h / 16 - 1));
    put(&w, 1, 1); /* frame_mbs_only */
    put(&w, 1, 1); /* direct_8x8_inference */
    put(&w, 0, 1); /* no cropping */
    put(&w, 1, 1); /* VUI */
    put(&w, 0, 4); /* no aspect ratio, overscan, video signal type, chroma location */
    put(&w, 1, 1), put(&w, 1, 32), put(&w, (uint32_t)e->fps * 2, 32), put(&w, 0, 1); /* timing, not fixed */
    put(&w, 0, 3); /* no HRD, no pic_struct */
    put(&w, 1, 1); /* bitstream_restriction: no reordering, one frame in the DPB */
    put(&w, 1, 1), ue(&w, 0), ue(&w, 0), ue(&w, 15), ue(&w, 15), ue(&w, 0), ue(&w, 1);
    trailing(&w);
    static const uint8_t sps_h[] = { 0x67 }, pps_h[] = { 0x68 };
    int bits = nal(out, sps_h, 1, &w);
    bw_t p = { { 0 }, 0 };
    ue(&p, 0), ue(&p, 0); /* pps, sps ids */
    put(&p, (uint32_t)e->high, 1); /* CABAC */
    put(&p, 0, 1);
    ue(&p, 0);          /* one slice group */
    ue(&p, 0), ue(&p, 0); /* one reference by default */
    put(&p, 0, 3);      /* no weighted prediction */
    se(&p, 0), se(&p, 0), se(&p, 0); /* pic_init_qp 26, qs, chroma offset */
    put(&p, 1, 1);      /* deblocking_filter_control_present */
    put(&p, 0, 2);      /* no constrained intra, no redundant_pic_cnt */
    if (e->high)
        put(&p, 1, 1), put(&p, 0, 1), se(&p, 0); /* 8x8 transforms, no scaling matrix, second chroma offset */
    trailing(&p);
    return bits + nal(out + bits / 8, pps_h, 1, &p);
}

static int h264_slice_header(vaenc_t *e, int key, uint8_t *out)
{
    bw_t w = { { 0 }, 0 };
    ue(&w, 0);                       /* first_mb_in_slice */
    ue(&w, key ? 7 : 5);             /* I / P, every slice the same */
    ue(&w, 0);                       /* pic_parameter_set_id */
    put(&w, e->n & 255, 8);          /* frame_num */
    if (key)
        ue(&w, e->idr_id & 0xFFFF);  /* idr_pic_id */
    if (!key)
        put(&w, 0, 1), put(&w, 0, 1); /* no num_ref_idx override, no list modification */
    if (key)
        put(&w, 0, 1), put(&w, 0, 1); /* dec_ref_pic_marking: no_output_of_prior_pics, long_term_reference */
    else
        put(&w, 0, 1);                /* sliding window */
    if (e->high && !key)
        ue(&w, 0);                   /* cabac_init_idc */
    se(&w, 0);                       /* slice_qp_delta */
    ue(&w, 0), se(&w, 0), se(&w, 0); /* deblocking on, no offsets */
    uint8_t h = key ? 0x65 : 0x41;   /* nal_ref_idc 3 IDR / 2 non-IDR */
    return nal(out, &h, 1, &w);
}

/* H.265 profile_tier_level: Main, Main tier */
static void hevc_ptl(bw_t *w, int level)
{
    put(w, 0, 2), put(w, 0, 1), put(w, 1, 5);      /* profile_space, tier, Main */
    put(w, 0x60000000, 32);                         /* compatible with Main and Main 10 */
    put(w, 1, 1), put(w, 0, 1), put(w, 0, 1), put(w, 1, 1); /* progressive, frame only */
    put(w, 0, 32), put(w, 0, 12);                   /* reserved 44 bits */
    put(w, (uint32_t)level, 8);
}

static int hevc_headers(vaenc_t *e, uint8_t *out, int level)
{
    static const uint8_t vps_h[] = { 0x40, 0x01 }, sps_h[] = { 0x42, 0x01 }, pps_h[] = { 0x44, 0x01 };
    bw_t v = { { 0 }, 0 };
    put(&v, 0, 4), put(&v, 1, 1), put(&v, 1, 1), put(&v, 0, 6), put(&v, 0, 3), put(&v, 1, 1); /* one layer */
    put(&v, 0xFFFF, 16);
    hevc_ptl(&v, level);
    put(&v, 1, 1), ue(&v, 1), ue(&v, 0), ue(&v, 0); /* DPB of 2, no reordering */
    put(&v, 0, 6), ue(&v, 0);                        /* max_layer_id, one layer set */
    put(&v, 0, 1), put(&v, 0, 1);                    /* no timing, no extension */
    trailing(&v);
    int bits = nal(out, vps_h, 2, &v);
    bw_t s = { { 0 }, 0 };
    put(&s, 0, 4), put(&s, 0, 3), put(&s, 1, 1);     /* VPS 0, one sub-layer, temporal id nesting */
    hevc_ptl(&s, level);
    ue(&s, 0), ue(&s, 1);                            /* sps 0, 4:2:0 */
    ue(&s, (uint32_t)e->w), ue(&s, (uint32_t)e->h);
    put(&s, 0, 1);                                   /* no conformance window */
    ue(&s, 0), ue(&s, 0);                            /* 8 bits */
    ue(&s, 4);                                       /* log2_max_pic_order_cnt_lsb_minus4 */
    put(&s, 1, 1), ue(&s, 1), ue(&s, 0), ue(&s, 0);  /* DPB of 2, no reordering */
    ue(&s, (uint32_t)(e->min_cb_log2 - 3)), ue(&s, (uint32_t)(e->ctb_log2 - e->min_cb_log2));
    ue(&s, 0), ue(&s, 3);                            /* transform blocks 4..32 */
    ue(&s, (uint32_t)e->tu_depth_inter), ue(&s, (uint32_t)e->tu_depth_intra);
    put(&s, 0, 1), put(&s, 1, 1), put(&s, 0, 1), put(&s, 0, 1); /* no scaling list, AMP, no SAO, no PCM */
    ue(&s, 0);                                       /* short-term RPS in each slice header */
    put(&s, 0, 1), put(&s, 0, 1), put(&s, 0, 1);     /* no long-term refs, no temporal MVP, no strong smoothing */
    put(&s, 1, 1);                                   /* VUI */
    put(&s, 0, 8);                                   /* aspect, overscan, signal type, chroma loc, neutral, field, frame-field, display window */
    put(&s, 1, 1), put(&s, 1, 32), put(&s, (uint32_t)e->fps, 32), put(&s, 0, 1), put(&s, 0, 1); /* timing; no POC prop., no HRD */
    put(&s, 0, 1);                                   /* no bitstream restriction */
    put(&s, 0, 1);                                   /* no extensions */
    trailing(&s);
    bits += nal(out + bits / 8, sps_h, 2, &s);
    bw_t p = { { 0 }, 0 };
    ue(&p, 0), ue(&p, 0);                            /* pps 0, sps 0 */
    put(&p, 0, 1), put(&p, 0, 1), put(&p, 0, 3), put(&p, 0, 1), put(&p, 0, 1); /* no dependent slices, output flag, extra bits, sign hiding, cabac_init */
    ue(&p, 0), ue(&p, 0);                            /* one reference by default */
    se(&p, 30 - 26);                                 /* init_qp */
    put(&p, 0, 1), put(&p, 0, 1);                    /* no constrained intra, no transform skip */
    put(&p, 1, 1), ue(&p, 0);                        /* cu_qp_delta at the CTB */
    se(&p, 0), se(&p, 0);                            /* chroma offsets */
    put(&p, 0, 1), put(&p, 0, 1), put(&p, 0, 1), put(&p, 0, 1); /* slice chroma offsets, weighted P, B, transquant bypass */
    put(&p, 0, 1), put(&p, 0, 1);                    /* no tiles, no WPP */
    put(&p, 1, 1);                                   /* loop filter across slices */
    put(&p, 0, 1), put(&p, 0, 1), put(&p, 0, 1);     /* no deblocking control, scaling list, list modification */
    ue(&p, 0);                                       /* log2_parallel_merge_level_minus2 */
    put(&p, 0, 1), put(&p, 0, 1);                    /* no slice header extension, no PPS extension */
    trailing(&p);
    return bits + nal(out + bits / 8, pps_h, 2, &p);
}

static int hevc_slice_header(vaenc_t *e, int key, uint8_t *out)
{
    bw_t w = { { 0 }, 0 };
    put(&w, 1, 1);                         /* first slice segment */
    if (key)
        put(&w, 0, 1);                     /* no_output_of_prior_pics */
    ue(&w, 0);                             /* pps */
    ue(&w, key ? 2 : 1);                   /* I / P */
    if (!key) {
        put(&w, e->n & 255, 8);            /* slice_pic_order_cnt_lsb */
        put(&w, 0, 1);                     /* the RPS follows: */
        ue(&w, 1), ue(&w, 0), ue(&w, 0), put(&w, 1, 1); /* the previous picture, used */
        put(&w, 0, 1);                     /* no num_ref_idx override */
        ue(&w, 0);                         /* five_minus_max_num_merge_cand */
    }
    se(&w, 0);                             /* slice_qp_delta */
    put(&w, 1, 1);                         /* slice_loop_filter_across_slices */
    trailing(&w);                          /* byte_alignment */
    uint8_t h[2] = { (uint8_t)((key ? 19 : 1) << 1), 1 }; /* IDR_W_RADL / TRAIL_R */
    return nal(out, h, 2, &w);
}

/* ---- H.264: POC type 2 (output order = decode order: the decoder shows each frame at once) ---- */

static int h264_level(const vaenc_t *e) { return (e->w * e->h > 2097152 || e->w * e->h * e->fps > 62914560) ? 51 : 42; }
static int hevc_level(const vaenc_t *e) /* 5.1 : 4.1 */
{
    return (e->w * e->h > 2228224 || e->w * e->h * e->fps > 133693440) ? 153 : 123;
}

static VAPictureH264 h264_pic(VASurfaceID s, unsigned n, unsigned flags)
{
    VAPictureH264 p = { .picture_id = s, .frame_idx = n & 255, .flags = flags, .TopFieldOrderCnt = (int32_t)(2 * n),
                        .BottomFieldOrderCnt = (int32_t)(2 * n) };
    return p;
}

static const VAPictureH264 H264_NONE = { .picture_id = VA_INVALID_SURFACE, .flags = VA_PICTURE_H264_INVALID };

static int h264_buffers(vaenc_t *e, int key, VABufferID *b)
{
    int nb = 0, mbw = e->w / 16, mbh = e->h / 16;
    if (key || e->rc_changed) {
        VAEncSequenceParameterBufferH264 s = { 0 };
        s.level_idc = (uint8_t)h264_level(e);
        s.intra_period = s.intra_idr_period = 0xFFFF; /* IDRs come on request */
        s.ip_period = 1;
        s.bits_per_second = (uint32_t)e->kbps * 1000;
        s.max_num_ref_frames = 1;
        s.picture_width_in_mbs = (uint16_t)mbw;
        s.picture_height_in_mbs = (uint16_t)mbh;
        s.seq_fields.bits.chroma_format_idc = 1;
        s.seq_fields.bits.frame_mbs_only_flag = 1;
        s.seq_fields.bits.direct_8x8_inference_flag = 1;
        s.seq_fields.bits.log2_max_frame_num_minus4 = 4;
        s.seq_fields.bits.pic_order_cnt_type = 2;
        s.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = 4;
        s.vui_parameters_present_flag = 1;
        s.vui_fields.bits.timing_info_present_flag = 1;
        s.num_units_in_tick = 1;
        s.time_scale = (uint32_t)e->fps * 2;
        b[nb++] = param(e, VAEncSequenceParameterBufferType, &s, sizeof s);
        nb += rate_buffers(e, b + nb);
    }
    if (key)
        nb += packed(e, b + nb, VAEncPackedHeaderSequence, e->hdr, h264_headers(e, e->hdr, h264_level(e)));
    VAEncPictureParameterBufferH264 p = { 0 };
    p.CurrPic = h264_pic(e->recon[e->cur], e->n, 0);
    for (int i = 0; i < 16; i++)
        p.ReferenceFrames[i] = H264_NONE;
    if (!key)
        p.ReferenceFrames[0] = h264_pic(e->recon[e->cur ^ 1], e->n - 1, VA_PICTURE_H264_SHORT_TERM_REFERENCE);
    p.coded_buf = e->coded;
    p.frame_num = (uint16_t)(e->n & 255);
    p.pic_init_qp = 26;
    p.pic_fields.bits.idr_pic_flag = (unsigned)key;
    p.pic_fields.bits.reference_pic_flag = 1;
    p.pic_fields.bits.entropy_coding_mode_flag = (unsigned)e->high;
    p.pic_fields.bits.transform_8x8_mode_flag = (unsigned)e->high;
    p.pic_fields.bits.deblocking_filter_control_present_flag = 1;
    b[nb++] = param(e, VAEncPictureParameterBufferType, &p, sizeof p);
    VAEncSliceParameterBufferH264 sl = { 0 };
    sl.num_macroblocks = (uint32_t)(mbw * mbh);
    sl.macroblock_info = VA_INVALID_ID;
    sl.slice_type = key ? 2 : 0; /* I : P */
    sl.idr_pic_id = (uint16_t)e->idr_id;
    sl.pic_order_cnt_lsb = (uint16_t)((2 * e->n) & 255);
    for (int i = 0; i < 32; i++)
        sl.RefPicList0[i] = sl.RefPicList1[i] = H264_NONE;
    if (!key)
        sl.RefPicList0[0] = p.ReferenceFrames[0];
    nb += packed(e, b + nb, VAEncPackedHeaderSlice, e->slh, h264_slice_header(e, key, e->slh));
    b[nb++] = param(e, VAEncSliceParameterBufferType, &sl, sizeof sl);
    return nb;
}

/* ---- H.265 ---- */

static const VAPictureHEVC HEVC_NONE = { .picture_id = VA_INVALID_SURFACE, .flags = VA_PICTURE_HEVC_INVALID };

static int hevc_buffers(vaenc_t *e, int key, VABufferID *b)
{
    int nb = 0, ctb = 1 << e->ctb_log2;
    if (key || e->rc_changed) {
        VAEncSequenceParameterBufferHEVC s = { 0 };
        s.general_profile_idc = 1; /* Main */
        s.general_level_idc = (uint8_t)hevc_level(e);
        s.intra_period = s.intra_idr_period = 0xFFFF;
        s.ip_period = 1;
        s.bits_per_second = (uint32_t)e->kbps * 1000;
        s.pic_width_in_luma_samples = (uint16_t)e->w;
        s.pic_height_in_luma_samples = (uint16_t)e->h;
        s.seq_fields.bits.chroma_format_idc = 1;
        s.seq_fields.bits.amp_enabled_flag = 1;
        s.seq_fields.bits.low_delay_seq = 1;
        s.log2_min_luma_coding_block_size_minus3 = (uint8_t)(e->min_cb_log2 - 3);
        s.log2_diff_max_min_luma_coding_block_size = (uint8_t)(e->ctb_log2 - e->min_cb_log2);
        s.log2_min_transform_block_size_minus2 = 0;
        s.log2_diff_max_min_transform_block_size = 3;
        s.max_transform_hierarchy_depth_inter = (uint8_t)e->tu_depth_inter;
        s.max_transform_hierarchy_depth_intra = (uint8_t)e->tu_depth_intra;
        s.vui_parameters_present_flag = 1;
        s.vui_fields.bits.vui_timing_info_present_flag = 1;
        s.vui_num_units_in_tick = 1;
        s.vui_time_scale = (uint32_t)e->fps;
        b[nb++] = param(e, VAEncSequenceParameterBufferType, &s, sizeof s);
        nb += rate_buffers(e, b + nb);
    }
    if (key)
        nb += packed(e, b + nb, VAEncPackedHeaderSequence, e->hdr, hevc_headers(e, e->hdr, hevc_level(e)));
    VAEncPictureParameterBufferHEVC p = { 0 };
    p.decoded_curr_pic = (VAPictureHEVC){ .picture_id = e->recon[e->cur], .pic_order_cnt = (int32_t)e->n };
    for (int i = 0; i < 15; i++)
        p.reference_frames[i] = HEVC_NONE;
    if (!key)
        p.reference_frames[0] = (VAPictureHEVC){ .picture_id = e->recon[e->cur ^ 1], .pic_order_cnt = (int32_t)e->n - 1 };
    p.coded_buf = e->coded;
    p.collocated_ref_pic_index = 0xFF;
    p.pic_init_qp = 30;
    p.nal_unit_type = key ? 19 : 1; /* IDR_W_RADL : TRAIL_R */
    p.pic_fields.bits.idr_pic_flag = (unsigned)key;
    p.pic_fields.bits.coding_type = key ? 1 : 2; /* I : P */
    p.pic_fields.bits.reference_pic_flag = 1;
    p.pic_fields.bits.cu_qp_delta_enabled_flag = 1; /* the rate control varies QP inside a picture */
    p.pic_fields.bits.pps_loop_filter_across_slices_enabled_flag = 1;
    b[nb++] = param(e, VAEncPictureParameterBufferType, &p, sizeof p);
    VAEncSliceParameterBufferHEVC sl = { 0 };
    sl.num_ctu_in_slice = (uint32_t)(((e->w + ctb - 1) / ctb) * ((e->h + ctb - 1) / ctb));
    sl.slice_type = key ? 2 : 1; /* I : P */
    for (int i = 0; i < 15; i++)
        sl.ref_pic_list0[i] = sl.ref_pic_list1[i] = HEVC_NONE;
    if (!key)
        sl.ref_pic_list0[0] = p.reference_frames[0];
    sl.max_num_merge_cand = 5;
    sl.slice_fields.bits.last_slice_of_pic_flag = 1;
    sl.slice_fields.bits.slice_loop_filter_across_slices_enabled_flag = 1;
    nb += packed(e, b + nb, VAEncPackedHeaderSlice, e->slh, hevc_slice_header(e, key, e->slh));
    b[nb++] = param(e, VAEncSliceParameterBufferType, &sl, sizeof sl);
    return nb;
}

/* ---- open, upload, encode ---- */

void vaenc_close(vaenc_t *e)
{
    if (!e)
        return;
    VADisplay dpy = e->dev.dpy;
    if (dpy) {
        if (e->img.image_id != VA_INVALID_ID)
            p_vaDestroyImage(dpy, e->img.image_id);
        if (e->coded != VA_INVALID_ID)
            p_vaDestroyBuffer(dpy, e->coded);
        if (e->ctx != VA_INVALID_ID)
            p_vaDestroyContext(dpy, e->ctx);
        if (e->recon[0] != VA_INVALID_SURFACE)
            p_vaDestroySurfaces(dpy, e->recon, 2);
        if (e->src != VA_INVALID_SURFACE)
            p_vaDestroySurfaces(dpy, &e->src, 1);
        if (e->cfg != VA_INVALID_ID)
            p_vaDestroyConfig(dpy, e->cfg);
    }
    dev_close(&e->dev);
    free(e->out);
    free(e);
}

/* H.265 block sizes from the driver when it says (libva 2.13+), else the common 32x32 CTB */
static void hevc_sizes(vaenc_t *e)
{
    e->ctb_log2 = 5;
    e->min_cb_log2 = 3;
    e->tu_depth_inter = e->tu_depth_intra = 3;
    VAConfigAttrib a = { .type = VAConfigAttribEncHEVCBlockSizes };
    if (p_vaGetConfigAttributes(e->dev.dpy, e->profile, e->entry, &a, 1) != VA_STATUS_SUCCESS ||
        a.value == VA_ATTRIB_NOT_SUPPORTED)
        return;
    VAConfigAttribValEncHEVCBlockSizes bs = { .value = a.value };
    e->ctb_log2 = bs.bits.log2_max_coding_tree_block_size_minus3 + 3;
    e->min_cb_log2 = bs.bits.log2_min_luma_coding_block_size_minus3 + 3;
    e->tu_depth_inter = bs.bits.max_max_transform_hierarchy_depth_inter;
    e->tu_depth_intra = bs.bits.max_max_transform_hierarchy_depth_intra;
}

vaenc_t *vaenc_open(int codec, int high, int w, int h, int fps, int kbps, char *err, size_t cap)
{
    if (w < 64 || h < 64 || w % 16 || h % 16) {
        snprintf(err, cap, "unsupported size %dx%d", w, h);
        return NULL;
    }
    vaenc_t *e = calloc(1, sizeof *e);
    e->dev.fd = -1;
    e->cfg = e->ctx = e->coded = VA_INVALID_ID;
    e->src = e->recon[0] = e->recon[1] = VA_INVALID_SURFACE;
    e->img.image_id = VA_INVALID_ID;
    e->codec = codec;
    e->w = w;
    e->h = h;
    e->fps = fps > 0 ? fps : 60;
    e->kbps = kbps > 0 ? kbps : 8000;
    if (dev_open(&e->dev, err, cap) < 0)
        goto fail;
    VADisplay dpy = e->dev.dpy;
    VAProfile cand[2];
    int nc = 0;
    if (codec == VC_H265)
        cand[nc++] = VAProfileHEVCMain;
    else if (high)
        cand[nc++] = VAProfileH264High, cand[nc++] = VAProfileH264Main;
    else
        cand[nc++] = VAProfileH264ConstrainedBaseline, cand[nc++] = VAProfileH264Main;
    for (int i = 0; i < nc && !e->entry; i++)
        if ((e->entry = enc_entry(dpy, cand[i])))
            e->profile = cand[i];
    if (!e->entry) {
        snprintf(err, cap, "this GPU doesn't encode %s", codec == VC_H265 ? "H.265" : "H.264");
        goto fail;
    }
    e->high = codec == VC_H264 && e->profile == VAProfileH264High; /* CABAC and 8x8 transforms only for High */
    VAConfigAttrib a[3] = { { .type = VAConfigAttribRTFormat }, { .type = VAConfigAttribRateControl },
                            { .type = VAConfigAttribEncPackedHeaders } };
    if (p_vaGetConfigAttributes(dpy, e->profile, e->entry, a, 3) != VA_STATUS_SUCCESS || !(a[0].value & VA_RT_FORMAT_YUV420)) {
        snprintf(err, cap, "the encoder takes no 4:2:0 pictures");
        goto fail;
    }
    e->rc_mode = a[1].value & VA_RC_CBR ? VA_RC_CBR : a[1].value & VA_RC_VBR ? VA_RC_VBR : 0;
    if (!e->rc_mode) {
        snprintf(err, cap, "the encoder has no bitrate control");
        goto fail;
    }
    unsigned want = VA_ENC_PACKED_HEADER_SEQUENCE | VA_ENC_PACKED_HEADER_SLICE;
    if (a[2].value == VA_ATTRIB_NOT_SUPPORTED || (a[2].value & want) != want) {
        snprintf(err, cap, "the encoder takes no packed headers");
        goto fail;
    }
    if (codec == VC_H265)
        hevc_sizes(e);
    VAConfigAttrib set[3] = { { .type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420 },
                              { .type = VAConfigAttribRateControl, .value = e->rc_mode },
                              { .type = VAConfigAttribEncPackedHeaders, .value = want } };
    VAStatus st = p_vaCreateConfig(dpy, e->profile, e->entry, set, 3, &e->cfg);
    if (st == VA_STATUS_SUCCESS)
        st = p_vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, (unsigned)w, (unsigned)h, &e->src, 1, NULL, 0);
    if (st == VA_STATUS_SUCCESS)
        st = p_vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, (unsigned)w, (unsigned)h, e->recon, 2, NULL, 0);
    if (st == VA_STATUS_SUCCESS)
        st = p_vaCreateContext(dpy, e->cfg, w, h, VA_PROGRESSIVE, e->recon, 2, &e->ctx);
    if (st == VA_STATUS_SUCCESS)
        st = p_vaCreateBuffer(dpy, e->ctx, VAEncCodedBufferType, (unsigned)(w * h * 3 / 2 + 65536), 1, NULL, &e->coded);
    if (st != VA_STATUS_SUCCESS) {
        snprintf(err, cap, "VA-API: %s", p_vaErrorStr(st));
        goto fail;
    }
    /* map the input surface directly when the driver allows it, else go through an image */
    VAImage probe;
    e->derive = p_vaDeriveImage(dpy, e->src, &probe) == VA_STATUS_SUCCESS;
    if (e->derive) {
        e->derive = probe.format.fourcc == VA_FOURCC_NV12;
        p_vaDestroyImage(dpy, probe.image_id);
    }
    if (!e->derive) {
        VAImageFormat nv12 = { .fourcc = VA_FOURCC_NV12, .byte_order = VA_LSB_FIRST, .bits_per_pixel = 12 };
        if ((st = p_vaCreateImage(dpy, &nv12, w, h, &e->img)) != VA_STATUS_SUCCESS) {
            e->img.image_id = VA_INVALID_ID;
            snprintf(err, cap, "VA-API: no NV12 upload (%s)", p_vaErrorStr(st));
            goto fail;
        }
    }
    /* "Mesa Gallium driver 24.3.4 for AMD Custom GPU 0932 (radeonsi, vangogh, …)" → "radeonsi, Mesa 24.3.4" */
    const char *vendor = p_vaQueryVendorString(dpy), *paren = vendor ? strchr(vendor, '(') : NULL, *mesa;
    char drv[64] = "";
    if (paren)
        snprintf(drv, sizeof drv, "%.*s", (int)strcspn(paren + 1, ",)"), paren + 1);
    if (vendor && (mesa = strstr(vendor, "Mesa Gallium driver ")))
        snprintf(drv + strlen(drv), sizeof drv - strlen(drv), "%sMesa %.*s", *drv ? ", " : "", (int)strcspn(mesa + 20, " "), mesa + 20);
    else if (!*drv && vendor)
        snprintf(drv, sizeof drv, "%.40s", vendor);
    snprintf(e->name, sizeof e->name, "VA-API %s (%s)", codec == VC_H265 ? "H.265" : "H.264", *drv ? drv : "?");
    return e;
fail:
    vaenc_close(e);
    return NULL;
}

static void copy_nv12(uint8_t *dst, const VAImage *im, const uint8_t *nv12, int w, int h)
{
    for (int y = 0; y < h; y++)
        memcpy(dst + im->offsets[0] + (size_t)y * im->pitches[0], nv12 + (size_t)y * w, (size_t)w);
    for (int y = 0; y < h / 2; y++)
        memcpy(dst + im->offsets[1] + (size_t)y * im->pitches[1], nv12 + (size_t)w * h + (size_t)y * w, (size_t)w);
}

static int upload(vaenc_t *e, const uint8_t *nv12)
{
    VADisplay dpy = e->dev.dpy;
    VAImage im;
    void *p = NULL;
    if (e->derive) {
        if (p_vaDeriveImage(dpy, e->src, &im) != VA_STATUS_SUCCESS)
            return -1;
        int ok = p_vaMapBuffer(dpy, im.buf, &p) == VA_STATUS_SUCCESS;
        if (ok) {
            copy_nv12(p, &im, nv12, e->w, e->h);
            p_vaUnmapBuffer(dpy, im.buf);
        }
        p_vaDestroyImage(dpy, im.image_id);
        return ok ? 0 : -1;
    }
    if (p_vaMapBuffer(dpy, e->img.buf, &p) != VA_STATUS_SUCCESS)
        return -1;
    copy_nv12(p, &e->img, nv12, e->w, e->h);
    p_vaUnmapBuffer(dpy, e->img.buf);
    return p_vaPutImage(dpy, e->src, e->img.image_id, 0, 0, (unsigned)e->w, (unsigned)e->h, 0, 0, (unsigned)e->w,
                        (unsigned)e->h) == VA_STATUS_SUCCESS ? 0 : -1;
}

const uint8_t *vaenc_frame(vaenc_t *e, const uint8_t *nv12, int key, size_t *n)
{
    *n = 0;
    if (upload(e, nv12) < 0) {
        LOGW("video: VA-API: cannot upload the picture");
        return NULL;
    }
    if (!e->have_ref)
        key = 1;
    if (key) {
        e->n = 0;
        e->idr_id++;
    }
    VABufferID b[16];
    int nb = e->codec == VC_H265 ? hevc_buffers(e, key, b) : h264_buffers(e, key, b);
    e->rc_changed = 0;
    VADisplay dpy = e->dev.dpy;
    VAStatus st = VA_STATUS_SUCCESS;
    for (int i = 0; i < nb; i++)
        if (b[i] == VA_INVALID_ID)
            st = VA_STATUS_ERROR_ALLOCATION_FAILED;
    if (st == VA_STATUS_SUCCESS && (st = p_vaBeginPicture(dpy, e->ctx, e->src)) == VA_STATUS_SUCCESS) {
        st = p_vaRenderPicture(dpy, e->ctx, b, nb);
        VAStatus end = p_vaEndPicture(dpy, e->ctx);
        if (st == VA_STATUS_SUCCESS)
            st = end;
    }
    if (st == VA_STATUS_SUCCESS)
        st = p_vaSyncSurface(dpy, e->src);
    for (int i = 0; i < nb; i++)
        if (b[i] != VA_INVALID_ID)
            p_vaDestroyBuffer(dpy, b[i]);
    VACodedBufferSegment *seg = NULL;
    if (st == VA_STATUS_SUCCESS)
        st = p_vaMapBuffer(dpy, e->coded, (void **)&seg);
    if (st != VA_STATUS_SUCCESS) {
        LOGW("video: VA-API encode: %s", p_vaErrorStr(st));
        e->have_ref = 0; /* start over with an IDR */
        return NULL;
    }
    size_t total = 0;
    for (VACodedBufferSegment *s = seg; s; s = s->next)
        total += s->size;
    if (total > e->out_cap) {
        free(e->out);
        e->out_cap = total + 65536;
        e->out = malloc(e->out_cap);
    }
    total = 0;
    for (VACodedBufferSegment *s = seg; s; s = s->next) {
        memcpy(e->out + total, s->buf, s->size);
        total += s->size;
    }
    p_vaUnmapBuffer(dpy, e->coded);
    e->have_ref = 1;
    e->cur ^= 1;
    e->n++;
    *n = total;
    return e->out;
}

void vaenc_bitrate(vaenc_t *e, int kbps)
{
    if (kbps > 0 && kbps != e->kbps) {
        e->kbps = kbps;
        e->rc_changed = 1;
    }
}

const char *vaenc_name(vaenc_t *e) { return e->name; }

/* ---------------- dsk_hwvideo: X11 capture → NV12 → VA-API, or → BGRA/NV12 → MPP ---------------- */

struct dsk_hwvideo {
    dsk_video_cfg_t cfg;
    dsk_au_cb cb;
    void *ud;
    dsk_capture_t *cap;
    vaenc_t *enc;
    mppenc_t *mpp;      /* instead of enc on Rockchip */
    int w, h;           /* the encode size: the fitted size cut to multiples of 16 */
    uint8_t *i420, *nv12;
    int have_frame, fresh, want_key, sent;
    uint64_t last_ms;
};

dsk_hwvideo_t *dsk_hwvideo_open(const dsk_video_cfg_t *c, dsk_au_cb cb, void *ud, char *err, size_t cap)
{
    dsk_hwvideo_t *v = calloc(1, sizeof *v);
    v->cfg = *c;
    v->cb = cb;
    v->ud = ud;
    v->w = c->width & ~15;
    v->h = c->height & ~15;
    char why[200] = "";
    if (mppenc_caps() && !(v->mpp = mppenc_open(c->codec, c->high, v->w, v->h, c->fps, c->kbps, why, sizeof why)))
        LOGW("video: %s; trying VA-API", why);
    if ((!v->mpp && !(v->enc = vaenc_open(c->codec, c->high, v->w, v->h, c->fps, c->kbps, err, cap))) ||
        !(v->cap = dsk_capture_open(&c->screen, err, cap))) {
        dsk_hwvideo_close(v);
        return NULL;
    }
    size_t ysz = (size_t)v->w * v->h;
    v->i420 = malloc(ysz * 3 / 2);
    v->nv12 = malloc(ysz * 3 / 2);
    return v;
}

int dsk_hwvideo_step(dsk_hwvideo_t *v, int timeout_ms)
{
    int frame_ms = 1000 / (v->cfg.fps > 0 ? v->cfg.fps : 60);
    uint64_t now = rd_mono_ms();
    int wait = timeout_ms;
    if (v->fresh) /* a new picture waits for its turn: no more than fps */
        wait = (int)RD_MAX(0, (int64_t)(v->last_ms + frame_ms) - (int64_t)now);
    dsk_frame_t f;
    int r = dsk_capture_next(v->cap, &f, wait);
    if (r < 0) { /* mode change: reopen the capture, keep the encoder */
        dsk_capture_close(v->cap);
        char err[200];
        rd_sleep_ms(100);
        v->cap = dsk_capture_open(&v->cfg.screen, err, sizeof err);
        return v->cap ? 0 : -1;
    }
    if (r > 0 && v->mpp && f.w == v->w && f.h == v->h) { /* the encoder converts BGRA itself */
        if (mppenc_load_bgra(v->mpp, f.bgra, f.stride) < 0)
            return -1;
        v->have_frame = v->fresh = 1;
    } else if (r > 0) {
        size_t ysz = (size_t)v->w * v->h;
        yuv_from_bgra(f.bgra, f.w, f.h, f.stride, v->i420, v->i420 + ysz, v->i420 + ysz * 5 / 4, v->w, v->h);
        memcpy(v->nv12, v->i420, ysz);
        yuv_nv12_uv(v->i420 + ysz, v->i420 + ysz * 5 / 4, v->nv12 + ysz, v->w, v->h);
        if (v->mpp && mppenc_load_nv12(v->mpp, v->nv12) < 0)
            return -1;
        v->have_frame = v->fresh = 1;
    }
    now = rd_mono_ms();
    if (!v->have_frame)
        return 0;
    /* a changed screen once its turn comes, a keyframe request, or a refresh of a still one every 500 ms */
    int due = v->fresh && now >= v->last_ms + (uint64_t)frame_ms;
    if (!v->want_key && !due && now - v->last_ms < 500)
        return 0;
    int key = v->want_key || !v->sent;
    size_t n;
    int is_key = key;
    const uint8_t *au = v->mpp ? mppenc_frame(v->mpp, key, &n, &is_key) : vaenc_frame(v->enc, v->nv12, key, &n);
    if (!au)
        return -1;
    v->want_key = v->fresh = 0;
    v->sent = 1;
    v->last_ms = now;
    if (n)
        v->cb(v->ud, au, n, (int64_t)now * 1000, is_key);
    return 0;
}

void dsk_hwvideo_keyframe(dsk_hwvideo_t *v) { v->want_key = 1; }
void dsk_hwvideo_bitrate(dsk_hwvideo_t *v, int kbps)
{
    v->cfg.kbps = kbps;
    if (v->mpp)
        mppenc_bitrate(v->mpp, kbps);
    else
        vaenc_bitrate(v->enc, kbps);
}

void dsk_hwvideo_close(dsk_hwvideo_t *v)
{
    if (!v)
        return;
    if (v->cap)
        dsk_capture_close(v->cap);
    vaenc_close(v->enc);
    mppenc_close(v->mpp);
    free(v->i420);
    free(v->nv12);
    free(v);
}

int dsk_hwvideo_has_h265(void) { return ((mppenc_caps() | vaenc_caps()) & 2) != 0; }
const char *dsk_hwvideo_name(dsk_hwvideo_t *v) { return v->mpp ? mppenc_name(v->mpp) : vaenc_name(v->enc); }
