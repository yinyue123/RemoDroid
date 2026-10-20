/* mbedTLS for the desktop library: the WebRTC stack's DTLS (rtc/) plus the
 * TLS client the server connection and pairing use (common/). */
#ifndef RD_DESKTOP_MBEDTLS_CONFIG_H
#define RD_DESKTOP_MBEDTLS_CONFIG_H
#include "../rtc/mbedtls_rtc_config.h"
#include "../common/rd_mbedtls_config.h"
#ifdef _WIN32
#define MBEDTLS_PLATFORM_C /* mbedTLS requires it on Windows (its snprintf shim) */
#endif
#endif
