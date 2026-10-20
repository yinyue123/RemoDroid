/* Unit tests for the shared desktop code (no display, no network):
 *   desk_test [web/app.js]
 * keymap.c against the viewer's table, input decoding, video sizes, colour
 * conversion and the software encoder. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "desk.h"
#include "stubs.h"

static int fails, checks;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(c)) {                                                                                                    \
            fails++;                                                                                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

/* ---------------- keymap ---------------- */

static void test_keymap(const char *app_js)
{
    size_t n;
    const keymap_t *k = keymap_all(&n);
    CHECK(n > 100, "only %zu keys", n);
    for (size_t i = 0; i < n; i++) {
        for (size_t j = i + 1; j < n; j++)
            CHECK(k[i].hid != k[j].hid, "duplicate usage 0x%02x", k[i].hid);
        CHECK(k[i].evdev != 0, "%s has no Linux code", k[i].code);
        CHECK(keymap_find(k[i].hid) == &k[i], "lookup of %s", k[i].code);
    }
    /* the viewer's KEYS table must name the same usages */
    FILE *f = app_js ? fopen(app_js, "rb") : NULL;
    if (!f) {
        printf("(skipping the web/app.js comparison)\n");
        return;
    }
    static char src[1 << 20];
    size_t len = fread(src, 1, sizeof src - 1, f);
    fclose(f);
    src[len] = 0;
    char *t = strstr(src, "const KEYS = {");
    CHECK(t != NULL, "KEYS table not found in %s", app_js);
    if (!t)
        return;
    char *end = strstr(t, "};");
    int seen = 0;
    for (char *p = t + 14; p && p < end;) {
        while (p < end && (*p == ' ' || *p == '\n' || *p == ','))
            p++;
        char name[40];
        unsigned v;
        int used;
        if (sscanf(p, "%39[A-Za-z0-9]: 0x%x%n", name, &v, &used) != 2)
            break;
        p += used;
        seen++;
        if (!strcmp(name, "OSLeft") || !strcmp(name, "OSRight")) /* old Firefox names */
            continue;
        const keymap_t *m = keymap_find((int)v);
        CHECK(m && !strcmp(m->code, name), "viewer %s = 0x%02x, keymap.c says %s", name, v, m ? m->code : "nothing");
    }
    CHECK(seen >= (int)n, "viewer has %d keys, keymap.c %zu", seen, n);
    for (size_t i = 0; i < n; i++) {
        char needle[64];
        snprintf(needle, sizeof needle, "%s: 0x%02X", k[i].code, k[i].hid);
        CHECK(strstr(t, needle) != NULL, "viewer lacks %s", needle);
    }
}

static void test_android_keys(void)
{
    CHECK(keymap_android_to_hid(66) == 0x28, "KEYCODE_ENTER");
    CHECK(keymap_android_to_hid(29) == 0x04, "KEYCODE_A");
    CHECK(keymap_android_to_hid(54) == 0x1D, "KEYCODE_Z");
    CHECK(keymap_android_to_hid(7) == 0x27, "KEYCODE_0");
    CHECK(keymap_android_to_hid(16) == 0x26, "KEYCODE_9");
    CHECK(keymap_android_to_hid(142) == 0x45, "KEYCODE_F12");
    CHECK(keymap_android_to_hid(26) == 0, "KEYCODE_POWER has no key");
}

/* ---------------- input ---------------- */

static int has(const char *ev)
{
    for (int i = 0; i < STUB.n; i++)
        if (!strcmp(STUB.ev[i], ev))
            return 1;
    return 0;
}

static void mouse(uint8_t action, uint8_t button, uint8_t buttons, uint16_t x, uint16_t y)
{
    uint8_t m[12] = { 3, action, button, buttons };
    rd_put_be16(m + 4, x);
    rd_put_be16(m + 6, y);
    input_handle(m, sizeof m);
}

static void key(uint8_t action, uint16_t hid, uint16_t mods)
{
    uint8_t m[10] = { 4, action };
    rd_put_be16(m + 2, hid);
    rd_put_be16(m + 4, mods);
    input_handle(m, sizeof m);
}

static void test_input(void)
{
    desk_refresh_screens(); /* the stub: 1000x500 points at 100,50 */
    STUB.n = 0;
    mouse(0, 0, 0, 32768, 0);
    CHECK(has("move 600 50"), "move: %s", STUB.ev[0]);
    STUB.n = 0;
    mouse(1, 2, 2, 65535, 65535); /* right down */
    CHECK(has("move 1100 550") && has("button 2 down"), "right down");
    mouse(0, 0, 0, 0, 0); /* moved with no buttons held: the right button was released elsewhere */
    CHECK(has("button 2 up"), "lost button released");
    STUB.n = 0;
    mouse(2, 0, 0, 0, 0); /* up of a button never pressed */
    CHECK(!has("button 0 up"), "stray up ignored");
    /* keys: down, auto-repeat, up; modifiers resync */
    STUB.n = 0;
    key(0, 0xE0, 0x01); /* Ctrl down */
    key(0, 0x06, 0x01); /* C */
    key(1, 0x06, 0x01);
    key(0, 0x19, 0x00); /* V with Ctrl no longer held: Ctrl is released first */
    CHECK(has("key 0xe0 down") && has("key 0x06 down") && has("key 0x06 up") && has("key 0xe0 up") && has("key 0x19 down"),
          "ctrl+c then v");
    /* reset releases everything held */
    STUB.n = 0;
    mouse(1, 0, 1, 100, 100);
    key(0, 0xE1, 0x02);
    uint8_t reset[5] = { 5 };
    input_handle(reset, sizeof reset);
    CHECK(has("button 0 up") && has("key 0x19 up") && has("key 0xe1 up"), "reset");
    /* touch from an old viewer: pointer 0 is the left button */
    STUB.n = 0;
    uint8_t t[14] = { 1, 0, 0, 1 };
    rd_put_be16(t + 4, 0);
    rd_put_be16(t + 6, 65535);
    input_handle(t, sizeof t);
    t[1] = 1;
    input_handle(t, sizeof t);
    CHECK(has("move 100 550") && has("button 0 down") && has("button 0 up"), "touch as click");
    /* scroll: 1/256 notches */
    STUB.n = 0;
    uint8_t s[13] = { 2 };
    rd_put_be16(s + 5, (uint16_t)(int16_t)-128);
    rd_put_be16(s + 7, 512);
    input_handle(s, sizeof s);
    CHECK(has("scroll -0.50 2.00"), "scroll: %s", STUB.n > 1 ? STUB.ev[1] : "-");
    /* view only */
    D.view_only = 1;
    STUB.n = 0;
    key(0, 0x04, 0);
    CHECK(STUB.n == 0, "view only");
    D.view_only = 0;
    input_release_all();
}

/* ---------------- video ---------------- */

static void test_fit(void)
{
    int w, h;
    video_fit(1920, 1080, 1080, 1, &w, &h);
    CHECK(w == 1920 && h == 1088, "1080p sw: %dx%d", w, h);
    video_fit(2560, 1440, 1080, 1, &w, &h);
    CHECK(w == 1920 && h == 1088, "1440p→1080: %dx%d", w, h);
    video_fit(2880, 1800, 1080, 1, &w, &h);
    CHECK(w == 1728 && h == 1088, "retina: %dx%d", w, h);
    video_fit(1366, 768, 0, 0, &w, &h);
    CHECK(w == 1366 && h == 768, "native hw: %dx%d", w, h);
    video_fit(1280, 1024, 720, 0, &w, &h);
    CHECK(w == 900 && h == 720, "720 hw: %dx%d", w, h);
}

static void test_yuv(void)
{
    enum { W = 32, H = 16 };
    static uint8_t bgra[W * H * 4], yuv[W * H * 3 / 2];
    for (int i = 0; i < W * H; i++) { /* left half red, right half white */
        int x = i % W;
        bgra[i * 4 + 0] = x < W / 2 ? 0 : 255;
        bgra[i * 4 + 1] = x < W / 2 ? 0 : 255;
        bgra[i * 4 + 2] = 255;
        bgra[i * 4 + 3] = 255;
    }
    yuv_from_bgra(bgra, W, H, W * 4, yuv, yuv + W * H, yuv + W * H * 5 / 4, W, H);
    CHECK(abs(yuv[0] - 82) <= 1 && abs(yuv[W - 1] - 235) <= 1, "Y red %d white %d", yuv[0], yuv[W - 1]);
    CHECK(abs(yuv[W * H] - 90) <= 2 && abs(yuv[W * H * 5 / 4] - 240) <= 2, "UV red %d %d", yuv[W * H], yuv[W * H * 5 / 4]);
    /* scaled down by 2: still red on the left, white on the right */
    static uint8_t small[16 * 8 * 3 / 2];
    yuv_from_bgra(bgra, W, H, W * 4, small, small + 128, small + 160, 16, 8);
    CHECK(abs(small[0] - 82) <= 2 && abs(small[15] - 235) <= 2, "scaled Y %d %d", small[0], small[15]);
}

static void test_swenc(void)
{
    enum { W = 64, H = 48 };
    static uint8_t yuv[W * H * 3 / 2];
    memset(yuv, 128, sizeof yuv);
    swenc_t *e = swenc_new(W, H, 30, 1000);
    CHECK(e != NULL, "swenc_new");
    if (!e)
        return;
    size_t n;
    const uint8_t *au = swenc_encode(e, yuv, yuv + W * H, yuv + W * H * 5 / 4, 1, &n);
    CHECK(au && n > 8 && !memcmp(au, "\0\0\0\1", 4), "keyframe starts with a start code (%zu bytes)", n);
    int sps = 0, idr = 0;
    for (size_t i = 0; au && i + 4 < n; i++)
        if (!memcmp(au + i, "\0\0\1", 3)) {
            sps |= (au[i + 3] & 31) == 7;
            idr |= (au[i + 3] & 31) == 5;
        }
    CHECK(sps && idr, "keyframe has SPS %d and IDR %d", sps, idr);
    yuv[100] = 200;
    au = swenc_encode(e, yuv, yuv + W * H, yuv + W * H * 5 / 4, 0, &n);
    int p = 0;
    for (size_t i = 0; au && i + 4 < n; i++)
        if (!memcmp(au + i, "\0\0\1", 3))
            p |= (au[i + 3] & 31) == 1;
    CHECK(p, "delta frame is a non-IDR slice");
    swenc_free(e);
    CHECK(swenc_new(100, 48, 30, 1000) == NULL, "width not a multiple of 16 is refused");
}

int main(int argc, char **argv)
{
    rd_log_init("test", RD_LOG_WARN);
    test_keymap(argc > 1 ? argv[1] : NULL);
    test_android_keys();
    test_input();
    test_fit();
    test_yuv();
    test_swenc();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
