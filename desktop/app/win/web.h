/* The panel in its own window on Windows, in WebView2 (web.c). */
#pragma once
#include <windows.h>

/* Shows the panel at url (http/https) in the panel window, opening it if needed. -1 when that can't be done
 * (no WebView2 Runtime): the caller opens the default browser instead. Call it on the UI thread. */
int rd_web_show(HINSTANCE inst, HICON icon, const char *url);
