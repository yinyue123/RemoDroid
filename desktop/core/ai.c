/* The AI control API on a computer (mcp.md): what the router agent relays to a phone's ApiServer,
 * answered here for the screen this agent shows. The server sends {"t": "api", "id", "d": {"op",
 * "args"}}; the answer is one binary message, u32 header length | {"id", "status", "type",
 * "headers"} | body, as from agent/api.c.
 *
 *   capabilities   the screen, the platform, the actions
 *   screenshot     JPEG of the screen, scaled to max_edge (X-RD-Width/-Height, X-RD-Display-W/-H)
 *   action         click, move, drag, scroll, key, text; points {x, y} in screen pixels, or
 *                  {x, y, "space": "image", w, h} in a screenshot's
 *   ui             not on computers (no accessibility tree yet): 501
 *
 * The person at the computer comes first: actions are refused for 5 s after viewer input, on a
 * view-only computer, and everything is refused where every viewer must be confirmed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "desk.h"
#include "msg.h"
#include "wsc.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "stb_image_write.h"
#pragma GCC diagnostic pop
#pragma GCC diagnostic ignored "-Wunused-function" /* stb's other writers, judged at the end of the file */

#define VIEWER_GRACE_MS 5000
#define NOTIFY_QUIET_MS (2 * 60 * 1000)

static uint64_t last_notice;

/* ---------------- answers ---------------- */

static void reply(long id, int status, const char *type, cJSON *headers, const void *body, size_t n)
{
    cJSON *h = cJSON_CreateObject();
    cJSON_AddNumberToObject(h, "id", (double)id);
    cJSON_AddNumberToObject(h, "status", status);
    cJSON_AddStringToObject(h, "type", type);
    if (headers)
        cJSON_AddItemToObject(h, "headers", headers);
    char *hs = cJSON_PrintUnformatted(h);
    cJSON_Delete(h);
    size_t hl = strlen(hs);
    uint8_t *m = malloc(4 + hl + n);
    rd_put_be32(m, (uint32_t)hl);
    memcpy(m + 4, hs, hl);
    if (n)
        memcpy(m + 4 + hl, body, n);
    wsc_send_binary(m, 4 + hl + n);
    free(m);
    free(hs);
}

static void reply_json(long id, int status, cJSON *b)
{
    char *s = cJSON_PrintUnformatted(b);
    cJSON_Delete(b);
    reply(id, status, "application/json", NULL, s, strlen(s));
    free(s);
}

static void fail(long id, int status, const char *code, const char *text, int retryable)
{
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "error", code);
    cJSON_AddStringToObject(b, "msg", text);
    cJSON_AddBoolToObject(b, "retryable", retryable);
    reply_json(id, status, b);
}

static const dsk_screen_t *screen(void)
{
    if (!D.nscreens)
        desk_refresh_screens();
    return D.screen >= 0 && D.screen < D.nscreens ? &D.screens[D.screen] : NULL;
}

/* ---------------- screenshot ---------------- */

static void jpeg_out(void *ctx, void *data, int size) { rd_buf_append(ctx, data, (size_t)size); }

static void screenshot(long id, const cJSON *a)
{
    const dsk_screen_t *s = screen();
    char err[200] = "";
    if (!s) {
        fail(id, 503, "capture_unavailable", "no screen to capture", 1);
        return;
    }
    dsk_capture_t *c = dsk_capture_open(s, err, sizeof err);
    dsk_frame_t f = { 0 };
    int got = 0;
    for (int i = 0; c && i < 8 && got == 0; i++) /* the first frame comes at once, or within a few hundred ms */
        got = dsk_capture_next(c, &f, 250);
    if (got != 1) {
        if (!err[0] && dsk_problem(err, sizeof err) != 0)
            err[0] = 0;
        fail(id, 503, "capture_unavailable", err[0] ? err : "the screen could not be captured (locked, or a secure desktop such as UAC?)", 1);
        if (c)
            dsk_capture_close(c);
        return;
    }
    /* scale to max_edge by averaging (sharp text at small sizes), straight to RGB */
    int max_edge = (int)RD_MAX(240, RD_MIN(2560, msg_num(a, "max_edge", 1280)));
    int quality = (int)RD_MAX(30, RD_MIN(95, msg_num(a, "quality", 75)));
    double k = (double)max_edge / RD_MAX(f.w, f.h);
    if (k > 1)
        k = 1;
    int w = RD_MAX(1, (int)(f.w * k + 0.5)), h = RD_MAX(1, (int)(f.h * k + 0.5));
    uint8_t *rgb = malloc((size_t)w * h * 3);
    for (int y = 0; y < h; y++) {
        int y0 = y * f.h / h, y1 = RD_MAX(y0 + 1, (y + 1) * f.h / h);
        for (int x = 0; x < w; x++) {
            int x0 = x * f.w / w, x1 = RD_MAX(x0 + 1, (x + 1) * f.w / w);
            unsigned sum[3] = { 0, 0, 0 }, cnt = 0;
            for (int yy = y0; yy < y1; yy++) {
                const uint8_t *p = f.bgra + (size_t)yy * f.stride + (size_t)x0 * 4;
                for (int xx = x0; xx < x1; xx++, p += 4) {
                    sum[0] += p[2];
                    sum[1] += p[1];
                    sum[2] += p[0];
                    cnt++;
                }
            }
            uint8_t *q = rgb + ((size_t)y * w + x) * 3;
            for (int i = 0; i < 3; i++)
                q[i] = (uint8_t)(sum[i] / cnt);
        }
    }
    int fw = f.w, fh = f.h;
    dsk_capture_close(c);
    rd_buf_t out;
    rd_buf_init(&out);
    int ok = stbi_write_jpg_to_func(jpeg_out, &out, w, h, 3, rgb, quality);
    free(rgb);
    if (!ok) {
        rd_buf_free(&out);
        fail(id, 500, "io_error", "JPEG encoding failed", 0);
        return;
    }
    cJSON *hd = cJSON_CreateObject();
    cJSON_AddNumberToObject(hd, "X-RD-Width", w);
    cJSON_AddNumberToObject(hd, "X-RD-Height", h);
    cJSON_AddNumberToObject(hd, "X-RD-Display-W", fw);
    cJSON_AddNumberToObject(hd, "X-RD-Display-H", fh);
    cJSON_AddStringToObject(hd, "X-RD-Backend", "desktop");
    reply(id, 200, "image/jpeg", hd, rd_buf_data(&out), rd_buf_avail(&out));
    rd_buf_free(&out);
}

/* ---------------- input ---------------- */

/* A point → 0..1 across the screen; -1 when it isn't one or lies outside. */
static int norm_point(const cJSON *p, const dsk_screen_t *s, double *nx, double *ny)
{
    const cJSON *x = cJSON_GetObjectItemCaseSensitive(p, "x"), *y = cJSON_GetObjectItemCaseSensitive(p, "y");
    if (!cJSON_IsNumber(x) || !cJSON_IsNumber(y))
        return -1;
    double w = s->pw, h = s->ph;
    if (!strcmp(msg_str(p, "space", "display"), "image")) {
        w = msg_num(p, "w", 0);
        h = msg_num(p, "h", 0);
        if (w <= 0 || h <= 0)
            return -1;
    }
    *nx = x->valuedouble / w;
    *ny = y->valuedouble / h;
    return *nx >= 0 && *nx < 1 && *ny >= 0 && *ny < 1 ? 0 : -1;
}

static void pause_ms(int ms)
{
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

/* "ctrl", "Enter", "a", "F5", "KeyA" … → USB HID usage, 0 if unknown */
static int key_hid(const char *name)
{
    static const char *ALIAS[][2] = {
        { "ctrl", "ControlLeft" }, { "control", "ControlLeft" }, { "shift", "ShiftLeft" }, { "alt", "AltLeft" },
        { "option", "AltLeft" }, { "win", "MetaLeft" }, { "windows", "MetaLeft" }, { "super", "MetaLeft" },
        { "meta", "MetaLeft" }, { "cmd", "MetaLeft" }, { "command", "MetaLeft" }, { "enter", "Enter" },
        { "return", "Enter" }, { "esc", "Escape" }, { "del", "Delete" }, { "pgup", "PageUp" }, { "pgdn", "PageDown" },
        { "up", "ArrowUp" }, { "down", "ArrowDown" }, { "left", "ArrowLeft" }, { "right", "ArrowRight" },
        { "menu", "ContextMenu" }, { "-", "Minus" }, { "=", "Equal" }, { ",", "Comma" }, { ".", "Period" },
        { "/", "Slash" }, { ";", "Semicolon" }, { "'", "Quote" }, { "[", "BracketLeft" }, { "]", "BracketRight" },
        { "\\", "Backslash" }, { "`", "Backquote" },
    };
    char code[32];
    size_t n = strlen(name);
    if (!n || n >= 24)
        return 0;
    snprintf(code, sizeof code, "%s", name);
    for (size_t i = 0; i < RD_ARRAY_LEN(ALIAS); i++)
        if (rd_streq_nocase(name, ALIAS[i][0]))
            snprintf(code, sizeof code, "%s", ALIAS[i][1]);
    if (n == 1 && ((name[0] | 32) >= 'a' && (name[0] | 32) <= 'z'))
        snprintf(code, sizeof code, "Key%c", name[0] & ~32);
    else if (n == 1 && name[0] >= '0' && name[0] <= '9')
        snprintf(code, sizeof code, "Digit%c", name[0]);
    size_t nk;
    const keymap_t *all = keymap_all(&nk);
    for (size_t i = 0; i < nk; i++)
        if (all[i].code && rd_streq_nocase(all[i].code, code))
            return all[i].hid;
    return 0;
}

static int do_action(const cJSON *a, const dsk_screen_t *s, char *err, size_t cap)
{
    const char *act = msg_str(a, "action", "");
    double nx, ny;
    if (!strcmp(act, "click") || !strcmp(act, "move")) {
        if (norm_point(cJSON_GetObjectItemCaseSensitive(a, "target"), s, &nx, &ny) < 0)
            return snprintf(err, cap, "\"target\" must be a point on the screen"), -1;
        dsk_mouse_move(s, nx, ny);
        if (!strcmp(act, "move"))
            return 0;
        const char *b = msg_str(a, "button", "left");
        int btn = !strcmp(b, "right") ? 2 : !strcmp(b, "middle") ? 1 : 0;
        int count = (int)RD_MAX(1, RD_MIN(3, msg_num(a, "count", 1)));
        pause_ms(30);
        for (int i = 0; i < count; i++) {
            dsk_mouse_button(btn, 1);
            pause_ms(20);
            dsk_mouse_button(btn, 0);
            if (i + 1 < count)
                pause_ms(60);
        }
        return 0;
    }
    if (!strcmp(act, "drag")) {
        const cJSON *path = cJSON_GetObjectItemCaseSensitive(a, "path"), *p;
        int n = cJSON_GetArraySize(path);
        if (!cJSON_IsArray(path) || n < 2 || n > 32)
            return snprintf(err, cap, "a drag \"path\" has 2 to 32 points"), -1;
        double px[32], py[32];
        int i = 0;
        cJSON_ArrayForEach(p, path) {
            if (norm_point(p, s, &px[i], &py[i]) < 0)
                return snprintf(err, cap, "drag point %d is not on the screen", i + 1), -1;
            i++;
        }
        dsk_mouse_move(s, px[0], py[0]);
        pause_ms(30);
        dsk_mouse_button(0, 1);
        for (i = 1; i < n; i++)
            for (int st = 1; st <= 8; st++) { /* steps, so apps see the pointer travel */
                pause_ms(12);
                dsk_mouse_move(s, px[i - 1] + (px[i] - px[i - 1]) * st / 8, py[i - 1] + (py[i] - py[i - 1]) * st / 8);
            }
        pause_ms(30);
        dsk_mouse_button(0, 0);
        return 0;
    }
    if (!strcmp(act, "scroll")) {
        const cJSON *t = cJSON_GetObjectItemCaseSensitive(a, "target");
        if (t) {
            if (norm_point(t, s, &nx, &ny) < 0)
                return snprintf(err, cap, "\"target\" must be a point on the screen"), -1;
            dsk_mouse_move(s, nx, ny);
            pause_ms(30);
        }
        const char *dir = msg_str(a, "direction", "down");
        double amount = RD_MAX(1, RD_MIN(30, msg_num(a, "amount", 3)));
        if (!strcmp(dir, "down"))
            dsk_mouse_scroll(0, -amount);
        else if (!strcmp(dir, "up"))
            dsk_mouse_scroll(0, amount);
        else if (!strcmp(dir, "left"))
            dsk_mouse_scroll(amount, 0);
        else if (!strcmp(dir, "right"))
            dsk_mouse_scroll(-amount, 0);
        else
            return snprintf(err, cap, "direction is down, up, left or right"), -1;
        return 0;
    }
    if (!strcmp(act, "key")) { /* "ctrl+shift+t": modifiers down in order, released in reverse */
        char buf[128];
        int hids[8], n = 0;
        snprintf(buf, sizeof buf, "%s", msg_str(a, "keys", ""));
        for (char *t = strtok(buf, "+ "); t; t = strtok(NULL, "+ ")) {
            int h = key_hid(t);
            if (!h)
                return snprintf(err, cap, "unknown key \"%s\"", t), -1;
            if (n == 8)
                return snprintf(err, cap, "at most 8 keys at once"), -1;
            hids[n++] = h;
        }
        if (!n)
            return snprintf(err, cap, "\"keys\" is required, e.g. enter or ctrl+c"), -1;
        int times = (int)RD_MAX(1, RD_MIN(20, msg_num(a, "count", 1)));
        for (int r = 0; r < times; r++) {
            for (int i = 0; i < n; i++) {
                dsk_key(hids[i], 1);
                pause_ms(15);
            }
            for (int i = n - 1; i >= 0; i--) {
                dsk_key(hids[i], 0);
                pause_ms(15);
            }
        }
        return 0;
    }
    if (!strcmp(act, "text")) {
        const char *t = msg_str(a, "text", NULL);
        if (!t)
            return snprintf(err, cap, "\"text\" is required"), -1;
        /* one character at a time, paced: in a burst, Windows 11's new Notepad (WinUI) turned
         * "Hello from…" into "Hello 好好好…", the last character over and over */
        for (const char *p = t; *p;) {
            char ch[8];
            size_t n = 1;
            while (p[n] && ((unsigned char)p[n] & 0xC0) == 0x80 && n < sizeof ch - 1) /* the rest of a UTF-8 sequence */
                n++;
            memcpy(ch, p, n);
            ch[n] = 0;
            dsk_text(ch);
            p += n;
            if (*p)
                pause_ms(8);
        }
        return 0;
    }
    snprintf(err, cap, "unknown action \"%s\" (click, move, drag, scroll, key, text)", act);
    return -1;
}

/* ---------------- entry ---------------- */

static cJSON *capabilities(const dsk_screen_t *s)
{
    cJSON *b = cJSON_CreateObject();
    cJSON_AddNumberToObject(b, "schema", 1);
    cJSON_AddStringToObject(b, "kind", "computer");
    cJSON_AddStringToObject(b, "platform", D.platform);
    cJSON_AddStringToObject(b, "os", D.os);
    if (s) {
        cJSON *sc = cJSON_AddObjectToObject(b, "screen");
        cJSON_AddNumberToObject(sc, "w", s->pw);
        cJSON_AddNumberToObject(sc, "h", s->ph);
        cJSON_AddStringToObject(sc, "name", s->name);
    }
    cJSON *acts = cJSON_AddArrayToObject(b, "actions");
    static const char *A[] = { "click", "move", "drag", "scroll", "key", "text" };
    for (size_t i = 0; i < RD_ARRAY_LEN(A); i++)
        cJSON_AddItemToArray(acts, cJSON_CreateString(A[i]));
    cJSON_AddBoolToObject(b, "ui", 0);
    cJSON_AddBoolToObject(b, "view_only", D.view_only);
    cJSON_AddBoolToObject(b, "viewer_active", session_active() && rd_mono_ms() - input_last_ms() < VIEWER_GRACE_MS);
    return b;
}

void ai_request(long id, const cJSON *d)
{
    const char *op = msg_str(d, "op", "");
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(d, "args");
    const dsk_screen_t *s = screen();
    if (!strcmp(op, "capabilities")) {
        reply_json(id, 200, capabilities(s));
        return;
    }
    if (desk_conf_bool("desktop", "confirm", 0)) {
        fail(id, 403, "forbidden", "this computer asks its user before every viewer, so assistants can't use it "
                                    "(turn off \"Ask at the computer before each session\" to allow them)", 0);
        return;
    }
    if (!strcmp(op, "ui")) {
        fail(id, 501, "unsupported", "a computer has no UI tree here; use screenshot", 0);
        return;
    }
    if (D.notify && rd_mono_ms() - last_notice > NOTIFY_QUIET_MS)
        desk_notify("An AI assistant is using this computer through RemoDroid.");
    last_notice = rd_mono_ms();
    if (!strcmp(op, "screenshot")) {
        screenshot(id, a);
        return;
    }
    if (strcmp(op, "action")) {
        fail(id, 404, "not_found", "unknown op", 0);
        return;
    }
    if (D.view_only) {
        fail(id, 403, "forbidden", "this computer is view only", 0);
        return;
    }
    if (session_active() && rd_mono_ms() - input_last_ms() < VIEWER_GRACE_MS) {
        fail(id, 409, "viewer_active", "a person is using the computer in the viewer", 1);
        return;
    }
    if (!s) {
        fail(id, 503, "capture_unavailable", "no screen", 1);
        return;
    }
    char err[200] = "";
    if (do_action(a, s, err, sizeof err) < 0) {
        fail(id, 400, "bad_request", err, 0);
        return;
    }
    LOGI("ai: %s", msg_str(a, "action", "?"));
    cJSON *b = cJSON_CreateObject();
    cJSON_AddBoolToObject(b, "ok", 1);
    cJSON_AddStringToObject(b, "action", msg_str(a, "action", ""));
    reply_json(id, 200, b);
}
