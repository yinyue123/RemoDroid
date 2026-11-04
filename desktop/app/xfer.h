/* The File Transfer window's list, shared by the three apps: rd_agent_transfers() read into
 * entries, with what each bubble shows (size, the time between groups, the file-type badge). */
#ifndef RD_XFER_H
#define RD_XFER_H

#include <stddef.h>

typedef struct {
    long long ms, size;
    int in;           /* 1: the viewer's browser sent it here; 0: sent from here to the viewer */
    const char *path; /* UTF-8, inside the text given to rd_xfers_parse */
    const char *name; /* the file name, inside path */
    int stamp;        /* a time label goes above this entry (the first, or 5 minutes after the one before) */
} rd_xfer_t;

/* Parses rd_agent_transfers() text in place; *out (caller frees) points into text. Returns the count. */
int  rd_xfers_parse(char *text, rd_xfer_t **out);
void rd_xfer_size(long long n, char *out, size_t cap);     /* "31.0 KB" */
void rd_xfer_when(long long ms, char *out, size_t cap);    /* "17:50" today, "10/21 11:11" before, local time */
/* The badge: up to 4 letters of the extension ("PDF", "ZIP", "FILE") and its colour (0xRRGGBB). */
void rd_xfer_badge(const char *name, char *ext, size_t cap, unsigned *rgb);
int  rd_xfer_is_image(const char *name);                   /* shown as a picture rather than a badge */

#endif
