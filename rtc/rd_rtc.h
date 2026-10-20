/* The C interface of libremodroid's WebRTC host, the same on Android, Linux,
 * macOS and Windows. The stack runs on its own thread with a libev loop; every
 * function here may be called from any thread (calls are queued to the loop),
 * and every callback runs on the loop thread. One host per process.
 *
 *   Android   android/native/jni.c binds it to io.remodroid.Native
 *   desktops  desktop/core/agent.c drives it in "embed" mode */
#ifndef RD_RTC_H
#define RD_RTC_H

#include <stddef.h>
#include <stdint.h>

#include "plat.h"
#include "rtc.h"

typedef struct {
    const char *listen;    /* "abstract:NAME", "tcp:HOST:PORT" or "embed" (see rtc_opts_t) */
    int direct;            /* phone: 1 = its own UDP sockets (Path B) */
    int offer_h265, offer_audio;
    int max_kbps;
    int log_level;         /* RD_LOG_* */
    const char *log_name;  /* log prefix ("phone", "desktop") */
} rd_rtc_opts_t;

typedef struct {
    rtc_cb_t rtc;                          /* the stack's callbacks (rtc.h), on the loop thread */
    void (*on_thread)(void *ud, int start); /* loop thread starting (1) / exiting (0): attach a JNIEnv… */
    void (*on_loop)(void *ud, struct ev_loop *loop); /* the loop exists: add the embedder's own watchers */
} rd_rtc_cb_t;

/* Starts the loop thread and the stack; returns 0, or -1 if the stack can't start
 * (e.g. the listen address is taken). Blocks until the stack is up. */
RD_EXPORT int    rd_rtc_start(const rd_rtc_opts_t *o, const rd_rtc_cb_t *cb, void *ud);
RD_EXPORT int    rd_rtc_running(void);
/* Ends the session, stops the loop and joins the thread. */
RD_EXPORT void   rd_rtc_stop(void);

RD_EXPORT void   rd_rtc_send_ctrl(const char *json);   /* to the agent / server: ready, status, event */
RD_EXPORT void   rd_rtc_ctrl_in(const char *json);     /* "embed": from the server */
RD_EXPORT void   rd_rtc_dc_send(const char *label, int binary, const void *d, size_t n);
RD_EXPORT void   rd_rtc_dc_open(const char *label);
/* One encoded access unit (Annex-B); prefix (parameter sets, may be NULL) goes first. Delta
 * frames are dropped while the loop is more than 4 MB behind. */
RD_EXPORT void   rd_rtc_video(const void *prefix, size_t prefix_len, const void *au, size_t n, int64_t pts_us, int key);
RD_EXPORT void   rd_rtc_audio(const void *opus, size_t n, int64_t pts_us);
RD_EXPORT void   rd_rtc_end_session(const char *reason);
RD_EXPORT void   rd_rtc_set_max_kbps(int kbps);
RD_EXPORT void   rd_rtc_set_direct(int direct);
/* The latest stats as JSON (refreshed twice a second); returns the length written. */
RD_EXPORT size_t rd_rtc_stats(char *buf, size_t cap);
/* Bytes queued in the data channels (approximate, refreshed twice a second). */
RD_EXPORT size_t rd_rtc_dc_buffered(void);
/* Runs fn(arg) on the loop thread. */
RD_EXPORT void   rd_rtc_call(void (*fn)(void *arg), void *arg);
/* Loop thread only: the stack itself. */
RD_EXPORT rtc_t *rd_rtc_stack(void);

#endif
