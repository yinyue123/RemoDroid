/* Shared by the Linux backend files. */
#ifndef RD_X11_H
#define RD_X11_H

#include <X11/Xlib.h>

Display *x11_open(void); /* a new connection to $DISPLAY, or NULL */

#endif
