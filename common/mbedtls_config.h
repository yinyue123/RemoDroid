/* Minimal mbedTLS configuration for the agent and server:
 * hashes (SHA-1 for WebSocket/HMAC/ADB, SHA-256, MD5 for TURN), base64,
 * and RSA key handling for ADB authentication. No TLS. */
#ifndef RD_MBEDTLS_CONFIG_H
#define RD_MBEDTLS_CONFIG_H

#define MBEDTLS_HAVE_TIME
#define MBEDTLS_FS_IO
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_GENPRIME

#define MBEDTLS_AES_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_MD_C
#define MBEDTLS_MD5_C
#define MBEDTLS_OID_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_PEM_WRITE_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PK_WRITE_C
#define MBEDTLS_RSA_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C

#endif
