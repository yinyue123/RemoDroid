# Page mode — send the page's elements, not a picture of them (plan, for review)

Contents

1. What you asked for, and what this plan does
2. Why the video costs what it costs
3. The idea in one picture
4. Puppeteer, and what is used instead
5. Reading the page: a snapshot, then the differences
6. Styles, pictures and fonts
7. What can't be sent as elements
8. Your finger: from the iPhone to the real page
9. The bar: address, back, forward, users, cookies
10. The viewer
11. The pieces and the wire format
12. What changes, by file
13. What it does to privacy and to detection
14. Failures and limits
15. Decisions for you
16. Tests
17. Order of work

---

## 1. What you asked for, and what this plan does

The remote browser today sends **a picture**: Xorg → H.265 → WebRTC → the iPhone. Every scroll is new
video, so it uses data whenever anything moves, and it is never as smooth as a page in Safari itself.

This plan adds a second way to look at the same browser, **Page mode**, and keeps the first one
(**Video mode**) exactly as it is:

| you asked | in this plan |
|---|---|
| get the elements in the page | a recorder script in the page reads the DOM and the CSS (§5, §6) |
| compare the differences, like React | the browser itself reports each change (`MutationObserver`); only the changes are sent, as a patch list by node id — what React's reconciler produces, without having to compare two trees (§5) |
| transfer them to my phone or computer browser | a new WebRTC data channel, `dom`; the viewer rebuilds the page from it (§10, §11) |
| transfer my operations back | taps, scrolls, typing and selects go back by **element**, and land on the real page as real touches and keys (§8) |
| address bar, back, forward, clear cookies, select user | the bar becomes real HTML in the viewer, driven by rd-shell's own state and functions (§9) |
| keep the H.265 transfer | untouched; a switch in the viewer, and Page mode falls back to it by itself where it can't work (§7) |
| JavaScript runs on the browser machine | yes: scripts are removed before sending, and the viewer's frame can't run any (§10, §13) |
| a picture is captured, compressed, then sent | the browser machine takes it from Chrome's own cache, shrinks it to the size shown, re-encodes it as WebP, sends it once (§6) |

What stays true: the site's code never runs on the iPhone, the site never sees the iPhone, and the
iPhone never connects to the site — not for a picture, not for a font.

## 2. Why the video costs what it costs

```
 Video mode                                   Page mode
 ──────────                                   ─────────
 a scroll  = 30 new frames a second           a scroll  = Safari moves what it already has: 0 bytes down,
             of blurred, then re-sharpened                a few bytes up ("I am at this element now")
             text, for as long as it moves
 idle page = nothing (XDamage)                idle page = nothing
 a new page = a burst of key-ish frames       a new page = its HTML + CSS, deflated (tens to hundreds of
                                                           KB once) + its pictures at the size shown
 latency   = encode + network + decode        latency   = none for scrolling, typing, pressing; the
             on every touch                               network only for what the page's JS answers
```

No numbers are promised here; step 1 of §17 measures both modes on the same five sites.

## 3. The idea in one picture

```
 iPhone / computer viewer (web/app.js)                    browser machine (the hqnode container)
┌───────────────────────────────────────┐                ┌──────────────────────────────────────────────┐
│ bar: ◀ ▶ ⟳  address   ☆  ⋯   (HTML)   │                │ rd-shell   bar + WM + supervisor             │
│┌─────────────────────────────────────┐│   bar state    │   owns the DevTools pipe ───────────┐        │
││ <iframe sandbox>  no scripts, no    ││◀───────────────│   new: /run/rd-shell.sock           │        │
││ network (CSP)                       ││   bar commands │      ▲ bar state/commands, CDP relay│        │
││                                     ││───────────────▶│      ▼                              ▼        │
││  the page's elements, rebuilt       ││                │ rd-mirror (new)               CloakBrowser   │
││  from the snapshot, patched by      ││  snapshot,     │   attaches to the tab in front   real page,  │
││  each difference                    ││  differences,  │   recorder in an isolated world  real JS,    │
││                                     ││  pictures      │   pictures: cache → resize →     real cookies│
││  scrolls natively                   ││◀───────────────│   WebP                                       │
││  inputs take the iOS keyboard       ││                │      ▲ /run/rd-mirror.sock                   │
│└─────────────────────────────────────┘│  tap on node   │      ▼                                       │
│  [Page | Video] switch                │  N, scroll to  │ remodroid-desktopd                           │
└───────────────────────────────────────┘  node M, text  │   "dom" channel ⇄ socket (relays bytes)      │
              ▲            │              ─────────────▶ │   taps → inputtest (real touch), text → XTest│
              │            └── WebRTC: dom (new), input, │   video: encoder parked in Page mode         │
              └─────────────── ctrl, audio, video ───────│   audio: stays on                            │
                                                         └──────────────────────────────────────────────┘
```

The real browser keeps running the real page at its real size the whole time. Page mode only changes
what is sent, so switching to Video mode is instant and shows the same page in the same state.

## 4. Puppeteer, and what is used instead

Puppeteer is a Node.js library that speaks **DevTools (CDP)** to Chrome. ★ This plan speaks the same
DevTools calls directly, from a small C program, and does not ship Puppeteer. Three reasons:

| | Puppeteer | DevTools directly (★) |
|---|---|---|
| detection | sends `Runtime.enable` to every page it attaches to. That is the call bot checks detect (youtube-stall.md §4), and it can't be turned off without patching Puppeteer | sends only the calls listed in §5; no `Runtime.enable`, no `Console`, no `Debugger` |
| the pipe | Chrome's `--remote-debugging-pipe` takes one client, and rd-shell is it. Puppeteer would need a relay and a custom transport anyway | rd-mirror uses the relay as it is |
| memory | Node + Puppeteer ≈ 80–120 MB on a 2 GB box that already swaps under Chrome | a C program, a few MB |

Everything you would have used Puppeteer for (find the page, run a script in it, read its resources,
follow navigations) is in the plan. It is D1 in §15 if you want Puppeteer itself anyway.

## 5. Reading the page: a snapshot, then the differences

**rd-mirror** attaches to **the tab in front only** (rd-shell already knows which), and only while a
viewer is in Page mode. Its calls:

```
 Target.attachToTarget {targetId, flatten:true}              → sessionId
 Page.enable                                                 → frameNavigated, lifecycle, dialogs
 on every new document (Page.frameNavigated, main frame):
   Page.createIsolatedWorld {frameId, worldName:"rd"}        → contextId
   Runtime.evaluate {contextId, expression: recorder}        installs the recorder
 loop:
   Runtime.evaluate {contextId, "rd.next()", awaitPromise}   resolves when the recorder has something:
                                                             a snapshot, or a batch of differences
 Page.getResourceTree / Page.getResourceContent              pictures, fonts and CSS from Chrome's cache (§6)
 Page.captureScreenshot {clip}                               what can't be sent as elements (§7)
 Page.handleJavaScriptDialog                                 alert / confirm / prompt (§7)
```

- The **isolated world** is what an extension's content script gets: the same DOM, separate JavaScript.
  The page can't see the recorder, its observer or its timers. The mechanism is the one youtube-stall.md
  proposes and tested (`createIsolatedWorld` + `Runtime.evaluate` work without `Runtime.enable`;
  `addScriptToEvaluateOnNewDocument` and `Runtime.addBinding` do not).
- `rd.next()` returns a promise, so one `Runtime.evaluate` waits inside Chrome until there is news: no
  polling, no binding. If `awaitPromise` turns out to need `Runtime.enable` (step 0 tests it), the fallback
  is asking every 50 ms.

**The recorder** (JavaScript, about 600 lines, `browser/rd-mirror/recorder.js`):

```
 1. snapshot     walk the document; give every node a number; emit
                   {id, tag, attrs, children} / {id, text}
                 leaving out: <script>, <noscript>, on* attributes, javascript: URLs, <base>,
                 <meta http-equiv>; and taking the live state, not the markup:
                 input value, checked, selected, scroll positions, open <details>/<dialog>
 2. differences  one MutationObserver on the document (childList, attributes, characterData, subtree),
                 plus one per open shadow root. Collected, then flushed once per frame as a batch:
                   add    {parent, before, node…}      a new subtree, numbered as in 1
                   remove {id}
                   attr   {id, name, value}
                   text   {id, value}
 3. what the observer can't see — these are compared, React-style, old against new:
                   form values, checked, selected       on input/change events + a 250 ms check
                   scroll positions                     scroll events, one per frame
                   CSS rules added from JS              a count + hash per stylesheet, each 250 ms (§6)
                   the size of the window               resize
```

A batch that adds and removes the same node drops both. A style attribute that changes every frame (a
JS animation) is sent at most 20 times a second, last value wins. A document over the limits of §14 is not
mirrored: the viewer goes to Video mode.

**Why not compare two trees, as React does?** React has to, because it is handed a whole new tree on
every render and must find what changed. A live page reports each change as it happens. The result is
the same thing React's reconciler produces — a list of small patches addressed to nodes — at none of the
cost, which matters on four A55 cores. The comparing is kept for what the page doesn't report (3 above).

## 6. Styles, pictures and fonts

The iPhone must never fetch from the site (it would show the site its own address). So every URL in the
snapshot is turned into a **resource reference**, and the bytes come from the browser machine.

| what | how it travels |
|---|---|
| `<style>` | its rules as text, from `sheet.cssRules` (so rules added by `insertRule` — styled-components, emotion — are there too, and changes are found by the 250 ms compare) |
| `<link rel=stylesheet>` | sent as a `<style>` with the same rules. A cross-origin sheet whose rules the script may not read: rd-mirror takes its text with `Page.getResourceContent` and follows its `@import`s the same way |
| `adoptedStyleSheets`, shadow roots | the same, attached to the mirror's document or shadow root |
| `url(…)` inside CSS, `<img>`, `srcset`, `<picture>`, `poster`, favicons | the address Chrome actually chose (`currentSrc`) → a reference `r:<n>`; the viewer asks for it when it needs it |
| fonts (`@font-face`) | a reference too; sent as they are (already compressed) |
| the element's computed style | **not sent.** The rules are sent and Safari computes them. Sending computed styles would be exact but 10–50× the size |

**A resource** is served only from what the real page already loaded (`Page.getResourceTree` lists it,
`Page.getResourceContent` returns it from Chrome's cache). rd-mirror never makes Chrome fetch something
the page didn't; the site sees no request it wouldn't have seen.

**Pictures** go through this before they are sent (a worker thread in rd-mirror):

```
 from Chrome's cache ─▶ SVG, or under 20 KB, or animated under 500 KB ──────────────────▶ as it is
                    └▶ decode (JPEG, PNG, GIF: stb_image; WebP: libwebp; AVIF: as it is)
                        └▶ shrink to the size it is shown at × the viewer's pixel ratio (never enlarge)
                            └▶ WebP, quality 75 (D6) ─▶ smaller than the original? send it : send the original
```

- Only pictures within two screens of what is visible are sent ahead; the rest when they come near
  (`IntersectionObserver` in the recorder — the page's own lazy loading keeps working, because the real
  page is scrolled too, §8).
- Each is sent once per session, under a hash of its bytes; the viewer keeps them in memory only, and
  drops them when the session ends. rd-mirror keeps the converted ones in the tmpfs cache
  (`/tmp/rd-cache`), which is already emptied when the user changes.

## 7. What can't be sent as elements

| thing | what Page mode does |
|---|---|
| `<video>` | its poster or a screenshot of its box, with a ▶. When it **plays**, the viewer goes to Video mode by itself and comes back when it pauses or ends (D5). Sound is the WebRTC audio track in both modes, so music keeps playing in Page mode |
| `<canvas>`, WebGL (maps, games, charts) | a screenshot of its box (`Page.captureScreenshot` with a clip, WebP), again when it is touched and at most once a second while visible. A canvas over half the screen → Video mode |
| a frame from another site (ads, embedded players, captchas, payment forms) | step 5: its own DevTools session and recorder, nested in the mirror. Until then, a screenshot of its box; a tap on it → Video mode |
| `alert` / `confirm` / `prompt` | `Page.javascriptDialogOpening` → a dialog in the viewer → `Page.handleJavaScriptDialog` |
| Chrome's own windows: permission prompts, HTTP sign-in, the `<select>` list, date pickers, downloads, "Leave site?" | rd-shell is the window manager and sees every extra window appear. While one is up → Video mode. (`<select>` and date fields don't open one in Page mode: the iPhone's own picker is used, §8) |
| rd-shell's own pages (Users, History, Favourites) | real HTML in the viewer (§9) |
| a page the mirror gets visibly wrong | the **Video** switch, always in the bar. Per site, remembered (D8) |

Text will not break at the same places as on the real page: Safari and its fonts are not Chrome and its
fonts, and the site's web fonts arrive a moment later. That is why operations go by element and never by
coordinates (§8).

## 8. Your finger: from the iPhone to the real page

The mirror has no JavaScript, so nothing in it reacts by itself except what the browser does natively:
scrolling, `:hover`/`:active`, focus, the keyboard, CSS animations. Everything else has to happen on the
real page and come back as differences.

```
 in the viewer                               sent                         on the browser machine
 ─────────────                               ────                         ──────────────────────
 tap                 ── element under the  ─▶ tap {node, fx, fy} ──▶ recorder: is it on screen? if not,
                        finger, and where                              scroll it in; its rectangle now →
                        in its box (0…1)                               screen point ─▶ desktopd ─▶ inputtest:
                                                                       a real touch down/up, as today
 long press, drag,   ── the same for the   ─▶ touch {node, fx, fy,  ─▶ the first point as above; the moves by
 swipe on a slider,     first point, then     then dx, dy…}            the same distances; real multi-touch
 pinch                  the moves
 scroll (native,     ── once a frame: the  ─▶ view {scroller, anchor, ─▶ recorder scrolls the same scroller so
 smooth, no wait)       topmost visible       offset, moving}          the anchor sits at the same offset (D3).
                        element and its                                 The page's lazy loading and "load more"
                        offset ("Where you                              fire; what they add comes back as
                        are looking", below)                            differences
 typing in a field   ── the iOS keyboard,  ─▶ focus {node}, then    ─▶ a real tap on the field, then the
                        natively, in the      text / key               existing paths: ctrl `text` → XTest,
                        mirror's own field                             0x04 keys (Backspace, Enter, arrows)
 <select>, date,     ── the iPhone's own   ─▶ set {node, value}     ─▶ recorder sets the value and fires
 colour, range          picker                                         input + change (D4)
 a link, a button    ── a tap, as above; the mirror's own navigation and form submit are switched off
```

### Where you are looking is always sent

A page only renders what its own window shows: lazy pictures, endless lists, virtual lists (only the rows
on screen exist as elements), "load more" at the bottom. So the real page's window has to be where
yours is, all the time, not only after a scroll ends. Without that the mirror would scroll into empty
space.

```
 viewer ─▶ real page   view {scroller, anchor node, offset in it, moving: up | down | no, speed}
                       · once per frame while the mirror moves (finger down or coasting), and when it stops
                       · for the page itself and for every scroller inside it that moved
                       · again after each snapshot, each rotation or resize, and when Page mode is switched on
 real page             the recorder puts the same scroller at the same anchor and offset (D3)
                       · a scroll that is still moving is led: the real page is put up to one screen ahead
                         in the direction of travel, so what comes next is rendered before you reach it
                       · when it stops, the real page is put exactly where you are
 real page ─▶ viewer   what the page then adds comes as differences, pictures near the new place first
```

- **The same window.** The mirror's frame has the real window's size in CSS pixels (§10), so "what is on
  my screen" and "what the real page has rendered" are the same area. A viewer of another shape (a
  computer, a rotated phone) asks for the screen that fits it, as Desktop mode does today.
- **The end of the page.** The mirror can't scroll past what the real page has made. At the bottom of an
  endless list you reach the end, the real page arrives there too, loads more, and the mirror grows: the
  same short wait as in Safari itself.
- **A virtual list** removes the rows above as it adds rows below. Those removals arrive as differences
  while you scroll; the anchor is always a row that is on your screen, so the place holds. A very fast
  fling can outrun the real page for a moment: empty rows that fill in, as such lists do natively.
- **When the real page moves by itself** (a "back to top" button, an anchor link, a page that scrolls on
  load), its position comes back and the mirror follows — unless your finger is down or the frame is
  still coasting; then yours wins and is sent again.
- Video mode shows the real window, so after a switch you are looking at the same place.

- **A tap or a scroll, which is it?** The viewer waits for the first 8 px of movement. If the mirror's
  frame starts to scroll (there is a scroller in that direction under the finger), it is a scroll and no
  touch is sent. If nothing scrolls, the gesture is sent as a touch from its first point: a carousel, a
  map, a slider, pull-to-refresh handled by the page.
- **The typed text** shows at once in the mirror's field. The real field's value comes back as a
  difference; it replaces the mirror's only if it is still different after 300 ms without a key (a page
  that reformats what you type: a card number, a phone number).
- **A mouse** on a computer viewer: moves are sent as `hover {node, fx, fy}` at most 20 a second, clicks
  and the wheel like taps and scrolls.
- **Where the real touch goes:** rd-mirror hands the screen point to desktopd, which sends it to Xorg's
  `inputtest` device as it does today. The page gets the same trusted touch events as in Video mode.

## 9. The bar: address, back, forward, users, cookies

In Video mode the bar is drawn by rd-shell on the X screen and is part of the picture. In Page mode there
is no picture, so the viewer draws the same bar in HTML and rd-shell does the work, with the functions
it already has:

```
 rd-shell ─▶ viewer   bar {url, title, loading, fav, user, users[], tabs[{id, title, url, front}],
                           proxy, sound, desktop}           on every change, and once at the start
                      list {kind: history | favourites, items[]}        when asked
 viewer ─▶ rd-shell   go {url | text}      → browser_open (a search if it isn't an address)
                      back / forward / reload               → the XTest keys, as today
                      home / tab.new / tab.select / tab.close
                      user {name}                           → user_switch
                      fav.toggle / fav.delete / list {kind}
                      clear.cookies / clear.all / identity.new   (asked twice in the viewer first)
                      restart / stop / start
```

The address field is a real `<input>`: the iOS keyboard, paste, autocorrect off, the favourites and the
history as suggestions while typing. The menu, Users, History and Favourites are sheets in the viewer.

The X bar stays where it is, so the real page keeps its size and Video mode shows what it always did.

## 10. The viewer

`web/app.js`, for a device whose `session.ready` says `caps.mirror`:

- A **Page | Video** switch. Page mode sends `mirror.on`; the agent parks the encoder (no frames, no
  bitrate) and keeps the audio. Video mode sends `mirror.off`; the agent sends a key frame.
- The page lives in `<iframe sandbox="allow-same-origin">` — **no `allow-scripts`, no `allow-forms`, no
  navigation** — holding a document with `Content-Security-Policy: default-src 'none'; style-src
  'unsafe-inline'; img-src blob: data:; font-src blob: data:; media-src 'none'`. So even if a script or a
  URL slipped past the recorder, Safari would neither run it nor fetch it.
- The frame is laid out at **the real page's size in CSS pixels** (440 × 860 on the OECT's `light`
  screen, so the site's phone layout and its media queries choose the same rules), then scaled to the
  viewer's width. On an iPhone 14 Plus that is 428/440: 3% smaller than Safari would draw it.
- The **applier** (about 400 lines, `web/mirror.js`): a map from node number to element; builds the
  snapshot with `createElementNS`, applies each difference, swaps `r:<n>` for a `blob:` URL when the
  resource arrives, and keeps the frame's scroll where the user put it (the real page's scroll events are
  ignored while a finger is down or the frame is still coasting).
- The applier's own events (`pointerdown`, `scroll`, `beforeinput`, `change`, `focusin`) are listened to
  from outside the frame, which `allow-same-origin` permits.

## 11. The pieces and the wire format

```
 viewer ⇄ desktopd          WebRTC data channel "dom" (reliable, ordered), opened by the agent in Page mode
 desktopd ⇄ rd-mirror       /run/rd-mirror.sock   (desktopd only relays; it doesn't read the messages,
                                                   except `point`, which it turns into touches)
 rd-mirror ⇄ rd-shell       /run/rd-shell.sock    bar state and commands; DevTools messages relayed to
                                                   the pipe for rd-mirror's own sessions only
```

A message: 1 byte kind, 4 bytes length, then the body; over 60 KB it is split, as the `file` channel
does (the stack's limit is 64 KB a message).

| kind | body | direction |
|---|---|---|
| `J` | JSON, raw-deflate (`DecompressionStream` in the viewer, zlib in rd-mirror): `doc` (snapshot, viewport, url), `diff` (a batch), `bar`, `list`, `dialog`, `mode` (go to Video / come back, and why), `res.none` | → viewer |
| `R` | a resource: its number, hash, MIME type, bytes | → viewer |
| `j` | JSON, as it is: `tap`, `touch`, `view`, `hover`, `focus`, `set`, `res.get`, `dialog.answer`, `viewport` (pixel ratio, size), the bar commands | → rd-mirror |

Text and keys keep using the `ctrl` and `input` channels they use today.

## 12. What changes, by file

| file | change |
|---|---|
| `browser/rd-mirror/rd-mirror.c` (new, C, libev + cJSON + zlib + libwebp + stb_image) | the DevTools sessions, the long poll, resources and pictures, the two sockets |
| `browser/rd-mirror/recorder.js` (new) | §5; built into rd-mirror as a string |
| `browser/rd-shell/rd-shell.c` | `/run/rd-shell.sock`: the bar's state out, its commands in (calling the functions the X bar calls), the DevTools relay (messages with rd-mirror's session ids and its own id range), "an extra window is up". `cdp_send` gets an optional `sessionId`. The header comment's rule becomes: nothing attaches to a page except rd-mirror, to the tab in front, while a viewer is in Page mode |
| `desktop/core/session.c`, `agent.c`, `video.c` | `[mirror] socket = /run/rd-mirror.sock`; `caps.mirror` in `session.ready`; `mirror.on` / `mirror.off`; the `dom` channel ⇄ the socket; the encoder parked and woken; `point` → `dsk_touch` |
| `web/mirror.js` (new), `web/app.js`, `web/app.css`, `web/index.html` | §10, §9 |
| `browser/hqnode/Dockerfile`, `units/rd-mirror.service`, `browser.mk` | build and run it, as `browser`; `libwebp7`, `zlib1g` in the image |
| `browser/hqnode/browser.conf.example`, `desktop.ini.example` | `page_mode = 1`, the limits of §14; `[mirror] socket` |
| `browser/remote-browser-plan.md`, `arch.md` | a section each, pointing here |
| `tests/e2e_mirror.py` (new) | §16 |

The image ships from the browser-image Action, as always. The server and the panel don't change: the
`dom` channel is inside the WebRTC session they already set up.

## 13. What it does to privacy and to detection

| | Video mode | Page mode |
|---|---|---|
| the site's JavaScript on the iPhone | never | never: removed by the recorder, and the frame can't run scripts (two locks) |
| the iPhone connects to the site | never | never: every URL becomes a reference; CSP `default-src 'none'` (two locks) |
| the site's HTML and CSS reach Safari | no, only pixels | **yes.** Safari parses markup and styles a hostile site wrote. A bug in Safari's HTML/CSS/font/image code is reachable in Page mode and isn't in Video mode. Use Video mode for sites you don't trust at all (D8 makes that a per-site memory) |
| a DevTools session on the page | none | one, on the tab in front, while Page mode is on; the recorder in an isolated world; no `Runtime.enable`. T7 checks what a detection page can see |
| what the page can notice | nothing | scrolls that arrive without touch events before them (D3), and `<select>` changes that aren't trusted (D4). Taps, drags and typing are as real as in Video mode |
| what stays on the iPhone | nothing | nothing after the session: the page and its pictures are in memory only |

## 14. Failures and limits

| case | what happens |
|---|---|
| a huge page (over 20 000 nodes or a 3 MB snapshot, `page_max_nodes`, `page_max_kb`) | not mirrored: Video mode, with a line saying why |
| a page that changes everything all the time (a ticker, a game in DOM) | over 200 KB/s of differences for 5 s → Video mode |
| the data channel is slower than the page | differences queue in rd-mirror; over 2 MB queued, the queue is dropped and a fresh snapshot is sent |
| a new document (navigation, reload) | the old mirror stays, dimmed, until the new snapshot arrives; the bar shows loading |
| the tab in front changes | detach from the old tab, attach to the new one, snapshot |
| two viewers | the second gets its own snapshot and the same differences; both drive the same page, as two viewers of the video do today |
| the layout differs (fonts, Safari's own form controls) | accepted; operations go by element. A page that draws by measuring itself in JS (a virtual list, a canvas-sized layout) uses the real page's measurements and can look cramped or loose |
| `position: fixed` headers, sticky bars | work: the frame is its own viewport |
| CSS `:hover` menus | open on a tap in the mirror by themselves; menus opened by JS open when the real page answers |
| the recorder costs the page time | one observer and a walk per change; measured in step 1 on the OECT. If a site suffers: Video mode |
| rd-mirror dies | systemd restarts it; the viewer shows Video mode meanwhile |
| the browser restarts (user switch, New identity) | sessions go with the pipe; rd-mirror attaches again when the tabs are back |
| iOS older than 16.4 | no `DecompressionStream`: Page mode isn't offered |

## 15. Decisions for you

The first option is my recommendation (★).

| # | question | ★ | other |
|---|---|---|---|
| D1 | Puppeteer | DevTools calls directly from a C program, no `Runtime.enable` | **Puppeteer (Node) as you said**: quicker to write, but ~100 MB more memory on the OECT, a custom transport through rd-shell's relay, and a `Runtime.enable` on every page unless Puppeteer is patched |
| D2 | the recorder | our own, small, in an isolated world, made for this (references instead of URLs, no scripts, CSS rules read from the sheets) | **rrweb** (MIT, the library session-replay products use): more cases handled on day one, but it finds CSS-in-JS rules by patching `insertRule`, which doesn't work from an isolated world, and its player is built to replay a recording, not to be operated |
| D3 | how the real page is scrolled | the recorder sets the scroll position (exact, and what keeps the two in step) | wheel events through DevTools (`Input.dispatchMouseEvent`): the page sees a real wheel, but smooth-scrolling pages drift and need correcting |
| D4 | `<select>`, date, colour | the iPhone's own picker; the recorder sets the value and fires `input`/`change` (not trusted events) | Video mode whenever one opens: trusted, but it is the kiosk's small list in a picture |
| D5 | a playing `<video>` | Video mode by itself while it plays, back to Page mode after | stay in Page mode with a still picture and the sound; a button to watch |
| D6 | pictures | WebP quality 75 at the size shown × the viewer's pixel ratio | quality 60 and at most 2× (about half the bytes; softer on a 3× iPhone) |
| D7 | which mode a session starts in | the one this viewer used last for this device; the first time, Video | always Page mode where it is offered |
| D8 | remembering sites | a per-site "always Video" list, kept in the viewer's local storage | none: the switch only |
| D9 | frames from other sites | step 5, after the rest works; screenshots until then | in the first version: more to get right before anything ships |
| D10 | which devices | the remote browser containers only (they have rd-shell and the DevTools pipe) | nothing else has a page to mirror; the phones and desktops stay video |

## 16. Tests

On the amd64 container on this PC first (steamdebain-browser), then on browser-oect — the A55 cores are
the ones that matter.

```
 T0 the calls work      before anything else, by hand over the pipe: createIsolatedWorld + Runtime.evaluate with
                        awaitPromise and no Runtime.enable; Page.getResourceContent for a cached picture, a
                        cross-origin stylesheet and a font; captureScreenshot with a clip in kiosk
 T1 the recorder        tests/e2e_mirror.py: local pages in the real browser — a static page, a React list that
                        adds/removes/reorders, styled-components (insertRule), a shadow root, a form, nested
                        scrollers. After each change the mirror's tree (rebuilt in headless Chromium and in
                        WebKit) equals the real one, node for node
 T2 operations          the same pages: tap a button by element with different fonts on each side; type into a
                        field that reformats; select; scroll to the bottom of an endless list and see more arrive
 T3 nothing leaks       the mirror's frame with the network logged: zero requests, on a page full of trackers,
                        external fonts and <script> tags; an inline onclick never runs
 T4 real sites          on the iPhone: Google search, Wikipedia, GitHub, a news site, YouTube Music, a webmail.
                        For each: looks right? every operation works? Where not: is it §7's fallback or a bug
 T5 cost                the same 2-minute routine on each T4 site in both modes: bytes down and up (the stats
                        overlay), rd-mirror's and Chrome's CPU on the OECT, time from tap to change
 T6 fallbacks           play a video; open a permission prompt; a 50 000-node page; kill rd-mirror; switch user:
                        Video mode each time, and back
 T7 nothing to see      CreepJS and sannysoft in Page mode against Video mode: the same scores; and the
                        Runtime.enable probe (a console.log of an object with a getter) stays silent
```

## 17. Order of work

Each step ends with something you can try.

```
 0  T0 by hand. If awaitPromise or getResourceContent fails without Runtime.enable, this plan changes
    before any code is written                                              ✔ done 2026-11-09, below
 1  rd-shell's socket + relay; rd-mirror with the snapshot only; desktopd's "dom" channel; the viewer shows
    a page that doesn't update. T5's first numbers: is it worth going on?
 2  differences, CSS, resources and pictures: a page that follows the real one. T1, T3
 3  operations: tap, scroll, typing, select. T2
 4  the bar in HTML; the Page | Video switch; the fallbacks of §7 and §14. T6, T4 on the iPhone
 5  frames from other sites; canvas refresh; a mouse on computer viewers
 6  T7 and T5 on browser-oect; the docs; the image through the Action
```

### Step 0, done (2026-11-09)

`browser/test/mirror/t0.py` starts a headless browser on a DevTools pipe and makes each call, with no
`Runtime.enable`. Run with Chrome 157 and with the container's own CloakBrowser 146.0.7680.177 (headless, on
this PC): the same answers from both.

| call | result |
|---|---|
| `Page.createIsolatedWorld` + `Runtime.evaluate {contextId}` | works; the world sees the DOM and not the page's variables |
| `Runtime.evaluate {awaitPromise}` as a long poll | works: each call returned when the page's next change came (0.2–0.3 s, the test page's own timer) |
| a `MutationObserver` in the isolated world | sees the changes the page's own script makes |
| `sheet.cssRules` from the isolated world | same-origin sheets readable, a rule added with `insertRule` included; a cross-origin sheet throws `SecurityError`, as §6 expects |
| `Page.getResourceContent` | returns the cross-origin stylesheet (text) and the picture (base64) from what the page loaded |
| `Page.getResourceTree` | lists them with their type |
| `Page.captureScreenshot {clip, format: webp}` | works |
| `Page.addScriptToEvaluateOnNewDocument {worldName}` | accepted, but the script did not run — what youtube-stall.md found. Not used |

Two things it did not settle:

- **Headless, not the kiosk on Xorg.** The calls are the same; step 1 runs them in the real container.
- **The `Runtime.enable` probe for T7.** The usual one (a `console.debug` of an Error whose `stack` is a
  getter) stayed silent even with `Runtime.enable` on, in both browsers, so it proves nothing any more. T7
  needs a probe that still works, or only the detection pages' scores.

### The recorder and the applier, done (2026-11-09)

`browser/rd-mirror/recorder.js` (snapshot, differences, CSS, form values, shadow roots) and `web/mirror.js`
(the applier, the two locks, resources by address). Not yet: operations (§8), the bar (§9), frames,
canvas and video (§7), and everything in C.

`browser/test/mirror/mirror_test.py` runs a real page in one tab of a headless browser, its mirror in
another, and compares the two trees after each of 20 changes: lists added to, emptied, reordered and
replaced (300 rows), attributes, text, `insertRule`, a moved node, a node removed and put back, shadow roots
(one attached late), form values set from script, stylesheets added late, a cross-origin stylesheet with an
`@import` and a relative picture, SVG, `innerHTML` with a `<script>`, an `onclick` and a `javascript:` link.

| check | result (Chrome 157) |
|---|---|
| the trees, after the snapshot and each change | equal, 21 of 21 |
| computed colours and backgrounds, the shadow root's, the picture's size | equal, 8 of 8 |
| requests from the viewer's tab for anything of the page's | none |
| in the mirror: scripts, handlers, link or form targets, `http` addresses; the page's script ran | none; no |
| the mobile Wikipedia article on WebRTC (`--url`) | 5 532 nodes equal; the two screenshots the same; snapshot 707 KB of JSON, **104 KB deflated**; 150 KB of pictures and fonts as they are (not yet recompressed); 144 addresses not sent because the real page hadn't loaded them (lazy pictures further down) |

What it showed that the plan didn't say:

- **A tab that isn't in front runs its timers once a second**, the recorder's included. The plan only
  mirrors the tab in front, so this is right, but nothing may depend on mirroring a background tab.
- **A shadow root attached to an element already sent** raises no mutation. The recorder looks for them
  once a second, so such content can be up to a second late.
- **`Page.captureScreenshot` of a tab that isn't in front never answers.** The same rule: the tab in front
  only, and a timeout on every DevTools call in rd-mirror.

### Wired through and passing on this PC (2026-11-10)

Steps 1–4 are built, and parts of 5. Nothing has run in a container or on an iPhone yet.

| piece | where | state |
|---|---|---|
| rd-shell's socket: DevTools relay, the bar's state out, its commands in | `browser/rd-shell/rd-shell.c` (`link_*`) | built; tested on Xvfb and Xorg with kiosk Chrome |
| rd-mirror: attach to the tab in front, the recorder, resources, pictures, dialogs, touches | `browser/rd-mirror/rd-mirror.c`, `picture.c`, `recorder.js` | built |
| the agent: `dom` channel ⇄ rd-mirror's socket, `mirror.set`, `mirror.video`, touches | `desktop/core/mirror.c`, `session.c` | built (Linux here; the Mac and Windows builds only in CI) |
| the viewer: Page / Video in the menu, the bar in HTML, operations | `web/pagemode.js`, `mirror.js`, `app.js` | built |
| the image: rd-mirror, its unit, the runtime directories, `[mirror]` added to an existing `desktop.ini` | `browser/hqnode/` | builds in the browser-image Action; not installed anywhere |

`tests/e2e_mirror.py` runs all of it for real — Xorg with the touch screen, rd-shell, Chrome in kiosk as
the remote browser, rd-mirror, the agent, the server, and the web viewer at an iPhone's size with touch:

| check | result |
|---|---|
| Page mode shows the page's elements under the viewer's bar; the frame is 440 × 860, sandboxed, no scripts in it | ✔ |
| a tap on the mirror's button | a trusted click on the real one (`isTrusted`), twice |
| typing into the mirror's field, with three characters deleted and retyped | the real field has it; the page's echo comes back |
| a `<select>` picked in the mirror | picked on the real page |
| `confirm()` on the real page | asked on the viewer, answered to the page |
| a `<video>` that plays for 3 s | the picture while it plays, the page again after |
| a finger dragged 96 px over something that doesn't scroll | the page's own touch handler saw it move 99 px |
| the mirror scrolled to its end | the real page went there and loaded more: 20 rows → 80 |
| a link tapped | the real page navigated; the viewer's frame stayed `about:srcdoc` |
| the bar: Back, then an address typed | the page before; then the page asked for |
| a CSS background picture | arrived over the channel, shown from a `blob:` |
| requests from the viewer's browser for anything of the site's | none |
| Video mode again | the picture came back |

Pictures (`browser/test/mirror/picture_test.c`): a 2400 × 1600 JPEG of 186 KB → a 1320 × 880 WebP of 28 KB
for a viewer 1320 pixels across; a 1200 × 800 PNG of 108 KB → 26 KB; under 20 KB, truncated files and junk
go as they are. D6 as recommended (quality 75), except the size: no wider than the viewer's screen, not the
size each one is shown at — the viewer doesn't know that before it has the picture.

What this showed that the plan didn't say:

- **A data-channel message over about 1200 bytes from the agent never reached the viewer** (one of 1538
  bytes, every run; smaller ones always arrived). The agent now sends `dom` in 1000-byte pieces, which
  works. Why the stack loses a message it has to split is not known; the `file` channel may have the same
  fault and deserves a look.
- **With `Page.enable`, the browser shows no `alert` of its own**: it waits for DevTools. So dialogs are
  the viewer's to ask (done), not a window that would bring the video.
- **A page back from the back/forward cache still holds the recorder it had**, with a waiter nobody
  listens to. A restart of the recorder drops that waiter first.
- **`mirror.video`** is new: the picture beside Page mode for a while (a playing video, a window of the
  browser's own, a page over 20 000 elements), without leaving Page mode — the bar's state has to keep
  arriving to know when to come back.

Not built: frames from other sites and `<canvas>` (blank boxes for now), a mouse's hover on computer
viewers, the per-site "always Video" list (D8), the limits by size and by rate of change (§14; only the
element count is checked), the cache of converted pictures, a second viewer's own snapshot without
resending everyone's. Not tested: Safari (every test here has Chrome on both sides), the OECT's four A55
cores, the cost against video (T5), and what a detection page sees (T7).
