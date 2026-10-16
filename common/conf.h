/* ini configuration files (plan §4.7), parsed with inih. */
#ifndef RD_CONF_H
#define RD_CONF_H

typedef struct rd_conf rd_conf_t;

rd_conf_t  *rd_conf_load(const char *path, int *errline);
void        rd_conf_free(rd_conf_t *c);
const char *rd_conf_get(const rd_conf_t *c, const char *sec, const char *key, const char *def);
long        rd_conf_int(const rd_conf_t *c, const char *sec, const char *key, long def);
int         rd_conf_bool(const rd_conf_t *c, const char *sec, const char *key, int def);
/* Calls fn for every key in a section, in file order. */
void        rd_conf_each(const rd_conf_t *c, const char *sec, void (*fn)(const char *key, const char *val, void *ud),
                         void *ud);

#endif
