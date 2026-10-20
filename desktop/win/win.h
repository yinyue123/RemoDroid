/* Shared by the Windows backend files. */
#ifndef RD_WIN_H
#define RD_WIN_H

#include <stddef.h>
#include <wchar.h>

void     win_to_utf8(const wchar_t *w, char *out, size_t cap);
wchar_t *win_from_utf8(const char *s); /* caller frees */

#endif
