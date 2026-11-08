/* RemoDroid for macOS: a menu bar item, its menu and the pairing / status
 * window with the two permissions RemoDroid needs, in AppKit (no nib). The
 * agent itself is libremodroid.dylib (desktop/core/rd_agent.h); its callbacks
 * come from the agent's thread and are dispatched to the main queue. */
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <ImageIO/ImageIO.h>
#import <ServiceManagement/ServiceManagement.h>
#import <UserNotifications/UserNotifications.h>

#include "icon.h"
#include "rd_agent.h"
#include "web.h"
#include "xfer.h"

static NSString *str(const char *s) { return [NSString stringWithUTF8String:s ?: ""] ?: @""; }

/* ---------------- permissions left from an earlier build ----------------
 * The app is signed ad hoc, so every build has a new signature (multi-platform.md §13.4). macOS keeps the
 * Screen Recording and Accessibility switches an earlier build was given, still on in System Settings, but
 * they don't count for this one, and macOS won't ask again while they are there. An app can't read that
 * list, but an entry this build doesn't get is stale by definition: so at launch, whichever permission is
 * missing is reset. A stale entry goes and "Open Settings" can ask again; where there is none, nothing
 * changes; a permission that works is never touched. */

static void tcc_reset(NSString *service)
{
    NSTask *t = [[NSTask alloc] init];
    t.executableURL = [NSURL fileURLWithPath:@"/usr/bin/tccutil"];
    t.arguments = @[ @"reset", service, NSBundle.mainBundle.bundleIdentifier ?: @"com.remodroid.desktop" ];
    t.standardOutput = t.standardError = [NSFileHandle fileHandleWithNullDevice];
    NSError *e = nil;
    if (![t launchAndReturnError:&e]) {
        NSLog(@"RemoDroid: tccutil reset %@: %@", service, e.localizedDescription);
        return;
    }
    [t waitUntilExit];
    NSLog(@"RemoDroid: %@ is off for this build: reset any stale entry (tccutil: %d)", service, t.terminationStatus);
}

static void forget_stale_permissions(void)
{
    if (!CGPreflightScreenCaptureAccess())
        tcc_reset(@"ScreenCapture");
    if (!AXIsProcessTrusted())
        tcc_reset(@"Accessibility");
}

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
@property(strong) NSTextField *title, *state, *info, *drop, *line, *name, *capState, *ctlState;
@property(strong) NSButton *pair, *unpair, *openPanel, *disconnect, *viewOnly, *login;
@property(strong) NSView *pairBox, *pairedBox;
@property(strong) NSTimer *permTimer;
@property(strong) NSWindow *logWindow;
@property(strong) NSTextView *logView;
@property(strong) NSTimer *logTimer;
@property(assign) unsigned logSeq;
@property(strong) NSWindow *ftWindow;
@property(strong) id ftList;
@property(strong) NSTimer *ftTimer;
@property(assign) unsigned ftSeq;
@property(assign) unsigned rxSeq; /* rd_agent_received_seq() last seen */
@property(strong) NSScrollView *ftScroll;
@property(strong) NSView *ftLive;        /* files under way, under the list */
@property(strong) NSString *ftLiveText;  /* rd_agent_transfers_active() as shown */
@property(assign) rd_status_t st;
@property(assign) BOOL pairing;
@property(strong) NSString *pairMsg;
- (void)refresh;
- (void)notify:(NSString *)text;
@end

static AppDelegate *app;

/* Files to the viewer, from a drop or the open panel: its File Transfer keeps them. */
static BOOL send_urls(NSArray<NSURL *> *urls)
{
    const char **paths = calloc(urls.count ? urls.count : 1, sizeof *paths);
    int k = 0;
    for (NSURL *u in urls)
        if (u.fileURL)
            paths[k++] = u.fileSystemRepresentation; /* valid while urls lives */
    BOOL ok = rd_agent_send_files(paths, k) == 0;
    free(paths);
    [app notify:!ok ? @"Nobody is viewing this Mac, so nothing was sent."
                : k == 1 ? @"Sending 1 file to the viewer…"
                         : [NSString stringWithFormat:@"Sending %d files to the viewer…", k]];
    return ok;
}

static NSArray<NSURL *> *dragged_files(id<NSDraggingInfo> info)
{
    return [info.draggingPasteboard readObjectsForClasses:@[ NSURL.class ]
                                                  options:@{ NSPasteboardURLReadingFileURLsOnlyKey : @YES }];
}

/* The window's content: files dropped on it go to the viewer. */
@interface DropView : NSView
@end

@implementation DropView
- (instancetype)initWithFrame:(NSRect)r
{
    if ((self = [super initWithFrame:r]))
        [self registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
    return self;
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)info
{
    /* a file dragged out of File Transfer and back in isn't a send */
    return dragged_files(info).count && ![(id)info.draggingSource isKindOfClass:NSView.class] ? NSDragOperationCopy
                                                                                             : NSDragOperationNone;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)info { return send_urls(dragged_files(info)); }
@end

/* ---------------- File Transfer ----------------
 * What the viewer's browser sent here (left) and what was sent to it (right), like a chat. A file
 * drags out to Finder or the desktop, double-click opens it, right-click shows it in Finder; files
 * dropped on the window go to the viewer. */

@interface FTItem : NSObject
@property(assign) long long ms, size;
@property(assign) BOOL incoming, gone, stamp;
@property(strong) NSString *path, *name;
@property(strong) NSImage *thumb;
@property(assign) NSRect bubble;
@property(assign) CGFloat stampY;
@end

@implementation FTItem
@end

static NSImage *thumbnail(NSString *path, CGFloat max)
{
    CGImageSourceRef src = CGImageSourceCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path], NULL);
    if (!src)
        return nil;
    NSDictionary *o = @{ (id)kCGImageSourceCreateThumbnailFromImageAlways : @YES,
                         (id)kCGImageSourceCreateThumbnailWithTransform : @YES,
                         (id)kCGImageSourceThumbnailMaxPixelSize : @(max * 2) };
    CGImageRef im = CGImageSourceCreateThumbnailAtIndex(src, 0, (__bridge CFDictionaryRef)o);
    CFRelease(src);
    if (!im)
        return nil;
    NSSize sz = NSMakeSize(CGImageGetWidth(im) / 2.0, CGImageGetHeight(im) / 2.0);
    CGFloat k = MIN(1, MIN(220 / sz.width, 160 / sz.height));
    NSImage *img = [[NSImage alloc] initWithCGImage:im size:NSMakeSize(round(sz.width * k), round(sz.height * k))];
    CGImageRelease(im);
    return img;
}

static NSColor *rgb(unsigned c, CGFloat a)
{
    return [NSColor colorWithSRGBRed:(c >> 16) / 255.0 green:((c >> 8) & 0xFF) / 255.0 blue:(c & 0xFF) / 255.0 alpha:a];
}

@interface FTList : NSView <NSDraggingSource>
@property(strong) NSArray<FTItem *> *items;
@end

@implementation FTList
- (BOOL)isFlipped { return YES; }

- (BOOL)dark
{
    return [[self.effectiveAppearance bestMatchFromAppearancesWithNames:@[ NSAppearanceNameAqua, NSAppearanceNameDarkAqua ]]
        isEqualToString:NSAppearanceNameDarkAqua];
}

/* Bubble places for this width; the view grows to hold them (at least the visible height). */
- (void)layoutFor:(CGFloat)W minHeight:(CGFloat)H
{
    CGFloat y = 10, m = 16;
    for (FTItem *it in self.items) {
        it.stampY = -1;
        if (it.stamp) {
            it.stampY = y + 6;
            y += 34;
        }
        NSSize b = it.thumb ? NSMakeSize(it.thumb.size.width + 8, it.thumb.size.height + 8)
                            : NSMakeSize(MIN(W - 2 * m - 60, 300), 76);
        it.bubble = NSMakeRect(it.incoming ? m : W - m - b.width, y, b.width, b.height);
        y += b.height + 12;
    }
    [self setFrameSize:NSMakeSize(W, MAX(y, H))];
    self.needsDisplay = YES;
}

- (void)drawRect:(NSRect)dirty
{
    BOOL dark = [self dark];
    [(dark ? rgb(0x1E1E1E, 1) : rgb(0xEDEDED, 1)) setFill];
    NSRectFill(dirty);
    NSColor *dim = rgb(dark ? 0x8A8A8A : 0x999999, 1);
    if (!self.items.count) {
        NSMutableParagraphStyle *ps = [[NSMutableParagraphStyle alloc] init];
        ps.alignment = NSTextAlignmentCenter;
        [@"Nothing yet.\n\nFiles the viewer sends land here, and files you drop on this window go to the viewer's browser."
            drawInRect:NSMakeRect(30, 70, self.bounds.size.width - 60, 120)
            withAttributes:@{ NSFontAttributeName : [NSFont systemFontOfSize:13], NSForegroundColorAttributeName : dim,
                              NSParagraphStyleAttributeName : ps }];
    }
    NSMutableParagraphStyle *wrap = [[NSMutableParagraphStyle alloc] init];
    wrap.lineBreakMode = NSLineBreakByCharWrapping;
    NSMutableParagraphStyle *center = [[NSMutableParagraphStyle alloc] init];
    center.alignment = NSTextAlignmentCenter;
    for (FTItem *it in self.items) {
        NSRect b = it.bubble;
        if (!NSIntersectsRect(NSInsetRect(b, 0, -40), dirty))
            continue;
        if (it.stampY >= 0) {
            char when[40];
            rd_xfer_when(it.ms, when, sizeof when);
            [str(when) drawInRect:NSMakeRect(0, it.stampY, self.bounds.size.width, 18)
                   withAttributes:@{ NSFontAttributeName : [NSFont systemFontOfSize:11], NSForegroundColorAttributeName : dim,
                                     NSParagraphStyleAttributeName : center }];
        }
        NSColor *fill = it.incoming ? (dark ? rgb(0x2C2C2C, 1) : NSColor.whiteColor) : (dark ? rgb(0x3EB575, 1) : rgb(0x95EC69, 1));
        [fill setFill];
        [[NSBezierPath bezierPathWithRoundedRect:b xRadius:6 yRadius:6] fill];
        NSBezierPath *tail = [NSBezierPath bezierPath];
        CGFloat tx = it.incoming ? NSMinX(b) : NSMaxX(b), s = it.incoming ? -1 : 1, ty = NSMinY(b) + 14;
        [tail moveToPoint:NSMakePoint(tx, ty)];
        [tail lineToPoint:NSMakePoint(tx + s * 6, ty + 6)];
        [tail lineToPoint:NSMakePoint(tx, ty + 12)];
        [tail fill];
        if (it.thumb) {
            [it.thumb drawInRect:NSInsetRect(b, 4, 4) fromRect:NSZeroRect operation:NSCompositingOperationSourceOver
                        fraction:1 respectFlipped:YES hints:nil];
            continue;
        }
        NSColor *ink = it.gone ? dim : dark && it.incoming ? rgb(0xE6E6E6, 1) : rgb(0x191919, 1);
        NSRect badge = NSMakeRect(NSMaxX(b) - 14 - 38, NSMidY(b) - 24, 38, 48);
        [it.name drawWithRect:NSMakeRect(NSMinX(b) + 14, NSMinY(b) + 12, NSMinX(badge) - NSMinX(b) - 26, 36)
                      options:NSStringDrawingUsesLineFragmentOrigin | NSStringDrawingTruncatesLastVisibleLine
                   attributes:@{ NSFontAttributeName : [NSFont systemFontOfSize:14], NSForegroundColorAttributeName : ink,
                                 NSParagraphStyleAttributeName : wrap }
                      context:nil];
        char sz[64];
        rd_xfer_size(it.size, sz, sizeof sz);
        NSString *meta = it.gone ? [NSString stringWithFormat:@"%s · moved or deleted", sz] : str(sz);
        [meta drawAtPoint:NSMakePoint(NSMinX(b) + 14, NSMaxY(b) - 24)
            withAttributes:@{ NSFontAttributeName : [NSFont systemFontOfSize:11],
                              NSForegroundColorAttributeName : it.incoming || dark ? dim : rgb(0x4E8A35, 1) }];
        char ext[8];
        unsigned c;
        rd_xfer_badge(it.name.UTF8String, ext, sizeof ext, &c);
        [(it.gone ? rgb(0xBBBBBB, 1) : rgb(c, 1)) setFill];
        [[NSBezierPath bezierPathWithRoundedRect:badge xRadius:4 yRadius:4] fill];
        [str(ext) drawInRect:NSMakeRect(NSMinX(badge), NSMinY(badge) + 22, NSWidth(badge), 16)
              withAttributes:@{ NSFontAttributeName : [NSFont boldSystemFontOfSize:10],
                                NSForegroundColorAttributeName : NSColor.whiteColor, NSParagraphStyleAttributeName : center }];
    }
}

- (FTItem *)itemAt:(NSEvent *)e
{
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    for (FTItem *it in self.items)
        if (NSPointInRect(p, it.bubble))
            return it;
    return nil;
}

- (void)open:(FTItem *)it inFinder:(BOOL)finder
{
    NSURL *u = [NSURL fileURLWithPath:it.path];
    if (it.gone || ![NSFileManager.defaultManager fileExistsAtPath:it.path])
        [app notify:@"That file is no longer there."];
    else if (finder)
        [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[ u ]];
    else
        [NSWorkspace.sharedWorkspace openURL:u];
}

- (void)mouseDown:(NSEvent *)e
{
    FTItem *it = [self itemAt:e];
    if (!it || it.gone)
        return;
    if (e.clickCount == 2) {
        [self open:it inFinder:NO];
        return;
    }
    /* a drag: the file itself, copied wherever it's dropped */
    NSEvent *next = [self.window nextEventMatchingMask:NSEventMaskLeftMouseUp | NSEventMaskLeftMouseDragged];
    if (next.type != NSEventTypeLeftMouseDragged)
        return;
    NSDraggingItem *d = [[NSDraggingItem alloc] initWithPasteboardWriter:[NSURL fileURLWithPath:it.path]];
    NSImage *img = it.thumb ?: [NSWorkspace.sharedWorkspace iconForFile:it.path];
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    [d setDraggingFrame:NSMakeRect(p.x - 24, p.y - 24, 48, 48) contents:img];
    [self beginDraggingSessionWithItems:@[ d ] event:e source:self];
}

- (NSDragOperation)draggingSession:(NSDraggingSession *)s sourceOperationMaskForDraggingContext:(NSDraggingContext)c
{
    return NSDragOperationCopy;
}

- (NSMenu *)menuForEvent:(NSEvent *)e
{
    FTItem *it = [self itemAt:e];
    if (!it || it.gone)
        return nil;
    NSMenu *m = [[NSMenu alloc] init];
    [m addItemWithTitle:@"Open" action:@selector(menuOpen:) keyEquivalent:@""].representedObject = it;
    [m addItemWithTitle:@"Show in Finder" action:@selector(menuReveal:) keyEquivalent:@""].representedObject = it;
    for (NSMenuItem *i in m.itemArray)
        i.target = self;
    return m;
}

- (void)resizeWithOldSuperviewSize:(NSSize)old
{
    [super resizeWithOldSuperviewSize:old];
    [self layoutFor:self.superview.bounds.size.width minHeight:self.superview.bounds.size.height];
}

- (void)menuOpen:(NSMenuItem *)i { [self open:i.representedObject inFinder:NO]; }
- (void)menuReveal:(NSMenuItem *)i { [self open:i.representedObject inFinder:YES]; }
@end

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

- (BOOL)loginEnabled { return SMAppService.mainAppService.status == SMAppServiceStatusEnabled; }

- (void)setStartAtLogin:(BOOL)on
{
    NSError *e = nil;
    if (on)
        [SMAppService.mainAppService registerAndReturnError:&e];
    else
        [SMAppService.mainAppService unregisterAndReturnError:&e];
    if (e)
        [self notify:[NSString stringWithFormat:@"Start at login: %@", e.localizedDescription]];
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
        [m addItemWithTitle:@"Open window" action:@selector(openPanel:) keyEquivalent:@""].target = self;
    [m addItemWithTitle:self.st.state == RD_UNPAIRED ? @"Pair with a panel…" : @"Pairing & status…" action:@selector(showWindow:) keyEquivalent:@""].target = self;
    [m addItemWithTitle:@"File Transfer" action:@selector(showFiles:) keyEquivalent:@""].target = self;
    [m addItemWithTitle:@"Show log" action:@selector(showLog:) keyEquivalent:@""].target = self;
    [m addItem:[NSMenuItem separatorItem]];
    NSMenuItem *li = [m addItemWithTitle:@"Start at login" action:@selector(toggleLogin:) keyEquivalent:@""];
    li.target = self;
    li.state = [self loginEnabled] ? NSControlStateValueOn : NSControlStateValueOff;
    [m addItemWithTitle:@"Quit RemoDroid" action:@selector(terminate:) keyEquivalent:@"q"];
}

- (void)disconnect:(id)sender { rd_agent_end_session(); }
- (void)toggleViewOnly:(id)sender { rd_agent_set("desktop.view_only", self.st.view_only ? "false" : "true"); }
- (void)toggleLogin:(id)sender { [self setStartAtLogin:![self loginEnabled]]; [self refresh]; }

/* the panel in a window of its own (web.m), not in the default browser */
- (void)openPanel:(id)sender
{
    if (self.st.panel[0])
        rd_web_show(self.st.panel, ^(NSString *text) { [app notify:text]; });
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
    self.window.contentView = [[DropView alloc] initWithFrame:NSMakeRect(0, 0, W, H)];
    NSView *v = self.window.contentView;
    self.title = label(@"", v, NSMakeRect(20, H - 50, W - 40, 26), 17, YES);
    self.state = label(@"", v, NSMakeRect(20, H - 92, W - 40, 38), 12, NO);

    self.pairBox = [[NSView alloc] initWithFrame:NSMakeRect(0, 140, W, 190)];
    [v addSubview:self.pairBox];
    label(@"Pairing command", self.pairBox, NSMakeRect(20, 160, W - 40, 20), 13, NO);
    self.line = field(self.pairBox, NSMakeRect(20, 130, W - 40, 26), @"remodroid enroll --panel https://… --code XXXX-XXXX");
    self.line.font = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
    self.line.usesSingleLineMode = YES; /* a pasted multi-line command becomes one line */
    self.line.cell.scrollable = YES;
    self.line.cell.wraps = NO;
    label(@"Paste the whole line from the panel: Agents → Add → Computer.", self.pairBox, NSMakeRect(20, 104, W - 40, 20), 12, NO).textColor = NSColor.secondaryLabelColor;
    label(@"Name", self.pairBox, NSMakeRect(20, 64, 60, 20), 13, NO);
    self.name = field(self.pairBox, NSMakeRect(86, 60, W - 106, 24), @"");
    self.name.stringValue = [[NSHost currentHost] localizedName] ?: @"";
    self.pair = button(@"Pair", self.pairBox, NSMakeRect(W - 120, 4, 100, 30), self, @selector(doPair:));
    self.pair.keyEquivalent = @"\r";

    self.pairedBox = [[NSView alloc] initWithFrame:NSMakeRect(0, 140, W, 190)];
    [v addSubview:self.pairedBox];
    self.info = label(@"", self.pairedBox, NSMakeRect(20, 134, W - 40, 44), 12, NO);
    self.info.selectable = YES;
    self.openPanel = button(@"Open panel", self.pairedBox, NSMakeRect(16, 90, 120, 30), self, @selector(openPanel:));
    self.disconnect = button(@"Disconnect viewer", self.pairedBox, NSMakeRect(140, 90, 150, 30), self, @selector(disconnect:));
    self.unpair = button(@"Unpair", self.pairedBox, NSMakeRect(W - 116, 90, 100, 30), self, @selector(doUnpair:));
    self.drop = label(@"", self.pairedBox, NSMakeRect(20, 30, W - 40, 40), 12, NO);
    self.drop.textColor = NSColor.secondaryLabelColor;

    /* permissions */
    label(@"Permissions", v, NSMakeRect(20, 108, W - 40, 20), 13, YES);
    self.capState = label(@"", v, NSMakeRect(20, 84, 260, 20), 12, NO);
    button(@"Open Settings", v, NSMakeRect(W - 140, 78, 124, 28), self, @selector(openCaptureSettings:));
    self.ctlState = label(@"", v, NSMakeRect(20, 56, 260, 20), 12, NO);
    button(@"Open Settings", v, NSMakeRect(W - 140, 50, 124, 28), self, @selector(openControlSettings:));

    self.viewOnly = [NSButton checkboxWithTitle:@"View only" target:self action:@selector(viewOnlyChanged:)];
    self.viewOnly.frame = NSMakeRect(18, 16, 88, 24);
    [v addSubview:self.viewOnly];
    self.login = [NSButton checkboxWithTitle:@"Start at login" target:self action:@selector(loginChanged:)];
    self.login.frame = NSMakeRect(106, 16, 112, 24);
    [v addSubview:self.login];
    button(@"File Transfer", v, NSMakeRect(W - 222, 12, 108, 30), self, @selector(showFiles:));
    button(@"Show log", v, NSMakeRect(W - 114, 12, 98, 30), self, @selector(showLog:));
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
    self.drop.stringValue = self.st.state == RD_IN_SESSION
                                ? @"Drop files on this window to send them to the viewer's File Transfer."
                                : @"While someone is viewing, drop files on this window to send them.";
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
        [self.window makeFirstResponder:self.line];
    if (!self.permTimer) /* permissions change in System Settings: keep the ticks current */
        self.permTimer = [NSTimer scheduledTimerWithTimeInterval:2 target:self selector:@selector(refresh) userInfo:nil repeats:YES];
}

- (void)windowWillClose:(NSNotification *)n
{
    if (n.object == self.logWindow) {
        [self.logTimer invalidate];
        self.logTimer = nil;
        return;
    }
    if (n.object == self.ftWindow) {
        [self.ftTimer invalidate];
        self.ftTimer = nil;
        return;
    }
    [self.permTimer invalidate];
    self.permTimer = nil;
}

- (void)doPair:(id)sender
{
    const char *bad = rd_agent_pair_line(self.line.stringValue.UTF8String, self.name.stringValue.UTF8String, on_paired, NULL);
    self.pairing = !bad;
    self.pairMsg = bad ? [NSString stringWithFormat:@"Pairing failed: %s", bad] : nil;
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
- (void)loginChanged:(id)sender { [self setStartAtLogin:self.login.state == NSControlStateValueOn]; }

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

/* ---------------- the File Transfer window ---------------- */

/* a file from the browser: File Transfer opens with it, as a chat would */
- (void)rxTick
{
    unsigned r = rd_agent_received_seq();
    if (r != self.rxSeq) {
        self.rxSeq = r;
        [self showFiles:nil];
    }
}

- (void)showFiles:(id)sender
{
    if (!self.ftWindow) {
        const CGFloat W = 460, H = 620, BAR = 50;
        NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, W, H)
                                                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                            NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                                                    backing:NSBackingStoreBuffered defer:NO];
        w.title = @"File Transfer";
        w.releasedWhenClosed = NO;
        w.delegate = self;
        w.minSize = NSMakeSize(340, 300);
        w.contentView = [[DropView alloc] initWithFrame:NSMakeRect(0, 0, W, H)];
        NSView *v = w.contentView;
        NSScrollView *sv = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, BAR, W, H - BAR)];
        sv.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        sv.hasVerticalScroller = YES;
        sv.drawsBackground = NO;
        FTList *list = [[FTList alloc] initWithFrame:NSMakeRect(0, 0, W, H - BAR)];
        list.autoresizingMask = NSViewWidthSizable;
        sv.documentView = list;
        [v addSubview:sv];
        NSBox *line = [[NSBox alloc] initWithFrame:NSMakeRect(0, BAR - 1, W, 1)];
        line.boxType = NSBoxSeparator;
        line.autoresizingMask = NSViewWidthSizable | NSViewMaxYMargin;
        [v addSubview:line];
        NSTextField *hint = label(@"Drop files here to send them", v, NSMakeRect(14, 16, W - 230, 18), 12, NO);
        hint.textColor = NSColor.secondaryLabelColor;
        hint.lineBreakMode = NSLineBreakByTruncatingTail;
        hint.autoresizingMask = NSViewWidthSizable;
        NSButton *send = button(@"Send files…", v, NSMakeRect(W - 210, 10, 112, 30), self, @selector(ftSend:));
        send.autoresizingMask = NSViewMinXMargin;
        send.bezelColor = NSColor.controlAccentColor;
        button(@"Clear", v, NSMakeRect(W - 94, 10, 80, 30), self, @selector(ftClear:)).autoresizingMask = NSViewMinXMargin;
        NSView *live = [[NSView alloc] initWithFrame:NSMakeRect(0, BAR, W, 0)];
        live.autoresizingMask = NSViewWidthSizable | NSViewMaxYMargin;
        [v addSubview:live];
        self.ftLive = live;
        self.ftScroll = sv;
        self.ftList = list;
        self.ftWindow = w;
        [w center];
    }
    self.ftLiveText = nil; /* the rows again, as they are now */
    self.ftSeq = rd_agent_transfers_seq() - 1; /* fill it now */
    [self ftTick];
    if (!self.ftTimer)
        self.ftTimer = [NSTimer scheduledTimerWithTimeInterval:0.7 target:self selector:@selector(ftTick) userInfo:nil repeats:YES];
    [NSApp activateIgnoringOtherApps:YES];
    [self.ftWindow makeKeyAndOrderFront:nil];
}

/* Files under way: a row each under the list, the name and how much of how much and how fast over a
 * progress bar; the list gives them room. */
- (void)ftLiveTick
{
    char *t = rd_agent_transfers_active();
    NSString *s = str(t);
    free(t);
    if (self.ftLiveText && [s isEqualToString:self.ftLiveText])
        return;
    self.ftLiveText = s;
    char *copy = strdup(s.UTF8String);
    rd_xfer_live_t v[16];
    int n = rd_xfers_live_parse(copy, v, 16);
    for (NSView *x in [self.ftLive.subviews copy])
        [x removeFromSuperview];
    const CGFloat ROW = 46, PAD = 10, BAR = 50, INFO = 170;
    NSRect b = self.ftWindow.contentView.bounds;
    CGFloat W = b.size.width, live = n ? n * ROW + PAD : 0;
    self.ftLive.frame = NSMakeRect(0, BAR, W, live);
    self.ftScroll.frame = NSMakeRect(0, BAR + live, W, b.size.height - BAR - live);
    for (int i = 0; i < n; i++) {
        CGFloat y = live - PAD / 2 - (i + 1) * ROW;
        char how[120];
        rd_xfer_live_text(&v[i], how, sizeof how);
        NSTextField *name = [NSTextField labelWithString:[NSString stringWithFormat:@"%@ %@", v[i].in ? @"↓" : @"↑", str(v[i].name)]];
        name.frame = NSMakeRect(14, y + 22, W - 28 - INFO, 18);
        name.lineBreakMode = NSLineBreakByTruncatingMiddle;
        name.autoresizingMask = NSViewWidthSizable;
        NSTextField *info = [NSTextField labelWithString:str(how)];
        info.frame = NSMakeRect(W - 14 - INFO, y + 22, INFO, 18);
        info.alignment = NSTextAlignmentRight;
        info.font = [NSFont systemFontOfSize:11];
        info.textColor = NSColor.secondaryLabelColor;
        info.autoresizingMask = NSViewMinXMargin;
        NSProgressIndicator *bar = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(14, y + 4, W - 28, 14)];
        bar.style = NSProgressIndicatorStyleBar;
        bar.indeterminate = NO;
        bar.minValue = 0;
        bar.maxValue = 1;
        bar.doubleValue = rd_xfer_live_frac(&v[i]);
        bar.autoresizingMask = NSViewWidthSizable;
        [self.ftLive addSubview:name];
        [self.ftLive addSubview:info];
        [self.ftLive addSubview:bar];
    }
    free(copy);
}

- (void)ftTick
{
    [self ftLiveTick];
    unsigned seq = rd_agent_transfers_seq();
    if (seq == self.ftSeq)
        return;
    self.ftSeq = seq;
    char *text = rd_agent_transfers();
    rd_xfer_t *v;
    int n = rd_xfers_parse(text, &v);
    NSMutableArray *items = [NSMutableArray arrayWithCapacity:(NSUInteger)n];
    NSFileManager *fm = NSFileManager.defaultManager;
    for (int i = 0; i < n; i++) {
        FTItem *it = [[FTItem alloc] init];
        it.ms = v[i].ms;
        it.size = v[i].size;
        it.incoming = v[i].in;
        it.stamp = v[i].stamp;
        it.path = str(v[i].path);
        it.name = str(v[i].name);
        BOOL dir = NO;
        it.gone = ![fm fileExistsAtPath:it.path isDirectory:&dir] || dir;
        if (!it.gone && rd_xfer_is_image(v[i].name) && i >= n - 60) /* the newest pictures as pictures */
            it.thumb = thumbnail(it.path, 220);
        [items addObject:it];
    }
    free(v);
    rd_agent_free(text);
    FTList *list = self.ftList;
    list.items = items;
    NSClipView *clip = list.enclosingScrollView.contentView;
    [list layoutFor:clip.bounds.size.width minHeight:clip.bounds.size.height];
    [list scrollPoint:NSMakePoint(0, NSHeight(list.frame))];
}

- (void)ftSend:(id)sender
{
    NSOpenPanel *p = [NSOpenPanel openPanel];
    p.allowsMultipleSelection = YES;
    p.canChooseDirectories = NO;
    p.prompt = @"Send";
    p.message = @"Send to the viewer";
    [p beginSheetModalForWindow:self.ftWindow completionHandler:^(NSModalResponse r) {
        if (r == NSModalResponseOK)
            send_urls(p.URLs);
    }];
}

- (void)ftClear:(id)sender
{
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = @"Clear the list?";
    a.informativeText = @"The files themselves stay where they are.";
    [a addButtonWithTitle:@"Clear"];
    [a addButtonWithTitle:@"Cancel"];
    if ([a runModal] == NSAlertFirstButtonReturn) {
        rd_agent_transfers_clear();
        [self ftTick];
    }
}

/* ---------------- the log window ---------------- */

- (void)showLog:(id)sender
{
    if (!self.logWindow) {
        const CGFloat W = 820, H = 480;
        NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, W, H)
                                                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                            NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                                                    backing:NSBackingStoreBuffered defer:NO];
        w.title = @"RemoDroid log";
        w.releasedWhenClosed = NO;
        w.delegate = self;
        w.minSize = NSMakeSize(420, 200);
        NSView *v = w.contentView;
        NSScrollView *sv = [[NSScrollView alloc] initWithFrame:NSMakeRect(12, 52, W - 24, H - 64)];
        sv.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        sv.hasVerticalScroller = YES;
        sv.hasHorizontalScroller = YES;
        sv.borderType = NSBezelBorder;
        NSTextView *tv = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, sv.contentSize.width, sv.contentSize.height)];
        tv.editable = NO;
        tv.selectable = YES;
        tv.richText = NO;
        tv.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
        tv.textContainerInset = NSMakeSize(4, 4);
        tv.horizontallyResizable = YES; /* long lines scroll sideways instead of wrapping */
        tv.maxSize = NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX);
        tv.textContainer.widthTracksTextView = NO;
        tv.textContainer.containerSize = NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX);
        tv.autoresizingMask = NSViewHeightSizable;
        sv.documentView = tv;
        [v addSubview:sv];
        NSTextField *path = label(str(rd_agent_log_path()), v, NSMakeRect(14, 18, W - 290, 18), 11, NO);
        path.textColor = NSColor.secondaryLabelColor;
        path.selectable = YES;
        path.lineBreakMode = NSLineBreakByTruncatingMiddle;
        path.maximumNumberOfLines = 1;
        path.autoresizingMask = NSViewWidthSizable;
        button(@"Copy", v, NSMakeRect(W - 262, 10, 110, 30), self, @selector(copyLog:)).autoresizingMask = NSViewMinXMargin;
        button(@"Show in Finder", v, NSMakeRect(W - 146, 10, 134, 30), self, @selector(revealLog:)).autoresizingMask =
            NSViewMinXMargin;
        self.logView = tv;
        self.logWindow = w;
        [w center];
    }
    self.logSeq = rd_agent_log_seq() - 1; /* fill it now */
    [self logTick];
    if (!self.logTimer)
        self.logTimer = [NSTimer scheduledTimerWithTimeInterval:0.5 target:self selector:@selector(logTick) userInfo:nil repeats:YES];
    [NSApp activateIgnoringOtherApps:YES];
    [self.logWindow makeKeyAndOrderFront:nil];
}

/* New lines appear as they are logged; the view follows the end unless scrolled up. */
- (void)logTick
{
    unsigned seq = rd_agent_log_seq();
    if (seq == self.logSeq)
        return;
    self.logSeq = seq;
    NSClipView *clip = self.logView.enclosingScrollView.contentView;
    BOOL atEnd = NSMaxY(clip.bounds) >= NSHeight(self.logView.frame) - 4;
    NSPoint keep = clip.bounds.origin;
    char *log = rd_agent_log(1000);
    self.logView.string = str(log);
    rd_agent_free(log);
    if (atEnd)
        [self.logView scrollRangeToVisible:NSMakeRange(self.logView.string.length, 0)];
    else
        [clip scrollToPoint:keep];
}

- (void)revealLog:(id)sender
{
    [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[ [NSURL fileURLWithPath:str(rd_agent_log_path())] ]];
}

- (void)copyLog:(id)sender
{
    char *log = rd_agent_log(1000);
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
    forget_stale_permissions(); /* after an update: so "Open Settings" can ask again */
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
    self.rxSeq = rd_agent_received_seq();
    [NSTimer scheduledTimerWithTimeInterval:0.7 target:self selector:@selector(rxTick) userInfo:nil repeats:YES];
    BOOL background = [args containsObject:@"--background"];
    if (!background || s.state == RD_UNPAIRED || !CGPreflightScreenCaptureAccess())
        [self showWindow:nil];
}

- (BOOL)applicationShouldHandleReopen:(NSApplication *)a hasVisibleWindows:(BOOL)visible
{
    if (!rd_web_reopen())
        [self showWindow:nil]; /* opened again from Finder / Launchpad */
    return NO;
}

- (void)applicationWillTerminate:(NSNotification *)n { rd_agent_stop(); }

@end

/* ⌘X/⌘C/⌘V/⌘A/⌘Z in text fields and ⌘W are key equivalents of the main menu, which an accessory app
 * never shows but still needs; while the panel window is open the app is a regular one and the bar shows */
static NSMenu *main_menu(void)
{
    NSMenu *bar = [[NSMenu alloc] init];
    NSMenu *appm = [[NSMenu alloc] initWithTitle:@"RemoDroid"];
    [appm addItemWithTitle:@"Hide RemoDroid" action:@selector(hide:) keyEquivalent:@"h"];
    [appm addItem:[NSMenuItem separatorItem]];
    [appm addItemWithTitle:@"Quit RemoDroid" action:@selector(terminate:) keyEquivalent:@"q"];
    [bar addItemWithTitle:@"" action:nil keyEquivalent:@""].submenu = appm;
    NSMenu *file = [[NSMenu alloc] initWithTitle:@"File"];
    /* tabs and windows of the panel (web.m): nil-targeted, they reach the panel window's delegate */
    [file addItemWithTitle:@"New Tab" action:NSSelectorFromString(@"newTab:") keyEquivalent:@"t"];
    [file addItemWithTitle:@"New Window" action:NSSelectorFromString(@"newWindow:") keyEquivalent:@"n"];
    [file addItem:[NSMenuItem separatorItem]];
    [file addItemWithTitle:@"Close Window" action:@selector(performClose:) keyEquivalent:@"w"];
    [bar addItemWithTitle:@"File" action:nil keyEquivalent:@""].submenu = file;
    NSMenu *edit = [[NSMenu alloc] initWithTitle:@"Edit"];
    [edit addItemWithTitle:@"Undo" action:NSSelectorFromString(@"undo:") keyEquivalent:@"z"];
    [edit addItemWithTitle:@"Redo" action:NSSelectorFromString(@"redo:") keyEquivalent:@"Z"];
    [edit addItem:[NSMenuItem separatorItem]];
    [edit addItemWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
    [edit addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
    [edit addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
    [edit addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];
    [bar addItemWithTitle:@"Edit" action:nil keyEquivalent:@""].submenu = edit;
    [bar addItemWithTitle:@"View" action:nil keyEquivalent:@""].submenu = rd_web_menu();
    NSMenu *win = [[NSMenu alloc] initWithTitle:@"Window"];
    [win addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    [win addItem:[NSMenuItem separatorItem]];
    [win addItemWithTitle:@"Show Next Tab" action:NSSelectorFromString(@"selectNextTab:") keyEquivalent:@"}"];
    [win addItemWithTitle:@"Show Previous Tab" action:NSSelectorFromString(@"selectPreviousTab:") keyEquivalent:@"{"];
    [bar addItemWithTitle:@"Window" action:nil keyEquivalent:@""].submenu = win;
    NSApp.windowsMenu = win;
    return bar;
}

int main(int argc, const char **argv)
{
    @autoreleasepool {
        NSApplication *a = [NSApplication sharedApplication];
        app = [[AppDelegate alloc] init];
        a.delegate = app;
        a.mainMenu = main_menu();
        [a setActivationPolicy:NSApplicationActivationPolicyAccessory]; /* menu bar only, no Dock icon */
        [a run];
    }
    return 0;
}
