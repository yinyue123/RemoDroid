/* macOS backend: system information, displays, permissions (Screen Recording
 * is needed to capture; Accessibility to send the mouse and keyboard), folders,
 * display sleep and keeping it awake. Coordinates are global points with the
 * origin at the top left of the main display, as Quartz events use them. */
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <Foundation/Foundation.h>
#import <IOKit/pwr_mgt/IOPMLib.h>

#include <spawn.h>
#include <sys/wait.h>

#include "desk.h"
#include "mac.h"

extern char **environ;

const char *dsk_platform(void) { return "macos"; }

void dsk_os_name(char *out, size_t cap)
{
    NSOperatingSystemVersion v = [[NSProcessInfo processInfo] operatingSystemVersion];
    if (v.patchVersion)
        snprintf(out, cap, "macOS %ld.%ld.%ld", (long)v.majorVersion, (long)v.minorVersion, (long)v.patchVersion);
    else
        snprintf(out, cap, "macOS %ld.%ld", (long)v.majorVersion, (long)v.minorVersion);
}

int dsk_init(char *problem, size_t cap) { return dsk_problem(problem, cap) > 0 ? -1 : 0; }

int mac_can_capture(void) { return CGPreflightScreenCaptureAccess() ? 1 : 0; }
int mac_can_control(void) { return AXIsProcessTrusted() ? 1 : 0; }

int dsk_problem(char *out, size_t cap)
{
    if (!mac_can_capture()) {
        snprintf(out, cap, "Screen Recording permission is off (System Settings → Privacy & Security → Screen Recording)");
        return 1;
    }
    out[0] = 0;
    return 0;
}

int dsk_screens(dsk_screen_t *out, int max)
{
    CGDirectDisplayID ids[DSK_MAX_SCREENS];
    uint32_t n = 0;
    if (CGGetActiveDisplayList(DSK_MAX_SCREENS, ids, &n) != kCGErrorSuccess)
        return 0;
    int k = 0;
    for (uint32_t i = 0; i < n && k < max; i++) {
        if (CGDisplayIsInMirrorSet(ids[i]) && CGDisplayMirrorsDisplay(ids[i]) != kCGNullDirectDisplay)
            continue; /* a mirror shows the same picture */
        CGRect b = CGDisplayBounds(ids[i]);
        dsk_screen_t *s = &out[k];
        memset(s, 0, sizeof *s);
        s->index = k;
        s->x = (int)b.origin.x;
        s->y = (int)b.origin.y;
        s->w = (int)b.size.width;
        s->h = (int)b.size.height;
        CGDisplayModeRef mode = CGDisplayCopyDisplayMode(ids[i]);
        s->pw = mode ? (int)CGDisplayModeGetPixelWidth(mode) : s->w;
        s->ph = mode ? (int)CGDisplayModeGetPixelHeight(mode) : s->h;
        if (mode)
            CGDisplayModeRelease(mode);
        s->primary = CGDisplayIsMain(ids[i]) != 0;
        snprintf(s->name, sizeof s->name, "Display %u", ids[i]);
        for (NSScreen *sc in [NSScreen screens]) {
            NSNumber *num = sc.deviceDescription[@"NSScreenNumber"];
            if (num && num.unsignedIntValue == ids[i])
                snprintf(s->name, sizeof s->name, "%s", sc.localizedName.UTF8String ?: "Display");
        }
        k++;
    }
    return k;
}

uint32_t mac_display_id(const dsk_screen_t *s)
{
    CGDirectDisplayID ids[DSK_MAX_SCREENS];
    uint32_t n = 0;
    CGGetActiveDisplayList(DSK_MAX_SCREENS, ids, &n);
    for (uint32_t i = 0; i < n; i++) {
        CGRect b = CGDisplayBounds(ids[i]);
        if ((int)b.origin.x == s->x && (int)b.origin.y == s->y)
            return ids[i];
    }
    return CGMainDisplayID();
}

/* ---------------- folders ---------------- */

const char *dsk_config_dir(void)
{
    static char dir[1024];
    if (!dir[0]) {
        snprintf(dir, sizeof dir, "%s/Library/Application Support/RemoDroid", rd_home_dir());
        rd_mkdirs(dir, 0700);
    }
    return dir;
}

const char *dsk_downloads_dir(void)
{
    static char dir[1024];
    if (!dir[0]) {
        NSArray *p = NSSearchPathForDirectoriesInDomains(NSDownloadsDirectory, NSUserDomainMask, YES);
        snprintf(dir, sizeof dir, "%s", p.count ? [p[0] UTF8String] : [[NSHomeDirectory() stringByAppendingString:@"/Downloads"] UTF8String]);
    }
    return dir;
}

/* ---------------- system ---------------- */

void dsk_notify(const char *title, const char *text) { /* the app shows them (rd_agent_cb_t.on_notify) */ }

static int run(const char *path, char *const argv[])
{
    pid_t pid;
    if (posix_spawn(&pid, path, NULL, NULL, argv, environ))
        return -1;
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

int dsk_screen_power(int on)
{
    if (on) {
        IOPMAssertionID id;
        return IOPMAssertionDeclareUserActivity(CFSTR("RemoDroid"), kIOPMUserActiveLocal, &id) == kIOReturnSuccess ? 0 : -1;
    }
    char *argv[] = { "pmset", "displaysleepnow", NULL };
    return run("/usr/bin/pmset", argv) == 0 ? 0 : -1;
}

/* gamepads: macOS has no API for a virtual game controller */
int dsk_pad_supported(char *why, size_t cap)
{
    snprintf(why, cap, "gamepads aren't supported on macOS");
    return 0;
}
int dsk_pad_open(int index) { return -1; }
void dsk_pad_update(int index, const dsk_pad_t *p) {}
void dsk_pad_close(int index) {}

static IOPMAssertionID awake;

void dsk_session(int active)
{
    if (active && !awake)
        IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleDisplaySleep, kIOPMAssertionLevelOn,
                                    CFSTR("A RemoDroid viewer is connected"), &awake);
    else if (!active && awake) {
        IOPMAssertionRelease(awake);
        awake = 0;
    }
}

/* There's no public "lock now"; sleeping the display locks when the Mac asks for the password
 * immediately (System Settings → Lock Screen). */
int dsk_lock(void) { return dsk_screen_power(0); }
