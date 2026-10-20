/* Redeeming a one-time pairing code from the panel (server/enroll.c, or the
 * Cloudflare Worker): POST /api/v1/enroll {code, name, arch, platform, version}
 * → {device_id, device_secret, ws_path, name}. Blocking: call it off the event
 * loop (the router CLI, or a desktop agent's pairing thread). */
#ifndef RD_ENROLL_H
#define RD_ENROLL_H

#include <stddef.h>

typedef struct {
    char ws_url[400];        /* wss://panel/ws/agent */
    char device_id[64], device_secret[128], name[64];
} rd_enroll_t;

/* panel: "https://remo.example.com" (http:// for a LAN panel). Returns 0, or -1 with err set to
 * a sentence for the user. */
int rd_enroll(const char *panel, const char *code, const char *name, const char *platform, const char *ca_file,
              const char *user_agent, rd_enroll_t *out, char *err, size_t errcap);

/* Reads the line the panel shows ("remodroid enroll --panel https://… --code X5SX-K3M7", the
 * desktop CLI's "remodroid-desktopd pair …", --panel=… forms, or just "https://… X5SX-K3M7") into
 * the panel address and the code. Either may be missing from the line and is then left as it
 * was, so a caller can pre-fill one (e.g. the panel it was paired with before). Returns the number
 * of the two that the line gave. */
int rd_enroll_parse(const char *line, char *panel, size_t panelcap, char *code, size_t codecap);

#endif
