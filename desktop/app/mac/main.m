/* RemoDroid for macOS: a menu bar item, its menu and the pairing / status
 * window with the two permissions RemoDroid needs, in AppKit (no nib). The
 * agent itself is libremodroid.dylib (desktop/core/rd_agent.h); its callbacks
 * come from the agent's thread and are dispatched to the main queue. */
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <ServiceManagement/ServiceManagement.h>
#import <UserNotifications/UserNotifications.h>

#include "icon.h"
#include "rd_agent.h"

static NSString *str(const char *s) { return [NSString stringWithUTF8String:s ?: ""] ?: @""; }

static NSImage *icon_image(int style, CGFloat points)
{
    int px = (int)(points * 2); /* drawn for Retina, shown at `points` */
    uint32_t *argb = malloc(sizeof *argb * (size_t)px * px);
    rd_icon_render(argb, px, style);
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:px pixelsHigh:px
                                                                  bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES
                                                                       isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace
                                                                   bitmapFormat:NSBitmapFormatAlphaNonpremultiplied
                                                                    bytesPerRow:px * 4 bitsPerPixel:32];
    uint8_t *d = rep.bitmapData;
    for (int i = 0; i < px * px; i++) {
        uint32_t c = argb[i];
        d[i * 4] = (uint8_t)(c >> 16), d[i * 4 + 1] = (uint8_t)(c >> 8), d[i * 4 + 2] = (uint8_t)c, d[i * 4 + 3] = (uint8_t)(c >> 24);
    }
    free(argb);
    NSImage *img = [[NSImage alloc] initWithSize:NSMakeSize(points, points)];
    [img addRepresentation:rep];
    img.template = style == RD_ICON_TEMPLATE;
    return img;
}

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate, NSMenuDelegate, UNUserNotificationCenterDelegate>
@property(strong) NSStatusItem *item;
@property(strong) NSWindow *window;
@property(strong) NSTextField *title, *state, *info, *panel, *code, *name, *capState, *ctlState;
@property(strong) NSButton *pair, *unpair, *openPanel, *disconnect, *viewOnly, *login;
@property(strong) NSView *pairBox, *pairedBox;
@property(strong) NSTimer *permTimer;
@property(assign) rd_status_t st;
@property(assign) BOOL pairing;
@property(strong) NSString *pairMsg;
@end

static AppDelegate *app;

static void on_status(void *ud, const rd_status_t *s)
{
    rd_status_t c = *s;
    dispatch_async(dispatch_get_main_queue(), ^{
        rd_status_t old = app.st;
        app.st = c;
        if (old.state == RD_UNPAIRED && c.state != RD_UNPAIRED)
            app.pairMsg = nil;
        [app refresh];
    });
}

static void on_paired(void *ud, int ok, const char *msg)
{
    NSString *m = str(msg);
    dispatch_async(dispatch_get_main_queue(), ^{
        app.pairing = NO;
        app.pairMsg = ok ? [NSString stringWithFormat:@"Paired as “%@”.", m] : [NSString stringWithFormat:@"Pairing failed: %@", m];
        [app refresh];
        if (ok)
            [app notify:app.pairMsg];
    });
}

static void on_confirm(void *ud, const char *viewer)
{
    NSString *v = str(viewer);
    dispatch_async(dispatch_get_main_queue(), ^{
        NSAlert *a = [[NSAlert alloc] init];
        a.messageText = [NSString stringWithFormat:@"%@ wants to see and control this Mac.", v];
        a.informativeText = @"Allow the connection?";
        [a addButtonWithTitle:@"Allow"];
        [a addButtonWithTitle:@"Deny"];
        [NSApp activateIgnoringOtherApps:YES];
        rd_agent_confirm([a runModal] == NSAlertFirstButtonReturn);
    });
}

static void on_notify(void *ud, const char *text)
{
    NSString *t = str(text);
    dispatch_async(dispatch_get_main_queue(), ^{
        [app notify:t];
    });
}

@implementation AppDelegate

- (NSString *)stateText
{
    switch (self.st.state) {
    case RD_UNPAIRED: return @"Not paired";
    case RD_CONNECTING: return @"Connecting…";
    case RD_ONLINE: return @"Online";
    case RD_IN_SESSION: return @"Someone is viewing";
    default: return @"Stopped";
    }
}

- (void)notify:(NSString *)text
{
    UNMutableNotificationContent *c = [[UNMutableNotificationContent alloc] init];
    c.title = @"RemoDroid";
    c.body = text;
    UNNotificationRequest *r = [UNNotificationRequest requestWithIdentifier:[[NSUUID UUID] UUIDString] content:c trigger:nil];
    [[UNUserNotificationCenter currentNotificationCenter] addNotificationRequest:r withCompletionHandler:nil];
}

- (void)userNotificationCenter:(UNUserNotificationCenter *)center
       willPresentNotification:(UNNotification *)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions))handler
{
    handler(UNNotificationPresentationOptionBanner | UNNotificationPresentationOptionList);
}

/* ---------------- start at login ---------------- */

- (BOOL)loginEnabled
{
    if (@available(macOS 13.0, *))
        return SMAppService.mainAppService.status == SMAppServiceStatusEnabled;
    return [[NSFileManager defaultManager] fileExistsAtPath:[self launchAgentPath]];
}

- (NSString *)launchAgentPath
{
    return [NSHomeDirectory() stringByAppendingPathComponent:@"Library/LaunchAgents/com.remodroid.desktop.plist"];
}

- (void)setLogin:(BOOL)on
{
    if (@available(macOS 13.0, *)) {
        NSError *e = nil;
        if (on)
            [SMAppService.mainAppService registerAndReturnError:&e];
        else
            [SMAppService.mainAppService unregisterAndReturnError:&e];
        if (e)
            [self notify:[NSString stringWithFormat:@"Start at login: %@", e.localizedDescription]];
        return;
    }
    NSString *p = [self launchAgentPath]; /* macOS 12 */
    if (!on) {
        [[NSFileManager defaultManager] removeItemAtPath:p error:nil];
        return;
    }
    NSDictionary *plist = @{ @"Label" : @"com.remodroid.desktop",
                             @"ProgramArguments" : @[ [[NSBundle mainBundle] executablePath], @"--background" ],
                             @"RunAtLoad" : @YES };
    [[NSFileManager defaultManager] createDirectoryAtPath:[p stringByDeletingLastPathComponent]
                              withIntermediateDirectories:YES attributes:nil error:nil];
    [plist writeToFile:p atomically:YES];
}

/* ---------------- menu ---------------- */

- (void)refreshItem
{
    BOOL live = self.st.state == RD_IN_SESSION;
    self.item.button.image = icon_image(live ? RD_ICON_LIVE : RD_ICON_TEMPLATE, 18);
    self.item.button.appearsDisabled = self.st.state != RD_ONLINE && !live;
    self.item.button.toolTip = live && self.st.viewer[0]
                                   ? [NSString stringWithFormat:@"RemoDroid — %@ is viewing", str(self.st.viewer)]
                                   : [NSString stringWithFormat:@"RemoDroid — %@", [self stateText]];
}

- (void)menuNeedsUpdate:(NSMenu *)m
{
    [m removeAllItems];
    NSMenuItem *s = [m addItemWithTitle:[NSString stringWithFormat:@"RemoDroid — %@", [self stateText]] action:nil keyEquivalent:@""];
    s.enabled = NO;
    if (self.st.state == RD_IN_SESSION) {
        NSMenuItem *v = [m addItemWithTitle:[NSString stringWithFormat:@"Viewer: %@", self.st.viewer[0] ? str(self.st.viewer) : @"someone"]
                                     action:nil keyEquivalent:@""];
        v.enabled = NO;
    }
    if (self.st.problem[0]) {
        NSMenuItem *p = [m addItemWithTitle:str(self.st.problem) action:nil keyEquivalent:@""];
        p.enabled = NO;
    }
    [m addItem:[NSMenuItem separatorItem]];
    if (self.st.state == RD_IN_SESSION)
        [m addItemWithTitle:@"Disconnect viewer" action:@selector(disconnect:) keyEquivalent:@""].target = self;
    NSMenuItem *vo = [m addItemWithTitle:@"View only (ignore mouse and keyboard)" action:@selector(toggleViewOnly:) keyEquivalent:@""];
    vo.target = self;
    vo.state = self.st.view_only ? NSControlStateValueOn : NSControlStateValueOff;
    if (self.st.panel[0])
        [m addItemWithTitle:@"Open panel" action:@selector(openPanel:) keyEquivalent:@""].target = self;
    [m addItemWithTitle:self.st.state == RD_UNPAIRED ? @"Pair with a panel…" : @"Pairing & status…" action:@selector(showWindow:) keyEquivalent:@""].target = self;
    [m addItem:[NSMenuItem separatorItem]];
    NSMenuItem *li = [m addItemWithTitle:@"Start at login" action:@selector(toggleLogin:) keyEquivalent:@""];
    li.target = self;
    li.state = [self loginEnabled] ? NSControlStateValueOn : NSControlStateValueOff;
    [m addItemWithTitle:@"Quit RemoDroid" action:@selector(terminate:) keyEquivalent:@"q"];
}

- (void)disconnect:(id)sender { rd_agent_end_session(); }
- (void)toggleViewOnly:(id)sender { rd_agent_set("desktop.view_only", self.st.view_only ? "false" : "true"); }
- (void)toggleLogin:(id)sender { [self setLogin:![self loginEnabled]]; [self refresh]; }

- (void)openPanel:(id)sender
{
    if (self.st.panel[0])
        [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:str(self.st.panel)]];
}

/* ---------------- window ---------------- */

static NSTextField *label(NSString *s, NSView *in, NSRect r, CGFloat size, BOOL bold)
{
    NSTextField *t = [NSTextField wrappingLabelWithString:s];
    t.frame = r;
    t.font = bold ? [NSFont boldSystemFontOfSize:size] : [NSFont systemFontOfSize:size];
    [in addSubview:t];
    return t;
}

static NSTextField *field(NSView *in, NSRect r, NSString *placeholder)
{
    NSTextField *f = [[NSTextField alloc] initWithFrame:r];
    f.placeholderString = placeholder;
    [in addSubview:f];
    return f;
}

static NSButton *button(NSString *t, NSView *in, NSRect r, id target, SEL a)
{
    NSButton *b = [NSButton buttonWithTitle:t target:target action:a];
    b.frame = r;
    [in addSubview:b];
    return b;
}

- (void)buildWindow
{
    const CGFloat W = 440, H = 430;
    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, W, H)
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"RemoDroid";
    self.window.releasedWhenClosed = NO;
    self.window.delegate = self;
    NSView *v = self.window.contentView;
    self.title = label(@"", v, NSMakeRect(20, H - 50, W - 40, 26), 17, YES);
    self.state = label(@"", v, NSMakeRect(20, H - 92, W - 40, 38), 12, NO);

    self.pairBox = [[NSView alloc] initWithFrame:NSMakeRect(0, 140, W, 190)];
    [v addSubview:self.pairBox];
    label(@"Panel address", self.pairBox, NSMakeRect(20, 150, 110, 20), 13, NO);
    self.panel = field(self.pairBox, NSMakeRect(136, 146, W - 156, 24), @"https://remodroid.example.com");
    label(@"Pairing code", self.pairBox, NSMakeRect(20, 114, 110, 20), 13, NO);
    self.code = field(self.pairBox, NSMakeRect(136, 110, W - 156, 24), @"XXXX-XXXX");
    label(@"Name", self.pairBox, NSMakeRect(20, 78, 110, 20), 13, NO);
    self.name = field(self.pairBox, NSMakeRect(136, 74, W - 156, 24), @"");
    self.name.stringValue = [[NSHost currentHost] localizedName] ?: @"";
    label(@"Get a code on the panel: Agents → Add → Computer.", self.pairBox, NSMakeRect(20, 44, W - 40, 20), 12, NO).textColor = NSColor.secondaryLabelColor;
    self.pair = button(@"Pair", self.pairBox, NSMakeRect(W - 120, 4, 100, 30), self, @selector(doPair:));
    self.pair.keyEquivalent = @"\r";

    self.pairedBox = [[NSView alloc] initWithFrame:NSMakeRect(0, 210, W, 120)];
    [v addSubview:self.pairedBox];
    self.info = label(@"", self.pairedBox, NSMakeRect(20, 64, W - 40, 44), 12, NO);
    self.info.selectable = YES;
    self.openPanel = button(@"Open panel", self.pairedBox, NSMakeRect(16, 20, 120, 30), self, @selector(openPanel:));
    self.disconnect = button(@"Disconnect viewer", self.pairedBox, NSMakeRect(140, 20, 150, 30), self, @selector(disconnect:));
    self.unpair = button(@"Unpair", self.pairedBox, NSMakeRect(W - 116, 20, 100, 30), self, @selector(doUnpair:));

    /* permissions */
    label(@"Permissions", v, NSMakeRect(20, 108, W - 40, 20), 13, YES);
    self.capState = label(@"", v, NSMakeRect(20, 84, 260, 20), 12, NO);
    button(@"Open Settings", v, NSMakeRect(W - 140, 78, 124, 28), self, @selector(openCaptureSettings:));
    self.ctlState = label(@"", v, NSMakeRect(20, 56, 260, 20), 12, NO);
    button(@"Open Settings", v, NSMakeRect(W - 140, 50, 124, 28), self, @selector(openControlSettings:));

    self.viewOnly = [NSButton checkboxWithTitle:@"View only" target:self action:@selector(viewOnlyChanged:)];
    self.viewOnly.frame = NSMakeRect(18, 16, 120, 24);
    [v addSubview:self.viewOnly];
    self.login = [NSButton checkboxWithTitle:@"Start at login" target:self action:@selector(loginChanged:)];
    self.login.frame = NSMakeRect(140, 16, 140, 24);
    [v addSubview:self.login];
    button(@"Copy log", v, NSMakeRect(W - 116, 12, 100, 30), self, @selector(copyLog:));
    [self.window center];
}

- (void)refresh
{
    [self refreshItem];
    if (!self.window)
        return;
    BOOL paired = self.st.state != RD_UNPAIRED && self.st.state != RD_STOPPED;
    self.title.stringValue = paired ? @"This Mac is paired" : @"Pair this Mac with your panel";
    NSString *s = self.pairing ? @"Pairing…" : self.pairMsg ?: (self.st.problem[0] ? [NSString stringWithFormat:@"%@ — %@", [self stateText], str(self.st.problem)] : [self stateText]);
    self.state.stringValue = s;
    self.state.textColor = (self.st.problem[0] || (self.pairMsg && [self.pairMsg hasPrefix:@"Pairing failed"])) ? NSColor.systemRedColor : NSColor.labelColor;
    self.pairBox.hidden = paired;
    self.pairedBox.hidden = !paired;
    self.pair.enabled = !self.pairing;
    self.info.stringValue = [NSString stringWithFormat:@"Panel:  %@\nDevice: %@", str(self.st.panel), str(self.st.device_id)];
    self.disconnect.hidden = self.st.state != RD_IN_SESSION;
    self.viewOnly.state = self.st.view_only ? NSControlStateValueOn : NSControlStateValueOff;
    self.login.state = [self loginEnabled] ? NSControlStateValueOn : NSControlStateValueOff;
    BOOL cap = CGPreflightScreenCaptureAccess(), ctl = AXIsProcessTrusted();
    self.capState.stringValue = [NSString stringWithFormat:@"%@ Screen Recording", cap ? @"✓" : @"✗"];
    self.ctlState.stringValue = [NSString stringWithFormat:@"%@ Accessibility (mouse and keyboard)", ctl ? @"✓" : @"✗"];
    self.capState.textColor = cap ? NSColor.labelColor : NSColor.systemRedColor;
    self.ctlState.textColor = ctl ? NSColor.labelColor : NSColor.systemRedColor;
}

- (void)showWindow:(id)sender
{
    if (!self.window)
        [self buildWindow];
    [self refresh];
    [NSApp activateIgnoringOtherApps:YES];
    [self.window makeKeyAndOrderFront:nil];
    if (self.st.state == RD_UNPAIRED)
        [self.window makeFirstResponder:self.panel];
    if (!self.permTimer) /* permissions change in System Settings: keep the ticks current */
        self.permTimer = [NSTimer scheduledTimerWithTimeInterval:2 target:self selector:@selector(refresh) userInfo:nil repeats:YES];
}

- (void)windowWillClose:(NSNotification *)n
{
    [self.permTimer invalidate];
    self.permTimer = nil;
}

- (void)doPair:(id)sender
{
    if (!self.panel.stringValue.length || !self.code.stringValue.length) {
        self.pairMsg = @"Pairing failed: enter the panel address and the code shown on it.";
        [self refresh];
        return;
    }
    self.pairing = YES;
    self.pairMsg = nil;
    rd_agent_pair(self.panel.stringValue.UTF8String, self.code.stringValue.UTF8String, self.name.stringValue.UTF8String,
                  on_paired, NULL);
    [self refresh];
}

- (void)doUnpair:(id)sender
{
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = @"Unpair this Mac?";
    a.informativeText = @"Viewers can't reach it until you pair it again.";
    [a addButtonWithTitle:@"Unpair"];
    [a addButtonWithTitle:@"Cancel"];
    if ([a runModal] == NSAlertFirstButtonReturn)
        rd_agent_unpair();
}

- (void)viewOnlyChanged:(id)sender { rd_agent_set("desktop.view_only", self.viewOnly.state == NSControlStateValueOn ? "true" : "false"); }
- (void)loginChanged:(id)sender { [self setLogin:self.login.state == NSControlStateValueOn]; }

- (void)openCaptureSettings:(id)sender
{
    CGRequestScreenCaptureAccess(); /* the first time, macOS asks */
    [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture"]];
}

- (void)openControlSettings:(id)sender
{
    NSDictionary *o = @{ (__bridge NSString *)kAXTrustedCheckOptionPrompt : @YES };
    AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)o);
    [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility"]];
}

- (void)copyLog:(id)sender
{
    char *log = rd_agent_log(300);
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    [pb setString:str(log) forType:NSPasteboardTypeString];
    rd_agent_free(log);
    [self notify:@"The log is on the clipboard."];
}

/* ---------------- lifecycle ---------------- */

- (void)applicationDidFinishLaunching:(NSNotification *)n
{
    UNUserNotificationCenter *nc = [UNUserNotificationCenter currentNotificationCenter];
    nc.delegate = self;
    [nc requestAuthorizationWithOptions:UNAuthorizationOptionAlert | UNAuthorizationOptionSound completionHandler:^(BOOL g, NSError *e) {}];
    self.item = [[NSStatusBar systemStatusBar] statusItemWithLength:NSSquareStatusItemLength];
    NSMenu *m = [[NSMenu alloc] init];
    m.delegate = self;
    self.item.menu = m;
    rd_status_t s = { .state = RD_STOPPED };
    self.st = s;
    [self refreshItem];
    NSArray *args = [[NSProcessInfo processInfo] arguments];
    NSUInteger ci = [args indexOfObject:@"--conf"];
    const char *conf = ci != NSNotFound && ci + 1 < args.count ? [args[ci + 1] UTF8String] : NULL;
    rd_agent_cb_t cb = { on_status, on_confirm, on_notify };
    if (rd_agent_start(conf, cb, NULL) < 0) {
        NSAlert *a = [[NSAlert alloc] init];
        a.messageText = @"RemoDroid could not start.";
        a.informativeText = @"See desktop.log in ~/Library/Application Support/RemoDroid.";
        [a runModal];
        [NSApp terminate:nil];
        return;
    }
    rd_agent_status(&s);
    self.st = s;
    [self refreshItem];
    BOOL background = [args containsObject:@"--background"];
    if (!background || s.state == RD_UNPAIRED || !CGPreflightScreenCaptureAccess())
        [self showWindow:nil];
}

- (BOOL)applicationShouldHandleReopen:(NSApplication *)a hasVisibleWindows:(BOOL)visible
{
    [self showWindow:nil]; /* opened again from Finder / Launchpad */
    return NO;
}

- (void)applicationWillTerminate:(NSNotification *)n { rd_agent_stop(); }

@end

int main(int argc, const char **argv)
{
    @autoreleasepool {
        NSApplication *a = [NSApplication sharedApplication];
        app = [[AppDelegate alloc] init];
        a.delegate = app;
        [a setActivationPolicy:NSApplicationActivationPolicyAccessory]; /* menu bar only, no Dock icon */
        [a run];
    }
    return 0;
}
