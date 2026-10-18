/* JSON message envelope (plan Appendix A.2):
 *   {"t": "<type>", "id": 17, "sid": "s_…", "d": { … }} */
#ifndef RD_MSG_H
#define RD_MSG_H

#include "cJSON.h"

cJSON      *msg_new(const char *type, long id, const char *sid); /* id <= 0 / sid NULL are omitted */
cJSON      *msg_d(cJSON *m);                                     /* the "d" object, created if missing */
cJSON      *msg_ack(long id, cJSON *d);                          /* takes ownership of d (may be NULL) */
cJSON      *msg_error(long id, const char *code, const char *text);
const char *msg_type(const cJSON *m);
long        msg_id(const cJSON *m);
const char *msg_sid(const cJSON *m);
const char *msg_str(const cJSON *obj, const char *key, const char *def);
double      msg_num(const cJSON *obj, const char *key, double def);
int         msg_bool(const cJSON *obj, const char *key, int def);
char       *msg_print(const cJSON *m); /* unformatted; caller frees with free() */
/* A URL query string ("a=1&b=x%20y") as an object of strings. */
cJSON      *msg_query_json(const char *q, size_t n);

#endif
