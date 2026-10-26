/* The RemoDroid desktop icons, drawn in code (no image files or libraries):
 * the app icon and the tray / menu bar states, for every platform's app and
 * for icongen (which writes .png / .ico / .icns at build time). The server's
 * Windows app (server/win/tray.c) has its own: two stacked rack units. */
#ifndef RD_ICON_H
#define RD_ICON_H

#include <stdint.h>

enum {
    RD_ICON_APP,        /* dark rounded square, monitor in the accent colour */
    RD_ICON_OFF,        /* tray: grey monitor (not paired / not connected) */
    RD_ICON_ON,         /* tray: accent monitor (online) */
    RD_ICON_LIVE,       /* tray: accent monitor with a red dot (someone is viewing) */
    RD_ICON_TEMPLATE,   /* macOS menu bar: black, the system tints it */
    RD_ICON_TEMPLATE_LIVE,
    RD_ICON_SERVER_APP, /* the server: dark rounded square, rack units in the accent colour */
    RD_ICON_SERVER_OFF, /* tray: grey rack (stopped) */
    RD_ICON_SERVER_ON,  /* tray: accent rack (running) */
};

/* size x size pixels, premultiplied=0: straight alpha ARGB (0xAARRGGBB) */
void rd_icon_render(uint32_t *argb, int size, int style);

#endif
