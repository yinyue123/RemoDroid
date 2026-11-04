/* The File Transfer list (rd_agent_transfers): files the viewer's browser sent to this computer and
 * files sent from it to the viewer, one line each in transfers.log beside desktop.ini:
 * "<unix ms>\t<in|out>\t<size>\t<path>". The apps' File Transfer windows show it; the newest
 * KEEP lines are kept. Written on the agent thread, read on the UI's. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RD_BUILDING_LIB
#include "rd_agent.h"

#include "desk.h"

#define KEEP 500
#define TRIM_AT (256 * 1024) /* bytes before the file is cut back to the newest KEEP lines */

static rd_mutex_t mu;
static unsigned seq, received; /* received: files the browser sent here */
static char path[1024];

void transfers_init(void)
{
    rd_mutex_init(&mu);
    snprintf(path, sizeof path, "%s%ctransfers.log", dsk_config_dir(), RD_PATH_SEP);
}

/* The whole file, NUL-terminated (an empty string if there is none); caller frees. */
static char *slurp(long *len)
{
    FILE *f = fopen(path, "rb");
    long n = 0;
    char *s = NULL;
    if (f && fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0) {
        s = malloc((size_t)n + 1);
        n = (long)fread(s, 1, (size_t)n, f);
    }
    if (f)
        fclose(f);
    if (!s)
        s = malloc(1), n = 0;
    s[n] = 0;
    *len = n;
    return s;
}

/* Where the newest KEEP lines start. */
static const char *newest(const char *s, long n)
{
    int lines = 0;
    for (long i = n - 1; i > 0; i--)
        if (s[i - 1] == '\n' && ++lines == KEEP)
            return s + i;
    return s;
}

void transfers_add(const char *dir, const char *file, long long size)
{
    if (!path[0] || strpbrk(file, "\t\r\n"))
        return;
    rd_mutex_lock(&mu);
    FILE *f = fopen(path, "ab");
    if (f) {
        fprintf(f, "%llu\t%s\t%lld\t%s\n", (unsigned long long)rd_now_ms(), dir, size, file);
        long end = ftell(f);
        fclose(f);
        if (end > TRIM_AT) {
            long n;
            char *s = slurp(&n);
            const char *keep = newest(s, n);
            if ((f = fopen(path, "wb"))) {
                fwrite(keep, 1, strlen(keep), f);
                fclose(f);
            }
            free(s);
        }
    }
    seq++;
    if (!strcmp(dir, "in"))
        received++;
    rd_mutex_unlock(&mu);
}

char *rd_agent_transfers(void)
{
    long n = 0;
    if (!path[0])
        return calloc(1, 1);
    rd_mutex_lock(&mu);
    char *s = slurp(&n);
    rd_mutex_unlock(&mu);
    const char *keep = newest(s, n);
    if (keep != s)
        memmove(s, keep, strlen(keep) + 1);
    return s;
}

unsigned rd_agent_transfers_seq(void)
{
    if (!path[0])
        return 0;
    rd_mutex_lock(&mu);
    unsigned s = seq;
    rd_mutex_unlock(&mu);
    return s;
}

char *rd_agent_transfers_active(void) { return files_live(); }

unsigned rd_agent_received_seq(void)
{
    if (!path[0])
        return 0;
    rd_mutex_lock(&mu);
    unsigned r = received;
    rd_mutex_unlock(&mu);
    return r;
}

void rd_agent_transfers_clear(void)
{
    if (!path[0])
        return;
    rd_mutex_lock(&mu);
    remove(path);
    seq++;
    rd_mutex_unlock(&mu);
}
