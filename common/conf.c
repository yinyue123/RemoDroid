#include "conf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ini.h"
#include "rd.h"

typedef struct kv {
    char *sec, *key, *val;
    struct kv *next;
} kv_t;

struct rd_conf {
    kv_t *head, *tail;
};

static int on_kv(void *ud, const char *sec, const char *key, const char *val)
{
    rd_conf_t *c = ud;
    kv_t *e = calloc(1, sizeof *e);
    if (!e)
        return 0;
    e->sec = rd_strdup(sec);
    e->key = rd_strdup(key);
    e->val = rd_strdup(val);
    if (c->tail)
        c->tail->next = e;
    else
        c->head = e;
    c->tail = e;
    return 1;
}

rd_conf_t *rd_conf_load(const char *path, int *errline)
{
    rd_conf_t *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    int r = ini_parse(path, on_kv, c);
    if (errline)
        *errline = r;
    if (r != 0) {
        rd_conf_free(c);
        return NULL;
    }
    return c;
}

void rd_conf_free(rd_conf_t *c)
{
    if (!c)
        return;
    for (kv_t *e = c->head, *n; e; e = n) {
        n = e->next;
        free(e->sec);
        free(e->key);
        free(e->val);
        free(e);
    }
    free(c);
}

const char *rd_conf_get(const rd_conf_t *c, const char *sec, const char *key, const char *def)
{
    const char *v = def;
    for (const kv_t *e = c ? c->head : NULL; e; e = e->next)
        if (!strcmp(e->sec, sec) && !strcmp(e->key, key))
            v = e->val; /* last one wins */
    return v;
}

long rd_conf_int(const rd_conf_t *c, const char *sec, const char *key, long def)
{
    const char *v = rd_conf_get(c, sec, key, NULL);
    if (!v || !*v)
        return def;
    char *end;
    long n = strtol(v, &end, 10);
    return *end ? def : n;
}

int rd_conf_bool(const rd_conf_t *c, const char *sec, const char *key, int def)
{
    const char *v = rd_conf_get(c, sec, key, NULL);
    if (!v || !*v)
        return def;
    if (!strcmp(v, "1") || rd_streq_nocase(v, "true") || rd_streq_nocase(v, "yes") || rd_streq_nocase(v, "on"))
        return 1;
    if (!strcmp(v, "0") || rd_streq_nocase(v, "false") || rd_streq_nocase(v, "no") || rd_streq_nocase(v, "off"))
        return 0;
    return def;
}

void rd_conf_each(const rd_conf_t *c, const char *sec, void (*fn)(const char *key, const char *val, void *ud), void *ud)
{
    for (const kv_t *e = c ? c->head : NULL; e; e = e->next)
        if (!strcmp(e->sec, sec))
            fn(e->key, e->val, ud);
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = 0;
    return s;
}

int rd_conf_write(const char *path, const char *sec, const char *key, const char *val)
{
    FILE *f = fopen(path, "r");
    struct stat st;
    mode_t mode = f && fstat(fileno(f), &st) == 0 ? (st.st_mode & 07777) : 0600;
    rd_buf_t out;
    rd_buf_init(&out);
    char line[2048], cur[64] = "";
    int done = 0, in_sec = 0, sec_seen = 0;
    long insert_at = -1; /* offset in out after the last line of the section */
    while (f && fgets(line, sizeof line, f)) {
        char copy[2048];
        snprintf(copy, sizeof copy, "%s", line);
        char *t = trim(copy);
        if (*t == '[') {
            char *e = strchr(t, ']');
            if (e) {
                *e = 0;
                snprintf(cur, sizeof cur, "%s", t + 1);
                in_sec = !strcmp(cur, sec);
                sec_seen |= in_sec;
            }
        } else if (in_sec && !done && *t && *t != ';' && *t != '#') {
            char *eq = strchr(t, '=');
            if (eq) {
                *eq = 0;
                if (!strcmp(trim(t), key)) {
                    done = 1;
                    if (!val)
                        continue; /* removed */
                    /* keep an inline comment (" ; ..."), if any */
                    const char *cm = strstr(line, " ;");
                    if (cm) {
                        char c2[2048];
                        snprintf(c2, sizeof c2, "%s", cm);
                        rd_buf_printf(&out, "%s = %s   %s\n", key, val, trim(c2));
                    } else {
                        rd_buf_printf(&out, "%s = %s\n", key, val);
                    }
                    insert_at = (long)out.len;
                    continue;
                }
            }
        }
        rd_buf_printf(&out, "%s", line);
        if (in_sec && *t && *t != ';' && *t != '#')
            insert_at = (long)out.len;
    }
    if (f)
        fclose(f);
    if (!done && val) {
        char add[2048];
        int n = snprintf(add, sizeof add, "%s = %s\n", key, val);
        if (sec_seen && insert_at >= 0) {
            rd_buf_reserve(&out, (size_t)n);
            memmove(out.p + insert_at + n, out.p + insert_at, out.len - (size_t)insert_at);
            memcpy(out.p + insert_at, add, (size_t)n);
            out.len += (size_t)n;
        } else if (sec_seen) { /* section exists but is empty: right after its header */
            rd_buf_t again;
            rd_buf_init(&again);
            int placed = 0;
            char hdr[80];
            snprintf(hdr, sizeof hdr, "[%s]", sec);
            for (char *p = (char *)out.p, *end = p + out.len; p < end;) {
                char *nl = memchr(p, '\n', (size_t)(end - p));
                size_t ll = nl ? (size_t)(nl - p) + 1 : (size_t)(end - p);
                rd_buf_append(&again, p, ll);
                if (!placed && ll >= strlen(hdr) && !strncmp(p, hdr, strlen(hdr))) {
                    if (!nl)
                        rd_buf_append(&again, "\n", 1);
                    rd_buf_append(&again, add, (size_t)n);
                    placed = 1;
                }
                p += ll;
            }
            rd_buf_free(&out);
            out = again;
        } else {
            if (out.len && out.p[out.len - 1] != '\n')
                rd_buf_printf(&out, "\n");
            rd_buf_printf(&out, "%s[%s]\n%s", out.len ? "\n" : "", sec, add);
        }
    }
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *w = fopen(tmp, "w");
    if (w)
        fchmod(fileno(w), mode);
    int ok = w && fwrite(out.p, 1, out.len, w) == out.len;
    if (w && fclose(w) != 0)
        ok = 0;
    rd_buf_free(&out);
    if (!ok || rename(tmp, path) < 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}
