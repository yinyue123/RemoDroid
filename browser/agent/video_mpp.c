/* Hardware video on Rockchip SoCs through MPP (the OECT's RK3566: its VEPU540 encoder, rated 1080p60).
 * librockchip_mpp is loaded with dlopen, so the agent still starts where it isn't installed and the
 * hardware-video layer falls back to VA-API, then software.
 *
 * The encoder reads B, G, R, A bytes itself (its own colour conversion), so a captured X11 picture
 * goes in as it is: one copy into a DMA buffer, no YUV work on the CPU. A picture the viewer wants
 * smaller is scaled to NV12 by the caller first; MPP doesn't scale. Low delay as the VA-API path: P
 * frames only (this encoder has no B frames), CBR, a long GOP with an IDR on request, and the
 * parameter sets in front of every IDR.
 *
 * In a container MPP needs /dev/mpp_service, the /dev/dma_heap heaps and /proc/device-tree/compatible (how it
 * tells which SoC it is on): hqnode's "rkmpp" device option (remote-browser-plan.md). */
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "desk.h"
#include "mpp_api.h"
#include "mppenc.h"

/* ---------------- librockchip_mpp, loaded at run time ---------------- */

static struct {
    RDMPP_RET (*create)(RdMppCtx *, RdMppApi **);
    RDMPP_RET (*init)(RdMppCtx, int, int);
    RDMPP_RET (*destroy)(RdMppCtx);
    RDMPP_RET (*cfg_init)(RdMppEncCfg *);           /* mpp_enc_cfg_init (every version) */
    RDMPP_RET (*cfg_deinit)(RdMppEncCfg);
    RDMPP_RET (*cfg_set_s32)(RdMppEncCfg, const char *, int32_t);
    RDMPP_RET (*group_get)(RdMppBufferGroup *, int, int, const char *, const char *);
    RDMPP_RET (*group_put)(RdMppBufferGroup);
    RDMPP_RET (*buf_get)(RdMppBufferGroup, RdMppBuffer *, size_t, const char *, const char *);
    RDMPP_RET (*buf_put)(RdMppBuffer, const char *);
    void *(*buf_ptr)(RdMppBuffer, const char *);
    RDMPP_RET (*sync_begin)(RdMppBuffer, int32_t, const char *); /* optional: older MPP has none */
    RDMPP_RET (*sync_end)(RdMppBuffer, int32_t, const char *);
    RDMPP_RET (*frame_init)(RdMppFrame *);
    RDMPP_RET (*frame_deinit)(RdMppFrame *);
    void (*frame_set_width)(RdMppFrame, uint32_t);
    void (*frame_set_height)(RdMppFrame, uint32_t);
    void (*frame_set_hor_stride)(RdMppFrame, uint32_t);
    void (*frame_set_ver_stride)(RdMppFrame, uint32_t);
    void (*frame_set_fmt)(RdMppFrame, int);
    void (*frame_set_buffer)(RdMppFrame, RdMppBuffer);
    void (*frame_set_eos)(RdMppFrame, uint32_t);
    void *(*packet_pos)(const RdMppPacket);
    size_t (*packet_len)(const RdMppPacket);
    RDMPP_RET (*packet_deinit)(RdMppPacket *);
} M;

static pthread_once_t mpp_once = PTHREAD_ONCE_INIT;
static int mpp_ok;

static void mpp_load(void)
{
    void *h = dlopen("librockchip_mpp.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!h)
        h = dlopen("librockchip_mpp.so", RTLD_NOW | RTLD_LOCAL);
    if (!h)
        return;
    struct { void **p; const char *name; int optional; } t[] = {
        { (void **)&M.create, "mpp_create" },        { (void **)&M.init, "mpp_init" },
        { (void **)&M.destroy, "mpp_destroy" },      { (void **)&M.cfg_init, "mpp_enc_cfg_init" },
        { (void **)&M.cfg_deinit, "mpp_enc_cfg_deinit" }, { (void **)&M.cfg_set_s32, "mpp_enc_cfg_set_s32" },
        { (void **)&M.group_get, "mpp_buffer_group_get" }, { (void **)&M.group_put, "mpp_buffer_group_put" },
        { (void **)&M.buf_get, "mpp_buffer_get_with_tag" }, { (void **)&M.buf_put, "mpp_buffer_put_with_caller" },
        { (void **)&M.buf_ptr, "mpp_buffer_get_ptr_with_caller" },
        { (void **)&M.sync_begin, "mpp_buffer_sync_begin_f", 1 }, { (void **)&M.sync_end, "mpp_buffer_sync_end_f", 1 },
        { (void **)&M.frame_init, "mpp_frame_init" }, { (void **)&M.frame_deinit, "mpp_frame_deinit" },
        { (void **)&M.frame_set_width, "mpp_frame_set_width" }, { (void **)&M.frame_set_height, "mpp_frame_set_height" },
        { (void **)&M.frame_set_hor_stride, "mpp_frame_set_hor_stride" },
        { (void **)&M.frame_set_ver_stride, "mpp_frame_set_ver_stride" },
        { (void **)&M.frame_set_fmt, "mpp_frame_set_fmt" }, { (void **)&M.frame_set_buffer, "mpp_frame_set_buffer" },
        { (void **)&M.frame_set_eos, "mpp_frame_set_eos" },
        { (void **)&M.packet_pos, "mpp_packet_get_pos" }, { (void **)&M.packet_len, "mpp_packet_get_length" },
        { (void **)&M.packet_deinit, "mpp_packet_deinit" },
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
        if (!(*t[i].p = dlsym(h, t[i].name)) && !t[i].optional) {
            LOGW("video: librockchip_mpp has no %s; not using it", t[i].name);
            dlclose(h);
            return;
        }
    mpp_ok = 1;
}

int mppenc_caps(void)
{
    static int caps = -1;
    if (caps >= 0)
        return caps;
    int fd = open("/dev/mpp_service", O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return caps = 0;
    close(fd);
    pthread_once(&mpp_once, mpp_load);
    return caps = mpp_ok ? 3 : 0; /* RK3566/RK3568's VEPU540 does both */
}

/* ---------------- one encoder ---------------- */

#define ALIGN(x, a) (((x) + (a) - 1) / (a) * (a))

struct mppenc {
    RdMppCtx ctx;
    RdMppApi *mpi;
    RdMppEncCfg cfg;
    RdMppBufferGroup group;
    RdMppBuffer buf;
    uint8_t *ptr;
    int codec, w, h, fps, fmt, hstride, vstride;
    uint8_t *out;
    size_t out_cap;
    char name[48];
};

static const char TAG[] = "remodroid";

static void set_rate(mppenc_t *e, int kbps)
{
    int bps = kbps * 1000;
    M.cfg_set_s32(e->cfg, "rc:mode", RDMPP_ENC_RC_MODE_CBR);
    M.cfg_set_s32(e->cfg, "rc:bps_target", bps);
    M.cfg_set_s32(e->cfg, "rc:bps_max", bps / 16 * 17);
    M.cfg_set_s32(e->cfg, "rc:bps_min", bps / 16 * 15);
}

/* the input layout: BGRA rows (stride in bytes, width aligned to 8 for the VEPU) or NV12 */
static int set_input(mppenc_t *e, int fmt)
{
    if (e->fmt == fmt)
        return 0;
    e->fmt = fmt;
    e->hstride = fmt == RDMPP_FMT_BGRA8888 ? ALIGN(e->w, 8) * 4 : ALIGN(e->w, 16);
    e->vstride = ALIGN(e->h, 16);
    M.cfg_set_s32(e->cfg, "prep:width", e->w);
    M.cfg_set_s32(e->cfg, "prep:height", e->h);
    M.cfg_set_s32(e->cfg, "prep:hor_stride", e->hstride);
    M.cfg_set_s32(e->cfg, "prep:ver_stride", e->vstride);
    M.cfg_set_s32(e->cfg, "prep:format", fmt);
    return e->mpi->control(e->ctx, RDMPP_ENC_SET_CFG, e->cfg) ? -1 : 0;
}

mppenc_t *mppenc_open(int codec, int high, int w, int h, int fps, int kbps, char *err, size_t cap)
{
    if (!mppenc_caps()) {
        snprintf(err, cap, "no Rockchip MPP (/dev/mpp_service, librockchip_mpp.so.1)");
        return NULL;
    }
    if (w < 64 || h < 64 || w % 16 || h % 16) {
        snprintf(err, cap, "MPP: %dx%d isn't a multiple of 16", w, h);
        return NULL;
    }
    mppenc_t *e = calloc(1, sizeof *e);
    e->codec = codec;
    e->w = w;
    e->h = h;
    e->fps = fps > 0 ? fps : 30;
    e->fmt = -1;
    int coding = codec == VC_H265 ? RDMPP_VIDEO_CodingHEVC : RDMPP_VIDEO_CodingAVC;
    const char *what = "";
    int block = RDMPP_POLL_BLOCK;
    if (M.create(&e->ctx, &e->mpi) || !e->mpi) {
        what = "mpp_create";
        goto fail;
    }
    e->mpi->control(e->ctx, RDMPP_SET_OUTPUT_TIMEOUT, &block); /* encode_get_packet waits for the frame */
    if (M.init(e->ctx, RDMPP_CTX_ENC, coding)) {
        what = "mpp_init (is this a Rockchip encoder, and can MPP read /proc/device-tree/compatible?)";
        goto fail;
    }
    if (M.cfg_init(&e->cfg) || e->mpi->control(e->ctx, RDMPP_ENC_GET_CFG, e->cfg)) {
        what = "encoder settings";
        goto fail;
    }
    M.cfg_set_s32(e->cfg, "codec:type", coding);
    M.cfg_set_s32(e->cfg, "rc:fps_in_flex", 0);
    M.cfg_set_s32(e->cfg, "rc:fps_in_num", e->fps);
    M.cfg_set_s32(e->cfg, "rc:fps_in_denom", 1);
    M.cfg_set_s32(e->cfg, "rc:fps_out_flex", 0);
    M.cfg_set_s32(e->cfg, "rc:fps_out_num", e->fps);
    M.cfg_set_s32(e->cfg, "rc:fps_out_denom", 1);
    M.cfg_set_s32(e->cfg, "rc:gop", e->fps * 60); /* keyframes come on request (loss, a new viewer) */
    set_rate(e, kbps);
    if (codec == VC_H264) {
        /* 66 is Constrained Baseline here (constraint_set0 and 1), what the SDP offers by default */
        M.cfg_set_s32(e->cfg, "h264:profile", high ? 100 : 66);
        M.cfg_set_s32(e->cfg, "h264:level", w * h > 2097152 ? 51 : 42);
        M.cfg_set_s32(e->cfg, "h264:cabac_en", high ? 1 : 0);
        M.cfg_set_s32(e->cfg, "h264:trans8x8", high ? 1 : 0);
    }
    if (set_input(e, RDMPP_FMT_BGRA8888) < 0) {
        what = "encoder settings for BGRA input";
        goto fail;
    }
    int mode = RDMPP_ENC_HEADER_MODE_EACH_IDR, sei = RDMPP_ENC_SEI_MODE_DISABLE;
    e->mpi->control(e->ctx, RDMPP_ENC_SET_HEADER_MODE, &mode);
    e->mpi->control(e->ctx, RDMPP_ENC_SET_SEI_CFG, &sei);
    /* one buffer big enough for either layout (BGRA is the larger) */
    size_t size = (size_t)ALIGN(ALIGN(w, 8) * 4, 64) * ALIGN(h, 64);
    if (M.group_get(&e->group, RDMPP_BUFFER_TYPE_DRM | RDMPP_BUFFER_FLAGS_CACHABLE, RDMPP_BUFFER_INTERNAL, TAG, __func__) ||
        M.buf_get(e->group, &e->buf, size, TAG, __func__) || !(e->ptr = M.buf_ptr(e->buf, __func__))) {
        what = "a DMA buffer (/dev/dma_heap)";
        goto fail;
    }
    snprintf(e->name, sizeof e->name, "Rockchip MPP %s", codec == VC_H265 ? "H.265" : "H.264");
    return e;
fail:
    snprintf(err, cap, "MPP: %s failed", what);
    mppenc_close(e);
    return NULL;
}

int mppenc_load_bgra(mppenc_t *e, const uint8_t *bgra, int stride)
{
    if (set_input(e, RDMPP_FMT_BGRA8888) < 0)
        return -1;
    if (M.sync_begin)
        M.sync_begin(e->buf, 0, __func__);
    for (int y = 0; y < e->h; y++)
        memcpy(e->ptr + (size_t)y * e->hstride, bgra + (size_t)y * stride, (size_t)e->w * 4);
    if (M.sync_end)
        M.sync_end(e->buf, 0, __func__);
    return 0;
}

int mppenc_load_nv12(mppenc_t *e, const uint8_t *nv12)
{
    if (set_input(e, RDMPP_FMT_YUV420SP) < 0)
        return -1;
    if (M.sync_begin)
        M.sync_begin(e->buf, 0, __func__);
    for (int y = 0; y < e->h; y++) /* Y, then UV at ver_stride rows */
        memcpy(e->ptr + (size_t)y * e->hstride, nv12 + (size_t)y * e->w, (size_t)e->w);
    uint8_t *uv = e->ptr + (size_t)e->hstride * e->vstride;
    for (int y = 0; y < e->h / 2; y++)
        memcpy(uv + (size_t)y * e->hstride, nv12 + (size_t)e->w * e->h + (size_t)y * e->w, (size_t)e->w);
    if (M.sync_end)
        M.sync_end(e->buf, 0, __func__);
    return 0;
}

/* an IDR among the NAL units: H.265 types 19-21 (IDR, CRA), H.264 type 5 */
static int has_idr(const uint8_t *p, size_t n, int hevc)
{
    for (size_t i = 0; i + 3 < n; i++)
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            int t = hevc ? (p[i + 3] >> 1) & 0x3f : p[i + 3] & 0x1f;
            if (hevc ? t >= 19 && t <= 21 : t == 5)
                return 1;
            i += 2;
        }
    return 0;
}

const uint8_t *mppenc_frame(mppenc_t *e, int key, size_t *n, int *is_key)
{
    if (key)
        e->mpi->control(e->ctx, RDMPP_ENC_SET_IDR_FRAME, NULL);
    RdMppFrame f = NULL;
    if (M.frame_init(&f))
        return NULL;
    M.frame_set_width(f, (uint32_t)e->w);
    M.frame_set_height(f, (uint32_t)e->h);
    M.frame_set_hor_stride(f, (uint32_t)e->hstride);
    M.frame_set_ver_stride(f, (uint32_t)e->vstride);
    M.frame_set_fmt(f, e->fmt);
    M.frame_set_buffer(f, e->buf);
    M.frame_set_eos(f, 0);
    RDMPP_RET r = e->mpi->encode_put_frame(e->ctx, f);
    M.frame_deinit(&f);
    if (r) {
        LOGW("video: MPP encode_put_frame: %d", r);
        return NULL;
    }
    RdMppPacket p = NULL;
    if (e->mpi->encode_get_packet(e->ctx, &p) || !p) {
        LOGW("video: MPP gave no packet");
        return NULL;
    }
    size_t len = M.packet_len(p);
    if (len > e->out_cap) {
        free(e->out);
        e->out_cap = len + len / 2;
        e->out = malloc(e->out_cap);
    }
    memcpy(e->out, M.packet_pos(p), len);
    M.packet_deinit(&p);
    *n = len;
    *is_key = has_idr(e->out, len, e->codec == VC_H265);
    return e->out;
}

void mppenc_bitrate(mppenc_t *e, int kbps)
{
    set_rate(e, kbps);
    e->mpi->control(e->ctx, RDMPP_ENC_SET_CFG, e->cfg);
}

const char *mppenc_name(mppenc_t *e) { return e->name; }

void mppenc_close(mppenc_t *e)
{
    if (!e)
        return;
    if (e->buf)
        M.buf_put(e->buf, __func__);
    if (e->group)
        M.group_put(e->group);
    if (e->cfg)
        M.cfg_deinit(e->cfg);
    if (e->ctx)
        M.destroy(e->ctx);
    free(e->out);
    free(e);
}
