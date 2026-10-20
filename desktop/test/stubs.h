#ifndef RD_STUBS_H
#define RD_STUBS_H

#include <stdarg.h>

#define STUB_MAX 64
typedef struct {
    char ev[STUB_MAX][64];
    int n;
} stub_log_t;
extern stub_log_t STUB;

#endif
