/* Inside the desktop agent: the core modules (desktop/core/) and the contract
 * each platform backend (desktop/win, desktop/mac, desktop/linux) implements.
 *
 *   loop thread    agent.c (server link, relay), session.c (ctrl), input.c, files.c
 *   video thread   video.c: platform hardware source, or capture + yuv.c + swenc.c
 */
#ifndef RD_DESK_H
#define RD_DESK_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "conf.h"
#include "ev.h"
#include "plat.h"
#include "rd.h"

/* ======================= platform backend (desktop/<os>/) ======================= */

typedef struct {
    int index;
    int x, y, w, h;     /* in the input coordinate space of the OS (Windows: virtual-desktop pixels,
                           macOS: global points, X11: root-window pixels) */
    int pw, ph;         /* size in physical pixels (what capture produces) */
    int primary;
    char name[64];
} dsk_screen_t;

#define DSK_MAX_SCREENS 8

const char *dsk_platform(void);                 /* "windows" / "macos" / "linux" */
void        dsk_os_name(char *out, size_t cap); /* "Windows 11 (build 22631)", "macOS 15.1", "Ubuntu 24.04 (X11)" */
int         dsk_init(char *problem, size_t cap); /* once, on the agent thread; 0, or -1 if desktops can't be used */
/* A reason the computer can't be controlled right now ("Screen Recording permission is off"), or 0. */
int         dsk_problem(char *out, size_t cap);
int         dsk_screens(dsk_screen_t *out, int max);

/* ---- raw capture (CPU BGRA frames) ---- */
typedef struct {
    const uint8_t *bgra; /* B, G, R, A bytes */
    int w, h, stride;
} dsk_frame_t;

typedef struct dsk_capture dsk_capture_t;
dsk_capture_t *dsk_capture_open(const dsk_screen_t *s, char *err, size_t cap);
/* Waits up to timeout_ms for the screen to change. Returns 1 with f filled (valid until the next
 * call), 0 when nothing changed, -1 when capture broke (reopen). */
int            dsk_capture_next(dsk_capture_t *c, dsk_frame_t *f, int timeout_ms);
void           dsk_capture_close(dsk_capture_t *c);
/* Whether capture draws the pointer into the picture (the default) or leaves it out, for a viewer that
 * draws its own (trackpad.md §9). Applies to captures already running; safe from any thread. */
void           dsk_capture_pointer(int show);

/* ---- hardware video (capture + encode on the GPU); optional: open returns NULL ---- */
typedef struct {
    dsk_screen_t screen;
    int codec;           /* VC_H264 / VC_H265 */
    int high;            /* H.264: High profile negotiated (else Constrained Baseline) */
    int width, height;   /* encode size (even), already scaled from the screen */
    int fps, kbps;
} dsk_video_cfg_t;

enum { VC_H264, VC_H265 };

typedef struct dsk_hwvideo dsk_hwvideo_t;
/* Annex-B access unit callback (from any thread the backend likes); key: IDR with SPS/PPS first. */
typedef void (*dsk_au_cb)(void *ud, const uint8_t *au, size_t n, int64_t pts_us, int key);
dsk_hwvideo_t *dsk_hwvideo_open(const dsk_video_cfg_t *c, dsk_au_cb cb, void *ud, char *err, size_t cap);
/* Drives capture: waits up to timeout_ms, encodes what changed (or repeats). Returns 0, -1 = broken. */
int            dsk_hwvideo_step(dsk_hwvideo_t *v, int timeout_ms);
void           dsk_hwvideo_keyframe(dsk_hwvideo_t *v);
void           dsk_hwvideo_bitrate(dsk_hwvideo_t *v, int kbps);
void           dsk_hwvideo_close(dsk_hwvideo_t *v);
int            dsk_hwvideo_has_h265(void);    /* a hardware HEVC encoder exists */
const char    *dsk_hwvideo_name(dsk_hwvideo_t *v); /* "NVIDIA H.264 Encoder MFT", "VideoToolbox", … */

/* ---- sound: what the computer plays (loopback); optional ---- */
/* Interleaved float samples, any rate and channel count; called on the backend's capture thread. */
typedef void (*dsk_pcm_cb)(const float *pcm, int frames, int channels, int rate);
int  dsk_audio_supported(void);       /* 1 if this computer's sound can be captured */
int  dsk_audio_start(dsk_pcm_cb cb);  /* 0, or -1 */
void dsk_audio_stop(void);            /* joins the capture; no cb after it returns; safe if not started */

/* ---- input (agent thread) ---- */
void dsk_mouse_move(const dsk_screen_t *s, double nx, double ny); /* 0..1 across the screen */
void dsk_mouse_button(int button, int down);                       /* 0 left, 1 middle, 2 right, 3 back, 4 forward */
void dsk_mouse_scroll(double dx, double dy);                       /* notches; dy > 0 = up, dx > 0 = left */
void dsk_key(int hid, int down);                                   /* USB HID usage (keyboard page) */
void dsk_text(const char *utf8);

/* ---- touch screen: real multi-touch, for viewers that send every finger (agent thread); optional ---- */
/* spec names the backend, e.g. "inputtest:/run/rd-touch.sock" (Linux: browser/agent/touch_inputtest.c);
 * points is how many fingers it takes. 0, or -1 with the reason. Opening again replaces the backend. */
int  dsk_touch_open(const char *spec, int points, char *err, size_t cap);
int  dsk_touch_points(void); /* fingers the open backend takes; 0 = none (touch falls back to the mouse) */
/* One finger: id is the viewer's pointer id (0..9); action 0 down, 1 up, 2 move, 3 cancel; 0..1 across s. */
void dsk_touch(const dsk_screen_t *s, int id, int action, double nx, double ny);
void dsk_touch_reset(void); /* lift every finger */

/* ---- gamepads: virtual Xbox 360 pads the computer's games see (agent thread); optional ---- */
/* Buttons in the W3C standard-gamepad order: bit 0 A, 1 B, 2 X, 3 Y, 4 LB, 5 RB, 6 LT, 7 RT, 8 View/Back,
 * 9 Menu/Start, 10 left stick, 11 right stick, 12 up, 13 down, 14 left, 15 right, 16 Guide. Sticks
 * -32767..32767 with y down positive; triggers 0..255. */
typedef struct {
    uint32_t buttons;
    int16_t lx, ly, rx, ry;
    uint8_t lt, rt;
} dsk_pad_t;
#define DSK_MAX_PADS 4
int  dsk_pad_supported(char *why, size_t cap); /* 1, or 0 with the reason */
int  dsk_pad_open(int index);                  /* 0, or -1 */
void dsk_pad_update(int index, const dsk_pad_t *p);
void dsk_pad_close(int index);

/* ---- clipboard (agent thread) ---- */
unsigned long dsk_clip_seq(void);  /* changes whenever the clipboard does */
char         *dsk_clip_get(void);  /* UTF-8 text or NULL; caller frees */
int           dsk_clip_set(const char *utf8);

/* ---- system ---- */
int  dsk_screen_power(int on);
/* A viewer connected (1) / left (0): keep the display awake meanwhile. */
void dsk_session(int active);
int  dsk_lock(void);
/* Notifies the person at the computer (session started / ended). */
void dsk_notify(const char *title, const char *text);
/* The per-user configuration folder (created). */
const char *dsk_config_dir(void);
/* The Downloads folder. */
const char *dsk_downloads_dir(void);

/* ======================= core ======================= */

typedef struct {
    char conf_path[512];
    rd_conf_t *conf;
    struct ev_loop *loop;
    char platform[16], os[128], host[64];
    dsk_screen_t screens[DSK_MAX_SCREENS];
    int nscreens;
    int screen;          /* index into screens of the captured one */
    int view_only, clip_sync, allow_shell, notify, lock_on_disc, audio;
} desk_t;

extern desk_t D;

const char *desk_conf(const char *sec, const char *key, const char *def);
long        desk_conf_int(const char *sec, const char *key, long def);
int         desk_conf_bool(const char *sec, const char *key, int def);
void        desk_refresh_screens(void);
void        desk_event(const char *kind, cJSON *info); /* takes info */
void        desk_status_changed(void);
void        desk_notify(const char *text); /* the app's notification, or the platform's */

/* session.c: the viewer session (ctrl channel, quality, stats) */
/* audio: the viewer took the Opus track */
void session_connected(const char *sid, const char *codec, const char *profile, const char *quality, int audio);
void session_audio_update(void); /* the sound setting changed */
void session_ended(const char *reason);
void session_channel_open(const char *label);
void session_message(const char *label, int binary, const uint8_t *d, size_t n);
void session_keyframe(void);
void session_bitrate(int kbps);
int  session_active(void);
void session_send(cJSON *m);  /* ctrl channel; frees m */
int  session_offer_files(const char *const *paths, int n); /* file.offer to the viewer; -1: no viewer */
void session_tick(void);      /* 1 s: stats, clipboard */

/* input.c */
void input_handle(const uint8_t *d, size_t n);
void input_release_all(void);
void input_pads_close(void); /* the session ended: unplug the virtual gamepads */
int  input_events(void);
uint64_t input_last_ms(void); /* rd_mono_ms() of the viewer's last input, 0 if none */

/* ai.c: the AI control API (mcp.md) — {"t": "api"} from the server, answered with one binary message */
void ai_request(long id, const cJSON *d);

/* files.c (port of android/java/…/Files.java) */
cJSON *files_control(const char *t, cJSON *d, char *code, size_t codecap, char *err, size_t errcap);
void   files_chunk(const uint8_t *d, size_t n);
void   files_cancel_all(void);
void   files_pump(void);       /* sends pulled chunks while the channel has room */
/* Files the person at the computer sends to the viewer: they may be pulled from anywhere until the
 * session ends. Returns the file.offer list ({path, name, size}), NULL if none is a file. */
cJSON *files_offer(const char *const *paths, int n);

/* keymap.c */
typedef struct {
    uint16_t hid;
    const char *code;   /* KeyboardEvent.code */
    uint16_t win;       /* set-1 scan code; 0xE0xx = extended; 0 = none */
    uint16_t mac;       /* kVK_*; 0xFFFF = none */
    uint16_t evdev;     /* Linux KEY_*; X11 keycode = evdev + 8; 0 = none */
} keymap_t;
const keymap_t *keymap_find(int hid);
const keymap_t *keymap_all(size_t *n);
int             keymap_android_to_hid(int android_keycode);

/* video.c: the encoder pipeline (own thread) */
typedef struct {
    int codec;      /* VC_* */
    int high;       /* H.264 High profile */
    int short_side; /* 0 = native */
    int fps, kbps;
    int software;   /* 1 = never the hardware encoder */
} video_req_t;
int  video_start(const video_req_t *r);  /* restarts if running */
void video_stop(void);
void video_keyframe(void);
void video_bitrate(int kbps);
void video_info(int *w, int *h, double *fps, const char **encoder);

/* audio.c: Opus over the WebRTC audio track (agent thread, except audio_pcm) */
int  audio_available(void);
int  audio_start(void);   /* 0, or -1 */
void audio_stop(void);
int  audio_running(void);
void audio_pcm(const float *pcm, int frames, int channels, int rate);
extern void (*audio_out)(const void *opus, size_t n, int64_t pts_us); /* rd_rtc_audio; tests replace it */

/* yuv.c */
/* BGRA (any size) → I420 at dw x dh (scaled, bilinear). y/u/v strides are dw, dw/2, dw/2. */
void yuv_from_bgra(const uint8_t *bgra, int sw, int sh, int sstride, uint8_t *y, uint8_t *u, uint8_t *v, int dw, int dh);
void yuv_nv12_uv(const uint8_t *u, const uint8_t *v, uint8_t *uv, int w, int h);

/* swenc.c: minih264 */
typedef struct swenc swenc_t;
swenc_t *swenc_new(int w, int h, int fps, int kbps);
/* Encodes one I420 frame (strides w, w/2, w/2). Returns Annex-B bytes (valid until the next call).
 * qp > 0: at that fixed quality, outside the rate control. */
const uint8_t *swenc_encode(swenc_t *e, const uint8_t *y, const uint8_t *u, const uint8_t *v, int key, int qp, size_t *n);
void     swenc_bitrate(swenc_t *e, int kbps);
void     swenc_free(swenc_t *e);

/* video sizes: the encode size for a screen and the viewer's preset (both even; multiples of 16
 * when mult16) */
void video_fit(int sw, int sh, int short_side, int mult16, int *w, int *h);

#endif
