/* The part of Rockchip MPP (github.com/rockchip-linux/mpp, Apache-2.0) that video_mpp.c uses, so the
 * agent builds without MPP's headers and loads librockchip_mpp.so.1 only where it exists (dlopen, as
 * libva). Values from inc/rk_type.h, rk_mpi_cmd.h, rk_venc_cmd.h, mpp_frame.h and mpp_buffer.h at
 * commit 20959ec3 (2026-10-16); they have been stable for years. RdMppApi's layout must match rk_mpi.h:
 * browser/test/mpptest.c compares it with the real header where MPP is installed. */
#ifndef RD_MPP_API_H
#define RD_MPP_API_H

#include <stddef.h>
#include <stdint.h>

typedef int32_t RDMPP_RET; /* 0 = MPP_OK, negative on failure */
typedef void *RdMppCtx, *RdMppFrame, *RdMppPacket, *RdMppBuffer, *RdMppBufferGroup, *RdMppEncCfg, *RdMppParam;

#define RDMPP_CTX_ENC 1
#define RDMPP_VIDEO_CodingAVC 0x7
#define RDMPP_VIDEO_CodingHEVC 0x1000004
#define RDMPP_FMT_YUV420SP 0x0     /* NV12 */
#define RDMPP_FMT_BGRA8888 0x1000c /* B, G, R, A bytes: a 24-bit X11 image as it is */

#define RDMPP_BUFFER_TYPE_DRM 3 /* served from /dev/dma_heap when the kernel has it */
#define RDMPP_BUFFER_FLAGS_CACHABLE 0x20000
#define RDMPP_BUFFER_INTERNAL 0

#define RDMPP_SET_OUTPUT_TIMEOUT 0x200007
#define RDMPP_ENC_SET_CFG 0x320001
#define RDMPP_ENC_GET_CFG 0x320002
#define RDMPP_ENC_SET_IDR_FRAME 0x320009
#define RDMPP_ENC_SET_SEI_CFG 0x32000f
#define RDMPP_ENC_SET_HEADER_MODE 0x328001

#define RDMPP_POLL_BLOCK (-1)
#define RDMPP_ENC_HEADER_MODE_EACH_IDR 1
#define RDMPP_ENC_SEI_MODE_DISABLE 0
#define RDMPP_ENC_RC_MODE_CBR 1

typedef struct {
    uint32_t size, version;
    RDMPP_RET (*decode)(RdMppCtx, RdMppPacket, RdMppFrame *);
    RDMPP_RET (*decode_put_packet)(RdMppCtx, RdMppPacket);
    RDMPP_RET (*decode_get_frame)(RdMppCtx, RdMppFrame *);
    RDMPP_RET (*encode)(RdMppCtx, RdMppFrame, RdMppPacket *);
    RDMPP_RET (*encode_put_frame)(RdMppCtx, RdMppFrame);
    RDMPP_RET (*encode_get_packet)(RdMppCtx, RdMppPacket *);
    RDMPP_RET (*isp)(RdMppCtx, RdMppFrame, RdMppFrame);
    RDMPP_RET (*isp_put_frame)(RdMppCtx, RdMppFrame);
    RDMPP_RET (*isp_get_frame)(RdMppCtx, RdMppFrame *);
    RDMPP_RET (*poll)(RdMppCtx, int, int);
    RDMPP_RET (*dequeue)(RdMppCtx, int, void **);
    RDMPP_RET (*enqueue)(RdMppCtx, int, void *);
    RDMPP_RET (*reset)(RdMppCtx);
    RDMPP_RET (*control)(RdMppCtx, int cmd, RdMppParam param);
    uint32_t reserv[16];
} RdMppApi;

#endif
