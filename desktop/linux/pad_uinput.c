/* Virtual gamepads on Linux: uinput devices that look like a wired Xbox 360 pad (the xpad driver's
 * name, USB IDs, buttons and axes), so Steam, SDL and the games' own mappings take them as one.
 * /dev/uinput must be writable by the user: Steam's udev rules (steam-devices; SteamOS has them)
 * tag it uaccess for whoever is logged in at the screen. */
#include <errno.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "desk.h"

static int fds[DSK_MAX_PADS] = { -1, -1, -1, -1 };
static dsk_pad_t last[DSK_MAX_PADS];

/* W3C buttons 0..11 and 16 → evdev; 6, 7 are the triggers (axes) and 12..15 the D-pad (a hat) */
static const int BTN[17] = { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_TL, BTN_TR, 0, 0, BTN_SELECT, BTN_START,
                             BTN_THUMBL, BTN_THUMBR, 0, 0, 0, 0, BTN_MODE };

int dsk_pad_supported(char *why, size_t cap)
{
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd >= 0) {
        close(fd);
        return 1;
    }
    if (errno == ENOENT)
        snprintf(why, cap, "no /dev/uinput (load the uinput module)");
    else
        snprintf(why, cap, "no permission to create gamepads (/dev/uinput): install Steam's udev rules (steam-devices)");
    return 0;
}

static void absinfo(int fd, int code, int min, int max, int fuzz, int flat)
{
    struct uinput_abs_setup a = { .code = (uint16_t)code, .absinfo = { .minimum = min, .maximum = max, .fuzz = fuzz, .flat = flat } };
    ioctl(fd, UI_SET_ABSBIT, code);
    ioctl(fd, UI_ABS_SETUP, &a);
}

int dsk_pad_open(int index)
{
    if (index < 0 || index >= DSK_MAX_PADS)
        return -1;
    if (fds[index] >= 0)
        return 0;
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        LOGW("gamepad %d: /dev/uinput: %s", index + 1, strerror(errno));
        return -1;
    }
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    for (int i = 0; i < 17; i++)
        if (BTN[i])
            ioctl(fd, UI_SET_KEYBIT, BTN[i]);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    absinfo(fd, ABS_X, -32768, 32767, 16, 128);
    absinfo(fd, ABS_Y, -32768, 32767, 16, 128);
    absinfo(fd, ABS_RX, -32768, 32767, 16, 128);
    absinfo(fd, ABS_RY, -32768, 32767, 16, 128);
    absinfo(fd, ABS_Z, 0, 255, 0, 0);
    absinfo(fd, ABS_RZ, 0, 255, 0, 0);
    absinfo(fd, ABS_HAT0X, -1, 1, 0, 0);
    absinfo(fd, ABS_HAT0Y, -1, 1, 0, 0);
    struct uinput_setup us = { .id = { .bustype = BUS_USB, .vendor = 0x045e, .product = 0x028e, .version = 0x0110 } };
    snprintf(us.name, sizeof us.name, "Microsoft X-Box 360 pad");
    if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        LOGW("gamepad %d: cannot create the device: %s", index + 1, strerror(errno));
        close(fd);
        return -1;
    }
    fds[index] = fd;
    memset(&last[index], 0, sizeof last[index]);
    LOGI("gamepad %d plugged in (virtual Xbox 360 pad)", index + 1);
    return 0;
}

static void emit(int fd, int type, int code, int value)
{
    struct input_event e = { .type = (uint16_t)type, .code = (uint16_t)code, .value = value };
    if (write(fd, &e, sizeof e) < 0) { /* the device went away; the next close cleans up */ }
}

void dsk_pad_update(int index, const dsk_pad_t *p)
{
    if (index < 0 || index >= DSK_MAX_PADS || fds[index] < 0)
        return;
    int fd = fds[index];
    dsk_pad_t *o = &last[index];
    for (int i = 0; i < 17; i++)
        if (BTN[i] && ((p->buttons ^ o->buttons) >> i & 1))
            emit(fd, EV_KEY, BTN[i], p->buttons >> i & 1);
    /* a viewer with digital triggers only (bits 6, 7) still pulls them all the way */
    int lt = p->lt ? p->lt : p->buttons >> 6 & 1 ? 255 : 0, rt = p->rt ? p->rt : p->buttons >> 7 & 1 ? 255 : 0;
    int olt = o->lt ? o->lt : o->buttons >> 6 & 1 ? 255 : 0, ort = o->rt ? o->rt : o->buttons >> 7 & 1 ? 255 : 0;
    if (lt != olt)
        emit(fd, EV_ABS, ABS_Z, lt);
    if (rt != ort)
        emit(fd, EV_ABS, ABS_RZ, rt);
    const int sx[4] = { ABS_X, ABS_Y, ABS_RX, ABS_RY };
    const int16_t nv[4] = { p->lx, p->ly, p->rx, p->ry }, ov[4] = { o->lx, o->ly, o->rx, o->ry };
    for (int i = 0; i < 4; i++)
        if (nv[i] != ov[i])
            emit(fd, EV_ABS, sx[i], nv[i]);
    int hx = (p->buttons >> 15 & 1) - (p->buttons >> 14 & 1), hy = (p->buttons >> 13 & 1) - (p->buttons >> 12 & 1);
    int ohx = (o->buttons >> 15 & 1) - (o->buttons >> 14 & 1), ohy = (o->buttons >> 13 & 1) - (o->buttons >> 12 & 1);
    if (hx != ohx)
        emit(fd, EV_ABS, ABS_HAT0X, hx);
    if (hy != ohy)
        emit(fd, EV_ABS, ABS_HAT0Y, hy);
    emit(fd, EV_SYN, SYN_REPORT, 0);
    *o = *p;
}

void dsk_pad_close(int index)
{
    if (index < 0 || index >= DSK_MAX_PADS || fds[index] < 0)
        return;
    ioctl(fds[index], UI_DEV_DESTROY);
    close(fds[index]);
    fds[index] = -1;
    LOGI("gamepad %d unplugged", index + 1);
}
