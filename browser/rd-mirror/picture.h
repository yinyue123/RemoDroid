/* picture.c: a page's picture made smaller for the viewer (browser/dom-mirror.md §6). */
#ifndef RD_PICTURE_H
#define RD_PICTURE_H

#include <stddef.h>
#include <stdint.h>

/* d: a JPEG, a PNG or a still WebP of n bytes; max_w: the widest the viewer can show it, in its own
 * pixels (0: any width). Returns a WebP (malloc'd, *out_n bytes) that is at least a tenth smaller, or NULL:
 * send the original. */
uint8_t *picture_smaller(const uint8_t *d, size_t n, int max_w, size_t *out_n);

#endif
