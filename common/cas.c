#include "cas.h"

#include <stdio.h>
#include <string.h>

#include <mbedtls/error.h>

#include "rd.h"

#if defined(_WIN32)
#include <windows.h>
#include <wincrypt.h>
#elif defined(__APPLE__) && defined(RD_DESKTOP)
#include <Security/Security.h>
#else
#include <unistd.h>
#endif

static int from_file(mbedtls_x509_crt *ca, const char *f, char *err, size_t errcap)
{
    int r = mbedtls_x509_crt_parse_file(ca, f);
    if (r < 0) {
        char e[160];
        mbedtls_strerror(r, e, sizeof e);
        snprintf(err, errcap, "cannot read CA file %s: %s", f, e);
        return -1;
    }
    return 0;
}

static int count(const mbedtls_x509_crt *ca)
{
    int n = 0;
    for (const mbedtls_x509_crt *c = ca; c && c->raw.len; c = c->next)
        n++;
    return n;
}

int rd_load_cas(mbedtls_x509_crt *ca, const char *ca_file, char *from, size_t fromcap, char *err, size_t errcap)
{
    char dummy[8];
    if (!from) {
        from = dummy;
        fromcap = sizeof dummy;
    }
    if (ca_file && *ca_file) {
        if (from_file(ca, ca_file, err, errcap) < 0)
            return -1;
        snprintf(from, fromcap, "%s", ca_file);
        return count(ca);
    }
#if defined(_WIN32)
    HCERTSTORE st = CertOpenSystemStoreA(0, "ROOT");
    if (st) {
        const CERT_CONTEXT *c = NULL;
        while ((c = CertEnumCertificatesInStore(st, c)))
            if (c->dwCertEncodingType & X509_ASN_ENCODING)
                mbedtls_x509_crt_parse_der(ca, c->pbCertEncoded, c->cbCertEncoded); /* skip ones mbedTLS can't read */
        CertCloseStore(st, 0);
    }
    snprintf(from, fromcap, "the Windows certificate store");
#elif defined(__APPLE__) && defined(RD_DESKTOP)
    CFArrayRef anchors = NULL;
    if (SecTrustCopyAnchorCertificates(&anchors) == errSecSuccess && anchors) {
        for (CFIndex i = 0; i < CFArrayGetCount(anchors); i++) {
            SecCertificateRef c = (SecCertificateRef)CFArrayGetValueAtIndex(anchors, i);
            CFDataRef der = SecCertificateCopyData(c);
            if (der) {
                mbedtls_x509_crt_parse_der(ca, CFDataGetBytePtr(der), (size_t)CFDataGetLength(der));
                CFRelease(der);
            }
        }
        CFRelease(anchors);
    }
    snprintf(from, fromcap, "the macOS system roots");
#else
    /* static binaries: look where distributions keep the CA bundle */
    static const char *bundles[] = {
        "/etc/ssl/certs/ca-certificates.crt", /* Debian, Ubuntu, Alpine, OpenWrt (ca-bundle) */
        "/etc/pki/tls/certs/ca-bundle.crt",   /* RHEL, CentOS, Fedora, Amazon Linux */
        "/etc/ssl/cert.pem",                  /* Alpine, BSD-style, macOS */
        "/etc/ssl/ca-bundle.pem",             /* openSUSE */
    };
    for (size_t i = 0; i < RD_ARRAY_LEN(bundles); i++)
        if (access(bundles[i], R_OK) == 0 && mbedtls_x509_crt_parse_file(ca, bundles[i]) >= 0) {
            snprintf(from, fromcap, "%s", bundles[i]);
            break;
        }
#endif
    int n = count(ca);
    if (n <= 0) {
        snprintf(err, errcap, "no CA bundle found (install ca-certificates / ca-bundle, or give a CA file)");
        return -1;
    }
    return n;
}
