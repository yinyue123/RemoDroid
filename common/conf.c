#include "conf.h"

#include <stdlib.h>
#include <string.h>

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
