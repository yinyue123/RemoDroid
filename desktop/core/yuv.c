/* BGRA → I420 (BT.601, limited range: what H.264 decoders assume without VUI),
 * scaled to the encode size with bilinear filtering. Integer arithmetic, written
 * so compilers vectorize the unscaled path. */
#include <stdlib.h>
#include <string.h>

#include "desk.h"

#define Y_OF(r, g, b) (uint8_t)(((66 * (r) + 129 * (g) + 25 * (b) + 128) >> 8) + 16)
#define U_OF(r, g, b) (uint8_t)(((-38 * (r) - 74 * (g) + 112 * (b) + 128) >> 8) + 128)
#define V_OF(r, g, b) (uint8_t)(((112 * (r) - 94 * (g) - 18 * (b) + 128) >> 8) + 128)

/* unscaled: one pass per pair of rows */
static void convert_same(const uint8_t *src, int w, int h, int stride, uint8_t *y, uint8_t *u, uint8_t *v)
{
    for (int j = 0; j < h; j += 2) {
        const uint8_t *r0 = src + (size_t)j * stride, *r1 = j + 1 < h ? r0 + stride : r0;
        uint8_t *y0 = y + (size_t)j * w, *y1 = j + 1 < h ? y0 + w : y0;
        uint8_t *uu = u + (size_t)(j / 2) * (w / 2), *vv = v + (size_t)(j / 2) * (w / 2);
        for (int i = 0; i < w; i += 2) {
            const uint8_t *a = r0 + i * 4, *b = r1 + i * 4;
            y0[i] = Y_OF(a[2], a[1], a[0]);
            y0[i + 1] = Y_OF(a[6], a[5], a[4]);
            y1[i] = Y_OF(b[2], b[1], b[0]);
            y1[i + 1] = Y_OF(b[6], b[5], b[4]);
            int R = (a[2] + a[6] + b[2] + b[6] + 2) >> 2, G = (a[1] + a[5] + b[1] + b[5] + 2) >> 2,
                B = (a[0] + a[4] + b[0] + b[4] + 2) >> 2;
            uu[i / 2] = U_OF(R, G, B);
            vv[i / 2] = V_OF(R, G, B);
        }
    }
}

/* Scaled: each output pixel is a bilinear sample of the source; chroma averages the 2x2 block. */
typedef struct {
    int i0, i1, f; /* source index, next index, weight of next (0..256) */
} tap_t;

static tap_t *taps(int src, int dst)
{
    tap_t *t = malloc(sizeof *t * (size_t)dst);
    for (int i = 0; i < dst; i++) {
        long p = ((2L * i + 1) * src * 256) / (2L * dst) - 128; /* centre-aligned, 8.8 fixed point */
        if (p < 0)
            p = 0;
        int i0 = (int)(p >> 8);
        if (i0 >= src - 1) {
            t[i] = (tap_t){ src - 1, src - 1, 0 };
            continue;
        }
        t[i] = (tap_t){ i0, i0 + 1, (int)(p & 255) };
    }
    return t;
}

static inline void sample(const uint8_t *src, int stride, const tap_t *tx, const tap_t *ty, int *r, int *g, int *b)
{
    const uint8_t *p00 = src + (size_t)ty->i0 * stride + tx->i0 * 4, *p01 = src + (size_t)ty->i0 * stride + tx->i1 * 4;
    const uint8_t *p10 = src + (size_t)ty->i1 * stride + tx->i0 * 4, *p11 = src + (size_t)ty->i1 * stride + tx->i1 * 4;
    int fx = tx->f, fy = ty->f, v[3];
    for (int c = 0; c < 3; c++) {
        int top = p00[c] * (256 - fx) + p01[c] * fx, bot = p10[c] * (256 - fx) + p11[c] * fx;
        v[c] = (top * (256 - fy) + bot * fy + 32768) >> 16;
    }
    *b = v[0];
    *g = v[1];
    *r = v[2];
}

void yuv_from_bgra(const uint8_t *src, int sw, int sh, int stride, uint8_t *y, uint8_t *u, uint8_t *v, int dw, int dh)
{
    if (sw == dw && sh == dh) {
        convert_same(src, dw, dh, stride, y, u, v);
        return;
    }
    tap_t *tx = taps(sw, dw), *ty = taps(sh, dh);
    for (int j = 0; j < dh; j += 2) {
        uint8_t *y0 = y + (size_t)j * dw, *y1 = y0 + dw;
        uint8_t *uu = u + (size_t)(j / 2) * (dw / 2), *vv = v + (size_t)(j / 2) * (dw / 2);
        for (int i = 0; i < dw; i += 2) {
            int R = 0, G = 0, B = 0;
            for (int k = 0; k < 4; k++) {
                int r, g, b;
                sample(src, stride, &tx[i + (k & 1)], &ty[j + (k >> 1)], &r, &g, &b);
                (k >> 1 ? y1 : y0)[i + (k & 1)] = Y_OF(r, g, b);
                R += r, G += g, B += b;
            }
            uu[i / 2] = U_OF((R + 2) >> 2, (G + 2) >> 2, (B + 2) >> 2);
            vv[i / 2] = V_OF((R + 2) >> 2, (G + 2) >> 2, (B + 2) >> 2);
        }
    }
    free(tx);
    free(ty);
}

void video_fit(int sw, int sh, int short_side, int mult16, int *w, int *h)
{
    double s = 1.0;
    int ss = sw < sh ? sw : sh;
    if (short_side > 0 && ss > short_side)
        s = (double)short_side / ss;
    int m = mult16 ? 16 : 2;
    int ow = (int)(sw * s + m / 2) / m * m, oh = (int)(sh * s + m / 2) / m * m;
    *w = ow < m ? m : ow;
    *h = oh < m ? m : oh;
}

/* I420 chroma planes → NV12's interleaved UV plane (for encoders that want NV12) */
void yuv_nv12_uv(const uint8_t *u, const uint8_t *v, uint8_t *uv, int w, int h)
{
    size_t n = (size_t)(w / 2) * (h / 2);
    for (size_t i = 0; i < n; i++) {
        uv[2 * i] = u[i];
        uv[2 * i + 1] = v[i];
    }
}
