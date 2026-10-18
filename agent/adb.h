/* Built-in ADB host client (plan §4.2): talks to the phone's adbd directly,
 * over USB (usbfs) or TCP, without an adb binary or adb server. */
#ifndef RD_ADB_H
#define RD_ADB_H

#include "ev.h"
#include "rd.h"

typedef struct adb adb_t;
typedef struct adb_stream adb_stream_t;

enum { ADB_OFFLINE, ADB_CONNECTING, ADB_UNAUTHORIZED, ADB_ONLINE };

/* ---- transport (usb.c) ---- */
typedef struct {
    int (*send)(void *t, const uint8_t hdr[24], const uint8_t *payload, size_t n);
    void (*close)(void *t);
    void *t;
    const char *kind;  /* "usb" / "tcp" */
    char speed[16];    /* "high", "super", ... */
} adb_transport_t;

/* ---- core ---- */
typedef struct {
    void (*on_state)(adb_t *a, int state, void *ud);
} adb_callbacks_t;

adb_t      *adb_new(struct ev_loop *loop, const char *keypath, adb_callbacks_t cb, void *ud);
/* Attach a connected transport and start the CNXN/AUTH handshake. */
void        adb_attach(adb_t *a, adb_transport_t tr, const char *serial);
/* Transport hooks. */
void        adb_on_packet(adb_t *a, const uint8_t hdr[24], const uint8_t *payload, size_t n);
void        adb_on_transport_closed(adb_t *a);
void        adb_disconnect(adb_t *a);
int         adb_state(const adb_t *a);
const char *adb_state_name(int state);
const char *adb_serial(const adb_t *a);
const char *adb_banner_prop(const adb_t *a, const char *key, char *out, size_t cap);
int         adb_has_feature(const adb_t *a, const char *feature);
uint32_t    adb_max_payload(const adb_t *a);
void        adb_stats(const adb_t *a, uint64_t *tx, uint64_t *rx, int *streams);
const adb_transport_t *adb_transport(const adb_t *a);

/* ---- streams ---- */
/* A stream is freed right after on_close, or after on_open with ok = 0;
 * don't touch it (or call adb_close) after either. adb_close() suppresses all
 * further callbacks except on_free, which runs exactly once when the stream
 * memory is released (use it to free ud). */
typedef struct {
    void (*on_open)(adb_stream_t *s, int ok, void *ud);
    void (*on_data)(adb_stream_t *s, const uint8_t *data, size_t n, void *ud);
    void (*on_close)(adb_stream_t *s, void *ud);
    void (*on_free)(void *ud);
} adb_stream_cb_t;

adb_stream_t *adb_open(adb_t *a, const char *service, adb_stream_cb_t cb, void *ud);
int           adb_write(adb_stream_t *s, const void *data, size_t n);
size_t        adb_backlog(const adb_stream_t *s);
/* Flow control towards us: while held, the phone gets no OKAY for its last write, so it
 * sends nothing more on this stream. Release with hold = 0. */
void          adb_hold(adb_stream_t *s, int hold);
void          adb_close(adb_stream_t *s); /* no further callbacks */

/* ---- services ---- */
/* Runs a command; out_cb (optional) streams stdout+stderr; done_cb gets the exit
 * code (-1 if unknown or the stream failed) and the collected output. */
typedef void (*adb_out_cb)(const uint8_t *data, size_t n, void *ud);
typedef void (*adb_done_cb)(int code, const char *output, size_t n, void *ud);
adb_stream_t *adb_shell(adb_t *a, const char *cmd, adb_out_cb out_cb, adb_done_cb done_cb, void *ud);

/* Push a local file; skipped when the remote file already has the same size and
 * mtime. done: code 0 = pushed, 1 = unchanged, -1 = error (msg set). */
typedef void (*adb_file_cb)(int code, const char *msg, void *ud);
int adb_push(adb_t *a, const char *local, const char *remote, unsigned mode, adb_file_cb cb, void *ud);
int adb_pull(adb_t *a, const char *remote, const char *local, adb_file_cb cb, void *ud);

#endif
