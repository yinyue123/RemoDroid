/* One viewer session on a computer: the ctrl channel protocol (the same
 * messages as android/java/…/Session.java, so the viewer's HUD, quality,
 * keyboard, clipboard and file sheets work unchanged), quality → video
 * pipeline, stats and clipboard sync. Agent thread. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "desk.h"
#include "msg.h"
#include "rd_rtc.h"

static struct {
    int active, ctrl_open, high;
    int audio_ok, audio_off; /* the viewer took the sound track / switched Sound off */
    char sid[64], codec[8];
    cJSON *quality;
    int target_kbps;
    unsigned long clip_seq;
    char *clip_sent, *clip_set;
} S;

int session_active(void) { return S.active; }

void session_send(cJSON *m)
{
    rtc_t *r = rd_rtc_stack();
    if (S.active && r) {
        char *s = msg_print(m);
        rtc_dc_send(r, "ctrl", 0, s, strlen(s));
        free(s);
    }
    cJSON_Delete(m);
}

static void reply(cJSON *req, cJSON *d)
{
    cJSON *m = msg_new("ack", 0, NULL);
    cJSON_ReplaceItemInObjectCaseSensitive(m, "d", d ? d : cJSON_CreateObject());
    cJSON *id = cJSON_GetObjectItemCaseSensitive(req, "id");
    if (id)
        cJSON_AddItemToObject(m, "id", cJSON_Duplicate(id, 1));
    session_send(m);
}

static void error(cJSON *req, const char *code, const char *text)
{
    cJSON *m = msg_new("error", 0, NULL);
    cJSON_AddStringToObject(msg_d(m), "code", code);
    cJSON_AddStringToObject(msg_d(m), "msg", text);
    cJSON *id = cJSON_GetObjectItemCaseSensitive(req, "id");
    if (id)
        cJSON_AddItemToObject(m, "id", cJSON_Duplicate(id, 1));
    session_send(m);
}

/* ---------------- video ---------------- */

static int short_side(const char *preset)
{
    if (!strcmp(preset, "native"))
        return 0;
    if (!strcmp(preset, "720p"))
        return 720;
    if (!strcmp(preset, "540p"))
        return 540;
    if (!strcmp(preset, "360p"))
        return 360;
    return 1080;
}

static const char *preset(void) { return msg_str(S.quality, "preset", desk_conf("video", "preset", "1080p")); }

static void start_video(void)
{
    int max_fps = (int)desk_conf_int("video", "max_fps", 60);
    int max_kbps = (int)desk_conf_int("video", "max_bitrate_kbps", 20000);
    video_req_t r = {
        .codec = !strcmp(S.codec, "h265") ? VC_H265 : VC_H264,
        .high = S.high,
        .short_side = short_side(preset()),
        .fps = RD_MIN((int)msg_num(S.quality, "fps", max_fps), max_fps),
        .kbps = S.target_kbps ? S.target_kbps : RD_MIN((int)msg_num(S.quality, "kbps", 4000), max_kbps),
        .software = !strcmp(desk_conf("video", "encoder", "auto"), "software"),
    };
    if (video_start(&r) < 0)
        LOGE("session: cannot start video");
}

static void apply_max_kbps(void)
{
    int max_kbps = (int)desk_conf_int("video", "max_bitrate_kbps", 20000);
    int want = (int)msg_num(S.quality, "kbps", max_kbps);
    rtc_t *r = rd_rtc_stack();
    if (r)
        rtc_set_max_kbps(r, RD_MIN(want, max_kbps));
}

static cJSON *screen_json(const dsk_screen_t *s)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "index", s->index);
    cJSON_AddNumberToObject(j, "w", s->pw);
    cJSON_AddNumberToObject(j, "h", s->ph);
    cJSON_AddBoolToObject(j, "primary", s->primary);
    cJSON_AddStringToObject(j, "name", s->name);
    return j;
}

static void send_screen_changed(void)
{
    if (D.screen < 0 || D.screen >= D.nscreens)
        return;
    cJSON *m = msg_new("screen.changed", 0, NULL);
    cJSON *d = msg_d(m);
    cJSON_AddNumberToObject(d, "w", D.screens[D.screen].pw);
    cJSON_AddNumberToObject(d, "h", D.screens[D.screen].ph);
    cJSON_AddNumberToObject(d, "rotation", 0);
    cJSON_AddNumberToObject(d, "index", D.screen);
    session_send(m);
}

/* ---------------- lifecycle ---------------- */

/* ---------------- sound ---------------- */

/* Capture runs only while it has somewhere to go: the viewer took the track, the computer's
 * desktop.audio setting is on and the viewer hasn't switched Sound off (as Session.java). */
void session_audio_update(void)
{
    int want = S.active && S.audio_ok && D.audio && !S.audio_off;
    if (want && !audio_running())
        audio_start();
    else if (!want && audio_running())
        audio_stop();
}

static void send_audio_state(void)
{
    cJSON *as = msg_new("audio.state", 0, NULL);
    cJSON_AddBoolToObject(msg_d(as), "on", audio_running());
    cJSON_AddBoolToObject(msg_d(as), "available", S.audio_ok && audio_available());
    cJSON_AddBoolToObject(msg_d(as), "enabled", D.audio);
    session_send(as);
}

void session_connected(const char *sid, const char *codec, const char *profile, const char *quality, int audio)
{
    if (S.active)
        session_ended("replaced");
    memset(&S, 0, sizeof S);
    S.active = 1;
    snprintf(S.sid, sizeof S.sid, "%s", sid ? sid : "");
    snprintf(S.codec, sizeof S.codec, "%s", codec ? codec : "h264");
    S.high = profile && !strcmp(profile, "high");
    S.quality = cJSON_Parse(quality ? quality : "{}");
    if (!cJSON_IsObject(S.quality)) {
        cJSON_Delete(S.quality);
        S.quality = cJSON_CreateObject();
    }
    S.clip_seq = dsk_clip_seq();
    dsk_session(1);
    desk_refresh_screens();
    apply_max_kbps();
    start_video();
    S.audio_ok = audio;
    session_audio_update();
}

void session_ended(const char *reason)
{
    if (!S.active)
        return;
    video_stop();
    audio_stop();
    input_release_all();
    files_cancel_all();
    dsk_session(0);
    cJSON_Delete(S.quality);
    free(S.clip_sent);
    free(S.clip_set);
    memset(&S, 0, sizeof S);
    if (D.lock_on_disc && strcmp(reason, "replaced"))
        dsk_lock();
}

void session_keyframe(void) { video_keyframe(); }

void session_bitrate(int kbps)
{
    S.target_kbps = kbps;
    if (strcmp(msg_str(S.quality, "mode", desk_conf("video", "mode", "auto")), "manual"))
        video_bitrate(kbps);
}

void session_channel_open(const char *label)
{
    if (strcmp(label, "ctrl"))
        return;
    S.ctrl_open = 1;
    cJSON *m = msg_new("session.ready", 0, NULL);
    cJSON *d = msg_d(m);
    cJSON_AddNumberToObject(d, "proto", 1);
    cJSON_AddStringToObject(d, "platform", D.platform);
    cJSON_AddStringToObject(d, "os", D.os);
    cJSON_AddStringToObject(d, "name", D.host);
    const dsk_screen_t *cur = D.screen >= 0 && D.screen < D.nscreens ? &D.screens[D.screen] : NULL;
    cJSON *sc = cJSON_AddObjectToObject(d, "screen");
    cJSON_AddNumberToObject(sc, "w", cur ? cur->pw : 1920);
    cJSON_AddNumberToObject(sc, "h", cur ? cur->ph : 1080);
    cJSON_AddNumberToObject(sc, "rotation", 0);
    cJSON_AddNumberToObject(sc, "index", D.screen);
    cJSON *arr = cJSON_AddArrayToObject(d, "screens");
    for (int i = 0; i < D.nscreens; i++)
        cJSON_AddItemToArray(arr, screen_json(&D.screens[i]));
    cJSON_AddStringToObject(d, "codec", S.codec);
    cJSON *caps = cJSON_AddObjectToObject(d, "caps");
    cJSON *codecs = cJSON_AddArrayToObject(caps, "codecs");
    if (dsk_hwvideo_has_h265())
        cJSON_AddItemToArray(codecs, cJSON_CreateString("h265"));
    cJSON_AddItemToArray(codecs, cJSON_CreateString("h264"));
    cJSON_AddNumberToObject(caps, "max_fps", desk_conf_int("video", "max_fps", 60));
    cJSON_AddBoolToObject(caps, "audio", S.audio_ok && audio_available());
    cJSON_AddBoolToObject(caps, "keyboard", 1);
    cJSON_AddBoolToObject(caps, "mouse", 1);
    cJSON_AddBoolToObject(caps, "view_only", D.view_only);
    cJSON_AddStringToObject(d, "home", rd_home_dir());
    char up[1024];
    const char *u = desk_conf("files", "upload_dir", "");
    if (*u)
        snprintf(up, sizeof up, "%s", u);
    else
        snprintf(up, sizeof up, "%s%cRemoDroid", dsk_downloads_dir(), RD_PATH_SEP);
    cJSON_AddStringToObject(d, "upload_dir", up);
    session_send(m);
    if (D.clip_sync) {
        char *c = dsk_clip_get();
        if (c && *c) {
            cJSON *cm = msg_new("clip.changed", 0, NULL);
            cJSON_AddStringToObject(msg_d(cm), "text", c);
            session_send(cm);
            free(S.clip_sent);
            S.clip_sent = c;
            c = NULL;
        }
        free(c);
    }
}

/* ---------------- ctrl messages ---------------- */

static void press_combo(const int *hids, int n)
{
    if (D.view_only)
        return;
    for (int i = 0; i < n; i++)
        dsk_key(hids[i], 1);
    for (int i = n - 1; i >= 0; i--)
        dsk_key(hids[i], 0);
}

static int is_mac(void) { return !strcmp(D.platform, "macos"); }
static int is_win(void) { return !strcmp(D.platform, "windows"); }

/* The phone's navigation buttons, as the nearest desktop shortcut. */
static int nav(const char *b)
{
    const int CTRL = 0xE0, ALT = 0xE2, GUI = 0xE3;
    if (!strcmp(b, "back")) {
        int k[] = { is_mac() ? GUI : ALT, is_mac() ? 0x2F /* [ */ : 0x50 /* ← */ };
        press_combo(k, 2);
    } else if (!strcmp(b, "home")) {
        if (is_mac()) {
            int k[] = { 0x44 }; /* F11: show desktop */
            press_combo(k, 1);
        } else {
            int k[] = { GUI, 0x07 /* D */ };
            press_combo(k, 2);
        }
    } else if (!strcmp(b, "recents")) {
        if (is_mac()) {
            int k[] = { CTRL, 0x52 /* ↑: Mission Control */ };
            press_combo(k, 2);
        } else if (is_win()) {
            int k[] = { GUI, 0x2B /* Tab */ };
            press_combo(k, 2);
        } else {
            int k[] = { GUI };
            press_combo(k, 1);
        }
    } else if (!strcmp(b, "notifications") && is_win()) {
        int k[] = { GUI, 0x11 /* N */ };
        press_combo(k, 2);
    } else if (!strcmp(b, "volume_up")) {
        int k[] = { 0x80 };
        press_combo(k, 1);
    } else if (!strcmp(b, "volume_down")) {
        int k[] = { 0x81 };
        press_combo(k, 1);
    } else {
        return -1;
    }
    return 0;
}

typedef struct {
    cJSON *req;
    char *cmd;
    long timeout_ms;
    cJSON *result;
} shell_job_t;

static void shell_done(void *arg)
{
    shell_job_t *j = arg;
    if (S.active)
        reply(j->req, j->result);
    else
        cJSON_Delete(j->result);
    cJSON_Delete(j->req);
    free(j->cmd);
    free(j);
}

/* shell.exec ([desktop] allow_shell): the command runs on its own thread */
static void *shell_thread(void *arg)
{
    shell_job_t *j = arg;
    uint64_t t0 = rd_mono_ms();
    rd_buf_t out;
    rd_buf_init(&out);
#ifdef _WIN32
    FILE *p = _popen(j->cmd, "r");
#else
    char cmd[4200];
    snprintf(cmd, sizeof cmd, "%s 2>&1", j->cmd);
    FILE *p = popen(cmd, "r");
#endif
    int code = -1, truncated = 0;
    if (p) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, p)) > 0) {
            if (out.len + n > 256 * 1024) {
                truncated = 1;
                continue;
            }
            rd_buf_append(&out, buf, n);
        }
#ifdef _WIN32
        code = _pclose(p);
#else
        code = pclose(p);
        if (code >= 0)
            code = (code >> 8) & 0xff;
#endif
    }
    rd_buf_append(&out, "", 1);
    j->result = cJSON_CreateObject();
    cJSON_AddNumberToObject(j->result, "exit_code", code);
    cJSON_AddStringToObject(j->result, "stdout", (const char *)out.p);
    cJSON_AddStringToObject(j->result, "stderr", "");
    cJSON_AddBoolToObject(j->result, "truncated", truncated);
    cJSON_AddNumberToObject(j->result, "ms", (double)(rd_mono_ms() - t0));
    rd_buf_free(&out);
    rd_rtc_call(shell_done, j);
    return NULL;
}

static void control(cJSON *m)
{
    const char *t = msg_type(m);
    cJSON *d = msg_d(m);
    if (!strcmp(t, "quality.set")) {
        const char *old_preset = preset();
        char op[16];
        snprintf(op, sizeof op, "%s", old_preset);
        int old_fps = (int)msg_num(S.quality, "fps", 0);
        cJSON_Delete(S.quality);
        S.quality = cJSON_Duplicate(d, 1);
        if (strcmp(msg_str(d, "codec", "auto"), "auto") && strcmp(msg_str(d, "codec", ""), S.codec))
            error(m, "unsupported", "the codec changes on the next connection");
        apply_max_kbps();
        int manual = !strcmp(msg_str(d, "mode", ""), "manual");
        if (manual)
            video_bitrate(RD_MIN((int)msg_num(d, "kbps", 4000), (int)desk_conf_int("video", "max_bitrate_kbps", 20000)));
        if (strcmp(preset(), op) || (int)msg_num(d, "fps", old_fps) != old_fps)
            start_video();
        int w, h;
        double fps;
        const char *enc;
        video_info(&w, &h, &fps, &enc);
        cJSON *qs = msg_new("quality.state", 0, NULL);
        cJSON *qd = cJSON_Duplicate(d, 1);
        cJSON_AddNumberToObject(qd, "width", w);
        cJSON_AddNumberToObject(qd, "height", h);
        cJSON_AddStringToObject(qd, "codec", S.codec);
        cJSON_ReplaceItemInObjectCaseSensitive(qs, "d", qd);
        session_send(qs);
        reply(m, NULL);
    } else if (!strcmp(t, "key")) {
        int hid = keymap_android_to_hid((int)msg_num(d, "code", 0));
        const char *a = msg_str(d, "action", "press");
        if (!hid) {
            error(m, "unsupported", "no such key on a computer");
            return;
        }
        if (!D.view_only) {
            if (!strcmp(a, "press") || !strcmp(a, "down"))
                dsk_key(hid, 1);
            if (!strcmp(a, "press") || !strcmp(a, "up"))
                dsk_key(hid, 0);
        }
        reply(m, NULL);
    } else if (!strcmp(t, "nav")) {
        if (nav(msg_str(d, "button", "")) < 0)
            error(m, "unsupported", "not on a computer");
        else
            reply(m, NULL);
    } else if (!strcmp(t, "text")) {
        if (!D.view_only)
            dsk_text(msg_str(d, "text", ""));
        reply(m, NULL);
    } else if (!strcmp(t, "clip.set")) {
        const char *text = msg_str(d, "text", "");
        free(S.clip_set);
        S.clip_set = rd_strdup(text);
        if (dsk_clip_set(text) < 0) {
            error(m, "io_error", "cannot set the clipboard");
            return;
        }
        S.clip_seq = dsk_clip_seq();
        if (msg_bool(d, "paste", desk_conf_bool("desktop", "auto_paste", 0))) {
            int k[] = { is_mac() ? 0xE3 : 0xE0, 0x19 /* V */ };
            press_combo(k, 2);
        }
        reply(m, NULL);
    } else if (!strcmp(t, "audio.set")) {
        S.audio_off = !msg_bool(d, "on", 1);
        session_audio_update();
        send_audio_state();
        cJSON *r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "on", audio_running());
        reply(m, r);
    } else if (!strcmp(t, "screen.power")) {
        if (dsk_screen_power(msg_bool(d, "on", 1)) < 0)
            error(m, "unsupported", "cannot change the display power");
        else
            reply(m, NULL);
    } else if (!strcmp(t, "screen.select")) {
        desk_refresh_screens();
        int i = (int)msg_num(d, "index", 0);
        if (i < 0 || i >= D.nscreens) {
            error(m, "not_found", "no such screen");
            return;
        }
        D.screen = i;
        start_video();
        send_screen_changed();
        reply(m, NULL);
    } else if (!strcmp(t, "shell.exec")) {
        if (!D.allow_shell) {
            error(m, "unsupported", "shell is off on this computer ([desktop] allow_shell)");
            return;
        }
        shell_job_t *j = calloc(1, sizeof *j);
        j->req = cJSON_Duplicate(m, 1);
        j->cmd = rd_strdup(msg_str(d, "cmd", "true"));
        rd_thread_t th;
        if (rd_thread_start(&th, shell_thread, j) < 0) {
            error(m, "io_error", "cannot start the command");
            cJSON_Delete(j->req);
            free(j->cmd);
            free(j);
        }
#ifndef _WIN32
        else
            pthread_detach(th);
#else
        else
            CloseHandle(th);
#endif
    } else if (!strcmp(t, "ping")) {
        cJSON *p = msg_new("pong", 0, NULL);
        cJSON *id = cJSON_GetObjectItemCaseSensitive(m, "id");
        if (id)
            cJSON_AddItemToObject(p, "id", cJSON_Duplicate(id, 1));
        cJSON *ts = cJSON_GetObjectItemCaseSensitive(d, "ts");
        if (ts)
            cJSON_AddItemToObject(msg_d(p), "ts", cJSON_Duplicate(ts, 1));
        cJSON_AddNumberToObject(msg_d(p), "phone_ts", (double)rd_now_ms());
        session_send(p);
    } else if (!strncmp(t, "file.", 5)) {
        char code[32] = "", err[300] = "";
        cJSON *r = files_control(t, d, code, sizeof code, err, sizeof err);
        if (r)
            reply(m, r);
        else
            error(m, code, err);
    } else {
        error(m, "unsupported", t);
    }
}

void session_message(const char *label, int binary, const uint8_t *d, size_t n)
{
    if (!S.active)
        return;
    if (!strcmp(label, "input")) {
        input_handle(d, n);
    } else if (!strcmp(label, "file")) {
        files_chunk(d, n);
    } else if (!strcmp(label, "ctrl")) {
        cJSON *m = cJSON_ParseWithLength((const char *)d, n);
        if (cJSON_IsObject(m))
            control(m);
        cJSON_Delete(m);
    }
}

/* ---------------- once a second ---------------- */

static void check_clipboard(void)
{
    if (!D.clip_sync || !S.ctrl_open)
        return;
    unsigned long seq = dsk_clip_seq();
    if (seq == S.clip_seq)
        return;
    S.clip_seq = seq;
    char *c = dsk_clip_get();
    if (!c || !*c || (S.clip_sent && !strcmp(c, S.clip_sent)) || (S.clip_set && !strcmp(c, S.clip_set))) {
        free(c);
        return;
    }
    cJSON *m = msg_new("clip.changed", 0, NULL);
    cJSON_AddStringToObject(msg_d(m), "text", c);
    session_send(m);
    free(S.clip_sent);
    S.clip_sent = c;
}

void session_tick(void)
{
    if (!S.active || !S.ctrl_open)
        return;
    check_clipboard();
    char st[512];
    rd_rtc_stats(st, sizeof st);
    cJSON *s = cJSON_Parse(st);
    int w, h;
    double fps;
    const char *enc;
    video_info(&w, &h, &fps, &enc);
    cJSON *m = msg_new("stats", 0, NULL);
    cJSON *d = msg_d(m);
    cJSON_AddNumberToObject(d, "fps", (int)(fps + 0.5));
    cJSON_AddNumberToObject(d, "kbps", msg_num(s, "acked_kbps", S.target_kbps));
    cJSON_AddNumberToObject(d, "target_kbps", msg_num(s, "target_kbps", 0));
    cJSON_AddNumberToObject(d, "rtt_ms", msg_num(s, "rtt_ms", 0));
    cJSON_AddNumberToObject(d, "loss_pct", msg_num(s, "loss_pct", 0));
    cJSON_AddStringToObject(d, "codec", S.codec);
    cJSON_AddNumberToObject(d, "width", w);
    cJSON_AddNumberToObject(d, "height", h);
    cJSON_AddStringToObject(d, "path", !strcmp(msg_str(s, "local", ""), "relay") ? "relay" : "direct");
    cJSON_AddStringToObject(d, "panel", "on");
    cJSON_AddNumberToObject(d, "inputs", input_events());
    cJSON_AddBoolToObject(d, "audio", audio_running());
    cJSON_AddStringToObject(d, "encoder", enc ? enc : "none");
    cJSON_Delete(s);
    session_send(m);
}
