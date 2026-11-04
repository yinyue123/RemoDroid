#include "xfer.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int rd_xfers_parse(char *text, rd_xfer_t **out)
{
    int cap = 64, n = 0;
    rd_xfer_t *v = malloc(sizeof *v * (size_t)cap);
    long long prev = 0;
    for (char *line = text; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        char *f[4];
        int k = 0;
        for (char *p = line; k < 4; k++) {
            f[k] = p;
            char *t = k < 3 ? strchr(p, '\t') : NULL;
            if (k < 3 && !t)
                break;
            if (t)
                *t = 0, p = t + 1;
        }
        if (k == 4 && *f[3]) {
            if (n == cap)
                v = realloc(v, sizeof *v * (size_t)(cap *= 2));
            rd_xfer_t *x = &v[n++];
            x->ms = atoll(f[0]);
            x->in = !strcmp(f[1], "in");
            x->size = atoll(f[2]);
            x->path = f[3];
            x->name = f[3];
            for (const char *s = f[3]; *s; s++)
                if (*s == '/' || *s == '\\')
                    x->name = s + 1;
            x->stamp = n == 1 || x->ms - prev > 5 * 60 * 1000;
            prev = x->ms;
        }
        line = nl ? nl + 1 : NULL;
    }
    *out = v;
    return n;
}

void rd_xfer_size(long long n, char *out, size_t cap)
{
    if (n < 1024)
        snprintf(out, cap, "%lld B", n);
    else if (n < 1024 * 1024)
        snprintf(out, cap, "%.1f KB", n / 1024.0);
    else if (n < 1024LL * 1024 * 1024)
        snprintf(out, cap, "%.1f MB", n / 1048576.0);
    else
        snprintf(out, cap, "%.2f GB", n / 1073741824.0);
}

void rd_xfer_when(long long ms, char *out, size_t cap)
{
    time_t t = (time_t)(ms / 1000), now = time(NULL);
    struct tm a, b;
#ifdef _WIN32
    localtime_s(&a, &t);
    localtime_s(&b, &now);
#else
    localtime_r(&t, &a);
    localtime_r(&now, &b);
#endif
    if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday)
        snprintf(out, cap, "%02d:%02d", a.tm_hour, a.tm_min);
    else if (a.tm_year == b.tm_year)
        snprintf(out, cap, "%02d/%02d %02d:%02d", a.tm_mon + 1, a.tm_mday, a.tm_hour, a.tm_min);
    else
        snprintf(out, cap, "%d/%02d/%02d %02d:%02d", a.tm_year + 1900, a.tm_mon + 1, a.tm_mday, a.tm_hour, a.tm_min);
}

static const char *ext_of(const char *name)
{
    const char *d = strrchr(name, '.');
    return d && d != name && d[1] ? d + 1 : "";
}

static int ext_in(const char *e, const char *list)
{
    size_t n = strlen(e);
    for (const char *p = list; *p;) {
        const char *s = strchr(p, ' ');
        size_t k = s ? (size_t)(s - p) : strlen(p);
        if (k == n) {
            size_t i = 0;
            while (i < n && tolower((unsigned char)e[i]) == p[i])
                i++;
            if (i == n)
                return 1;
        }
        p += k + (s != NULL);
    }
    return 0;
}

int rd_xfer_is_image(const char *name) { return ext_in(ext_of(name), "png jpg jpeg gif bmp webp heic"); }

void rd_xfer_badge(const char *name, char *ext, size_t cap, unsigned *rgb)
{
    static const struct {
        const char *exts;
        unsigned rgb;
    } K[] = {
        { "pdf", 0xD9453B },
        { "doc docx odt rtf pages", 0x2B6CD9 },
        { "xls xlsx csv ods numbers", 0x1F9D55 },
        { "ppt pptx odp key", 0xE0782A },
        { "zip rar 7z gz tgz xz bz2 tar", 0xC99A1C },
        { "png jpg jpeg gif bmp webp heic svg", 0x8E5BD9 },
        { "mp4 mov mkv avi webm mp3 m4a wav flac ogg", 0xD94A8C },
        { "apk exe msi dmg pkg deb rpm appimage", 0x4F7A8A },
        { "txt md log json xml yml yaml ini", 0x6B7280 },
    };
    const char *e = ext_of(name);
    *rgb = 0x8A94A6;
    for (size_t i = 0; i < sizeof K / sizeof K[0]; i++)
        if (*e && ext_in(e, K[i].exts))
            *rgb = K[i].rgb;
    size_t n = strlen(e);
    if (!n || n > 4) {
        snprintf(ext, cap, "FILE");
        return;
    }
    size_t i = 0;
    for (; i < n && i + 1 < cap; i++)
        ext[i] = (char)toupper((unsigned char)e[i]);
    ext[i] = 0;
}
