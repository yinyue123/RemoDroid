/* Hardware H.265 / H.264 on Rockchip SoCs (the OECT's RK3566) through MPP: video_mpp.c. The Linux
 * hardware-video layer (desktop/linux/video_vaapi.c) uses it in preference to VA-API when the device
 * and library are there. */
#ifndef RD_MPPENC_H
#define RD_MPPENC_H

#include <stddef.h>
#include <stdint.h>

typedef struct mppenc mppenc_t;

/* 3 (H.264 and H.265) when /dev/mpp_service opens and librockchip_mpp loads, else 0. */
int mppenc_caps(void);
/* codec: VC_H264 / VC_H265; high: H.264 High (else Constrained Baseline); w, h multiples of 16.
 * NULL with the reason in err. */
mppenc_t *mppenc_open(int codec, int high, int w, int h, int fps, int kbps, char *err, size_t cap);
/* The next picture, copied into the encoder's own buffer: BGRA at exactly w x h, or NV12 (Y plane w x h,
 * then interleaved UV) when the picture had to be scaled first. 0, or -1. */
int mppenc_load_bgra(mppenc_t *e, const uint8_t *bgra, int stride);
int mppenc_load_nv12(mppenc_t *e, const uint8_t *nv12);
/* Encodes the loaded picture (an IDR with VPS/SPS/PPS when key): an Annex-B access unit, valid until the
 * next call, and whether it is a keyframe. NULL on failure. */
const uint8_t *mppenc_frame(mppenc_t *e, int key, size_t *n, int *is_key);
void mppenc_bitrate(mppenc_t *e, int kbps);
const char *mppenc_name(mppenc_t *e);
void mppenc_close(mppenc_t *e);

#endif
