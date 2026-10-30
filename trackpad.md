# RemoDroid Trackpad: design

A Magic Trackpad for the web viewer, for a phone (an iPhone first) that controls a Windows, macOS or
Linux computer. It works the same in any browser, with a finger or a mouse, and a switch in the menu turns
it on and off. Status: **implemented** (tested end to end on Linux by `tests/e2e_desktop.py`; the
Windows and macOS capture changes are built by CI). Companion documents: [`multi-platform.md`](multi-platform.md) §9 (how
the viewer drives a computer today), [`arch.md`](arch.md).

Contents

1. [The problem](#1-the-problem)
2. [What it looks like](#2-what-it-looks-like)
3. [The cursor stays in the middle](#3-the-cursor-stays-in-the-middle)
4. [Zoom](#4-zoom)
5. [Gestures](#5-gestures)
6. [The bottom panel: trackpad, keyboard, collapsed](#6-the-bottom-panel-trackpad-keyboard-collapsed)
7. [Resizing, and the limits](#7-resizing-and-the-limits)
8. [Settings and what is remembered](#8-settings-and-what-is-remembered)
9. [Protocol and agent changes](#9-protocol-and-agent-changes)
10. [Code layout](#10-code-layout)
11. [Testing](#11-testing)
12. [Milestones](#12-milestones)
13. [Risks and open decisions](#13-risks-and-open-decisions)

---

## 1. The problem

Today an iPhone controls a computer as a touch screen (multi-platform.md §9, `deskTouch()` in
`web/app.js`): the computer's whole screen is shrunk to fit the phone, a tap clicks where the finger
lands, and a long press right-clicks. A 2560×1440 desktop on a 390-point-wide phone is drawn at about
1/6.5 of its size, so a 13 px menu label becomes 2 px tall. You can't read it, and a fingertip covers a
dozen buttons, so you can't hit the one you want either.

A trackpad fixes both:

- **The picture is magnified** so the text is readable, and only a part of the screen is shown at a time.
- **The finger no longer points at the target.** It moves a cursor, the way a finger on a MacBook's
  trackpad does, so precision comes from the cursor's tip, not from the fingertip.
- **The view follows the cursor.** The cursor sits still in the middle of the visible area and the
  picture slides under it, so wherever the cursor goes, it is in view and the text around it is readable.

## 2. What it looks like

Portrait iPhone, trackpad open:

```
┌──────────────────────────────┐  ← top of the screen (safe area in Safari, the status bar in the Home Screen app)
│                              │
│   …ntitled — Notepad   File  │
│   Edit  Format  View  Help   │      the picture, magnified;
│                              │      it slides under the cursor
│   Hello wor▌                 │
│              ↖               │  ← the cursor, always in the middle of this area
│                              │
│                              │
├──────────────────────────────┤
│ ⚙  ⤢        ═══     ⋯  ⌨  ⌄  │  ← the header: settings, whole screen; the grip; Keys sheet, keyboard, collapse
│                              │
│          trackpad            │      the trackpad surface
│                              │
│                              │
└──────────────────────────────┘  ← home indicator (the panel pads itself above it)
```

Keyboard open (the same panel, switched; the same size and the same header, with ▭ where ⌨ was):

```
├──────────────────────────────┤
│ ⚙  ⤢        ═══     ⋯  ▭  ⌄  │  ← ▭ = back to the trackpad, in the same spot as ⌨
│ esc tab  .  ←  ↓  ↑  →       │
│ q w e r t y u i o p          │
│  a s d f g h j k l           │
│ ⇧ z x c v b n m ⌫            │
│ 123 ctrl alt ⌘  ␣   ↵        │
└──────────────────────────────┘
```

**The switch (⌨ / ▭) and the collapse key (⌄) are in the right corner of the header, in the same
place in both states**, so going back and forth between the trackpad and the keyboard is one tap in
one spot. **The trackpad and the keyboard are the same size**: one panel whose contents switch, as the
iPhone's keyboard becomes a trackpad in place. On a phone the panel is docked at the bottom, full width.
On a bigger screen (an iPad, a desktop browser) it floats instead, like the on-screen keyboard of the
code repo's terminal (`web/components/FloatingKeyboard.tsx`) and the viewer's own floating keyboard: a
560 pt card (at most 96% of the screen) over the picture, dragged anywhere by its header, its spot
remembered; ⠿⠿ on the header's left shows where to hold it.

Collapsed: the panel is gone and a small round button with a keyboard icon floats at the bottom
corner. The picture uses the whole screen and the cursor moves to the middle of the whole screen.

**The switch.** The menu (the ball) has a **Trackpad** item whenever the viewer shows a computer. It is
lit while the trackpad is on; tapping it switches between the trackpad and Direct (tap where you touch,
§8) at once, on any device, and the choice is remembered. It is the same item on a phone, an iPad and a
desktop browser.

## 3. The cursor stays in the middle

**The visible area** is the rectangle between the top of the screen (the top safe-area inset in Safari,
the very top in the Home Screen app, as `layout()` does today) and the top of the panel. The cursor's
home, **the anchor**, is the centre of that rectangle. When the panel is resized, switched or collapsed,
the visible area changes and the anchor moves with it; the picture moves the same amount at the same
time, so the cursor never jumps off the thing it points at.

**The cursor is drawn here, not on the computer.** It is a DOM element (an SVG arrow, with its tip at the
anchor) above the video. It never moves, so it never lags behind the finger: what moves is the picture,
and that move is also local and instant. Only the click lands a frame or two later on the computer.

**The cursor's position on the computer** is kept in the viewer as `tp.cx, tp.cy`, normalised to 0..1 of
the shared screen, the same coordinates that 0x03 mouse messages already carry (multi-platform.md §8.2).
The picture is placed so that the point `(cx, cy)` of the screen is drawn exactly at the anchor:

```
scale  = CSS px per screen pixel (the zoom, §4)
left   = anchorX − cx · screenW · scale
top    = anchorY − cy · screenH · scale
```

`(cx, cy)` is clamped to 0..1, so the cursor can reach every corner of the screen. When it is near an
edge, the picture's edge comes into the visible area and the space beyond it is the stage's black
background. That is the price of "the cursor is always in the middle", and it is what macOS's own
zoom does with "Keep the pointer centred".

The picture is positioned with a CSS `transform: translate(…) scale(…)` on `#video`, not by setting
`left`/`top`/`width`/`height` the way `layout()` does for phones, so a drag repaints at 60 fps without
a relayout. `layout()` keeps its job for phones and for the Direct mode (§8); in trackpad mode it hands
over to `tpLayout()`.

**Every change of `(cx, cy)` sends one 0x03 move** (absolute, at most one per animation frame, through
the existing `queueMove()`), so the real pointer on the computer follows: hover effects, tooltips and
the I-beam over text work as they do with a real mouse.

**A second pointer in the picture**: every capture draws the computer's own pointer into the picture
(ScreenCaptureKit's `showsCursor` on macOS, the DXGI pointer shape or `DrawIconEx` on Windows, the XFixes
cursor image on Linux), so there would be two arrows, the remote one a little behind. In trackpad mode the
viewer asks the agent to leave the pointer out of the picture (§9).

## 4. Zoom

**The zoom is a scale: CSS pixels per screen pixel.** At 1.0 a screen pixel is a CSS pixel (three phone
pixels on an iPhone), and the computer's text is as big as it is on its own monitor, which is
comfortable to read. That is the default.

| | value | why |
|---|---|---|
| smallest | the scale at which the whole screen fits the visible area | there is nothing more to see; zooming out further only adds black |
| default | 1.0, but never below the smallest | readable text |
| largest | 4.0 | enough for small print and pixel-exact work; beyond it the video is only blur |

**Spread two fingers to magnify, pinch to shrink**, like a photo on the iPhone. The zoom is about the
cursor: the point under the cursor stays under it, and since the cursor stays at the anchor, only the
scale changes. Past the limits the picture follows the fingers a little with resistance (rubber band)
and springs back on release, so the limit is felt rather than being a hard stop.

The zoom is **local**: nothing is sent to the computer, and the computer's own resolution does not
change. (On a real Mac trackpad a pinch zooms the app instead; that is a later option, §13.)

**The ⤢ key** on the trackpad's top row (§6) toggles between the smallest zoom (the whole screen) and the
last zoom you chose, so you can glance at the whole desktop and come back. It is a key and not a gesture
on purpose: every tap gesture already means a click (§5), and macOS's two-finger double tap for this
would first send a two-finger tap, which is a right click.

The zoom is remembered per device, like the gamepad's `S.view` today (it reuses that store, `viewOf()` /
`setView()`, with a new `scale` field), so each computer opens at the size you last read it at.

## 5. Gestures

The trackpad surface takes these. **The picture area takes the same gestures too**, so when the
keyboard is open or the panel is collapsed you can still move the cursor, scroll and zoom by touching
the picture. It is all one big trackpad, and the panel is just the comfortable part under your thumb.

| Gesture | Does | Sent |
|---|---|---|
| One finger moves | moves the cursor (the picture slides under it) | 0x03 move |
| Tap | left click | 0x03 down/up, button 0 |
| Two taps | double click | two clicks; the computer decides it is a double click by its own timing, as with a real mouse |
| Tap, then touch again and drag | drag (hold the button while moving), like "Dragging" in macOS settings | 0x03 down, moves, up |
| Two-finger tap | right click | 0x03 down/up, button 2 |
| Two fingers move together | scroll, with momentum after a flick | 0x02 wheel at the cursor |
| Two fingers spread / pinch | zoom the view (§4) | nothing |
| Three-finger tap | middle click | 0x03 down/up, button 1 |

Details that make it feel like an Apple trackpad rather than a touch screen:

- **Movement is relative and accelerated.** A slow finger moves the cursor a little for precise work,
  a fast flick crosses the screen. The cursor's motion is `gain(v) · Δfinger`, with `v` the finger's
  speed (from the events' own timestamps, so moves that queue up behind a busy page and arrive in a
  burst don't count as a flick) and `gain` a curve like macOS's: flat about 1.0 below 50 pt/s, rising to about 4 above
  800 pt/s. It is in CSS pixels of the visible picture, so at any zoom the cursor moves over what you see
  at the speed your finger does; the **Move speed** setting (⚙, §8) scales it.
- **Click without delay.** A tap is a tap if the finger lifts within 250 ms and moves less than 8 pt.
  The click is sent at lift, not after waiting to see if a second tap follows, so a single click is not
  slowed down. Double-clicking to open a file is two taps: each is a click, and the computer turns the
  pair into a double click itself, by its own double-click time (500 ms by default on Windows and macOS),
  as it would for a mouse. Nothing in the viewer waits for, or reinterprets, a second tap.
- **Both clicks of a double click land on the same pixel.** The cursor does not move while a finger is
  still within its 8 pt tap distance, so a wobble during a tap moves nothing, and touching down again
  somewhere else on the pad doesn't move the cursor either, because the pad is relative. Windows only
  counts two clicks as a double click when they are within 4 px of each other, so this matters.
- **Tap then drag**: if, within 300 ms of a tap, the finger comes down again and moves more than 8 pt, the first tap's
  click is not a click any more but the start of a drag: the button goes down at the second touch and up
  when that finger lifts. To make this possible without delaying every click, the first tap sends
  down/up at once, and the drag's down is a new press; for selecting text and moving windows this is the
  same thing. A drag that reaches the edge of the visible area keeps the cursor moving (the picture
  scrolls under it) as long as the finger stays there.
- **Scroll or zoom, decided once.** With two fingers down, the first 10 pt of movement decides: if the
  distance between the fingers changed more than their middle moved, it is a zoom; otherwise it is a
  scroll. The choice holds until the fingers lift, so a scroll does not wobble the zoom.
- **Scrolling follows the fingers** (natural scrolling, as on an iPhone and on a Mac by default): drag up
  and the content moves up. A setting (⚙, §8) turns it round for those used to a Windows wheel. The amount is in
  screen pixels × zoom, so the content moves under the fingers at the speed they do, times the
  **Scrolling speed** setting.
- **Momentum**: after a flick, the scroll keeps going and slows down (decay about 0.95 per 16 ms frame)
  until it is slow or a finger touches the pad again. The viewer sends these wheel events itself; the
  computer sees an ordinary smooth wheel.
- **A palm or a third finger resting** while one finger moves is ignored: only the first finger down
  moves the cursor until it lifts.

The wheel message is unchanged (0x02, `sendScroll()`), sent at `(cx, cy)` so it scrolls whatever is under
the cursor, as a real trackpad does.

**With a mouse it is the same trackpad.** In a desktop browser, or with a mouse or pen attached to a
phone or iPad, the pad and the picture take the pointer as a finger: pressing the left button is a
finger touching down, moving with it held moves the cursor by how far the mouse moves, and releasing it
lifts the finger. So a click is a tap (a click at the cursor, not where the mouse is), two clicks are a
double click, and click-then-press-and-move drags. The right button is a two-finger tap (right click at
the cursor) and the middle button a three-finger tap. Moving the mouse with no button down does nothing,
as hovering over a trackpad does. The wheel scrolls at the cursor (with the wheel's own direction, so the
natural-scrolling setting doesn't turn it round), and Ctrl + wheel, which is also what a Mac's
trackpad pinch gives a browser, zooms the view (§4). A physical keyboard keeps typing through `onKey()`.

## 6. The bottom panel: trackpad, keyboard, collapsed

The panel is docked at the bottom above the home indicator on a phone, and floats as a card on a
bigger screen (§2). It has three states, and a header row that stays the same in all of
them (the grip in the middle; ⚙ and ⤢ on the left; ⋯, the switch and ⌄ in the right corner):

```
              ⌨ key                      ⌄ key
 trackpad  ────────────▶  keyboard  ─────────────▶  collapsed
     ▲  ◀────────────── ▭ key             │              (the ⌨ button)
     │                                    │
     └──────────────── ⌄ key ─────────────┘
       collapsed ── tap the ⌨ button ──▶ the state it was in before
```

- **Trackpad**: the surface of §5, under the header. The header's switch key shows **⌨** (open the
  keyboard).
- **Keyboard**: the on-screen keyboard, the existing keys (`esc tab . ← ↓ ↑ →` and four rows) under the
  same header, whose switch key now shows **▭** (back to the trackpad). The header also has **⋯** (the
  Keys sheet), **⚙** (§8) and **⤢** (§4) in both states. Typing does not move the cursor, and the
  picture and the cursor stay where they were.
- **Collapsed**: the panel is hidden and becomes a small round button (44 pt, the keyboard icon) at the
  bottom right, above the home indicator. Tapping it brings the panel back in the state it was
  collapsed from (trackpad or keyboard). The button can be dragged to another spot, which is remembered,
  like the menu ball (`placeBall()`). With the panel collapsed, the whole picture is the trackpad (§5).

Switching animates the panel's height over 200 ms, and the visible area, the anchor and the picture
move with it in the same frames (§3), so the cursor stays on its target.

**The keyboard reuses the floating keyboard's keys** (`renderFkb()`, `fkbKey()`, `fkbChar()`): the
same layers (abc, 123, #+=), the one-shot Shift and Ctrl, key repeat for ⌫ and the arrows, the key
click sound. The keys go over the ctrl channel as `text` and `key`, as they do now. It gains:

- **alt** (⌥ towards a Mac) and **⌘ / Win**, one-shot like Ctrl, in the bottom row, because a computer
  needs them (Alt+Tab, ⌘Tab, Win key menus). They are sent the same way as `ctrl` (Android key codes
  that `desktop/core/keymap.c` maps: `KEYCODE_ALT_LEFT` 57, `KEYCODE_META_LEFT` 117).
- The **⋯** key in the top row opens the existing Keys sheet (F-keys and combinations).

In trackpad mode the docked panel replaces the floating keyboard: the **Keyboard** item in the menu
opens the panel in its keyboard state. The floating keyboard stays for the Direct mode (§8) and for an
iPad, where a floating keyboard over a big picture still makes sense.

The iOS system keyboard is not used: it can't be resized, it pushes the page around when it opens, and
it has no Ctrl, Esc or arrows. The **Text** sheet (`#kbText`) stays for pasting or dictating a long text.

## 7. Resizing, and the limits

On a phone, dragging the header (anywhere but its keys) up or down resizes the docked panel live. The
trackpad and the keyboard are one size: the drag changes the keyboard's key height, and the trackpad is
always as tall as the keyboard, so switching never moves the picture.

| | smallest | largest | default |
|---|---|---|---|
| Key height (the panel's height follows: the header and 5 rows of keys) | 34 pt | 58 pt | 46 pt portrait, 34 pt landscape |
| Keyboard width (a phone in landscape) | 480 pt | the screen width | 720 pt, centred |
| The visible area left above the panel | at least 35% of the screen height, always | | |

- In landscape, where a full-width keyboard is too wide to reach, the keyboard is centred and its side
  edges drag its width.
- When a drag hits a limit, the panel stops with a short rubber band, so the limit is felt.
- The last rule wins over the others: on a short landscape phone the panel's largest size shrinks so
  that the picture still has 35% of the height.
- **Double-tap the header** to go back to the default size.
- Sizes are remembered per orientation (portrait and landscape need different heights).
- The floating card (iPad, desktop) has a fixed size, 560 × the keyboard's height with 38 pt keys; its
  header moves it instead of resizing it. It floats over the picture, so the pointer stays in the
  middle of the whole screen.

## 8. Settings and what is remembered

**The ⚙ key on the trackpad** opens the trackpad's own settings, so you can change the speed without
leaving the picture. It is a small card that opens just above the panel, over the bottom of the
picture, and leaves the trackpad uncovered: every change applies at once, so you move a slider and try
it on the pad straight away. ⚙ again, or the card's ✕, closes it; tapping the pad does not, because
that is how you try the speed.

| In the ⚙ card | Values | Default |
|---|---|---|
| Move speed | slider, 0.25×–3× | 1× |
| Scrolling speed | slider, 0.25×–3× | 1× |
| Natural scrolling | on / off (off: drag up and the content moves down, like a Windows wheel) | on |
| Reset | button: both speeds, scrolling direction and the panel's sizes back to the defaults | |

The speed changes the gain (§5) as a whole: at 2× every movement of the finger moves the cursor twice
as far, at every finger speed, so the acceleration still feels the same, just faster. The speeds are
the viewer's own, not the computer's: Windows' and macOS's own pointer speed don't apply, because the
viewer sends absolute positions (§3).

The menu's **Trackpad** switch (§2) sets Touch control to Trackpad or Direct. Settings → Stream, in the
computers' part (`renderTab()`), has the same choice with Auto as well:

| Setting | Values | Default |
|---|---|---|
| Touch control | **Auto** / **Trackpad** / **Direct** (the touch-screen behaviour: tap where you touch) | Auto: Trackpad on a phone (a touch screen whose short side is under 500 pt), Direct on an iPad |

On Auto, a computer with a touch screen of its own (`caps.touch`, the remote browser's) keeps taking
fingers as they are; Trackpad chosen by hand wins over that. The trackpad steps aside while the on-screen
gamepad is up, and turning it on with the switch closes the gamepad. While it is on, a mouse or pen is a
finger on it (§5); in Direct, a mouse is a mouse (`deskPointer()`) and a finger taps where it touches.

Stored in the viewer's settings object `S` (`localStorage`, `save()`), like the other viewer settings:

- `S.touchCtl` (`'auto'` / `'trackpad'` / `'direct'`), `S.tpMove`, `S.tpScroll`, `S.tpNatural`
- `S.tpPanel = { state: 'trackpad'|'keyboard'|'collapsed', from, portrait: { key }, landscape: { key, kbw } }`
  (`from`: the state to come back to from collapsed; `key` and `kbw` in CSS px)
- the collapsed button's spot, under `KEY + '-tpball'`, like the menu ball's, and the floating card's, under
  `KEY + '-tpfloat'`
- per device, in `S.view`: `scale` (§4), `lastScale` (for ⤢) and the cursor position `cx, cy`, so
  reconnecting puts the cursor and the view back where you left them

## 9. Protocol and agent changes

Almost nothing changes on the wire: the trackpad sends the absolute 0x03 mouse and 0x02 wheel
messages the viewer already sends. One addition:

**ctrl `cursor.show`** `{ show: false | true }`, viewer → agent, acked. With `false`, the agent leaves the
pointer out of the captured picture; `true` puts it back. The viewer sends `false` when trackpad mode
starts (and at every `session.ready` while it is on) and `true` when it ends; the agent puts it back by
itself when the session ends, so a Direct or mouse viewer that comes next is not left without a pointer.
The agent advertises it as `session.ready.caps.cursor_hide: true`, so an older agent is not sent a
message it doesn't know (with one, the Mac's own pointer shows a little behind the viewer's arrow).

It is one platform call, `dsk_capture_pointer(int show)` (`desktop/core/desk.h`), which applies to the
captures already running, from any thread:

- **macOS** (`desktop/mac/capture.m`): `SCStreamConfiguration.showsCursor`, changed on each running stream
  with `-[SCStream updateConfiguration:completionHandler:]`, no restart.
- **Windows** (`desktop/win/capture.c`): the DXGI pointer isn't composed onto the frame, and the GDI fallback
  doesn't `DrawIconEx` it. A change recomposes the last frame at once, so the old pointer doesn't linger.
- **Linux** (`desktop/linux/capture_x11.c`): the XFixes cursor image isn't drawn and the pointer isn't polled
  for moves. A change makes every capture grab again.

The MCP assistant's screenshots (`desktop/core/ai.c`) use the same capture, so they leave the pointer out
too while a trackpad viewer is connected.

## 10. Code layout

| File | Change |
|---|---|
| `web/index.html` | `#tpad` (the docked panel: header with the grip, `#tpadSurface`, `#tpadKeys`), `#tpCursor` (the arrow), `#tpBall` (the collapsed button) |
| `web/app.css` | the panel, its states and the 200 ms height transition, the grip, the cursor arrow with a white outline and shadow so it reads on any background, the round button; padding by `env(safe-area-inset-bottom)` |
| `web/app.js` | the state `tp` next to `layout()`, which hands over to `tpRelayout()` while it is on; a section `computers: the trackpad` after the floating keyboard: `tpSync()` (on and off), `tpLayout()`, the gesture recogniser `tpTouch(type, e)` on `#stage` and `#tpadSurface` (the picture's own handlers let touches bubble to the stage), acceleration, momentum, `tpZoomTo()`, `tpWhole()` (⤢), `tpShow(state)`, the resize grip and the keyboard's side edges, the collapsed button, the ⚙ card (`tpSettings()`, with the `slider()` the gamepad's ⚙ now shares); `renderFkb()` draws the docked keyboard too, with ⋯ ▭ ⌄ and alt and ⌘ / Win; the menu's Keyboard item opens the docked keyboard and its Trackpad item (`#menuTrackpad`) is the switch; a mouse or pen goes through `tpTouch()` as a finger (`tpMouse()` for the right and middle buttons) and the wheel through `tpWheel()`; Settings → Stream gets Touch control |
| `desktop/core/session.c`, `desktop/core/desk.h` | ctrl `cursor.show`, `caps.cursor_hide`, `dsk_capture_pointer()` |
| `desktop/mac/capture.m`, `desktop/win/capture.c`, `desktop/linux/capture_x11.c` | `dsk_capture_pointer()` (§9) |
| `multi-platform.md` §8, §9, `arch.md`, `README.md` | `cursor.show`; the touch viewer points here |

No server, Worker or panel changes.

## 11. Testing

**Automated** (`trackpad()` in `tests/e2e_desktop.py`: an iPhone-sized touch context, 390×844, with CDP
touch events, against the Linux agent on Xvfb, checking what the X server receives). The touch-screen
checks before it run with Touch control set to Direct.

- `#tpCursor` sits at the centre of the area above `#tpad`, and the picture starts at 1 CSS px per pixel
- a tap clicks at the pointer (the middle of the screen at first), not where the finger is
- a slow 60 px drag on the pad moves the pointer 60 px, and the arrow on the phone doesn't move
- two taps, the second elsewhere on the pad and wobbling 5 px: two clicks on the same pixel
- a two-finger tap right-clicks at the pointer; two fingers dragged up scroll down (button 5)
- ⚙ opens above the panel; at Move speed 2× a 30 px drag moves the pointer 60 px
- spreading two fingers zooms in and stops at 4×, and the next tap still clicks at the same pixel;
  ⤢ shows the whole screen and a second ⤢ goes back to 4×
- ⌨ shows the keyboard and "a", "b" arrive as a, b; ▭ brings the trackpad back
- ⌄ folds the panel to `#tpBall`, the arrow moves to the middle of the screen, and a tap on the picture
  clicks at the same pixel as before; tapping `#tpBall` brings the trackpad back
- the grip stops at 55% of the screen going up and at 120 px going down
- a reload keeps the panel's height and the zoom
- the X pointer is left out of the picture (no pixel around it off the quadrant's colour), and
  switching to Direct in Settings puts it back and hides the panel and the arrow
- the menu's Trackpad switch turns it back on, lit, and off again
- in a desktop context with a mouse (no touch screen, so Auto leaves it off), the switch turns it on; a
  mouse click on the pad clicks at the pointer, a press-drag of 60 px moves it 60 px, the right button
  right-clicks at the pointer, and the wheel scrolls

**By hand** on a real iPhone (Safari and the Home Screen app, portrait and landscape) against the Windows
11 VM and a Mac: reading small text, selecting a word by tap-and-drag, moving a window, scrolling a long
page with momentum, typing with the docked keyboard, and on the Mac, checking there is only one pointer.

## 12. Milestones

All four are done, and the menu switch with mouse input after them; the hand test on a real iPhone is still to do.

| | What | Done when |
|---|---|---|
| **T1** | The anchored cursor, relative movement with acceleration, tap / double tap / two-finger right click, the trackpad panel with ⌨, ⚙ (move speed) and ⌄, the collapsed button, the Touch control setting | an iPhone can read and click around Windows 11 comfortably |
| **T2** | Pinch zoom with limits, two-finger scroll with momentum, tap-then-drag, three-finger middle click | the gestures in §5 all work and the e2e checks pass |
| **T3** | The docked keyboard with ▭ / ⌄ and alt / ⌘, resizing both with limits, sizes per orientation | one-thumb switching between typing and pointing |
| **T4** | `cursor.show` on macOS; docs | a Mac shows one pointer |

## 13. Risks and open decisions

- **The cursor's shape.** The viewer always draws an arrow; the computer may be showing an I-beam or
  a resize arrow. Sending the real shape is in multi-platform.md M7 ("remote cursor shape"); when it
  comes, `#tpCursor` draws it instead.
- **Safari's own gestures.** Pinch and double-tap must not zoom the page and an edge swipe must not go
  back. The viewer already blocks `gesturestart` and uses `touch-action: none`; the panel needs the same,
  and the Home Screen app is the recommended way to use it.
- **A drag that runs off the edge** of the picture's visible area is handled by keeping the cursor
  moving while the finger rests near the panel's edge (§5); if that feels too fast or slow it gets
  its own speed.
- **Pinch on the computer.** On a real Mac trackpad a pinch zooms the app (a browser page, a photo).
  Here it zooms the view, which is what this feature is for. Sending it to the app (Ctrl+wheel on
  Windows, a magnify gesture on macOS) could become a setting later.
- **Latency.** The cursor and the picture move locally, so pointing is instant; but what is under the
  cursor is a frame or two old, so a click right after a fast flick might land a few pixels off from
  what you saw. A click is sent with the position at the moment of the tap, which is what you aimed at.
- **The iPad and desktop browsers.** The same code works there, but with a big screen Direct is
  usually better, so Auto picks it there; the menu's switch turns the trackpad on with one tap.
