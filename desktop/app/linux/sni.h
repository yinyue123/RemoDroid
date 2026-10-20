/* A tray icon for Linux desktops: StatusNotifierItem + com.canonical.dbusmenu
 * over D-Bus (KDE, GNOME with the AppIndicator extension as Ubuntu ships it,
 * XFCE, Cinnamon, MATE…), or GtkStatusIcon where no watcher is running. */
#ifndef RD_SNI_H
#define RD_SNI_H

#include <stdint.h>

typedef struct {
    int id;            /* > 0; 0 = separator */
    const char *label;
    int enabled, check, checked;
} sni_item_t;

typedef struct {
    void (*activate)(void);         /* left click */
    void (*menu)(int id);           /* a menu item was chosen */
} sni_cb_t;

void sni_start(sni_cb_t cb);
/* icon: size x size ARGB pixels; tooltip; attention = someone is viewing */
void sni_set_icon(const uint32_t *argb, int size, const char *tooltip, int attention);
void sni_set_menu(const sni_item_t *items, int n); /* copied */

#endif
