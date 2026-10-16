#include "msg.h"

#include <stdlib.h>

cJSON *msg_new(const char *type, long id, const char *sid)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "t", type);
    if (id > 0)
        cJSON_AddNumberToObject(m, "id", (double)id);
    if (sid)
        cJSON_AddStringToObject(m, "sid", sid);
    cJSON_AddObjectToObject(m, "d");
    return m;
}

cJSON *msg_d(cJSON *m)
{
    cJSON *d = cJSON_GetObjectItemCaseSensitive(m, "d");
    if (!cJSON_IsObject(d)) {
        cJSON_DeleteItemFromObjectCaseSensitive(m, "d");
        d = cJSON_AddObjectToObject(m, "d");
    }
    return d;
}

cJSON *msg_ack(long id, cJSON *d)
{
    cJSON *m = msg_new("ack", id, NULL);
    if (d)
        cJSON_ReplaceItemInObjectCaseSensitive(m, "d", d);
    return m;
}

cJSON *msg_error(long id, const char *code, const char *text)
{
    cJSON *m = msg_new("error", id, NULL);
    cJSON *d = msg_d(m);
    cJSON_AddStringToObject(d, "code", code);
    cJSON_AddStringToObject(d, "msg", text ? text : code);
    return m;
}

const char *msg_str(const cJSON *obj, const char *key, const char *def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) ? v->valuestring : def;
}

double msg_num(const cJSON *obj, const char *key, double def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

int msg_bool(const cJSON *obj, const char *key, int def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : def;
}

const char *msg_type(const cJSON *m) { return msg_str(m, "t", ""); }
long        msg_id(const cJSON *m) { return (long)msg_num(m, "id", 0); }
const char *msg_sid(const cJSON *m) { return msg_str(m, "sid", NULL); }
char       *msg_print(const cJSON *m) { return cJSON_PrintUnformatted(m); }
