/* Shared helpers for the RemoDroid agent and server. */
#ifndef RD_H
#define RD_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

#define RD_VERSION "0.1.0"
#define RD_PROTO   1

#define RD_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define RD_MIN(a, b) ((a) < (b) ? (a) : (b))
#define RD_MAX(a, b) ((a) > (b) ? (a) : (b))

/* ---- logging ---- */
enum { RD_LOG_ERR, RD_LOG_WARN, RD_LOG_INFO, RD_LOG_DEBUG, RD_LOG_TRACE };
extern int rd_log_level;
void rd_log_init(const char *prog, int level);
int  rd_log_parse_level(const char *s, int def);
void rd_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* Optional hook that also receives every formatted line (used for `diag`). */
void rd_log_set_hook(void (*hook)(const char *line));

#define LOGE(...) rd_log(RD_LOG_ERR, __VA_ARGS__)
#define LOGW(...) rd_log(RD_LOG_WARN, __VA_ARGS__)
#define LOGI(...) rd_log(RD_LOG_INFO, __VA_ARGS__)
#define LOGD(...) do { if (rd_log_level >= RD_LOG_DEBUG) rd_log(RD_LOG_DEBUG, __VA_ARGS__); } while (0)
#define LOGT(...) do { if (rd_log_level >= RD_LOG_TRACE) rd_log(RD_LOG_TRACE, __VA_ARGS__); } while (0)

/* ---- growable byte buffer: [off, len) is unread data ---- */
typedef struct {
    uint8_t *p;
    size_t len, cap, off;
} rd_buf_t;

void   rd_buf_init(rd_buf_t *b);
void   rd_buf_free(rd_buf_t *b);
int    rd_buf_reserve(rd_buf_t *b, size_t extra);
int    rd_buf_append(rd_buf_t *b, const void *data, size_t n);
int    rd_buf_printf(rd_buf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void   rd_buf_consume(rd_buf_t *b, size_t n);
void   rd_buf_reset(rd_buf_t *b);
static inline uint8_t *rd_buf_data(rd_buf_t *b) { return b->p + b->off; }
static inline size_t   rd_buf_avail(const rd_buf_t *b) { return b->len - b->off; }

/* ---- misc utilities ---- */
uint64_t rd_now_ms(void);           /* wall clock, Unix epoch ms */
uint64_t rd_mono_ms(void);          /* monotonic ms */
int      rd_random(void *out, size_t n);
void     rd_hex(const uint8_t *in, size_t n, char *out);   /* out: 2n+1 */
int      rd_set_nonblock(int fd);
char    *rd_strdup(const char *s);
int      rd_streq_nocase(const char *a, const char *b);
size_t   rd_b64_encode(const void *in, size_t n, char *out, size_t outcap);   /* NUL-terminated */
int      rd_b64_decode(const char *in, size_t n, uint8_t *out, size_t outcap, size_t *outlen);
void     rd_sha1(const void *data, size_t n, uint8_t out[20]);
void     rd_hmac_sha1(const void *key, size_t keylen, const void *data, size_t n, uint8_t out[20]);
void     rd_sha256(const void *data, size_t n, uint8_t out[32]);
uint16_t rd_get_be16(const uint8_t *p);
uint32_t rd_get_be32(const uint8_t *p);
void     rd_put_be16(uint8_t *p, uint16_t v);
void     rd_put_be32(uint8_t *p, uint32_t v);
uint32_t rd_get_le32(const uint8_t *p);
void     rd_put_le32(uint8_t *p, uint32_t v);
/* mkdir -p; the last component gets mode. */
int      rd_mkdirs(const char *path, unsigned mode);
/* Parse "host:port" / "[v6]:port"; host/port are written NUL-terminated. */
int      rd_split_hostport(const char *in, char *host, size_t hostcap, char *port, size_t portcap, const char *defport);

#endif
