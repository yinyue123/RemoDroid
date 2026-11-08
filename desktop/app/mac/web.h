/* The panel in its own window on macOS (web.m). */
#pragma once
#import <AppKit/AppKit.h>

/* Shows the panel at url (http/https) in the last active panel tab, opening a window if there is none;
 * notify reports downloads. The window has Chrome-style tabs (web.m). */
void rd_web_show(const char *url, void (^notify)(NSString *));
/* Brings the last active panel tab back if one is open (Dock icon clicked); 0 if there is none. */
int rd_web_reopen(void);
/* The View menu for the main menu bar: reload, back/forward, zoom, Open in Browser. */
NSMenu *rd_web_menu(void);
