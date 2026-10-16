/* RFC 6455 WebSocket framing, shared by the agent (client) and server. */
#ifndef RD_WS_H
#define RD_WS_H

#include "rd.h"

enum { WS_OP_CONT = 0, WS_OP_TEXT = 1, WS_OP_BIN = 2, WS_OP_CLOSE = 8, WS_OP_PING = 9, WS_OP_PONG = 10 };

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define WS_MAX_MESSAGE (1u << 20) /* 1 MB: control messages only */

typedef struct {
    int fin, opcode;
    uint8_t *payload; /* points into the parse buffer, already unmasked */
    size_t len;
} ws_frame_t;

/* Parse one frame from data. Returns bytes consumed (>0), 0 if more data is
 * needed, or -1 on a protocol error. Masked payloads are unmasked in place.
 * `need_mask`: 1 = frames must be masked (server side), 0 = must not be (client side). */
int ws_parse(uint8_t *data, size_t len, int need_mask, ws_frame_t *f);

/* Append one complete frame. `mask` = 1 on the client side. */
int ws_build(rd_buf_t *out, int opcode, const void *data, size_t len, int mask);

/* Sec-WebSocket-Accept value for a client key (out: 29 bytes incl. NUL). */
void ws_accept_key(const char *client_key, char out[29]);
/* Random Sec-WebSocket-Key (out: 25 bytes incl. NUL). */
void ws_client_key(char out[25]);

/* Reassembles fragmented messages. Feed parsed frames; when a complete data
 * message is ready, op, msg and len describe it (valid until the next call). */
typedef struct {
    rd_buf_t frag;
    int frag_op;
} ws_asm_t;

/* Returns 1 when a complete message is available, 0 when not (control frames
 * are returned directly as complete messages), -1 on error. */
int  ws_asm_feed(ws_asm_t *a, const ws_frame_t *f, int *op, const uint8_t **msg, size_t *len);
void ws_asm_free(ws_asm_t *a);

#endif
