/* RemoDroid desktop agent: the interface the desktop apps (desktop/app/) use.
 * Everything behind it (server connection, WebRTC, capture, encoding, input,
 * files) lives in libremodroid (remodroid.dll, libremodroid.dylib,
 * libremodroid.so). Functions may be called from any thread; on_status runs on
 * the agent's thread, so a UI marshals it to its own thread. */
#ifndef RD_AGENT_H
#define RD_AGENT_H

#include <stddef.h>

#if defined(_WIN32)
#if defined(RD_BUILDING_LIB)
#define RD_AGENT_API __declspec(dllexport)
#else
#define RD_AGENT_API __declspec(dllimport)
#endif
#else
#define RD_AGENT_API __attribute__((visibility("default")))
#endif

typedef enum {
    RD_UNPAIRED,    /* no [server] credentials yet */
    RD_CONNECTING,  /* paired, server not reached (yet) */
    RD_ONLINE,      /* connected, waiting for a viewer */
    RD_IN_SESSION,  /* a viewer is connected */
    RD_STOPPED,     /* not started, or stopped */
} rd_state_t;

typedef struct {
    rd_state_t state;
    char device_id[64];
    char panel[256];     /* https://… derived from the server URL */
    char viewer[64];     /* who is viewing, when RD_IN_SESSION ("" if the server didn't say) */
    long long since_ms;  /* session start (Unix ms), when RD_IN_SESSION */
    char problem[200];   /* "Screen Recording permission is off", "cannot resolve …", or "" */
    int view_only;
} rd_status_t;

typedef struct {
    void (*on_status)(void *ud, const rd_status_t *s);
    /* [desktop] confirm = true: a viewer wants in; answer with rd_agent_confirm() within 30 s.
     * Without this callback such sessions are refused. */
    void (*on_confirm)(void *ud, const char *viewer);
    /* Optional: show a notification ("alice is viewing this computer."); without it the agent uses
     * the system's own (macOS, Linux). */
    void (*on_notify)(void *ud, const char *text);
} rd_agent_cb_t;

/* The per-user desktop.ini path (created on the first save). */
RD_AGENT_API const char *rd_agent_default_conf(void);
/* Starts the agent with this configuration file (NULL = the default). Returns 0, or -1 with the
 * reason logged (e.g. the WebRTC stack failed to start). */
RD_AGENT_API int  rd_agent_start(const char *conf_path, rd_agent_cb_t cb, void *ud);
RD_AGENT_API void rd_agent_stop(void);
/* Redeems a pairing code (Agents → Add → Computer on the panel). Runs in the background;
 * on_done gets ok = 1 and the device name, or ok = 0 and the reason. */
RD_AGENT_API void rd_agent_pair(const char *panel, const char *code, const char *name,
                                void (*on_done)(void *ud, int ok, const char *msg), void *ud);
/* Forgets the server credentials and disconnects. */
RD_AGENT_API void rd_agent_unpair(void);
/* The answer to on_confirm. */
RD_AGENT_API void rd_agent_confirm(int allow);
/* Ends the current viewer session ("Disconnect viewer"). */
RD_AGENT_API void rd_agent_end_session(void);
/* Changes a [desktop] / [video] / [files] setting ("desktop.view_only", "true") and saves it. */
RD_AGENT_API void rd_agent_set(const char *key, const char *value);
RD_AGENT_API void rd_agent_status(rd_status_t *out);
/* The last log lines, newest last, one per line (for a "Copy log" button). Caller frees. */
RD_AGENT_API char *rd_agent_log(int lines);
RD_AGENT_API void rd_agent_free(void *p);
RD_AGENT_API const char *rd_agent_version(void);

#endif
