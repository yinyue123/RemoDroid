/* STUN (RFC 8489) / TURN (RFC 8656) messages for ICE and the TURN client. */
#ifndef RTC_STUN_H
#define RTC_STUN_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#define STUN_MAGIC 0x2112A442u

enum {
    STUN_BINDING = 0x0001, STUN_ALLOCATE = 0x0003, STUN_REFRESH = 0x0004, STUN_SEND = 0x0006,
    STUN_DATA = 0x0007, STUN_CREATE_PERMISSION = 0x0008, STUN_CHANNEL_BIND = 0x0009,
};
enum { STUN_REQUEST = 0x0000, STUN_INDICATION = 0x0010, STUN_SUCCESS = 0x0100, STUN_ERROR = 0x0110 };

enum {
    STUN_A_MAPPED_ADDRESS = 0x0001, STUN_A_USERNAME = 0x0006, STUN_A_MESSAGE_INTEGRITY = 0x0008,
    STUN_A_ERROR_CODE = 0x0009, STUN_A_CHANNEL_NUMBER = 0x000C, STUN_A_LIFETIME = 0x000D,
    STUN_A_XOR_PEER_ADDRESS = 0x0012, STUN_A_DATA = 0x0013, STUN_A_REALM = 0x0014, STUN_A_NONCE = 0x0015,
    STUN_A_XOR_RELAYED_ADDRESS = 0x0016, STUN_A_REQUESTED_TRANSPORT = 0x0019, STUN_A_XOR_MAPPED_ADDRESS = 0x0020,
    STUN_A_PRIORITY = 0x0024, STUN_A_USE_CANDIDATE = 0x0025, STUN_A_SOFTWARE = 0x8022,
    STUN_A_FINGERPRINT = 0x8028, STUN_A_ICE_CONTROLLED = 0x8029, STUN_A_ICE_CONTROLLING = 0x802A,
};

static inline uint16_t stun_method(uint16_t type) { return type & 0x3EEF; }
static inline uint16_t stun_class(uint16_t type) { return type & 0x0110; }

typedef struct {
    uint8_t buf[1500];
    size_t len;
} stun_msg_t;

void stun_new(stun_msg_t *m, uint16_t type, const uint8_t tid[12]); /* tid NULL = random */
void stun_add(stun_msg_t *m, uint16_t attr, const void *v, size_t n);
void stun_add_u32(stun_msg_t *m, uint16_t attr, uint32_t v);
void stun_add_u64(stun_msg_t *m, uint16_t attr, uint64_t v);
void stun_add_xaddr(stun_msg_t *m, uint16_t attr, const struct sockaddr_storage *a);
void stun_add_integrity(stun_msg_t *m, const void *key, size_t keylen);
void stun_add_fingerprint(stun_msg_t *m);
const uint8_t *stun_tid(const stun_msg_t *m);

typedef struct {
    uint16_t type;
    const uint8_t *msg;
    size_t len;
    const uint8_t *tid;
} stun_view_t;

/* Quick test: does this datagram look like STUN? */
int stun_is(const uint8_t *d, size_t n);
int stun_parse(const uint8_t *d, size_t n, stun_view_t *v);
const uint8_t *stun_attr(const stun_view_t *v, uint16_t attr, size_t *len);
int  stun_get_xaddr(const stun_view_t *v, uint16_t attr, struct sockaddr_storage *out);
int  stun_get_u32(const stun_view_t *v, uint16_t attr, uint32_t *out);
int  stun_check_integrity(const stun_view_t *v, const void *key, size_t keylen);
int  stun_check_fingerprint(const stun_view_t *v);
int  stun_error_code(const stun_view_t *v); /* 0 when absent */
/* TURN long-term credential key: MD5(user ":" realm ":" pass). */
void stun_lt_key(const char *user, const char *realm, const char *pass, uint8_t out[16]);
uint32_t stun_crc32(const uint8_t *d, size_t n);

/* sockaddr helpers */
int  addr_equal(const struct sockaddr_storage *a, const struct sockaddr_storage *b);
void addr_str(const struct sockaddr_storage *a, char *out, size_t cap); /* "1.2.3.4:5" / "[::1]:5" */
int  addr_parse(const char *ip, int port, struct sockaddr_storage *out);
int  addr_port(const struct sockaddr_storage *a);
socklen_t addr_len(const struct sockaddr_storage *a);

#endif
