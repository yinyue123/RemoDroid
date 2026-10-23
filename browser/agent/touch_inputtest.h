/* The inputtest driver's socket protocol (xserver hw/xfree86/drivers/inputtest/
 * xf86-input-inputtest-protocol.h, version 1.1, MIT): only what touch_inputtest.c sends and reads.
 * The layout must match the X server's own build of these structs; browser/test/touch_test.c
 * compares them with the real header where it is installed. */
#ifndef RD_TOUCH_INPUTTEST_H
#define RD_TOUCH_INPUTTEST_H

#include <stdint.h>

#define IT_VERSION_MAJOR 1
#define IT_VERSION_MINOR 1
#define IT_MAX_VALUATORS 64

enum { IT_RESPONSE_SERVER_VERSION = 0, IT_RESPONSE_SYNC_FINISHED = 1 };
enum { IT_EVENT_CLIENT_VERSION = 0, IT_EVENT_WAIT_FOR_SYNC = 1, IT_EVENT_TOUCH = 6 };
enum { IT_TOUCH_BEGIN = 18, IT_TOUCH_UPDATE = 19, IT_TOUCH_END = 20 }; /* XI_TouchBegin … */

typedef struct {
    uint32_t length; /* the whole message, header included */
    uint32_t type;   /* an enum in the original: int-sized on every ABI the X server builds for */
} it_header_t;

typedef struct {
    uint32_t has_unaccelerated;
    uint8_t mask[(IT_MAX_VALUATORS + 7) / 8];
    double valuators[IT_MAX_VALUATORS];
    double unaccelerated[IT_MAX_VALUATORS];
} it_valuators_t;

typedef struct {
    it_header_t header;
    uint16_t major, minor;
} it_client_version_t, it_server_version_t;

typedef struct {
    it_header_t header;
} it_wait_for_sync_t, it_sync_finished_t;

typedef struct {
    it_header_t header;
    uint32_t touchid;
    uint32_t touch_type;
    it_valuators_t valuators;
} it_touch_t;

#endif
