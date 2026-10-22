/* The VA-API encoder core (video_vaapi.c), apart from capture, so desktop/test/vatest.c can drive it
 * with synthetic frames on a machine without a display. */
#ifndef RD_VAENC_H
#define RD_VAENC_H

#include <stddef.h>
#include <stdint.h>

typedef struct vaenc vaenc_t;

/* codec: VC_H264 / VC_H265; w, h multiples of 16. NULL with the reason in err. */
vaenc_t *vaenc_open(int codec, int high, int w, int h, int fps, int kbps, char *err, size_t cap);
/* One NV12 frame (Y plane w x h, then interleaved UV), IDR when key: an Annex-B access unit, valid until
 * the next call. NULL on failure. */
const uint8_t *vaenc_frame(vaenc_t *e, const uint8_t *nv12, int key, size_t *n);
void vaenc_bitrate(vaenc_t *e, int kbps);
const char *vaenc_name(vaenc_t *e);
void vaenc_close(vaenc_t *e);
/* The hardware here encodes H.264 (1) / H.265 (2); 0 = neither or no VA-API. */
int vaenc_caps(void);

#endif
