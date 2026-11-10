/* picture.c on real files: picture_test <max width> <file>…  prints what each became.
 * cc -o picture_test browser/test/mirror/picture_test.c browser/rd-mirror/picture.c -Ibrowser/rd-mirror -lwebp -lpng -ljpeg */
#include <stdio.h>
#include <stdlib.h>
#include <webp/decode.h>

#include "picture.h"

int main(int argc, char **argv)
{
    int bad = 0;
    for (int i = 2; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) {
            perror(argv[i]);
            return 2;
        }
        fseek(f, 0, SEEK_END);
        size_t n = (size_t)ftell(f), wn = 0;
        rewind(f);
        uint8_t *d = malloc(n);
        if (fread(d, 1, n, f) != n)
            return 2;
        fclose(f);
        uint8_t *w = picture_smaller(d, n, atoi(argv[1]), &wn);
        int ww = 0, wh = 0;
        if (w && !WebPGetInfo(w, wn, &ww, &wh)) {
            printf("%s: NOT A WEBP\n", argv[i]);
            bad = 1;
        } else if (w)
            printf("%s: %zu -> %zu bytes, WebP %dx%d\n", argv[i], n, wn, ww, wh);
        else
            printf("%s: %zu bytes, as it is\n", argv[i], n);
        free(w);
        free(d);
    }
    return bad;
}
