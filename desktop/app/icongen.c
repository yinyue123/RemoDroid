/* Build-time tool: writes the app icon as PNG, ICO (Windows) or ICNS (macOS)
 * from desktop/app/icon.c. No zlib: PNG data is compressed with a small
 * fixed-Huffman deflate that only looks for repeats of the previous pixel,
 * which is most of an icon.
 *   icongen out.png SIZE [style]   icongen out.ico   icongen out.icns */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "icon.h"

typedef struct {
    unsigned char *p;
    size_t n, cap;
} buf_t;

static void put(buf_t *b, const void *d, size_t n)
{
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2;
        b->p = realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}

static void be32(buf_t *b, unsigned v)
{
    unsigned char x[4] = { (unsigned char)(v >> 24), (unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v };
    put(b, x, 4);
}

static unsigned crc32(const unsigned char *d, size_t n)
{
    unsigned c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++)
            c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    return ~c;
}

static void chunk(buf_t *b, const char *type, const unsigned char *d, size_t n)
{
    be32(b, (unsigned)n);
    size_t start = b->n;
    put(b, type, 4);
    if (n)
        put(b, d, n);
    be32(b, crc32(b->p + start, n + 4));
}

/* ---- deflate, fixed Huffman codes (RFC 1951 3.2.6), matches at distance 4 (one pixel back) ---- */
typedef struct {
    buf_t *b;
    unsigned acc;
    int nbits;
} bits_t;

static void bits(bits_t *w, unsigned v, int n) /* LSB first */
{
    for (int i = 0; i < n; i++) {
        w->acc |= ((v >> i) & 1u) << w->nbits++;
        if (w->nbits == 8) {
            unsigned char c = (unsigned char)w->acc;
            put(w->b, &c, 1);
            w->acc = 0;
            w->nbits = 0;
        }
    }
}

static void huff(bits_t *w, unsigned code, int len) /* Huffman codes go MSB first */
{
    for (int i = len - 1; i >= 0; i--)
        bits(w, (code >> i) & 1u, 1);
}

static void sym(bits_t *w, int v)
{
    if (v < 144)
        huff(w, 0x30 + (unsigned)v, 8);
    else if (v < 256)
        huff(w, 0x190 + (unsigned)(v - 144), 9);
    else if (v < 280)
        huff(w, (unsigned)(v - 256), 7);
    else
        huff(w, 0xC0 + (unsigned)(v - 280), 8);
}

static void deflate_fixed(buf_t *out, const unsigned char *d, size_t n)
{
    static const int base[] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99,
                                115, 131, 163, 195, 227, 258 };
    static const int extra[] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
    bits_t w = { out, 0, 0 };
    bits(&w, 1, 1); /* final block */
    bits(&w, 1, 2); /* fixed Huffman */
    for (size_t i = 0; i < n;) {
        size_t len = 0;
        while (i >= 4 && i + len < n && len < 258 && d[i + len] == d[i + len - 4])
            len++;
        if (len < 3) {
            sym(&w, d[i++]);
            continue;
        }
        int k = 28;
        while (base[k] > (int)len)
            k--;
        sym(&w, 257 + k);
        bits(&w, (unsigned)((int)len - base[k]), extra[k]);
        huff(&w, 3, 5); /* distance 4: code 3, no extra bits */
        i += len;
    }
    sym(&w, 256);
    if (w.nbits)
        bits(&w, 0, 8 - w.nbits);
}

static buf_t png(int size, int style)
{
    uint32_t *px = malloc(sizeof *px * (size_t)size * size);
    rd_icon_render(px, size, style);
    buf_t raw = { 0 }, z = { 0 }, out = { 0 };
    for (int y = 0; y < size; y++) { /* filter byte 0, then RGBA */
        unsigned char f = 0;
        put(&raw, &f, 1);
        for (int x = 0; x < size; x++) {
            uint32_t c = px[y * size + x];
            unsigned char rgba[4] = { (unsigned char)(c >> 16), (unsigned char)(c >> 8), (unsigned char)c,
                                      (unsigned char)(c >> 24) };
            put(&raw, rgba, 4);
        }
    }
    unsigned char hdr[2] = { 0x78, 0x01 }; /* zlib header */
    put(&z, hdr, 2);
    deflate_fixed(&z, raw.p, raw.n);
    unsigned a = 1, bb = 0;
    for (size_t i = 0; i < raw.n; i++) {
        a = (a + raw.p[i]) % 65521;
        bb = (bb + a) % 65521;
    }
    be32(&z, bb << 16 | a);
    static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    put(&out, sig, 8);
    unsigned char ihdr[13] = { 0 };
    ihdr[0] = (unsigned char)(size >> 24), ihdr[1] = (unsigned char)(size >> 16), ihdr[2] = (unsigned char)(size >> 8),
    ihdr[3] = (unsigned char)size;
    memcpy(ihdr + 4, ihdr, 4);
    ihdr[8] = 8, ihdr[9] = 6; /* 8-bit RGBA */
    chunk(&out, "IHDR", ihdr, 13);
    chunk(&out, "IDAT", z.p, z.n);
    chunk(&out, "IEND", NULL, 0);
    free(px);
    free(raw.p);
    free(z.p);
    return out;
}

static void le16(buf_t *b, unsigned v)
{
    unsigned char x[2] = { (unsigned char)v, (unsigned char)(v >> 8) };
    put(b, x, 2);
}

static void le32(buf_t *b, unsigned v)
{
    le16(b, v & 0xFFFF);
    le16(b, v >> 16);
}

static int write_file(const char *path, buf_t *b)
{
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(b->p, 1, b->n, f) != b->n) {
        fprintf(stderr, "icongen: cannot write %s\n", path);
        return 1;
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: icongen out.png SIZE [style] | out.ico | out.icns\n");
        return 2;
    }
    const char *out = argv[1], *ext = strrchr(out, '.');
    buf_t b = { 0 };
    if (ext && !strcmp(ext, ".png")) {
        buf_t p = png(argc > 2 ? atoi(argv[2]) : 256, argc > 3 ? atoi(argv[3]) : RD_ICON_APP);
        return write_file(out, &p);
    }
    if (ext && !strcmp(ext, ".ico")) { /* PNG-compressed entries (Windows Vista+) */
        static const int sizes[] = { 16, 20, 24, 32, 40, 48, 64, 256 };
        const int n = sizeof sizes / sizeof sizes[0];
        buf_t imgs[sizeof sizes / sizeof sizes[0]];
        le16(&b, 0);
        le16(&b, 1);
        le16(&b, (unsigned)n);
        unsigned off = 6 + 16 * (unsigned)n;
        for (int i = 0; i < n; i++) {
            imgs[i] = png(sizes[i], RD_ICON_APP);
            unsigned char dim = (unsigned char)(sizes[i] >= 256 ? 0 : sizes[i]);
            unsigned char e[4] = { dim, dim, 0, 0 };
            put(&b, e, 4);
            le16(&b, 1);
            le16(&b, 32);
            le32(&b, (unsigned)imgs[i].n);
            le32(&b, off);
            off += (unsigned)imgs[i].n;
        }
        for (int i = 0; i < n; i++)
            put(&b, imgs[i].p, imgs[i].n);
        return write_file(out, &b);
    }
    if (ext && !strcmp(ext, ".icns")) {
        static const struct { const char *type; int size; } E[] = {
            { "icp4", 16 }, { "icp5", 32 }, { "ic11", 32 }, { "ic12", 64 }, { "ic07", 128 },
            { "ic13", 256 }, { "ic08", 256 }, { "ic14", 512 }, { "ic09", 512 }, { "ic10", 1024 },
        };
        buf_t body = { 0 };
        for (size_t i = 0; i < sizeof E / sizeof E[0]; i++) {
            buf_t p = png(E[i].size, RD_ICON_APP);
            put(&body, E[i].type, 4);
            be32(&body, (unsigned)p.n + 8);
            put(&body, p.p, p.n);
            free(p.p);
        }
        put(&b, "icns", 4);
        be32(&b, (unsigned)body.n + 8);
        put(&b, body.p, body.n);
        return write_file(out, &b);
    }
    fprintf(stderr, "icongen: unknown output type %s\n", out);
    return 2;
}
