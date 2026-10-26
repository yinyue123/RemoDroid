/* mbedTLS configuration for the agent and server (a unique file name, so
 * MBEDTLS_CONFIG_FILE can't pick up mbedTLS's own mbedtls_config.h instead):
 * hashes (SHA-1 for WebSocket/HMAC/ADB, SHA-256, MD5 for TURN), base64,
 * RSA key handling for ADB authentication, and TLS 1.2: the agent's wss://
 * client, the server's HTTPS listener, and the server's ACME client (ECDSA
 * P-256 keys, certificate requests). */
#ifndef RD_MBEDTLS_CONFIG_H
#define RD_MBEDTLS_CONFIG_H

#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE
#define MBEDTLS_FS_IO
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_GENPRIME

#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_SSL_SERVER_NAME_INDICATION

#define MBEDTLS_AES_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_CHACHAPOLY_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_ERROR_C
#define MBEDTLS_GCM_C
#define MBEDTLS_MD_C
#define MBEDTLS_MD5_C
#define MBEDTLS_OID_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_PEM_WRITE_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PK_WRITE_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_RSA_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_SRV_C
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_X509_CREATE_C
#define MBEDTLS_X509_CSR_WRITE_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C

#ifdef _WIN32
#define MBEDTLS_PLATFORM_C /* mbedTLS requires it on Windows (its snprintf shim) */
#endif

#endif
