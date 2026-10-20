/* Shared by the macOS backend files. */
#ifndef RD_MAC_H
#define RD_MAC_H

#include <stdint.h>

#include "desk.h"

int      mac_can_capture(void);                  /* Screen Recording permission */
int      mac_can_control(void);                  /* Accessibility permission */
uint32_t mac_display_id(const dsk_screen_t *s);  /* CGDirectDisplayID */

#endif
