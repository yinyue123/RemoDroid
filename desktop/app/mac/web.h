/* The panel in its own window on macOS (web.m). */
#pragma once
#import <AppKit/AppKit.h>

/* Shows the panel at url (http/https) in the panel window, opening it if needed; notify reports downloads. */
void rd_web_show(const char *url, void (^notify)(NSString *));
/* Brings the panel window back if it is open (Dock icon clicked); 0 if there is none. */
int rd_web_reopen(void);
/* The View menu for the main menu bar: reload, back/forward, zoom, Open in Browser. */
NSMenu *rd_web_menu(void);
