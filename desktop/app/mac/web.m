/* The panel in a window of its own: "Open window" in the menu bar menu shows the panel (device list,
 * viewer) in a WKWebView instead of the default browser, like a small single-site browser.
 *
 * The sign-in cookie has a Max-Age, so the default (persistent) website data store keeps it across
 * launches. While the window is open the app is a regular app (Dock icon, ⌘-Tab, its menus); when it
 * closes, the app goes back to the menu bar only. Links to other sites open in the default browser;
 * files the viewer downloads (files sent from a device) land in ~/Downloads. */
#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>

#include "web.h"

static BOOL is_web(NSURL *u)
{
    NSString *s = u.scheme.lowercaseString;
    return [s isEqual:@"http"] || [s isEqual:@"https"];
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

@interface PanelWindow : NSObject <NSWindowDelegate, WKNavigationDelegate, WKUIDelegate, WKDownloadDelegate>
@property(strong) NSWindow *window;
@property(strong) WKWebView *web;
@property(strong) NSURL *base;
@property(copy) void (^notify)(NSString *);
@property(strong) NSMapTable<WKDownload *, NSURL *> *saving; /* download → where it lands */
@end

static PanelWindow *panel;
static char title_kvo;

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
    w.tabbingMode = NSWindowTabbingModeDisallowed;
    w.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
    [w center];
    [w setFrameAutosaveName:@"RemoDroidPanel"];

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
    if (self.window.miniaturized)
        [self.window deminiaturize:nil];
    [self.window makeKeyAndOrderFront:nil];
    [self.window makeFirstResponder:self.web];
}

- (void)observeValueForKeyPath:(NSString *)k ofObject:(id)o change:(NSDictionary *)ch context:(void *)ctx
{
    if (ctx != &title_kvo) {
        [super observeValueForKeyPath:k ofObject:o change:ch context:ctx];
        return;
    }
    NSString *t = self.web.title;
    self.window.title = t.length ? t : @"RemoDroid";
}

/* Closing the window drops the page (and any live viewer session in it); the next "Open window" starts afresh. */
- (void)windowWillClose:(NSNotification *)n
{
    [self.web removeObserver:self forKeyPath:@"title" context:&title_kvo];
    [self.web stopLoading];
    [self.web removeFromSuperview];
    self.web.navigationDelegate = nil;
    self.web.UIDelegate = nil;
    self.web = nil;
    self.window.delegate = nil;
    self.window = nil;
    self.base = nil;
    /* back to the menu bar only, once the window is gone */
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!panel.window)
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
    if (!panel)
        panel = [[PanelWindow alloc] init];
    panel.notify = notify;
    [panel show:u];
}

int rd_web_reopen(void)
{
    if (!panel.window)
        return 0;
    [panel show:panel.base];
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
