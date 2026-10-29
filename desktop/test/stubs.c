/* A fake platform backend for desk_test: records what the core asks for. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "desk.h"
#include "stubs.h"

stub_log_t STUB;

static void rec(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void rec(const char *fmt, ...)
{
    if (STUB.n == STUB_MAX)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(STUB.ev[STUB.n++], sizeof STUB.ev[0], fmt, ap);
    va_end(ap);
}

const char *dsk_platform(void) { return "test"; }
void dsk_os_name(char *out, size_t cap) { snprintf(out, cap, "Test OS"); }
int dsk_init(char *problem, size_t cap) { return 0; }
int dsk_problem(char *out, size_t cap)
{
    out[0] = 0;
    return 0;
}
int dsk_screens(dsk_screen_t *out, int max)
{
    memset(out, 0, sizeof *out);
    out->x = 100, out->y = 50, out->w = 1000, out->h = 500, out->pw = 2000, out->ph = 1000, out->primary = 1;
    return 1;
}
dsk_capture_t *dsk_capture_open(const dsk_screen_t *s, char *err, size_t cap) { return NULL; }
int dsk_capture_next(dsk_capture_t *c, dsk_frame_t *f, int timeout_ms) { return -1; }
void dsk_capture_close(dsk_capture_t *c) {}
dsk_hwvideo_t *dsk_hwvideo_open(const dsk_video_cfg_t *c, dsk_au_cb cb, void *ud, char *err, size_t cap) { return NULL; }
int dsk_hwvideo_step(dsk_hwvideo_t *v, int timeout_ms) { return -1; }
void dsk_hwvideo_keyframe(dsk_hwvideo_t *v) {}
void dsk_hwvideo_bitrate(dsk_hwvideo_t *v, int kbps) {}
void dsk_hwvideo_close(dsk_hwvideo_t *v) {}
int dsk_hwvideo_has_h265(void) { return 0; }
const char *dsk_hwvideo_name(dsk_hwvideo_t *v) { return "none"; }
void dsk_session(int active) {}

void dsk_mouse_move(const dsk_screen_t *s, double nx, double ny)
{
    rec("move %d %d", (int)(s->x + nx * s->w + 0.5), (int)(s->y + ny * s->h + 0.5));
}
void dsk_mouse_button(int button, int down) { rec("button %d %s", button, down ? "down" : "up"); }
void dsk_mouse_scroll(double dx, double dy) { rec("scroll %.2f %.2f", dx, dy); }
void dsk_key(int hid, int down) { rec("key 0x%02x %s", hid, down ? "down" : "up"); }
void dsk_text(const char *utf8) { rec("text %s", utf8); }

unsigned long dsk_clip_seq(void) { return 0; }
char *dsk_clip_get(void) { return NULL; }
int dsk_clip_set(const char *utf8) { return 0; }
int dsk_screen_power(int on) { return 0; }
void dsk_capture_pointer(int show) { (void)show; }
int dsk_pad_supported(char *why, size_t cap) { return 1; }
int dsk_pad_open(int index) { rec("pad %d open", index); return 0; }
void dsk_pad_update(int index, const dsk_pad_t *p)
{
    rec("pad %d %05x %d %d %d %d %d %d", index, (unsigned)p->buttons, p->lx, p->ly, p->rx, p->ry, p->lt, p->rt);
}
void dsk_pad_close(int index) { rec("pad %d close", index); }
int STUB_TOUCH;
int dsk_touch_open(const char *spec, int points, char *err, size_t cap)
{
    STUB_TOUCH = *spec ? points : 0;
    return 0;
}
int dsk_touch_points(void) { return STUB_TOUCH; }
void dsk_touch(const dsk_screen_t *s, int id, int action, double nx, double ny)
{
    rec("touch %d %d %d %d", id, action, (int)(s->x + nx * s->w + 0.5), (int)(s->y + ny * s->h + 0.5));
}
void dsk_touch_reset(void) { rec("touch reset"); }
int dsk_lock(void) { return 0; }
void dsk_notify(const char *title, const char *text) {}
const char *dsk_config_dir(void) { return "."; }
const char *dsk_downloads_dir(void) { return "."; }

/* sound: the test feeds audio_pcm itself */
int dsk_audio_supported(void) { return 1; }
int dsk_audio_start(dsk_pcm_cb cb) { return 0; }
void dsk_audio_stop(void) {}
