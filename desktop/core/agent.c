/* The desktop agent (rd_agent.h): one thread runs a libev loop with the
 * server WebSocket (common/wsc.c) and the WebRTC stack in "embed" mode
 * (rtc/host.c), so the signaling the router agent relays to a phone over ADB
 * goes straight from the WebSocket into the stack here. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RD_BUILDING_LIB
#include "rd_agent.h"

#include "desk.h"
#include "enroll.h"
#include "msg.h"
#include "rd_rtc.h"
#include "wsc.h"

desk_t D;

static struct {
    rd_agent_cb_t cb;
    void *ud;
    int started;
    rd_mutex_t mu;      /* status, log */
    rd_status_t st;
    ev_timer tick;
    char sid[64];       /* the server's session, "" if none */
    char viewer[64];
    long long since;
    int rtc_state;      /* RTC_SESSION_* of sid, -1 none */
    cJSON *held_start;  /* session.start waiting for the local user's answer */
    ev_timer confirm_timer;
    char wsc_problem[200];
    /* log ring */
    char *log[300];
    int log_head, log_n;
    FILE *log_file;
    long log_size;
} G;

/* ---------------- configuration ---------------- */

const char *desk_conf(const char *sec, const char *key, const char *def) { return rd_conf_get(D.conf, sec, key, def); }
long desk_conf_int(const char *sec, const char *key, long def) { return rd_conf_int(D.conf, sec, key, def); }
int desk_conf_bool(const char *sec, const char *key, int def) { return rd_conf_bool(D.conf, sec, key, def); }

static void load_conf(void)
{
    rd_conf_t *c = rd_conf_load(D.conf_path, NULL);
    if (!c) { /* missing file: empty settings */
        FILE *f = fopen(D.conf_path, "a");
        if (f)
            fclose(f);
        c = rd_conf_load(D.conf_path, NULL);
    }
    if (c) {
        rd_conf_free(D.conf);
        D.conf = c;
    }
    const char *env = getenv("RD_LOG_LEVEL");
    rd_log_level = rd_log_parse_level(env && *env ? env : desk_conf("desktop", "log_level", NULL), RD_LOG_INFO);
    D.view_only = desk_conf_bool("desktop", "view_only", 0);
    D.clip_sync = desk_conf_bool("desktop", "clipboard_sync", 1);
    D.allow_shell = desk_conf_bool("desktop", "allow_shell", 0);
    D.notify = desk_conf_bool("desktop", "notify", 1);
    D.lock_on_disc = desk_conf_bool("desktop", "lock_on_disconnect", 0);
}

void desk_refresh_screens(void)
{
    dsk_screen_t s[DSK_MAX_SCREENS];
    int n = dsk_screens(s, DSK_MAX_SCREENS);
    if (n <= 0)
        return;
    int want = D.nscreens ? D.screen : (int)desk_conf_int("desktop", "screen", -1);
    memcpy(D.screens, s, sizeof s[0] * (size_t)n);
    D.nscreens = n;
    if (want < 0 || want >= n) {
        want = 0;
        for (int i = 0; i < n; i++)
            if (s[i].primary)
                want = i;
    }
    D.screen = want;
}

/* ---------------- logging ---------------- */

static void log_hook(const char *line)
{
    rd_mutex_lock(&G.mu);
    if (G.log_n == (int)RD_ARRAY_LEN(G.log)) {
        free(G.log[G.log_head]);
        G.log[G.log_head] = rd_strdup(line);
        G.log_head = (G.log_head + 1) % (int)RD_ARRAY_LEN(G.log);
    } else {
        G.log[(G.log_head + G.log_n++) % (int)RD_ARRAY_LEN(G.log)] = rd_strdup(line);
    }
    if (G.log_file) {
        G.log_size += fprintf(G.log_file, "%s\n", line);
        fflush(G.log_file);
        if (G.log_size > 2 * 1024 * 1024) { /* keep one old file */
            char path[600], old[610];
            snprintf(path, sizeof path, "%s%cdesktop.log", dsk_config_dir(), RD_PATH_SEP);
            snprintf(old, sizeof old, "%s.1", path);
            fclose(G.log_file);
            remove(old);
            rename(path, old);
            G.log_file = fopen(path, "w");
            G.log_size = 0;
        }
    }
    rd_mutex_unlock(&G.mu);
}

static cJSON *log_tail(int lines)
{
    cJSON *arr = cJSON_CreateArray();
    rd_mutex_lock(&G.mu);
    int n = RD_MIN(lines, G.log_n);
    for (int i = G.log_n - n; i < G.log_n; i++)
        cJSON_AddItemToArray(arr, cJSON_CreateString(G.log[(G.log_head + i) % (int)RD_ARRAY_LEN(G.log)]));
    rd_mutex_unlock(&G.mu);
    return arr;
}

/* ---------------- status ---------------- */

static void panel_from_url(const char *url, char *out, size_t cap)
{
    const char *p = url;
    int tls = !strncmp(p, "wss://", 6);
    if (tls)
        p += 6;
    else if (!strncmp(p, "ws://", 5))
        p += 5;
    else {
        out[0] = 0;
        return;
    }
    size_t hl = strcspn(p, "/");
    snprintf(out, cap, "%s://%.*s", tls ? "https" : "http", (int)hl, p);
}

/* Recomputes the UI status; tells the app when it changed. Agent thread. */
void desk_status_changed(void)
{
    rd_status_t s;
    memset(&s, 0, sizeof s);
    const char *url = desk_conf("server", "url", "");
    snprintf(s.device_id, sizeof s.device_id, "%s", desk_conf("server", "device_id", ""));
    panel_from_url(url, s.panel, sizeof s.panel);
    if (!*url || !s.device_id[0])
        s.state = RD_UNPAIRED;
    else if (!wsc_online())
        s.state = RD_CONNECTING;
    else if (G.sid[0] && G.rtc_state == RTC_SESSION_CONNECTED)
        s.state = RD_IN_SESSION;
    else
        s.state = RD_ONLINE;
    if (s.state == RD_IN_SESSION) {
        snprintf(s.viewer, sizeof s.viewer, "%s", G.viewer);
        s.since_ms = G.since;
    }
    if (dsk_problem(s.problem, sizeof s.problem) <= 0 && s.state == RD_CONNECTING)
        snprintf(s.problem, sizeof s.problem, "%s", G.wsc_problem);
    s.view_only = D.view_only;
    rd_mutex_lock(&G.mu);
    int changed = memcmp(&s, &G.st, sizeof s) != 0;
    G.st = s;
    rd_mutex_unlock(&G.mu);
    if (changed && G.cb.on_status)
        G.cb.on_status(G.ud, &s);
}

static cJSON *device_json(void)
{
    cJSON *j = cJSON_CreateObject();
    char problem[200] = "";
    int bad = dsk_problem(problem, sizeof problem) > 0;
    cJSON_AddBoolToObject(j, "attached", 1);
    cJSON_AddStringToObject(j, "server", bad ? "stopped" : "running");
    if (bad)
        cJSON_AddStringToObject(j, "problem", problem);
    cJSON_AddStringToObject(j, "model", D.host);
    cJSON_AddStringToObject(j, "os", D.os);
    cJSON_AddStringToObject(j, "platform", D.platform);
    cJSON *codecs = cJSON_AddArrayToObject(j, "codecs");
    if (dsk_hwvideo_has_h265())
        cJSON_AddItemToArray(codecs, cJSON_CreateString("h265"));
    cJSON_AddItemToArray(codecs, cJSON_CreateString("h264"));
    cJSON *sc = cJSON_AddArrayToObject(j, "screens");
    for (int i = 0; i < D.nscreens; i++) {
        cJSON *s = cJSON_CreateObject();
        cJSON_AddNumberToObject(s, "w", D.screens[i].pw);
        cJSON_AddNumberToObject(s, "h", D.screens[i].ph);
        cJSON_AddBoolToObject(s, "primary", D.screens[i].primary);
        cJSON_AddItemToArray(sc, s);
    }
    cJSON_AddBoolToObject(j, "view_only", D.view_only);
    return j;
}

static cJSON *status_json(void *ud)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddItemToObject(j, "phone", device_json()); /* "phone": the controlled device, for wire compatibility */
    return j;
}

void desk_notify(const char *text)
{
    if (G.cb.on_notify)
        G.cb.on_notify(G.ud, text);
    else
        dsk_notify("RemoDroid", text);
}

void desk_event(const char *kind, cJSON *info)
{
    LOGI("event: %s", kind);
    cJSON *m = msg_new("event", 0, NULL);
    cJSON *d = msg_d(m);
    cJSON_AddStringToObject(d, "kind", kind);
    cJSON_AddNumberToObject(d, "ts", (double)rd_now_ms());
    cJSON_AddItemToObject(d, "info", info ? info : cJSON_CreateObject());
    wsc_send(m);
}

static void send_status(void)
{
    if (!wsc_online())
        return;
    cJSON *m = msg_new("status", 0, NULL);
    cJSON_ReplaceItemInObjectCaseSensitive(m, "d", status_json(NULL));
    wsc_send(m);
}

/* ---------------- WebRTC stack callbacks (agent thread) ---------------- */

static void rtc_on_ctrl(void *ud, const char *json, size_t n) { /* nothing else comes through in embed mode */ }
static void rtc_on_link(void *ud, int up) {}

static void rtc_on_session(void *ud, const char *sid, int state, const char *codec, const char *profile,
                           const char *quality, const char *reason, int audio)
{
    /* ENDED may arrive after session.ended went out (which clears G.sid): end whatever runs */
    if (state != RTC_SESSION_ENDED && (!sid || strcmp(sid, G.sid)))
        return;
    G.rtc_state = state;
    if (state == RTC_SESSION_CONNECTED) {
        G.since = (long long)rd_now_ms();
        session_connected(sid, codec, profile, quality);
        LOGI("session %s: %s is viewing", sid, G.viewer[0] ? G.viewer : "a viewer");
        if (D.notify) {
            char t[160];
            snprintf(t, sizeof t, "%s is viewing this computer.", G.viewer[0] ? G.viewer : "Someone");
            desk_notify(t);
        }
        desk_event("session.connected", NULL);
    } else if (state == RTC_SESSION_ENDED) {
        int was = session_active();
        session_ended(reason ? reason : "ended");
        if (!sid || !strcmp(sid, G.sid))
            G.sid[0] = 0;
        G.rtc_state = -1;
        if (was && D.notify)
            desk_notify("The viewer disconnected.");
    }
    desk_status_changed();
}

static void rtc_on_open(void *ud, const char *label) { session_channel_open(label); }

static void rtc_on_message(void *ud, const char *label, int binary, const uint8_t *d, size_t n)
{
    session_message(label, binary, d, n);
}

static void rtc_on_keyframe(void *ud) { session_keyframe(); }
static void rtc_on_bitrate(void *ud, int kbps) { session_bitrate(kbps); }

/* signal / session.ended from the stack → the server */
static void rtc_on_ctrl_out(void *ud, const char *json, size_t n)
{
    cJSON *m = cJSON_ParseWithLength(json, n);
    if (!cJSON_IsObject(m)) {
        cJSON_Delete(m);
        return;
    }
    const char *t = msg_type(m);
    if (!strcmp(t, "session.ended")) {
        const char *sid = msg_sid(m) ? msg_sid(m) : msg_str(msg_d(m), "sid", "");
        if (!strcmp(sid, G.sid))
            G.sid[0] = 0;
    }
    if (!strcmp(t, "signal") || !strcmp(t, "session.ended") || !strcmp(t, "event") || !strcmp(t, "status"))
        wsc_send(m);
    else
        cJSON_Delete(m);
}

/* ---------------- server link ---------------- */

static void to_stack(cJSON *m)
{
    rtc_t *r = rd_rtc_stack();
    if (!r)
        return;
    char *s = msg_print(m);
    rtc_ctrl_in(r, s, strlen(s));
    free(s);
}

static void refuse(const char *sid, const char *reason)
{
    cJSON *e = msg_new("session.ended", 0, sid);
    cJSON_AddStringToObject(msg_d(e), "sid", sid);
    cJSON_AddStringToObject(msg_d(e), "reason", reason);
    wsc_send(e);
}

static void start_held(int allow)
{
    cJSON *m = G.held_start;
    G.held_start = NULL;
    ev_timer_stop(D.loop, &G.confirm_timer);
    if (!m)
        return;
    const char *sid = msg_sid(m) ? msg_sid(m) : msg_str(msg_d(m), "sid", "");
    if (!allow) {
        LOGI("session %s: declined at the computer", sid);
        refuse(sid, "declined");
        G.sid[0] = 0;
    } else if (!strcmp(sid, G.sid)) {
        to_stack(m);
    }
    cJSON_Delete(m);
}

static void on_confirm_timeout(struct ev_loop *loop, ev_timer *w, int revents) { start_held(0); }

static void session_start(cJSON *m)
{
    const char *sid = msg_sid(m) ? msg_sid(m) : msg_str(msg_d(m), "sid", "");
    char problem[200];
    if (dsk_problem(problem, sizeof problem) > 0) {
        LOGW("session %s refused: %s", sid, problem);
        refuse(sid, "computer_unavailable");
        return;
    }
    if (G.held_start)
        start_held(0); /* a newer request replaces one still waiting */
    snprintf(G.sid, sizeof G.sid, "%s", sid);
    snprintf(G.viewer, sizeof G.viewer, "%s", msg_str(msg_d(m), "viewer", ""));
    G.rtc_state = RTC_SESSION_CONNECTING;
    if (desk_conf_bool("desktop", "confirm", 0)) {
        if (!G.cb.on_confirm) {
            LOGW("session %s refused: [desktop] confirm is on, but nobody is there to answer", sid);
            refuse(sid, "declined");
            G.sid[0] = 0;
            return;
        }
        G.held_start = cJSON_Duplicate(m, 1);
        ev_timer_set(&G.confirm_timer, 30., 0.);
        ev_timer_start(D.loop, &G.confirm_timer);
        G.cb.on_confirm(G.ud, G.viewer[0] ? G.viewer : "Someone");
        return;
    }
    to_stack(m);
}

static int apply_setting(const char *key, const char *val, char *err, size_t errcap);

static void on_server_message(void *ud, cJSON *m)
{
    const char *t = msg_type(m);
    long id = msg_id(m);
    if (!strcmp(t, "session.start")) {
        session_start(m);
    } else if (!strcmp(t, "signal")) {
        if (!G.held_start)
            to_stack(m);
    } else if (!strcmp(t, "session.end")) {
        const char *sid = msg_sid(m) ? msg_sid(m) : msg_str(msg_d(m), "sid", "");
        if (G.held_start && !strcmp(sid, G.sid)) {
            cJSON_Delete(G.held_start);
            G.held_start = NULL;
            ev_timer_stop(D.loop, &G.confirm_timer);
            G.sid[0] = 0;
        } else {
            to_stack(m);
        }
    } else if (!strcmp(t, "cmd")) {
        cJSON *d = msg_d(m), *args = cJSON_GetObjectItemCaseSensitive(d, "args");
        const char *cmd = msg_str(d, "cmd", "");
        LOGI("server command: %s", cmd);
        if (!strcmp(cmd, "agent.config.get")) {
            cJSON *r = cJSON_CreateObject();
            static const char *keys[][3] = { /* section, key, default */
                { "desktop", "view_only", "false" },      { "desktop", "confirm", "false" },
                { "desktop", "clipboard_sync", "true" },  { "desktop", "notify", "true" },
                { "desktop", "lock_on_disconnect", "false" }, { "desktop", "allow_shell", "false" },
                { "video", "preset", "1080p" },           { "video", "max_fps", "60" },
                { "video", "max_bitrate_kbps", "20000" }, { "video", "encoder", "auto" },
                { "files", "upload_dir", "" },            { "files", "max_mb", "2048" },
            };
            for (size_t i = 0; i < RD_ARRAY_LEN(keys); i++) {
                const char *v = desk_conf(keys[i][0], keys[i][1], keys[i][2]);
                if (v) {
                    char k[64], *end;
                    snprintf(k, sizeof k, "%s.%s", keys[i][0], keys[i][1]);
                    double num = strtod(v, &end);
                    if (rd_streq_nocase(v, "true") || rd_streq_nocase(v, "yes") || rd_streq_nocase(v, "on"))
                        cJSON_AddTrueToObject(r, k);
                    else if (rd_streq_nocase(v, "false") || rd_streq_nocase(v, "no") || rd_streq_nocase(v, "off"))
                        cJSON_AddFalseToObject(r, k);
                    else if (*v && !*end)
                        cJSON_AddNumberToObject(r, k, num);
                    else
                        cJSON_AddStringToObject(r, k, v);
                }
            }
            wsc_send(msg_ack(id, r));
            return;
        }
        char err[200] = "";
        int ok = 0;
        if (!strcmp(cmd, "agent.config") || !strcmp(cmd, "quality.default")) {
            ok = 1;
            for (cJSON *it = args ? args->child : NULL; it && ok; it = it->next) {
                char v[256];
                if (cJSON_IsBool(it))
                    snprintf(v, sizeof v, "%s", cJSON_IsTrue(it) ? "true" : "false");
                else if (cJSON_IsNumber(it))
                    snprintf(v, sizeof v, "%g", it->valuedouble);
                else if (cJSON_IsString(it))
                    snprintf(v, sizeof v, "%s", it->valuestring);
                else
                    continue;
                ok = apply_setting(it->string, v, err, sizeof err) == 0;
            }
        } else if (!strcmp(cmd, "agent.reload")) {
            load_conf();
            ok = 1;
        } else if (!strcmp(cmd, "phone.wake")) {
            ok = dsk_screen_power(1) == 0;
        } else if (!strcmp(cmd, "phone.sleep")) {
            ok = dsk_lock() == 0;
        } else if (!strcmp(cmd, "server.restart")) {
            if (G.sid[0])
                rd_rtc_end_session("restarted");
            ok = 1;
        }
        if (ok) {
            cJSON *r = cJSON_CreateObject();
            cJSON_AddStringToObject(r, "cmd", cmd);
            wsc_send(msg_ack(id, r));
        } else {
            wsc_send(msg_error(id, err[0] ? "bad_request" : "unsupported", err[0] ? err : cmd));
        }
        desk_status_changed();
    } else if (!strcmp(t, "diag")) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddItemToObject(r, "agent_log", log_tail((int)msg_num(msg_d(m), "lines", 200)));
        cJSON_AddItemToObject(r, "server_stderr", cJSON_CreateArray());
        wsc_send(msg_ack(id, r));
    } else if (!strcmp(t, "api")) {
        wsc_send(msg_error(id, "unsupported", "the AI control API is for phones"));
    } else if (!strcmp(t, "error")) {
        LOGW("server error: %s", msg_str(msg_d(m), "msg", "?"));
    }
}

static void on_server_open(void *ud)
{
    cJSON *m = msg_new("hello", 0, NULL);
    cJSON *d = msg_d(m);
    cJSON_AddNumberToObject(d, "proto", RD_PROTO);
    cJSON_AddStringToObject(d, "version", RD_VERSION);
    cJSON_AddStringToObject(d, "arch", rd_machine());
    cJSON_AddStringToObject(d, "platform", D.platform);
    cJSON_AddStringToObject(d, "os", D.os);
    cJSON_AddItemToObject(d, "phone", device_json());
    wsc_send(m);
    desk_status_changed();
}

static void on_server_lost(void *ud)
{
    /* the server has ended the session and told the viewer */
    if (G.held_start)
        start_held(0);
    if (G.sid[0])
        rd_rtc_end_session("server_lost");
    desk_status_changed();
}

static void on_server_state(void *ud, int online, const char *problem)
{
    snprintf(G.wsc_problem, sizeof G.wsc_problem, "%s", problem ? problem : "");
    desk_status_changed();
}

static void apply_server(void)
{
    wsc_conf_t c;
    memset(&c, 0, sizeof c);
    snprintf(c.url, sizeof c.url, "%s", desk_conf("server", "url", ""));
    snprintf(c.device_id, sizeof c.device_id, "%s", desk_conf("server", "device_id", ""));
    snprintf(c.device_secret, sizeof c.device_secret, "%s", desk_conf("server", "device_secret", ""));
    snprintf(c.ca_file, sizeof c.ca_file, "%s", desk_conf("server", "ca_file", ""));
    snprintf(c.user_agent, sizeof c.user_agent, "remodroid-desktop/%s (%s)", RD_VERSION, D.platform);
    c.ping_s = (int)desk_conf_int("keepalive", "ping_seconds", 20);
    c.timeout_s = (int)desk_conf_int("keepalive", "timeout_seconds", 60);
    c.heartbeat_s = (int)desk_conf_int("keepalive", "heartbeat_seconds", 30);
    c.reconnect_max_s = (int)desk_conf_int("keepalive", "reconnect_max_seconds", 30);
    if (!c.device_id[0] || !c.device_secret[0])
        c.url[0] = 0; /* not paired */
    wsc_configure(&c);
    desk_status_changed();
}

/* ---------------- settings ---------------- */

/* "desktop.view_only" / "view_only" (desktop) / "video.preset" … */
static int apply_setting(const char *key, const char *val, char *err, size_t errcap)
{
    char sec[32] = "desktop", k[64];
    const char *dot = strchr(key, '.');
    if (dot && (size_t)(dot - key) < sizeof sec) {
        memcpy(sec, key, (size_t)(dot - key));
        sec[dot - key] = 0;
        snprintf(k, sizeof k, "%s", dot + 1);
    } else {
        snprintf(k, sizeof k, "%s", key);
    }
    if (strcmp(sec, "desktop") && strcmp(sec, "video") && strcmp(sec, "files")) {
        snprintf(err, errcap, "unknown setting: %s", key);
        return -1;
    }
    if (rd_conf_write(D.conf_path, sec, k, val) < 0) {
        snprintf(err, errcap, "cannot write %s", D.conf_path);
        return -1;
    }
    LOGI("desktop.ini: [%s] %s = %s", sec, k, val);
    load_conf();
    if (!strcmp(sec, "desktop") && !strcmp(k, "view_only") && D.view_only)
        input_release_all();
    send_status();
    return 0;
}

/* ---------------- periodic ---------------- */

static void on_tick(struct ev_loop *loop, ev_timer *w, int revents)
{
    static int n;
    session_tick();
    if (++n % 10 == 0) { /* monitors plugged / unplugged, permissions granted */
        int before = D.nscreens;
        char p0[200] = "", p1[200] = "";
        dsk_problem(p0, sizeof p0);
        if (!session_active())
            desk_refresh_screens();
        dsk_problem(p1, sizeof p1);
        if (before != D.nscreens || strcmp(p0, p1))
            send_status();
    }
    desk_status_changed();
}

/* ---------------- start (agent thread) ---------------- */

static void on_loop(void *ud, struct ev_loop *loop)
{
    D.loop = loop;
    char problem[200] = "";
    if (dsk_init(problem, sizeof problem) < 0)
        LOGW("desktop: %s", problem);
    dsk_os_name(D.os, sizeof D.os);
    desk_refresh_screens();
    LOGI("remodroid-desktop %s on %s (%s), %d screen(s)", RD_VERSION, D.os, rd_machine(), D.nscreens);
    for (int i = 0; i < D.nscreens; i++)
        LOGI("  screen %d: %dx%d px at %d,%d%s", i, D.screens[i].pw, D.screens[i].ph, D.screens[i].x, D.screens[i].y,
             D.screens[i].primary ? " (primary)" : "");
    wsc_init(loop, (wsc_cb_t){ on_server_open, on_server_message, on_server_lost, status_json, on_server_state }, NULL);
    ev_timer_init(&G.tick, on_tick, 1., 1.);
    ev_timer_start(loop, &G.tick);
    ev_timer_init(&G.confirm_timer, on_confirm_timeout, 30., 0.);
    apply_server();
}

const char *rd_agent_default_conf(void)
{
    static char path[600];
    if (!path[0])
        snprintf(path, sizeof path, "%s%cdesktop.ini", dsk_config_dir(), RD_PATH_SEP);
    return path;
}

int rd_agent_start(const char *conf_path, rd_agent_cb_t cb, void *ud)
{
    if (G.started)
        return 0;
    rd_mutex_init(&G.mu);
    G.cb = cb;
    G.ud = ud;
    G.rtc_state = -1;
    snprintf(D.conf_path, sizeof D.conf_path, "%s", conf_path && *conf_path ? conf_path : rd_agent_default_conf());
    snprintf(D.platform, sizeof D.platform, "%s", dsk_platform());
    rd_hostname(D.host, sizeof D.host);
    char logp[600];
    snprintf(logp, sizeof logp, "%s%cdesktop.log", dsk_config_dir(), RD_PATH_SEP);
    G.log_file = fopen(logp, "a");
    rd_log_set_hook(log_hook);
    load_conf();
    rd_rtc_opts_t o = {
        .listen = "embed",
        .offer_h265 = dsk_hwvideo_has_h265() && strcmp(desk_conf("video", "codec", "auto"), "h264") &&
                      strcmp(desk_conf("video", "encoder", "auto"), "software"),
        .offer_audio = 0,
        .max_kbps = (int)desk_conf_int("video", "max_bitrate_kbps", 20000),
        .log_level = rd_log_level,
        .log_name = "desktop",
    };
    rd_rtc_cb_t rcb = {
        .rtc = { rtc_on_ctrl, rtc_on_link, rtc_on_session, rtc_on_open, rtc_on_message, rtc_on_keyframe,
                 rtc_on_bitrate, rtc_on_ctrl_out },
        .on_loop = on_loop,
    };
    if (rd_rtc_start(&o, &rcb, NULL) < 0) {
        LOGE("cannot start the WebRTC stack");
        return -1;
    }
    G.started = 1;
    return 0;
}

static void do_stop(void *arg)
{
    if (G.sid[0])
        rtc_end_session(rd_rtc_stack(), "shutdown");
    session_ended("shutdown");
    wsc_stop();
    ev_timer_stop(D.loop, &G.tick);
}

void rd_agent_stop(void)
{
    if (!G.started)
        return;
    rd_rtc_call(do_stop, NULL);
    rd_rtc_stop();
    G.started = 0;
    G.st.state = RD_STOPPED;
}

/* ---------------- pairing ---------------- */

typedef struct {
    char panel[300], code[32], name[64];
    void (*on_done)(void *ud, int ok, const char *msg);
    void *ud;
    int ok;
    rd_enroll_t en;
    char msg[300];
} pair_t;

static void pair_finish(void *arg)
{
    pair_t *p = arg;
    if (p->ok) {
        if (rd_conf_write(D.conf_path, "server", "url", p->en.ws_url) < 0 ||
            rd_conf_write(D.conf_path, "server", "device_id", p->en.device_id) < 0 ||
            rd_conf_write(D.conf_path, "server", "device_secret", p->en.device_secret) < 0) {
            p->ok = 0;
            snprintf(p->msg, sizeof p->msg, "cannot save %s", D.conf_path);
        } else {
            LOGI("paired as \"%s\" (%s) with %s", p->en.name, p->en.device_id, p->panel);
            snprintf(p->msg, sizeof p->msg, "%s", p->en.name);
            load_conf();
            apply_server();
        }
    }
    if (p->on_done)
        p->on_done(p->ud, p->ok, p->msg);
    free(p);
}

static void *pair_thread(void *arg)
{
    pair_t *p = arg;
    char ua[64];
    snprintf(ua, sizeof ua, "remodroid-desktop/%s", RD_VERSION);
    p->ok = rd_enroll(p->panel, p->code, p->name, D.platform, desk_conf("server", "ca_file", ""), ua, &p->en, p->msg,
                      sizeof p->msg) == 0;
    if (!p->ok)
        LOGW("pairing with %s failed: %s", p->panel, p->msg);
    rd_rtc_call(pair_finish, p);
    return NULL;
}

void rd_agent_pair(const char *panel, const char *code, const char *name,
                   void (*on_done)(void *ud, int ok, const char *msg), void *ud)
{
    pair_t *p = calloc(1, sizeof *p);
    snprintf(p->panel, sizeof p->panel, "%s", panel ? panel : "");
    snprintf(p->code, sizeof p->code, "%s", code ? code : "");
    snprintf(p->name, sizeof p->name, "%s", name && *name ? name : D.host);
    p->on_done = on_done;
    p->ud = ud;
    rd_thread_t th;
    if (rd_thread_start(&th, pair_thread, p) < 0) {
        if (on_done)
            on_done(ud, 0, "cannot start");
        free(p);
        return;
    }
#ifdef _WIN32
    CloseHandle(th);
#else
    pthread_detach(th);
#endif
}

static void do_unpair(void *arg)
{
    if (G.sid[0])
        rtc_end_session(rd_rtc_stack(), "unpaired");
    rd_conf_write(D.conf_path, "server", "device_secret", NULL);
    rd_conf_write(D.conf_path, "server", "device_id", NULL);
    rd_conf_write(D.conf_path, "server", "url", NULL);
    LOGI("unpaired");
    load_conf();
    apply_server();
}

void rd_agent_unpair(void) { rd_rtc_call(do_unpair, NULL); }

static void do_end(void *arg)
{
    if (G.held_start)
        start_held(0);
    else if (G.sid[0])
        rtc_end_session(rd_rtc_stack(), "ended_at_computer");
}

void rd_agent_end_session(void) { rd_rtc_call(do_end, NULL); }

static void do_confirm(void *arg) { start_held(arg != NULL); }

void rd_agent_confirm(int allow) { rd_rtc_call(do_confirm, allow ? (void *)1 : NULL); }

typedef struct {
    char key[64], val[256];
} set_t;

static void do_set(void *arg)
{
    set_t *s = arg;
    char err[200];
    if (apply_setting(s->key, s->val, err, sizeof err) < 0)
        LOGW("%s", err);
    desk_status_changed();
    free(s);
}

void rd_agent_set(const char *key, const char *value)
{
    set_t *s = calloc(1, sizeof *s);
    snprintf(s->key, sizeof s->key, "%s", key);
    snprintf(s->val, sizeof s->val, "%s", value ? value : "");
    if (G.started)
        rd_rtc_call(do_set, s);
    else
        free(s);
}

void rd_agent_status(rd_status_t *out)
{
    if (!G.started) {
        memset(out, 0, sizeof *out);
        out->state = RD_STOPPED;
        return;
    }
    rd_mutex_lock(&G.mu);
    *out = G.st;
    rd_mutex_unlock(&G.mu);
}

char *rd_agent_log(int lines)
{
    rd_buf_t b;
    rd_buf_init(&b);
    if (G.started) {
        rd_mutex_lock(&G.mu);
        int n = RD_MIN(lines, G.log_n);
        for (int i = G.log_n - n; i < G.log_n; i++)
            rd_buf_printf(&b, "%s\n", G.log[(G.log_head + i) % (int)RD_ARRAY_LEN(G.log)]);
        rd_mutex_unlock(&G.mu);
    }
    rd_buf_append(&b, "", 1);
    return (char *)b.p;
}

void rd_agent_free(void *p) { free(p); }

const char *rd_agent_version(void) { return RD_VERSION; }
