/* Trusted root certificates for wss:// and https:// clients: a given file,
 * or the operating system's store (Linux bundle files, the Windows ROOT
 * store, the macOS keychain anchors). */
#ifndef RD_CAS_H
#define RD_CAS_H

#include <stddef.h>

#include <mbedtls/x509_crt.h>

/* ca_file "" or NULL: the system store. Returns the number of certificates loaded (> 0), or -1
 * with err set. `from` (may be NULL) describes where they came from, for logs. */
int rd_load_cas(mbedtls_x509_crt *ca, const char *ca_file, char *from, size_t fromcap, char *err, size_t errcap);

#endif
