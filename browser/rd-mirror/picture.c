/* picture.c: a picture of the page's, made smaller for the viewer (browser/dom-mirror.md §6).
 *
 *   JPEG, PNG or a still WebP → pixels → no wider than the viewer shows it → WebP, quality 75
 *
 * Anything else (SVG, GIF, AVIF, an animated WebP, a font, a stylesheet) and anything under 20 KB goes as
 * it is; so does a picture whose WebP isn't smaller than what the site sent. */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>
#include <png.h>
#include <webp/decode.h>
#include <webp/encode.h>

#include "picture.h"

#define SMALL (20u << 10)
#define MAX_SIDE 16384
#define MAX_PIXELS (40u << 20) /* 160 MB of RGBA: a picture past this is left alone */

struct jerr {
    struct jpeg_error_mgr pub;
    jmp_buf jb;
};

static void jpeg_fail(j_common_ptr c) { longjmp(((struct jerr *)c->err)->jb, 1); }

static uint8_t *from_jpeg(const uint8_t *d, size_t n, int *w, int *h)
{
    struct jpeg_decompress_struct c;
    struct jerr e;
    uint8_t *volatile px = NULL;
    c.err = jpeg_std_error(&e.pub);
    e.pub.error_exit = jpeg_fail;
    if (setjmp(e.jb)) {
        jpeg_destroy_decompress(&c);
        free(px);
        return NULL;
    }
    jpeg_create_decompress(&c);
    jpeg_mem_src(&c, d, (unsigned long)n);
    jpeg_read_header(&c, TRUE);
    c.out_color_space = JCS_EXT_RGBA;
    jpeg_start_decompress(&c);
    if (c.output_width > MAX_SIDE || c.output_height > MAX_SIDE || (size_t)c.output_width * c.output_height > MAX_PIXELS)
        longjmp(e.jb, 1);
    *w = (int)c.output_width;
    *h = (int)c.output_height;
    px = malloc((size_t)*w * (size_t)*h * 4);
    if (!px)
        longjmp(e.jb, 1);
    while (c.output_scanline < c.output_height) {
        JSAMPROW row = px + (size_t)c.output_scanline * (size_t)*w * 4;
        jpeg_read_scanlines(&c, &row, 1);
    }
    jpeg_finish_decompress(&c);
    jpeg_destroy_decompress(&c);
    return px;
}

static uint8_t *from_png(const uint8_t *d, size_t n, int *w, int *h)
{
    png_image im;
    memset(&im, 0, sizeof im);
    im.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&im, d, n))
        return NULL;
    im.format = PNG_FORMAT_RGBA;
    uint8_t *px = im.width <= MAX_SIDE && im.height <= MAX_SIDE && (size_t)im.width * im.height <= MAX_PIXELS
                      ? malloc(PNG_IMAGE_SIZE(im)) : NULL;
    if (!px || !png_image_finish_read(&im, NULL, px, 0, NULL)) {
        png_image_free(&im);
        free(px);
        return NULL;
    }
    *w = (int)im.width;
    *h = (int)im.height;
    return px;
}

static uint8_t *from_webp(const uint8_t *d, size_t n, int *w, int *h)
{
    WebPBitstreamFeatures f;
    if (WebPGetFeatures(d, n, &f) != VP8_STATUS_OK || f.has_animation || f.width > MAX_SIDE || f.height > MAX_SIDE ||
        (size_t)f.width * (size_t)f.height > MAX_PIXELS)
        return NULL;
    uint8_t *p = WebPDecodeRGBA(d, n, w, h), *px = NULL;
    if (p && (px = malloc((size_t)*w * (size_t)*h * 4)))
        memcpy(px, p, (size_t)*w * (size_t)*h * 4);
    WebPFree(p);
    return px;
}

/* down to nw × nh, each new pixel the average of the old ones it covers */
static uint8_t *shrink(const uint8_t *px, int w, int h, int nw, int nh)
{
    uint8_t *o = malloc((size_t)nw * (size_t)nh * 4);
    if (!o)
        return NULL;
    for (int y = 0; y < nh; y++) {
        int y0 = (int)((int64_t)y * h / nh), y1 = (int)((int64_t)(y + 1) * h / nh);
        if (y1 <= y0)
            y1 = y0 + 1;
        for (int x = 0; x < nw; x++) {
            int x0 = (int)((int64_t)x * w / nw), x1 = (int)((int64_t)(x + 1) * w / nw);
            if (x1 <= x0)
                x1 = x0 + 1;
            uint32_t s[4] = { 0, 0, 0, 0 }, a = 0, cnt = (uint32_t)((x1 - x0) * (y1 - y0));
            for (int yy = y0; yy < y1; yy++) {
                const uint8_t *p = px + ((size_t)yy * (size_t)w + (size_t)x0) * 4;
                for (int xx = x0; xx < x1; xx++, p += 4) { /* colours weighted by their alpha: no dark fringes */
                    s[0] += (uint32_t)p[0] * p[3];
                    s[1] += (uint32_t)p[1] * p[3];
                    s[2] += (uint32_t)p[2] * p[3];
                    a += p[3];
                }
            }
            uint8_t *q = o + ((size_t)y * (size_t)nw + (size_t)x) * 4;
            q[0] = a ? (uint8_t)(s[0] / a) : 0;
            q[1] = a ? (uint8_t)(s[1] / a) : 0;
            q[2] = a ? (uint8_t)(s[2] / a) : 0;
            q[3] = (uint8_t)(a / cnt);
        }
    }
    return o;
}

uint8_t *picture_smaller(const uint8_t *d, size_t n, int max_w, size_t *out_n)
{
    int w = 0, h = 0;
    uint8_t *px = NULL;
    if (n < SMALL)
        return NULL;
    if (n >= 3 && !memcmp(d, "\xff\xd8\xff", 3))
        px = from_jpeg(d, n, &w, &h);
    else if (n >= 8 && !memcmp(d, "\x89PNG", 4))
        px = from_png(d, n, &w, &h);
    else if (n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4))
        px = from_webp(d, n, &w, &h);
    if (!px || w < 1 || h < 1) {
        free(px);
        return NULL;
    }
    if (max_w >= 16 && w > max_w) {
        int nh = (int)((int64_t)h * max_w / w);
        uint8_t *s = shrink(px, w, h, max_w, nh < 1 ? 1 : nh);
        free(px);
        if (!s)
            return NULL;
        px = s;
        h = nh < 1 ? 1 : nh;
        w = max_w;
    }
    uint8_t *webp = NULL, *out = NULL;
    size_t wn = WebPEncodeRGBA(px, w, h, w * 4, 75, &webp);
    free(px);
    if (wn && wn < n - n / 10 && (out = malloc(wn))) { /* worth it only if a tenth smaller */
        memcpy(out, webp, wn);
        *out_n = wn;
    }
    WebPFree(webp);
    return out;
}
