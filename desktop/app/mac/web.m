/* The panel in a window of its own: "Open window" in the menu bar menu shows the panel (device list,
 * viewer) in a WKWebView instead of the default browser, like a small single-site browser.
 *
 * Tabs work as in Chrome: the title bar holds a strip of tabs with a "+" after the last one (⌘T too);
 * a tab is dragged along the strip to reorder it, pulled off the strip to become a window of its own,
 * and dropped on another window's strip to join it. Each tab is in fact a window of its own with its
 * own WKWebView, and only the selected one of a group is on screen, so a tab moves between windows
 * without reloading (a running viewer keeps going). The strip is our own: AppKit's native tab bar would
 * add a row under the title bar.
 *
 * Every tab uses the default (persistent) website data store: the sign-in cookie has a Max-Age and is
 * kept across launches, and the viewer's settings live in localStorage. Which device a tab shows is its
 * own (sessionStorage, app.js), so tabs on different devices do not pull each other along. While a
 * window is open the app is a regular app (Dock icon, ⌘-Tab, its menus); when the last one closes, the
 * app goes back to the menu bar only. Links to other sites open in the default browser; files the
 * viewer downloads (files sent from a device) land in ~/Downloads. */
#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>

#include "web.h"

static const CGFloat kTabsHeight = 26;
static const CGFloat kTabsMinWidth = 120;
static const CGFloat kTabsLeading = 150;     /* window buttons plus the name, before the strip is placed */
static const CGFloat kTabsTrailing = 24;     /* kept free at the right, or the toolbar overflows */
static const CGFloat kTabGap = 6;
static const CGFloat kTabMaxWidth = 200;
static const CGFloat kTabCloseWidth = 16;
static const CGFloat kTabCloseGap = 2;
static const CGFloat kTabCloseFloor = 64;    /* a narrower tab drops its close button (⌘W still closes) */
static const CGFloat kTabNewWidth = 22;      /* the "+" after the last tab */
static const CGFloat kTabPad = 9;            /* the title's inset from the tab's rounded ends */
static const CGFloat kTabDragSlop = 4;       /* a press that moves less than this is a click */
static const CGFloat kTabTearDistance = 24;  /* how far off the strip a tab is pulled to leave it */

static BOOL is_web(NSURL *u)
{
    NSString *s = u.scheme.lowercaseString;
    return [s isEqual:@"http"] || [s isEqual:@"https"];
}

static NSRect screen_rect(NSView *v)
{
    if (!v.window)
        return NSZeroRect;
    return [v.window convertRectToScreen:[v convertRect:v.bounds toView:nil]];
}

/* A free name in ~/Downloads: "name.ext", then "name (1).ext", "name (2).ext", … */
static NSURL *download_path(NSString *name)
{
    NSFileManager *fm = NSFileManager.defaultManager;
    NSURL *dir = [fm URLsForDirectory:NSDownloadsDirectory inDomains:NSUserDomainMask].firstObject;
    name = name.lastPathComponent.length ? name.lastPathComponent : @"download";
    NSString *base = name.stringByDeletingPathExtension, *ext = name.pathExtension;
    NSURL *u = [dir URLByAppendingPathComponent:name];
    for (int i = 1; [fm fileExistsAtPath:u.path] && i < 1000; i++) {
        NSString *n = [NSString stringWithFormat:@"%@ (%d)", base, i];
        u = [dir URLByAppendingPathComponent:ext.length ? [n stringByAppendingPathExtension:ext] : n];
    }
    return u;
}

/* the strip's width is set from the free room in the title bar (layoutTabs) */
@interface TabStrip : NSView
@property CGFloat stripWidth;
@end
@implementation TabStrip
- (NSSize)intrinsicContentSize { return NSMakeSize(self.stripWidth > 0 ? self.stripWidth : 360, kTabsHeight); }
@end

/* One tab: a window with its page. The tabs of one window share `group` (in strip order); the
 * selected one is the group's window on screen, the rest are ordered out. */
@interface PanelWindow : NSObject <NSWindowDelegate, NSToolbarDelegate, WKNavigationDelegate, WKUIDelegate, WKDownloadDelegate>
@property(strong) NSWindow *window;
@property(strong) WKWebView *web;
@property(strong) NSURL *base;
@property(copy) void (^notify)(NSString *);
@property(strong) NSMapTable<WKDownload *, NSURL *> *saving; /* download → where it lands */
@property(strong) NSMutableArray<PanelWindow *> *group;
@property(strong) NSTextField *name;
@property(strong) TabStrip *tabs;
@property(strong) NSLayoutConstraint *tabsWidth;
@property NSInteger tabsPasses;
@property CGFloat tabStep; /* one tab plus its gap, as last laid out */
@property BOOL sliding;    /* this tab is being dragged along its strip */
@property CGFloat slideX;  /* the dragged tab's left edge in the strip */
- (void)show:(NSURL *)url;
- (void)selectTab:(id)sender;
- (void)closeTab:(id)sender;
- (void)refreshTabs;
- (void)dragTab:(NSView *)chip from:(NSEvent *)down;
@end

static NSMutableArray<PanelWindow *> *panels; /* every open tab, the most recently active last */
static char title_kvo;

static void refresh_all_tabs(void)
{
    for (PanelWindow *p in [panels copy])
        [p refreshTabs];
}

/* A tab in the strip: a pill with the title and, inside it as in Chrome, the tab's close button, so the
 * button moves with the tab. A press hands the drag to the owner, which works in screen points: the
 * strip is rebuilt (and this view dropped from it) many times while the button is down. */
@interface TabChip : NSView
@property(weak) PanelWindow *owner;
@property BOOL selected;
@property BOOL hovered;
@property(strong) NSButton *close;
@property(strong) NSTextField *label;
@end

@implementation TabChip

- (instancetype)initWithFrame:(NSRect)frame owner:(PanelWindow *)owner title:(NSString *)title
                     selected:(BOOL)selected closable:(BOOL)closable
{
    if (!(self = [super initWithFrame:frame]))
        return nil;
    _owner = owner;
    _selected = selected;
    NSColor *ink = selected ? NSColor.alternateSelectedControlTextColor : NSColor.labelColor; /* white on the accent */
    CGFloat right = NSWidth(frame) - kTabPad;
    if (closable) {
        NSImage *x = [[NSImage imageWithSystemSymbolName:@"xmark" accessibilityDescription:@"Close tab"]
            imageWithSymbolConfiguration:[NSImageSymbolConfiguration configurationWithPointSize:9 weight:NSFontWeightSemibold]];
        self.close = [NSButton buttonWithImage:x target:owner action:@selector(closeTab:)];
        self.close.bordered = NO;
        self.close.toolTip = @"Close tab";
        self.close.contentTintColor = selected ? ink : NSColor.secondaryLabelColor;
        self.close.frame = NSMakeRect(NSWidth(frame) - 5 - kTabCloseWidth, (kTabsHeight - 16) / 2, kTabCloseWidth, 16);
        right = NSMinX(self.close.frame) - kTabCloseGap;
    }
    NSTextField *l = [NSTextField labelWithString:title];
    l.font = [NSFont systemFontOfSize:11];
    l.textColor = ink;
    l.lineBreakMode = NSLineBreakByTruncatingTail;
    l.maximumNumberOfLines = 1;
    CGFloat h = ceil(l.fittingSize.height);
    l.frame = NSMakeRect(kTabPad, floor((kTabsHeight - h) / 2), MAX(0, right - kTabPad), h);
    [self addSubview:l];
    self.label = l;
    if (self.close)
        [self addSubview:self.close];
    self.toolTip = title;
    self.accessibilityElement = YES;
    self.accessibilityRole = NSAccessibilityRadioButtonRole;
    self.accessibilityLabel = title;
    self.accessibilityValue = @(selected);
    [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:NSZeroRect
                                                       options:NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
                                                         owner:self
                                                      userInfo:nil]];
    return self;
}

- (void)drawRect:(NSRect)dirty
{
    NSColor *fill = self.selected ? NSColor.controlAccentColor
                  : self.hovered  ? [NSColor.labelColor colorWithAlphaComponent:0.08]
                                  : nil;
    if (!fill)
        return;
    NSRect pill = NSInsetRect(self.bounds, 0, 2);
    [fill setFill];
    [[NSBezierPath bezierPathWithRoundedRect:pill xRadius:NSHeight(pill) / 2 yRadius:NSHeight(pill) / 2] fill];
}

/* the title bar drags the window from views that let it; a tab drags itself */
- (BOOL)mouseDownCanMoveWindow { return NO; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { return YES; }

/* only the close button takes clicks of its own; the title is part of the tab */
- (NSView *)hitTest:(NSPoint)p
{
    NSView *hit = [super hitTest:p];
    if (!hit)
        return nil;
    return self.close && [hit isDescendantOf:self.close] ? hit : self;
}

- (void)mouseEntered:(NSEvent *)e { self.hovered = YES; self.needsDisplay = YES; }
- (void)mouseExited:(NSEvent *)e { self.hovered = NO; self.needsDisplay = YES; }

- (void)mouseDown:(NSEvent *)e
{
    TabChip *held = self; /* the drag rebuilds the strip: keep this view until AppKit is done with the press */
    [self.owner dragTab:self from:e];
    dispatch_async(dispatch_get_main_queue(), ^{ (void)held; });
}

/* middle click closes, as in a browser */
- (void)otherMouseUp:(NSEvent *)e
{
    if (e.buttonNumber == 2)
        [self.owner closeTab:nil];
}

- (BOOL)accessibilityPerformPress
{
    [self.owner selectTab:nil];
    return YES;
}

@end

@implementation PanelWindow

- (void)build
{
    NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1200, 800)
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                        NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                backing:NSBackingStoreBuffered defer:NO];
    w.title = @"RemoDroid";
    w.releasedWhenClosed = NO;
    w.delegate = self;
    w.minSize = NSMakeSize(480, 360);
    NSWindow.allowsAutomaticWindowTabbing = NO;
    w.tabbingMode = NSWindowTabbingModeDisallowed;
    w.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
    [w center];
    [w setFrameAutosaveName:@"RemoDroidPanel"];

    /* the title bar: the name, then the tab strip across the rest of it. AppKit's own title view would
     * claim the spare room and push the strip to the far right, so the name is a plain label. */
    w.titleVisibility = NSWindowTitleHidden;
    self.name = [NSTextField labelWithString:@"RemoDroid"];
    self.name.font = [NSFont titleBarFontOfSize:13];
    self.tabs = [[TabStrip alloc] initWithFrame:NSMakeRect(0, 0, 360, kTabsHeight)];
    self.tabs.translatesAutoresizingMaskIntoConstraints = NO;
    self.tabsWidth = [self.tabs.widthAnchor constraintEqualToConstant:360];
    self.tabsWidth.active = YES;
    [self.tabs.heightAnchor constraintEqualToConstant:kTabsHeight].active = YES;
    NSToolbar *bar = [[NSToolbar alloc] initWithIdentifier:@"RemoDroidTabs"];
    bar.delegate = self;
    bar.displayMode = NSToolbarDisplayModeIconOnly;
    bar.allowsUserCustomization = NO;
    w.toolbarStyle = NSWindowToolbarStyleUnifiedCompact;
    w.toolbar = bar;

    WKWebViewConfiguration *c = [[WKWebViewConfiguration alloc] init];
    c.websiteDataStore = WKWebsiteDataStore.defaultDataStore;
    c.mediaTypesRequiringUserActionForPlayback = WKAudiovisualMediaTypeNone; /* the device's sound plays at once */
    c.preferences.elementFullscreenEnabled = YES;                            /* the viewer's Full screen */
    WKWebView *v = [[WKWebView alloc] initWithFrame:w.contentView.bounds configuration:c];
    v.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    v.navigationDelegate = self;
    v.UIDelegate = self;
    v.allowsBackForwardNavigationGestures = YES;
    v.allowsMagnification = YES;
    [v addObserver:self forKeyPath:@"title" options:0 context:&title_kvo];
    [w.contentView addSubview:v];
    self.window = w;
    self.web = v;
    if (!self.group)
        self.group = [NSMutableArray arrayWithObject:self];
    if (!panels)
        panels = [NSMutableArray array];
    [panels addObject:self];
}

- (void)show:(NSURL *)url
{
    BOOL fresh = !self.window;
    if (fresh)
        [self build];
    if (fresh || ![url isEqual:self.base])
        [self.web loadRequest:[NSURLRequest requestWithURL:url]];
    self.base = url;
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp activateIgnoringOtherApps:YES];
    [self selectTab:nil];
}

/* a new tab opens on the device page, to pick what it shows */
- (void)openTabIn:(NSMutableArray<PanelWindow *> *)group at:(NSRect)frame
{
    PanelWindow *t = [[PanelWindow alloc] init];
    t.notify = self.notify;
    t.base = self.base;
    t.group = group;
    [group addObject:t];
    [t build];
    [t.window setFrame:frame display:NO];
    NSURL *devices = [NSURL URLWithString:@"panel.html#devices" relativeToURL:self.base].absoluteURL;
    [t.web loadRequest:[NSURLRequest requestWithURL:devices ?: self.base]];
    [t selectTab:nil];
}

- (void)newTab:(id)sender { [self openTabIn:self.group at:self.window.frame]; }

- (void)newWindow:(id)sender
{
    NSRect f = NSOffsetRect(self.window.frame, 24, -24);
    [self openTabIn:[NSMutableArray array] at:f];
}

/* shows this tab: it takes the window frame of the tab that was showing, which is ordered out */
- (void)selectTab:(id)sender
{
    for (PanelWindow *p in self.group) {
        if (p == self)
            continue;
        if (p.window.visible || p.window.miniaturized) {
            [self.window setFrame:p.window.frame display:NO];
            [p.window orderOut:nil];
        }
        p.window.excludedFromWindowsMenu = YES;
    }
    self.window.excludedFromWindowsMenu = NO;
    if (self.window.miniaturized)
        [self.window deminiaturize:nil];
    [self.window makeKeyAndOrderFront:nil];
    [self.window makeFirstResponder:self.web];
    refresh_all_tabs();
}

- (void)closeTab:(id)sender { [self.window performClose:nil]; }

/* ⇧⌘] and ⇧⌘[ (the Window menu) */
- (void)selectNextTab:(id)sender { [self selectTabBy:1]; }
- (void)selectPreviousTab:(id)sender { [self selectTabBy:-1]; }
- (void)selectTabBy:(NSInteger)d
{
    NSInteger n = (NSInteger)self.group.count, i = (NSInteger)[self.group indexOfObjectIdenticalTo:self];
    if (n > 1 && i != NSNotFound)
        [self.group[(NSUInteger)(((i + d) % n + n) % n)] selectTab:nil];
}

- (void)windowDidBecomeKey:(NSNotification *)n
{
    [panels removeObjectIdenticalTo:self];
    [panels addObject:self];
}

- (void)windowDidResize:(NSNotification *)n { [self refreshTabs]; }

/* ---- the tab strip ---- */

- (NSArray<NSToolbarItemIdentifier> *)toolbarDefaultItemIdentifiers:(NSToolbar *)t
{
    return @[ @"RDName", @"RDTabs", NSToolbarFlexibleSpaceItemIdentifier ];
}

- (NSArray<NSToolbarItemIdentifier> *)toolbarAllowedItemIdentifiers:(NSToolbar *)t { return [self toolbarDefaultItemIdentifiers:t]; }

- (NSToolbarItem *)toolbar:(NSToolbar *)t itemForItemIdentifier:(NSToolbarItemIdentifier)ident willBeInsertedIntoToolbar:(BOOL)ins
{
    NSToolbarItem *it = [[NSToolbarItem alloc] initWithItemIdentifier:ident];
    BOOL name = [ident isEqual:@"RDName"];
    it.view = name ? self.name : self.tabs;
    it.label = name ? @"RemoDroid" : @"Tabs";
    it.visibilityPriority = NSToolbarItemVisibilityPriorityHigh;
    return it;
}

- (void)refreshTabs
{
    self.tabsPasses = 0;
    [self layoutTabs];
}

/* the strip takes the title bar's free room: what AppKit granted plus the gap left after it. A width
 * change moves the toolbar around, so it is measured again on the next pass, a bounded number of times. */
- (CGFloat)availableTabsWidth
{
    NSView *strip = self.tabs, *name = self.name;
    CGFloat whole = NSWidth(self.window.frame);
    if (!strip.window) {
        CGFloat start = name.window ? NSMaxX([name convertRect:name.bounds toView:nil]) + 12 : kTabsLeading;
        return MAX(kTabsMinWidth, whole - start - kTabsTrailing);
    }
    CGFloat end = NSMaxX([strip convertRect:strip.bounds toView:nil]);
    return MAX(kTabsMinWidth, NSWidth(strip.frame) + whole - end - kTabsTrailing);
}

- (void)layoutTabs
{
    if (!self.window || !self.tabs)
        return;
    [self.window layoutIfNeeded];
    CGFloat width = [self availableTabsWidth];
    if (fabs(width - self.tabsWidth.constant) > 1) {
        self.tabsWidth.constant = width;
        self.tabs.stripWidth = width;
        [self.window layoutIfNeeded];
        if (self.tabsPasses++ < 5)
            dispatch_async(dispatch_get_main_queue(), ^{ [self layoutTabs]; });
    }
    CGFloat granted = NSWidth(self.tabs.frame);
    if (granted > 1)
        width = MIN(width, granted);
    for (NSView *v in [self.tabs.subviews copy])
        [v removeFromSuperview];
    NSArray<PanelWindow *> *members = [self.group copy];
    NSUInteger count = members.count;
    if (!count)
        return;
    /* every tab gets an equal slice: more tabs squeeze rather than scroll, long titles end in "…" */
    CGFloat room = MAX(kTabNewWidth, width - kTabNewWidth - kTabGap);
    CGFloat slot = room / count;
    CGFloat gap = MIN(kTabGap, slot / 5);
    CGFloat cell = MIN(kTabMaxWidth, slot - gap);
    self.tabStep = cell + gap;
    BOOL closable = cell >= kTabCloseFloor;
    CGFloat x = 0;
    TabChip *lifted = nil;
    for (PanelWindow *p in members) {
        NSString *title = p.web.title.length ? p.web.title : @"New tab";
        if ([title hasSuffix:@" · RemoDroid"])
            title = [title substringToIndex:title.length - 12];
        /* the dragged tab follows the pointer; the others sit in the slots of the order so far */
        BOOL dragged = p == self && self.sliding;
        CGFloat at = dragged ? MAX(0, MIN(self.slideX, (count - 1) * self.tabStep)) : x;
        TabChip *chip = [[TabChip alloc] initWithFrame:NSMakeRect(at, 0, cell, kTabsHeight) owner:p title:title
                                              selected:p == self closable:closable];
        if (dragged)
            lifted = chip;
        else
            [self.tabs addSubview:chip];
        x += cell + gap;
    }
    if (lifted)
        [self.tabs addSubview:lifted]; /* above the tabs it passes */
    NSButton *add = [NSButton buttonWithImage:[NSImage imageWithSystemSymbolName:@"plus" accessibilityDescription:@"New tab"]
                                       target:self
                                       action:@selector(newTab:)];
    add.bordered = NO;
    add.toolTip = @"New tab";
    add.frame = NSMakeRect(MIN(x, width - kTabNewWidth), (kTabsHeight - kTabNewWidth) / 2, kTabNewWidth, kTabNewWidth);
    [self.tabs addSubview:add];
}

/* Chrome's tab drag. A press selects the tab; moving it sideways slides it along the strip and
 * reorders; pulling it off the strip tears it out into a window of its own that follows the pointer;
 * holding that over another window's strip joins that window, and the drag goes on as a slide there.
 * A window's only tab drags the whole window. All in screen points: each tab is its own window, and
 * which one is on screen (so which strip is under the pointer) changes during the drag. */
- (void)dragTab:(NSView *)chip from:(NSEvent *)down
{
    NSPoint start = NSEvent.mouseLocation;
    CGFloat grab = start.x - NSMinX(screen_rect(chip)); /* the pointer's offset into the tab */
    [self selectTab:nil];
    BOOL pinned = (self.window.styleMask & NSWindowStyleMaskFullScreen) != 0; /* full screen: reorder only */
    enum { PRESSED, SLIDING, CARRYING } phase = PRESSED;
    NSPoint anchor = NSZeroPoint; /* the pointer minus the carried window's origin */
    for (;;) {
        NSEvent *e = [NSApp nextEventMatchingMask:NSEventMaskLeftMouseDragged | NSEventMaskLeftMouseUp
                                        untilDate:NSDate.distantFuture
                                           inMode:NSEventTrackingRunLoopMode
                                          dequeue:YES];
        if (!e || e.type == NSEventTypeLeftMouseUp || !self.window)
            break;
        NSPoint p = NSEvent.mouseLocation;
        if (phase == PRESSED) {
            if (hypot(p.x - start.x, p.y - start.y) < kTabDragSlop)
                continue;
            if (self.group.count < 2 && pinned)
                continue;
            phase = self.group.count > 1 ? SLIDING : CARRYING;
            anchor = NSMakePoint(p.x - NSMinX(self.window.frame), p.y - NSMinY(self.window.frame));
        }
        if (phase == SLIDING) {
            NSRect strip = screen_rect(self.tabs);
            BOOL on = p.y > NSMinY(strip) - kTabTearDistance && p.y < NSMaxY(strip) + kTabTearDistance;
            if (on || pinned || self.group.count < 2) {
                [self slideTabTo:p.x - grab - NSMinX(strip)];
                continue;
            }
            anchor = [self tearOffAt:p grab:grab];
            phase = CARRYING;
        }
        [self.window setFrameOrigin:NSMakePoint(p.x - anchor.x, p.y - anchor.y)];
        PanelWindow *host = [self stripHostAt:p];
        if (host) {
            [self joinTabsOf:host at:p grab:grab];
            phase = SLIDING;
            [self slideTabTo:p.x - grab - NSMinX(screen_rect(self.tabs))];
        }
    }
    self.sliding = NO;
    refresh_all_tabs();
}

- (void)slideTabTo:(CGFloat)x
{
    NSMutableArray<PanelWindow *> *g = self.group;
    NSInteger last = (NSInteger)g.count - 1;
    CGFloat step = MAX(1, self.tabStep);
    x = MAX(0, MIN(x, last * step));
    NSInteger to = MAX(0, MIN(last, (NSInteger)floor(x / step + 0.5))); /* the slot its middle is over */
    NSUInteger from = [g indexOfObjectIdenticalTo:self];
    if (from != NSNotFound && (NSInteger)from != to) {
        [g removeObjectAtIndex:from];
        [g insertObject:self atIndex:(NSUInteger)to];
    }
    self.sliding = YES;
    self.slideX = x;
    [self layoutTabs];
}

/* Takes this tab out into a window of its own; its neighbour takes over the window it leaves. The new
 * window is placed so the tab, now first in its strip, stays under the pointer; returns the pointer's
 * offset from that window's origin, for carrying it. */
- (NSPoint)tearOffAt:(NSPoint)p grab:(CGFloat)grab
{
    NSMutableArray<PanelWindow *> *g = self.group;
    NSUInteger i = [g indexOfObjectIdenticalTo:self];
    if (i != NSNotFound)
        [g removeObjectAtIndex:i];
    PanelWindow *next = g.count ? g[MIN(i, g.count - 1)] : nil;
    self.group = [NSMutableArray arrayWithObject:self];
    self.sliding = NO;
    self.window.excludedFromWindowsMenu = NO;
    if (next) {
        [next.window setFrame:self.window.frame display:NO];
        [next.window orderWindow:NSWindowBelow relativeTo:self.window.windowNumber];
        next.window.excludedFromWindowsMenu = NO;
        [next refreshTabs];
    }
    [self refreshTabs];
    NSRect f = self.window.frame;
    for (NSView *v in self.tabs.subviews) {
        if (![v isKindOfClass:TabChip.class] || ((TabChip *)v).owner != self)
            continue;
        NSRect t = screen_rect(v);
        f.origin.x += p.x - grab - NSMinX(t);
        f.origin.y += p.y - NSMidY(t);
    }
    return NSMakePoint(p.x - NSMinX(f), p.y - NSMinY(f));
}

/* the other window whose strip is under the pointer, if no window of ours covers it there */
- (PanelWindow *)stripHostAt:(NSPoint)p
{
    for (NSWindow *w in NSApp.orderedWindows) {
        if (w == self.window || !w.visible || w.miniaturized || !NSPointInRect(p, w.frame))
            continue;
        if (w.styleMask & NSWindowStyleMaskFullScreen)
            return nil;
        for (PanelWindow *o in panels)
            if (o.window == w && [self.group indexOfObjectIdenticalTo:o] == NSNotFound && NSPointInRect(p, screen_rect(o.tabs)))
                return o;
        return nil;
    }
    return nil;
}

/* puts this tab into host's window, at the slot under the pointer, and shows it there */
- (void)joinTabsOf:(PanelWindow *)host at:(NSPoint)p grab:(CGFloat)grab
{
    NSMutableArray<PanelWindow *> *g = host.group;
    CGFloat step = host.tabStep > 1 ? host.tabStep : kTabMaxWidth;
    CGFloat x = p.x - grab - NSMinX(screen_rect(host.tabs));
    NSUInteger at = (NSUInteger)MAX(0, MIN((CGFloat)g.count, floor(x / step + 0.5)));
    [g insertObject:self atIndex:at];
    self.group = g;
    [self selectTab:nil];
}

- (void)observeValueForKeyPath:(NSString *)k ofObject:(id)o change:(NSDictionary *)ch context:(void *)ctx
{
    if (ctx != &title_kvo) {
        [super observeValueForKeyPath:k ofObject:o change:ch context:ctx];
        return;
    }
    NSString *t = self.web.title;
    self.window.title = t.length ? t : @"RemoDroid"; /* hidden in the bar, but the Window menu and Mission Control show it */
    refresh_all_tabs();
}

/* Closing a tab drops its page (and any live viewer session in it). Closing the one on screen shows the
 * tab that slides into its place, as in Chrome. */
- (void)windowWillClose:(NSNotification *)n
{
    BOOL shown = self.window.visible || self.window.miniaturized;
    NSRect frame = self.window.frame;
    NSUInteger i = [self.group indexOfObjectIdenticalTo:self];
    [self.group removeObjectIdenticalTo:self];
    PanelWindow *next = shown && self.group.count ? self.group[MIN(i, self.group.count - 1)] : nil;
    self.group = nil;
    [self.web removeObserver:self forKeyPath:@"title" context:&title_kvo];
    [self.web stopLoading];
    [self.web removeFromSuperview];
    self.web.navigationDelegate = nil;
    self.web.UIDelegate = nil;
    self.web = nil;
    self.window.delegate = nil;
    self.window = nil;
    self.tabs = nil;
    self.tabsWidth = nil;
    self.name = nil;
    [panels removeObjectIdenticalTo:self];
    if (next) {
        [next.window setFrame:frame display:NO];
        [next selectTab:nil];
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        refresh_all_tabs();
        if (!panels.count) /* back to the menu bar only, once the last window is gone */
            [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    });
}

/* ---- navigation: the panel's own pages here, everything else in the default browser ---- */

- (BOOL)samePanel:(NSURL *)u
{
    NSURL *b = self.base;
    return [u.host isEqual:b.host] && (u.port ?: @0).integerValue == (b.port ?: @0).integerValue;
}

- (void)webView:(WKWebView *)w decidePolicyForNavigationAction:(WKNavigationAction *)a
    decisionHandler:(void (^)(WKNavigationActionPolicy))h
{
    NSURL *u = a.request.URL;
    if (a.shouldPerformDownload) {
        h(WKNavigationActionPolicyDownload);
        return;
    }
    if ([u.scheme isEqual:@"about"] || [u.scheme isEqual:@"blob"] || [u.scheme isEqual:@"data"]) {
        h(WKNavigationActionPolicyAllow);
        return;
    }
    if (!is_web(u)) { /* mailto: links go to the mail app; other schemes go nowhere */
        if ([u.scheme isEqual:@"mailto"] && a.navigationType == WKNavigationTypeLinkActivated)
            [[NSWorkspace sharedWorkspace] openURL:u];
        h(WKNavigationActionPolicyCancel);
        return;
    }
    if (a.targetFrame.isMainFrame && a.navigationType == WKNavigationTypeLinkActivated && ![self samePanel:u]) {
        [[NSWorkspace sharedWorkspace] openURL:u];
        h(WKNavigationActionPolicyCancel);
        return;
    }
    h(WKNavigationActionPolicyAllow);
}

/* target="_blank" and window.open(): the panel's pages load here, others go to the browser */
- (WKWebView *)webView:(WKWebView *)w createWebViewWithConfiguration:(WKWebViewConfiguration *)c
    forNavigationAction:(WKNavigationAction *)a windowFeatures:(WKWindowFeatures *)f
{
    NSURL *u = a.request.URL;
    if (is_web(u) && [self samePanel:u])
        [w loadRequest:a.request];
    else if (is_web(u))
        [[NSWorkspace sharedWorkspace] openURL:u];
    return nil;
}

- (void)webView:(WKWebView *)w decidePolicyForNavigationResponse:(WKNavigationResponse *)r
    decisionHandler:(void (^)(WKNavigationResponsePolicy))h
{
    h(r.canShowMIMEType ? WKNavigationResponsePolicyAllow : WKNavigationResponsePolicyDownload);
}

- (void)webView:(WKWebView *)w didFailProvisionalNavigation:(WKNavigation *)n withError:(NSError *)e
{
    if (e.code == NSURLErrorCancelled || [e.domain isEqual:@"WebKitErrorDomain"])
        return; /* a navigation replaced by another, or one turned into a download */
    NSString *page = [NSString stringWithFormat:
        @"<!doctype html><meta name=viewport content='width=device-width'><title>RemoDroid</title>"
        @"<style>body{font:15px -apple-system,sans-serif;margin:15vh auto;max-width:34em;padding:0 16px;color:#333}"
        @"@media(prefers-color-scheme:dark){body{background:#1e1e1e;color:#ddd}}a{color:#0a84ff}</style>"
        @"<h2>Can't reach the panel</h2><p>%@</p><p><a href='%@'>Try again</a></p>",
        [[e.localizedDescription stringByReplacingOccurrencesOfString:@"&" withString:@"&amp;"] stringByReplacingOccurrencesOfString:@"<" withString:@"&lt;"],
        [self.base.absoluteString stringByReplacingOccurrencesOfString:@"'" withString:@"%27"]];
    [w loadHTMLString:page baseURL:nil];
}

- (void)webViewWebContentProcessDidTerminate:(WKWebView *)w { [w reload]; }

/* ---- the page's dialogs ---- */

- (void)webView:(WKWebView *)w runJavaScriptAlertPanelWithMessage:(NSString *)m initiatedByFrame:(WKFrameInfo *)f
    completionHandler:(void (^)(void))h
{
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = m;
    [a beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse r) { h(); }];
}

- (void)webView:(WKWebView *)w runJavaScriptConfirmPanelWithMessage:(NSString *)m initiatedByFrame:(WKFrameInfo *)f
    completionHandler:(void (^)(BOOL))h
{
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = m;
    [a addButtonWithTitle:@"OK"];
    [a addButtonWithTitle:@"Cancel"];
    [a beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse r) { h(r == NSAlertFirstButtonReturn); }];
}

- (void)webView:(WKWebView *)w runJavaScriptTextInputPanelWithPrompt:(NSString *)p defaultText:(NSString *)d
    initiatedByFrame:(WKFrameInfo *)f completionHandler:(void (^)(NSString *))h
{
    NSAlert *a = [[NSAlert alloc] init];
    a.messageText = p;
    NSTextField *t = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 300, 24)];
    t.stringValue = d ?: @"";
    a.accessoryView = t;
    [a addButtonWithTitle:@"OK"];
    [a addButtonWithTitle:@"Cancel"];
    a.window.initialFirstResponder = t;
    [a beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse r) {
        h(r == NSAlertFirstButtonReturn ? t.stringValue : nil);
    }];
}

/* <input type=file>: files to upload to a device */
- (void)webView:(WKWebView *)w runOpenPanelWithParameters:(WKOpenPanelParameters *)p initiatedByFrame:(WKFrameInfo *)f
    completionHandler:(void (^)(NSArray<NSURL *> *))h
{
    NSOpenPanel *o = [NSOpenPanel openPanel];
    o.allowsMultipleSelection = p.allowsMultipleSelection;
    o.canChooseDirectories = p.allowsDirectories;
    [o beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse r) { h(r == NSModalResponseOK ? o.URLs : nil); }];
}

/* ---- downloads: straight to ~/Downloads, as a browser does ---- */

- (void)webView:(WKWebView *)w navigationAction:(WKNavigationAction *)a didBecomeDownload:(WKDownload *)d { d.delegate = self; }
- (void)webView:(WKWebView *)w navigationResponse:(WKNavigationResponse *)r didBecomeDownload:(WKDownload *)d { d.delegate = self; }

- (void)download:(WKDownload *)d decideDestinationUsingResponse:(NSURLResponse *)r suggestedFilename:(NSString *)name
    completionHandler:(void (^)(NSURL *))h
{
    if (!self.saving)
        self.saving = [NSMapTable strongToStrongObjectsMapTable];
    NSURL *u = download_path(name);
    [self.saving setObject:u forKey:d];
    h(u);
}

- (void)downloadDidFinish:(WKDownload *)d
{
    NSURL *u = [self.saving objectForKey:d];
    [self.saving removeObjectForKey:d];
    if (u && self.notify)
        self.notify([NSString stringWithFormat:@"%@ is in Downloads.", u.lastPathComponent]);
}

- (void)download:(WKDownload *)d didFailWithError:(NSError *)e resumeData:(NSData *)data
{
    [self.saving removeObjectForKey:d];
    if (self.notify)
        self.notify([@"Download failed: " stringByAppendingString:e.localizedDescription]);
}

/* ---- View menu actions: nil-targeted, they reach this object as the panel window's delegate ---- */

- (void)zoomIn:(id)sender { self.web.pageZoom = MIN(3, self.web.pageZoom + 0.1); }
- (void)zoomOut:(id)sender { self.web.pageZoom = MAX(0.5, self.web.pageZoom - 0.1); }
- (void)zoomReset:(id)sender { self.web.pageZoom = 1; }

- (void)openInBrowser:(id)sender
{
    NSURL *u = is_web(self.web.URL) ? self.web.URL : self.base;
    if (u)
        [[NSWorkspace sharedWorkspace] openURL:u];
}

@end

void rd_web_show(const char *url, void (^notify)(NSString *))
{
    NSURL *u = [NSURL URLWithString:[NSString stringWithUTF8String:url ?: ""]];
    if (!is_web(u))
        return;
    PanelWindow *p = panels.lastObject ?: [[PanelWindow alloc] init];
    p.notify = notify;
    [p show:u];
}

int rd_web_reopen(void)
{
    PanelWindow *p = panels.lastObject;
    if (!p)
        return 0;
    [p show:p.base];
    return 1;
}

/* the View menu: Reload, Back, Forward, zoom, Open in Browser — acting on the panel window */
NSMenu *rd_web_menu(void)
{
    NSMenu *m = [[NSMenu alloc] initWithTitle:@"View"];
    [m addItemWithTitle:@"Reload Page" action:@selector(reload:) keyEquivalent:@"r"];
    [m addItemWithTitle:@"Back" action:@selector(goBack:) keyEquivalent:@"["];
    [m addItemWithTitle:@"Forward" action:@selector(goForward:) keyEquivalent:@"]"];
    [m addItem:[NSMenuItem separatorItem]];
    [m addItemWithTitle:@"Actual Size" action:@selector(zoomReset:) keyEquivalent:@"0"];
    [m addItemWithTitle:@"Zoom In" action:@selector(zoomIn:) keyEquivalent:@"="];
    [m addItemWithTitle:@"Zoom Out" action:@selector(zoomOut:) keyEquivalent:@"-"];
    [m addItem:[NSMenuItem separatorItem]];
    [m addItemWithTitle:@"Open in Browser" action:@selector(openInBrowser:) keyEquivalent:@""];
    [m addItemWithTitle:@"Enter Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"].keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagControl;
    return m;
}
