/* Hand-written libsrtp2 config for RemoDroid (Linux / Android, any CPU).
 * Internal crypto (AES-CM + HMAC-SHA1), no OpenSSL/NSS. */
#ifndef RD_SRTP_CONFIG_H
#define RD_SRTP_CONFIG_H
#define PACKAGE_NAME "libsrtp2"
#define PACKAGE_STRING "libsrtp2 2.6.0"
#define PACKAGE_VERSION "2.6.0"
#define CPU_RISC 1
#define HAVE_ARPA_INET_H 1
#define HAVE_BYTESWAP_H 1
#define HAVE_INET_PTON 1
#define HAVE_INT16_T 1
#define HAVE_INT32_T 1
#define HAVE_INT8_T 1
#define HAVE_INTTYPES_H 1
#define HAVE_NETINET_IN_H 1
#define HAVE_SIGACTION 1
#define HAVE_SOCKET 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_SOCKET_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_UIO_H 1
#define HAVE_UINT16_T 1
#define HAVE_UINT32_T 1
#define HAVE_UINT64_T 1
#define HAVE_UINT8_T 1
#define HAVE_UNISTD_H 1
#define HAVE_USLEEP 1
#define SIZEOF_UNSIGNED_LONG __SIZEOF_LONG__
#define SIZEOF_UNSIGNED_LONG_LONG 8
#define STDC_HEADERS 1
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define WORDS_BIGENDIAN 1
#endif
#endif
