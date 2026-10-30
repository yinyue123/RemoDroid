# RemoDroid Desktop: design

Remote control of Windows, macOS and Linux computers from the same panel and web viewer that control
Android phones today. Status: **implemented** (Linux tested end to end by `tests/e2e_desktop.py`; Windows and macOS built and
unit-tested in CI). Companion documents: [`arch.md`](arch.md) (how the
Android path works), [`plan.md`](plan.md) (API), [`webrtc.md`](webrtc.md).

Contents

1. [Goals and non-goals](#1-goals-and-non-goals)
2. [The four agents](#2-the-four-agents)
3. [One native library, four platforms](#3-one-native-library-four-platforms)
4. [Source layout](#4-source-layout)
5. [Inside the desktop agent](#5-inside-the-desktop-agent)
6. [Platform backends](#6-platform-backends)
7. [The desktop app: menu bar icon and pairing](#7-the-desktop-app-menu-bar-icon-and-pairing)
8. [Protocol additions](#8-protocol-additions)
9. [Web viewer: mouse and keyboard](#9-web-viewer-mouse-and-keyboard)
10. [Server, Worker and panel changes](#10-server-worker-and-panel-changes)
11. [Configuration](#11-configuration)
12. [Security](#12-security)
13. [Build, CI and packaging](#13-build-ci-and-packaging)
14. [Testing](#14-testing)
15. [Milestones](#15-milestones)
16. [Risks and open decisions](#16-risks-and-open-decisions)

---

## 1. Goals and non-goals

**Goals**

- A remote desktop tool for each of Windows, macOS and Linux. You install it on the computer, pair it with
  your panel once, and then open the computer from the web viewer like a phone. Video comes over WebRTC, and
  the browser's mouse and keyboard control the computer.
- The desktop agents **share the native library with Android**: the same C WebRTC stack (ICE, DTLS, SRTP,
  RTP, congestion control, SCTP, SDP), built as `libremodroid.so` (Android, Linux), `remodroid.dll` (Windows)
  and `libremodroid.dylib` (macOS). All the agent logic that isn't platform-specific is also C in that
  library, written once for all three desktops.
- A small native app per platform, written in C or Objective-C directly against the system UI APIs, with no
  third-party libraries. It has a menu bar or tray icon, a pairing window and a status display. The app
  contains only UI code; everything else is in the library.
- No ADB. The desktop agent runs on the computer it controls. ADB, USB and the router agent stay Android-only.

**Non-goals for the first release** (see §15 for when they come)

- Wayland capture and input on Linux. v1 needs an X11 session.
- Unattended access to the login screen, the Windows UAC secure desktop, or a Mac before login. That needs a
  Windows service or a macOS LaunchDaemon. (Since then, Windows' lock screen: with the RemoDroidInput service
  installed, a locked computer can be seen and unlocked from the viewer, below.)
- Sound, a remote cursor shape, and viewing several monitors at once. (Hardware encoding on Linux came
  later: VA-API, §5.5.)
- An installer with code signing or notarization (§13.4 says what is needed).

---

## 2. The four agents

```
                                    ┌───────────────────────── remodroid-server / Worker ──┐
 Browser (web/app.js) ── HTTPS/WS ──┤  login · devices · signaling relay · TURN             │
        ▲                           └──────▲─────────────────────────▲─────────────────────┘
        │ WebRTC (video, data channels)    │ WS /ws/agent            │ WS /ws/agent
        │                                  │                         │
        │                     ┌────────────┴──────────┐   ┌──────────┴───────────────────────┐
        │                     │ router agent (C)      │   │ desktop agent (one process)       │
        │                     │ ADB client · tunnel   │   │  app: tray icon + pairing (UI)    │
        │                     └────────────┬──────────┘   │  libremodroid: server connection, │
        │                                  │ USB/ADB      │   session, input, files, video,   │
        │                     ┌────────────┴──────────┐   │   WebRTC stack                    │
        └─────────────────────┤ phone: Java + libremo-│   └──────────────────────────────────┘
                              │ droid.so (WebRTC)     │        Windows · macOS · Linux
                              └───────────────────────┘
```

| Platform | What runs there | Talks to the server through | WebRTC stack | Network path |
|---|---|---|---|---|
| Android | Java server + `libremodroid.so`, started over ADB | the router agent (tunnel) | `rtc/` | Path A (tunnel) or B (phone's Wi-Fi) |
| Windows | `RemoDroid.exe` + `remodroid.dll` | itself (`common/wsc.c`) | `rtc/`, same sources | the computer's own sockets ("embedded" mode, §5.2) |
| macOS | `RemoDroid.app` (+ `libremodroid.dylib`) | itself | `rtc/` | same |
| Linux | `remodroid-desktop` (+ `libremodroid.so`), or headless `remodroid-desktopd` | itself | `rtc/` | same |

The server sees each desktop as one more device in `[devices]`. Desktops are enrolled with the same
one-time codes, and they use the same viewer sessions, signaling and TURN as phones. The only new field
on the server side is `platform` (§8.1).

---

## 3. One native library, four platforms

`jni.c` currently does two jobs. It is a **host**: it runs a thread with a libev loop, queues calls from
other threads with `ev_async`, keeps a stats snapshot, and drops delta frames when the queue backs up. It is
also a **JNI binding**. The design splits these jobs.

```
                 ┌─────────────────────────── libremodroid (every platform) ───────────────────────────┐
                 │  rtc/        ice dtls srtp rtp cc sctp sdp net rtc     ← unchanged, ported to Win32   │
                 │  rtc/host.c  loop thread · command queue · stats  →  C ABI  rd_rtc_*  (rd_rtc.h)     │
                 │  common/     util msg ws conf stun tunnel + plat (POSIX/Win32 shims)                │
                 ├──────────────────── Android only ─────────┬──────── desktop only ─────────────────────┤
                 │  android/native/jni.c   rd_rtc_* ⇄ JNI    │  common/wsc.c httpc.c enroll.c            │
                 │  android/native/image.c screenshots       │  desktop/core/   agent session input files │
                 │                                           │                  video keymap yuv audio    │
                 │                                           │  desktop/<os>/   capture encode input clip │
                 │                                           │                  audio (loopback)          │
                 │                                           │  third_party/minih264 (software H.264)     │
                 │                                           │  third_party/opus (the computer's sound)   │
                 └───────────────────────────────────────────┴────────────────────────────────────────────┘
                          ▲ System.load (Java)                        ▲ linked by the app  (rd_agent_*  API)
```

- **`rd_rtc.h`** is a plain C ABI, the same on all four platforms: `rd_rtc_start(opts, callbacks, ud)`,
  `rd_rtc_video()`, `rd_rtc_audio()`, `rd_rtc_dc_send()`, `rd_rtc_ctrl_in()`, `rd_rtc_end_session()`,
  `rd_rtc_set_max_kbps()`, `rd_rtc_stats()`, and `rd_rtc_call(fn, arg)` to run a function on the loop thread.
  Every function is thread-safe and works by queueing. Callbacks run on the loop thread. `jni.c` shrinks to
  about 150 lines of conversion between `jstring`/`jbyteArray` and these calls. The Java side and the
  `Native` class are unchanged.
- The Android `.so` keeps its file name, its contents (apart from the split) and its ABI towards Java. The
  router agent pushes exactly what it pushes today.
- On the desktops the same library also exports **`rd_agent.h`** (§5.1), the API the app calls.

What is shared, and with whom:

| Code | Android | Router agent | Server | Win/mac/Linux |
|---|:-:|:-:|:-:|:-:|
| `rtc/` WebRTC stack | ✓ | | | ✓ |
| `rtc/host.c` thread + C ABI | ✓ | | | ✓ |
| `common/` util, msg, ws, conf, stun | ✓ | ✓ | ✓ | ✓ |
| `common/wsc.c` server connection (moved from `agent/`) | | ✓ | | ✓ |
| `common/enroll.c` code redemption (moved from `agent/`) | | ✓ | | ✓ |
| `desktop/core/` session, ctrl protocol, input decoding, keymap, files, video pipeline | | | | ✓ all three |
| `third_party/minih264` software encoder + `yuv.c` | | | | ✓ (fallback on all three, primary on Linux v1) |
| `third_party/opus` + `desktop/core/audio.c` the computer's sound | | | | ✓ all three (phones use Android's encoder) |

Each desktop OS needs only its own capture, encoder, input injection, clipboard, system integration and UI
code, roughly 1,500 to 2,500 lines per OS. Everything else is shared.

---

## 4. Source layout

```
rtc/                         ← git mv android/native/{ice,dtls,sctp,rtp,cc,sdp,net,rtc}.{c,h}, test/, rtc.mk
  host.c  rd_rtc.h           ← the host half of jni.c, as a C ABI
android/native/              jni.c (JNI binding only), image.c
common/
  plat.h plat.c              ← sockets, time, randomness, threads, paths, system CA store (POSIX / Win32)
  wsc.c wsc.h                ← from agent/wsc.c, callback-based (router agent and desktops)
  enroll.c enroll.h          ← the HTTP part of agent/enroll.c (CLI stays in agent/)
desktop/
  core/   rd_agent.h agent.c session.c input.c keymap.c files.c video.c yuv.c conf.c status.c
  win/    capture.c (DXGI)  encode_mf.c (Media Foundation)  input.c (SendInput)  clip.c  sys.c
  mac/    capture.m (ScreenCaptureKit)  encode_vt.c (VideoToolbox)  input.c (CGEvent)  clip.m  sys.m
  linux/  capture_x11.c (XShm)  input_x11.c (XTest)  clip_x11.c  sys.c
  app/
    win/   main.c  app.rc  app.manifest  icons/          → RemoDroid.exe
    mac/   main.m  Info.plist  icons/                    → RemoDroid.app
    linux/ main.c (GTK 3)  sni.c (tray over D-Bus)       → remodroid-desktop
    headless.c                                           → remodroid-desktopd (any OS, no UI)
  desktop.mk
third_party/minih264/        minih264e.h (CC0), vendored like the other libraries
third_party/opus/            libopus 1.5.2 (BSD): celt/ silk/ src/ include/, float build, no SIMD or DNN extras
```

Moving `android/native` → `rtc/` is a pure `git mv`, plus path updates in `rtc.mk`, `android/build.sh`,
`arch.md` and the tests. The reason is that a directory named `android/` would otherwise be compiled into
Windows binaries.

---

## 5. Inside the desktop agent

### 5.1 The library API the app uses (`rd_agent.h`)

```c
typedef enum { RD_UNPAIRED, RD_CONNECTING, RD_ONLINE, RD_IN_SESSION, RD_STOPPED } rd_state_t;

typedef struct {
    rd_state_t state;
    char device_id[64], panel[256];
    char viewer[64];        /* who is connected, when RD_IN_SESSION */
    char problem[160];      /* "Screen Recording permission is off", "server rejected the device", … */
    int  view_only;
} rd_status_t;

typedef struct {
    void (*on_status)(void *ud, const rd_status_t *s);  /* loop thread: the app marshals to its UI thread */
} rd_agent_cb_t;

const char *rd_agent_default_conf(void);                 /* per-user path, §11 */
int  rd_agent_start(const char *conf, rd_agent_cb_t cb, void *ud);
void rd_agent_stop(void);
/* Asynchronous: the result comes back through on_done on the loop thread. */
void rd_agent_pair(const char *panel, const char *code, const char *name,
                   void (*on_done)(void *ud, int ok, const char *msg), void *ud);
void rd_agent_unpair(void);
void rd_agent_end_session(void);                         /* "Disconnect viewer" */
void rd_agent_set(const char *key, const char *value);   /* view_only, screen, … (writes desktop.ini) */
void rd_agent_status(rd_status_t *out);
```

### 5.2 One loop, in-process signaling ("embedded" mode)

On the phone, signaling reaches the WebRTC stack through the tunnel socket that the router agent opens. On a
desktop the agent and the stack are in the same process, so `net.c` gets a third mode next to `abstract:` and
`tcp:`:

- `listen = "embed"`: no listener, and the link is always up. `net_ctrl_send()` calls a callback instead of
  writing a tunnel frame. All UDP sockets are direct (Path B, the computer's own interfaces), with
  `getifaddrs` / `GetAdaptersAddresses` for host candidates. The tunnel code stays as it is for Android.
- `rd_rtc_ctrl_in(json)` feeds `session.start`, `signal` and `session.end` in, as the tunnel does today.

So a single libev thread runs both the server WebSocket (`common/wsc.c`) and the WebRTC stack. The relay
that `agent/phone.c` does for the phone becomes about 30 lines in `desktop/core/agent.c`: `session.*` and
`signal` from the server go to `rd_rtc_ctrl_in`, and `signal` and `session.ended` from the stack go to
`wsc_send`.

### 5.3 Threads

| Thread | Runs | Talks to the others through |
|---|---|---|
| UI (the app's main thread) | tray icon, pairing window, notifications | `rd_agent_*` (queued to the loop); `on_status` is marshalled back with `PostMessage`, `dispatch_async` or `g_idle_add` |
| Loop (library) | libev: server WebSocket, ICE/DTLS/SCTP/RTP, ctrl protocol, input injection, file chunks | `rd_rtc_call` queue |
| Video (library) | capture → encode → `rd_rtc_video()` | atomics for "keyframe now" and bitrate; configuration changes go through a mutex-protected struct |

Input is injected on the loop thread. `SendInput`, `CGEventPost` and XTest (on its own `Display`
connection) all work from any thread, and this keeps input latency independent of encoding.

### 5.4 Session protocol (`desktop/core/session.c`)

This is a C port of the parts of `Session.java` that make sense on a computer. Message names and shapes stay
the same, so the viewer's HUD, quality sheet, keyboard sheet and file sheet keep working:

| ctrl message | Desktop behaviour |
|---|---|
| `session.ready` (sent when ctrl opens) | adds `platform`, `screens` (§8.3) |
| `quality.set`, `quality.state`, `stats` | same; preset 720p/1080p/**native** scales the capture |
| `text` | types Unicode text (IME-safe path, §6) |
| `key` (`press`/`down`/`up` with Android key codes) | the common ones (Enter, Backspace, arrows…) are mapped; the rest return `unsupported` |
| `nav` | `back` → Alt+Left / Cmd+[, `home` → show desktop; the others return `unsupported` |
| `clip.set`, `clip.changed` | system clipboard (§6) |
| `file.*` + `file` channel | port of `Files.java`: uploads go to `[files] upload_dir`, paths limited to the user's home |
| `screen.power` | turns displays off/on (Windows `SC_MONITORPOWER`, macOS `pmset displaysleepnow`, Linux DPMS) |
| `shell.exec`, `logcat.*`, `rotate` | `unsupported` (shell can be enabled with `[desktop] allow_shell`, §12) |
| new: `screen.select {index}` | switches the captured monitor |
| new: `cursor.show {show}` | leaves the pointer out of the captured picture (`false`) for a viewer that draws its own, the trackpad ([`trackpad.md`](trackpad.md) §9); back on at the session's end |

When a session ends, or when the input channel sends reset (0x05), `input.c` releases every key and mouse
button it is holding, and centres the virtual gamepads (unplugged at the session's end). A dropped connection
can't leave Ctrl stuck down.

### 5.5 Video pipeline (`desktop/core/video.c`)

The platform supplies either a **fused source**, where capture and encode both stay on the GPU, or a **raw
capture**, which the core pairs with the shared software encoder:

```c
typedef struct {                       /* fused: Windows DXGI→MF, macOS SCK→VideoToolbox */
    void *(*open)(const rd_video_cfg_t *c, rd_au_cb on_au, void *ud);   /* Annex-B access units */
    void  (*keyframe)(void *s);
    void  (*bitrate)(void *s, int kbps);
    void  (*close)(void *s);
} rd_video_source_t;

typedef struct {                       /* raw: X11, and the fallback everywhere */
    void *(*open)(int screen, int fps, rd_frame_cb on_bgra, void *ud);
    void  (*close)(void *s);
} rd_capture_t;
```

The software path is BGRA → I420 (`yuv.c`, scalar code written to auto-vectorize, with SSE2/NEON later if
profiling calls for it), then **minih264e** (H.264 Constrained Baseline, CC0, about 11k lines in one header,
SSE2/NEON). This matches the existing SDP: `42e02a` Baseline is offered, and a Baseline stream also decodes
on the High payload type. HEVC is offered only when a fused hardware source supports it (VideoToolbox on
Apple Silicon and T2 Macs, Media Foundation where the GPU has an HEVC encoder, VA-API where the GPU has
an HEVC encode entry point).

**VA-API on Linux** (`desktop/linux/video_vaapi.c`): libva and libva-drm are loaded with `dlopen` from the
first `/dev/dri/renderD*` that initializes, so machines without them fall back to software. XShm capture
→ `yuv.c` NV12 at the encode size (cut to multiples of 16) → a VA surface (`vaDeriveImage`, else
`vaPutImage`) → `EncSlice` with one reference, P frames only, CBR, an IDR on request, up to 60 fps. The
parameter sets (H.264 SPS/PPS with POC type 2 and `max_dec_frame_buffering` 1; HEVC VPS/SPS/PPS with
the driver's block sizes from `VAConfigAttribEncHEVCBlockSizes`) and every slice header are written here
and handed over as packed headers: Mesa's radeonsi writes none itself. `make desktop-vatest` checks a GPU
by hand (synthetic frames, or the X screen through `dsk_hwvideo_*`); on a Steam Deck both codecs encode a
1280x800 frame in about 3 ms.

Encoder choice (`[video] encoder = auto|hardware|software`): `auto` tries hardware first and falls back to
software if hardware fails to open or fails 3 times in a session. Behaviour copied from the phone: keyframes
mostly on request (PLI), repeat the last frame after 100 ms on a static screen, bitrate set by the stack's
`on_bitrate`, and capture limited to `max_fps`.

---

## 6. Platform backends

All system APIs, and nothing third-party. Minimum versions: **Windows 10 1809+**, **macOS 13+**
(ScreenCaptureKit, including its sound), **Linux with an X11 session** (glibc 2.31+).

| | Windows (C, COM through `lpVtbl`) | macOS (C / Objective-C) | Linux (C) |
|---|---|---|---|
| Capture | DXGI Desktop Duplication (D3D11), dirty-rect aware | ScreenCaptureKit `SCStream`, NV12 (`420v`) output | XShm `XShmGetImage` (+ XDamage to skip unchanged frames) |
| Colour conversion | `yuv.c` on the CPU (BGRA→NV12, scaled); an `ID3D11VideoProcessor` GPU path is a later optimisation | none needed (SCK delivers NV12, scaled) | `yuv.c` |
| Encode | Media Foundation H.264/HEVC MFT, hardware async first, then the Microsoft software MFT, then minih264 | VideoToolbox `VTCompressionSession` (H.264, HEVC), real-time, no B-frames | VA-API H.264/HEVC through `dlopen` (`video_vaapi.c`: AMD and Intel GPUs, a Steam Deck), then minih264 |
| Mouse | `SendInput` absolute with `MOUSEEVENTF_VIRTUALDESK` | `CGEventCreateMouseEvent` / `CGEventPost` (with drag events, click count for double-clicks) | `XTestFakeMotionEvent` / `ButtonEvent` |
| Keys | `SendInput` with scan codes (`KEYEVENTF_SCANCODE`, `EXTENDEDKEY`) | `CGEventCreateKeyboardEvent` with `kVK_*` | `XTestFakeKeyEvent` with keycode = evdev + 8 |
| Text | `KEYEVENTF_UNICODE` | `CGEventKeyboardSetUnicodeString` | temporary keysym remap of a spare keycode (the xdotool method) |
| Scroll | `MOUSEEVENTF_WHEEL`/`HWHEEL` | `CGEventCreateScrollWheelEvent2` (pixel units) | buttons 4–7 |
| Clipboard | `AddClipboardFormatListener`, `CF_UNICODETEXT` | `NSPasteboard` `changeCount` poll (1 s) | `CLIPBOARD` selection owner/requester (Xlib) |
| Monitors | `EnumDisplayMonitors` / DXGI outputs | `SCShareableContent` displays | XRandR (`XRRGetMonitors`) |
| Permissions | none (runs in the user's session) | Screen Recording (`CGRequestScreenCaptureAccess`), Accessibility (`AXIsProcessTrustedWithOptions`) | none on X11 |
| System CAs for wss:// | `CertOpenSystemStore("ROOT")` → mbedTLS DER | `SecTrustCopyAnchorCertificates` → DER | bundle files (as the router agent does) |
| Lock on disconnect (opt.) | `LockWorkStation()` | `pmset displaysleepnow` (locks when "require password" is on) | `loginctl lock-session` |

**Key mapping** is one shared table in `desktop/core/keymap.c`. The viewer sends USB HID usages, which are
physical keys and independent of layout (§8.2). Each row gives the HID usage, the Windows set-1 scan code
(with the E0 flag), the macOS `kVK_` code, and the Linux evdev code. Because the keys are physical, the
remote computer's own keyboard layout applies, as with a real keyboard plugged into it.

**Coordinates**: the viewer sends 0–65535 across the video frame. The core maps that to the selected
monitor's rectangle in that OS's input space: Windows virtual-desktop 0–65535, macOS global points (HiDPI
aware), X11 root-window pixels.

**Porting the stack to Win32** (`common/plat.h`): Winsock with `WSAStartup`; libev's select backend with
`EV_SELECT_IS_WINSOCKET=1`, and `EV_FD_TO_WIN32_HANDLE` / `EV_WIN32_HANDLE_TO_FD` defined as identity so
`SOCKET` values can be used as fds; `FD_SETSIZE 1024`. Also `closesocket`, `send`/`recv` instead of
`read`/`write` on sockets, `BCryptGenRandom` instead of `getrandom`, `QueryPerformanceCounter` for the
monotonic clock, `GetAdaptersAddresses` instead of `getifaddrs`, and a small thread/mutex/condvar wrapper
(Win32 `SRWLOCK`/`CONDITION_VARIABLE`). usrsctp, libsrtp and mbedTLS already support Windows and macOS.
They need their per-OS defines in `desktop.mk`.

---

## 7. The desktop app: menu bar icon and pairing

The app is a thin UI over `rd_agent.h`, with one source file per platform. It runs as a single instance. A
second launch opens the window of the instance that is already running.

### 7.1 Tray / menu bar

The icon has three looks: grey (not paired, or offline), normal (online), and accent with a dot (**someone
is viewing**). It always shows when the computer is being watched.

```
 ● RemoDroid — Online as "office-pc"          (disabled status line)
   Viewer: alice (since 14:02)                (only during a session)
 ─────────────────────────────────
   Disconnect viewer                          (only during a session)
   View only (block mouse and keyboard)       ✓ toggle
   Open panel                                 → default browser
   Pair with a panel…  /  Pairing & status…   → window
   Show log                                   → log window
 ─────────────────────────────────
   Start at login                             ✓ toggle
   Quit RemoDroid
```

A system notification is shown when a session starts and when it ends ("alice is viewing this computer").

**Show log** (also a button in the pairing window) opens a resizable log window: the agent's last 1000
lines in a monospaced view that follows new lines (unless scrolled up), the path of `desktop.log` (which
keeps everything), **Copy**, and **Open folder** / **Show in Finder**. It reads `rd_agent_log()` whenever
`rd_agent_log_seq()` moves.

### 7.2 Pairing window

```
 ┌──────────────── RemoDroid ────────────────┐
 │  Pair this computer with your panel        │
 │                                            │
 │  Pairing command                           │
 │  [remodroid-desktopd pair --panel https:/…]│
 │  Paste the whole line from the panel:      │
 │  Agents → Add → Computer                   │
 │  Name           [office-pc            ]    │
 │                         [ Cancel ] [ Pair ] │
 ├────────────────────────────────────────────┤
 │  Permissions (macOS only)                  │
 │   ✓ Screen Recording       [Open Settings] │
 │   ✗ Accessibility          [Open Settings] │
 └────────────────────────────────────────────┘
```

When the computer is paired, the same window shows the panel, the device name, the state and the last
problem, plus **Unpair**. The one field takes the whole line the panel shows, like the router's LuCI and
Merlin pages: `rd_agent_pair_line()` → `rd_enroll_parse()` picks out `--panel` and `--code` (or a bare
address and code) from `remodroid-desktopd pair …`, `remodroid enroll …` or the install one-liner, then
calls `rd_agent_pair()` → `common/enroll.c` → `POST /api/v1/enroll`,
the same API the router's `remodroid-agent enroll` uses. It writes `[server]` into `desktop.ini` and
connects.

### 7.3 Per-platform UI technology

| | Windows | macOS | Linux |
|---|---|---|---|
| Language | C | Objective-C (AppKit) | C |
| Tray | `Shell_NotifyIconW` + `TrackPopupMenu` | `NSStatusItem` + `NSMenu`, `LSUIElement` (no Dock icon) | StatusNotifierItem + `com.canonical.dbusmenu` over GDBus; `GtkStatusIcon` (XEmbed) if no SNI watcher is present |
| Window | `CreateDialogIndirect` from `app.rc`, Common Controls v6 (manifest), per-monitor DPI aware | `NSWindow` built in code (no nib) | GTK 3 dialog |
| Notifications | `NIF_INFO` balloon (a toast on Windows 10+) | `UNUserNotificationCenter` | `org.freedesktop.Notifications` (GDBus) |
| Start at login | `HKCU\…\Run` | `SMAppService.mainApp` | `~/.config/autostart/remodroid.desktop` |
| Links against | user32, shell32, comctl32, gdi32, d3d11, dxgi, mfplat, mf, ole32, bcrypt, ws2_32, iphlpapi, crypt32 | AppKit, ScreenCaptureKit, VideoToolbox, CoreMedia, CoreVideo, ApplicationServices, Security, UserNotifications | libgtk-3, libgio-2.0, libX11, libXext, libXtst, libXrandr, libXdamage, libXfixes |

On Linux, "system UI APIs" means GTK 3 and GLib/GIO. They are the desktop's own libraries and ship
preinstalled on every mainstream distribution. Nothing is vendored or bundled (see open decision D1).
`remodroid-desktopd` is a headless binary that links only `libremodroid.so` and the X11 libraries. It is for
machines where you would rather run a systemd user service. Pairing is then done with
`remodroid-desktopd pair --panel … --code …`.

---

## 8. Protocol additions

All additions are backward-compatible. An old viewer still works with a desktop (touch input arrives as a
left click). A desktop-aware viewer still works with phones.

### 8.1 Agent ⇄ server

- `hello.d` gains `platform` (`"android"` from the router agent, `"windows"` / `"macos"` / `"linux"` from
  desktops; missing means `"android"`) and `os` (for example `"Windows 11 23H2"` or `"macOS 15.1"`).
- A desktop's `status.d.phone` describes **the controlled device**. The key stays `phone` for wire
  compatibility. It contains `{attached: true, server: "running"|"stopped", model: <computer name>, os,
  screens: [{w,h,primary}], problem?}`. `problem` carries "Screen Recording permission is off", so the
  viewer's existing overlay shows it. Desktops never send `adb_state`.
- No new message types are needed. `session.*`, `signal`, `cmd` (`agent.config`, `agent.reload`),
  `diag` and `event` are reused.

### 8.2 Input channel (binary, `input`)

The existing 0x01 (touch, 14 bytes) and 0x02 (scroll, 13 bytes) stay. New messages, big-endian like the
existing ones:

```
0x03 mouse (12 bytes)
  0  u8   0x03
  1  u8   action      0 move · 1 button down · 2 button up
  2  u8   button      DOM MouseEvent.button: 0 left · 1 middle · 2 right · 3 back · 4 forward
  3  u8   buttons     held buttons, bit n = button n (the viewer converts MouseEvent.buttons)
  4  u16  x           0..65535 across the video frame
  6  u16  y
  8  u32  ts          viewer ms (low 32 bits)

0x04 key (10 bytes)
  0  u8   0x04
  1  u8   action      0 down · 1 up
  2  u16  usage       USB HID usage, keyboard page 0x07 (from KeyboardEvent.code; 0xE0–0xE7 modifiers)
  4  u16  mods        HID modifier byte in bits 0–7; bit 8 CapsLock; bit 9 NumLock (for resync)
  6  u32  ts

0x05 reset (5 bytes)  release every held key and button (viewer lost focus, tab hidden)
  0  u8   0x05
  1  u32  ts

0x06 gamepad (20 bytes)  one virtual pad's whole state; sent when it changes
  0  u8   0x06
  1  u8   index       0..3 (pad 1..4)
  2  u32  buttons     W3C standard-gamepad bits: 0 A · 1 B · 2 X · 3 Y · 4 LB · 5 RB · 6 LT · 7 RT ·
                      8 View · 9 Menu · 10 LS · 11 RS · 12 up · 13 down · 14 left · 15 right · 16 Guide
  6  i16  lx, ly, rx, ry   sticks, -32767..32767, y down
  14 u8   lt, rt      triggers 0..255 (0 with bit 6/7 set = fully pulled)
  16 u32  ts
```

A pad is plugged in on its first 0x06 and unplugged when the session ends; 0x05 centres it. Backends:
Linux creates a uinput device that is a wired Xbox 360 pad to Steam, SDL and games (`pad_uinput.c`,
`045e:028e`, the xpad driver's buttons and axes), which needs a writable `/dev/uinput` (Steam's
`steam-devices` udev rules; SteamOS has them). Windows would need the ViGEmBus driver, and macOS has no
API for a virtual controller, so both report `caps.gamepad: false` with `caps.gamepad_problem`.

On a desktop, 0x01 touch with pointer 0 maps to left-button down/move/up, which keeps old viewers usable.
0x02 scroll keeps its units: 1/256 of a "notch" of 100 CSS pixels.

### 8.3 ctrl channel

- `session.ready.d` gains `platform`, plus `screens: [{index, w, h, primary, name}]` and `screen` (the
  captured one).
- New `screen.select {index}` → ack, then `screen.changed` as today.
- New `cursor.show {show}` → ack; `session.ready.caps.cursor_hide` says the agent takes it ([`trackpad.md`](trackpad.md) §9).
- `quality.set.mode`: `auto` follows the bandwidth estimate from 300 kbps up to `kbps`; `manual` starts at
  `kbps` and keeps it (also while the screen is idle), easing to `kbps / 2` at most on a congested path.
- `quality.set.preset` accepts `"native"` on desktops. The viewer keeps a computer's resolution apart from a phone's (`S.deskPreset`), Native by
  default: any downscale of a desktop blurs its text (a 1024×768 screen at 720p becomes 960×720).

---

## 9. Web viewer: mouse and keyboard

`web/app.js` selects an input mode from `device.platform`. Phones are unchanged.

**Desktop mode, mouse or trackpad viewer** (`pointerType === 'mouse'` or `'pen'`; with the menu's Trackpad
switch on, a mouse drives [`trackpad.md`](trackpad.md)'s pad instead, as a finger):

- `pointermove` sends 0x03 move, including hover with no button held, and is throttled to one per
  animation frame. `pointerdown`/`up` send 0x03 down/up with `e.button`. `contextmenu` is suppressed so
  right-click reaches the computer. `wheel` sends 0x02 (existing).
- The browser's own pointer stays visible over the video (local cursor, zero latency). The remote cursor is
  included in the captured picture where the OS draws it (macOS SCK `showsCursor`; DXGI and X11 don't, which
  is why the local pointer is the primary one).

**Keyboard**: while the video has focus, `keydown`/`keyup` send 0x04 with the HID usage looked up from
`e.code` (a table of about 120 entries in `app.js`), and `preventDefault()` keeps the browser from acting on
them. In fullscreen on Chromium, `navigator.keyboard.lock()` also captures Esc, Alt+Tab, Cmd+W and similar.
`blur` and `visibilitychange` send 0x05 reset. IME composition (`compositionend`) is sent as ctrl `text`.
A viewer setting **"Mac ⌘ ↔ Ctrl"** (on by default when a Mac browser controls Windows or Linux) swaps the
two modifiers' usages.

**Touch viewer** (iPhone controlling a desktop): a phone gets the trackpad ([`trackpad.md`](trackpad.md)); with
the menu's Trackpad switch off, or Settings → Stream → Touch control: Direct (an iPad's default): tap → left click, long press → right click, one-finger drag
→ left drag, two-finger drag → scroll. The existing keyboard sheet (`#kbText`) types through ctrl `text`. A
new **Keys** sheet has Esc, Tab, Win/⌘, Ctrl+Alt+Del-style combos, F1–F12 and arrows, sent as 0x04
sequences. Local pinch-zoom and pan of the picture come in M5.

**Menu**: the Android-only items (Back/Home/Recents, rotation, screen-off-on-connect, logcat, shell) are
hidden for desktops. Desktops add **Screen** (monitor picker) and **Keys**. The quality sheet gains
`native`.

---

## 10. Server, Worker and panel changes

Small changes, made in both implementations:

- `server/hub.c` `hub_device_json` and `worker/src/hub.js`: add `platform` (top level) and `agent.os` from
  `hello`. Everything else passes through unchanged.
- `web/agent.html` (Agents → Add) gets a **Router / Computer** choice. Router shows the
  `remodroid-agent enroll` command, as today. Computer shows one line,
  `remodroid-desktopd pair --panel … --code …`, to paste into the RemoDroid app's pairing window (or to run in a
  terminal), and download links. The enroll API is unchanged.
- `web/panel.js` device list: a platform icon, and "computer" wording instead of "phone" where it appears.
- The router agent sends `"platform": "android"` in `hello`.

---

## 11. Configuration

`desktop.ini` uses the same inih format and the same `rd_conf_write` editor as `agent.ini`. It is per-user:

| OS | Path |
|---|---|
| Windows | `%APPDATA%\RemoDroid\desktop.ini` |
| macOS | `~/Library/Application Support/RemoDroid/desktop.ini` |
| Linux | `$XDG_CONFIG_HOME/remodroid/desktop.ini` (`~/.config/…`) |

```ini
[server]
url = wss://remo.example.com/ws/agent      ; written by pairing
device_id = office-pc
device_secret = …
ca_file =                                   ; optional; default: the OS trust store

[desktop]
view_only = false            ; ignore mouse and keyboard from viewers
screen = 0                   ; monitor captured at session start
notify = true                ; notifications when a viewer connects or leaves
confirm = false              ; ask the local user to Allow/Deny each session (30 s timeout = deny)
lock_on_disconnect = false
clipboard_sync = true
allow_shell = false          ; ctrl shell.exec (off: unsupported)
log_level = info

[video]
encoder = auto               ; auto | hardware | software
codec = auto                 ; auto | h264 | h265
preset = 1080p               ; 720p | 1080p | native
max_fps = 60
max_bitrate_kbps = 20000
mf_timing = real            ; Windows: real (each frame stands for the time since the last; Microsoft's
                             ; CPU encoder's default) | fixed (the frame rate's; a GPU encoder's default)
mf_max_fps = 30              ; Windows: the frame rate the encoder budgets the bitrate for, at most
                             ; (30 for the CPU encoder by default, none for a GPU's)

[files]
upload_dir = ~/Downloads/RemoDroid
max_mb = 2048
overwrite = false
```

The panel's per-device settings (`agent.config`) map onto these keys the way `phone.c`'s `CFG` table does
for the router agent.

---

## 12. Security

Controlling a computer gives more power than controlling a phone through `shell`, so the defaults are
conservative:

- **Visible**: the icon changes and a notification appears whenever someone is viewing. "Disconnect viewer"
  is always one click away. `confirm = true` makes each session ask the person at the computer first.
- **Credentials**: `device_secret` lives in the per-user `desktop.ini`, created with mode 0600 on
  macOS/Linux and inside the user profile on Windows. Unpair deletes it. The server already supports
  removing a device from the panel.
- **TLS**: `wss://` is verified against the OS trust store (§6), with the hostname checked. Plain `ws://` is
  accepted only for `localhost`/LAN panels, as today.
- **Reach**: file operations are limited to the user's home, and `shell.exec` is off by default. The agent
  runs as the logged-in user without elevation, so it can't reach anything that user can't. On Windows this
  also means elevated (admin) windows don't accept input from it (UIPI). The optional `RemoDroidInput` service
  (`remodroid-input.exe`, installed from the tray menu by an administrator) fixes that: it runs a SYSTEM helper
  in each signed-in session, and the agent sends it its input over `\\.\pipe\RemoDroidInput-<session>`. The
  pipe is owned by SYSTEM (the agent checks, so nothing else can pose as it and read keystrokes) and takes
  only clients from its own session. Trade-off: with it installed, any program of the signed-in user can drive
  elevated windows, as UAC itself does not separate an admin's own programs.
- **The lock screen** (with the service): a locked Windows session shows the lock and sign-in screen on the
  secure desktop (`Winlogon`), which a program running as the user can neither capture nor type into, so the
  viewer used to freeze on the last picture. While Windows reports the session locked
  (`WTSSessionInfoEx` → `WTS_SESSIONSTATE_LOCK`), and only then, the helper attaches to the input desktop to
  inject, and serves the agent pictures of it on `\\.\pipe\RemoDroidScreen-<session>` (same ownership and
  session checks; it answers "not locked" otherwise). The agent checks the lock state twice a second and,
  while locked, encodes the helper's pictures (GDI, about 10 a second, sent only when they change) instead of
  its own capture. So the viewer sees the lock screen and types the Windows password into it; the password
  is still needed, and an unlocked session's secure desktop (a UAC prompt) stays out of reach and out of
  sight.
- **Input safety**: held keys and buttons are released when a session ends or the input channel resets.
  `view_only` is enforced in the library, not in the viewer.
- The WebRTC media is end-to-end DTLS-SRTP between the browser and the computer, as with phones. The
  server/TURN relays only ciphertext.

---

## 13. Build, CI and packaging

### 13.1 Build

`desktop/desktop.mk`, included from the top `Makefile` like `rtc.mk`:

```
make desktop                       host OS: libremodroid + app + headless
make desktop desktop-app TARGET=win-x86_64 CC=gcc    (MSYS2 UCRT64 on Windows)
make desktop desktop-app TARGET=win-arm64 CC=clang WINDRES=llvm-windres   (MSYS2 CLANGARM64)
make desktop desktop-app TARGET=mac CC=clang ARCHFLAGS="-arch arm64 -arch x86_64 -mmacosx-version-min=13.0"
```

Windows builds natively on the Windows runners with MSYS2 (UCRT64 gcc for x86_64, CLANGARM64 clang for arm64),
which also provides `windres` for the manifest and icon, and lets CI run the tests there. Codec property GUIDs
that mingw declares only for C++ are defined locally. macOS requires a macOS runner, because Apple's frameworks can't be
redistributed for cross builds. Linux is built natively on Ubuntu 22.04, for glibc 2.35 compatibility,
against the distribution's X11/GTK development packages.

### 13.2 CI jobs (added to `.github/workflows/build.yml`)

| Job | Runner | Produces | Runs tests |
|---|---|---|---|
| `desktop-linux` (x86_64, aarch64) | ubuntu-22.04 / ubuntu-22.04-arm | `remodroid-desktop`, `remodroid-desktopd`, `libremodroid.so` | `desk_test`, a start-up smoke test, and `tests/e2e_desktop.py` on Xvfb (x86_64) |
| `desktop-windows` (x86_64, arm64) | windows-2022 / windows-11-arm (MSYS2) | `RemoDroid.exe`, `remodroid-desktopd.exe`, `remodroid.dll` | `desk_test.exe`, a check that only system DLLs are imported, and a start-up smoke test |
| `desktop-macos` (universal2) | macos-14 | `RemoDroid.app` | `desk_test`, signature check, a start-up smoke test (capture needs a TCC grant runners don't have) |
| `phone` | unchanged | `libremodroid.so` built from `rtc/` + `host.c` + `jni.c` | unchanged |

### 13.3 Packages

| OS | Files |
|---|---|
| Windows | `RemoDroid-<ver>-windows-x86_64.zip` / `-arm64.zip` (exe + dll). An installer can follow (§13.4) |
| macOS | `RemoDroid-<ver>-macos.zip` (`RemoDroid.app`, universal2, dylib in `Contents/Frameworks`, ad-hoc signed) |
| Linux | `.deb`, `.rpm` (nfpm, as today, with dependencies on the GTK/X11 runtime packages), `.tar.gz`, `.AppImage` (appimagetool; the system GTK/X11 libraries, not bundled); a `.desktop` file, icon, systemd user unit for `remodroid-desktopd` |

`doc/downloads.py` gains a Desktop grid next to the router and server grids, and the README grids are
regenerated after each release (as for the router packages).

### 13.4 Signing (needs your accounts)

- **macOS**: Screen Recording and Accessibility permissions are tied to the code signature. An ad-hoc
  signature changes with every build, so **macOS asks for permission again after every update**, and
  Gatekeeper needs right-click → Open. A Developer ID certificate plus notarization fixes both and can be
  added to the macos job as secrets.
- **Windows**: unsigned binaries trigger SmartScreen warnings. Authenticode signing is optional.

---

## 14. Testing

- **Shared stack**: the existing `rtc_test`, `rtc_interop.py` (aiortc) and `rtp_depack.py` run unchanged
  against `rtc/`. They also run on the Windows and macOS runners, which checks the Win32 port and the
  usrsctp/libsrtp builds there.
- **Embedded mode**: a new `rtc/test/embed_test.c` drives `rd_rtc_*` with `listen = "embed"`, exchanging
  signaling in-process with an aiortc peer.
- **Desktop end to end (Linux, CI and here)**: Xvfb + `remodroid-desktopd` + `remodroid-server` + Playwright
  Chromium (extending `tests/e2e_browser.py`). It checks that video frames decode in the browser and match a
  test pattern drawn on the X display, that a mouse click on the viewer lands at the right X coordinates (a
  tiny X client records events), that key usages arrive as the expected keycodes, and that clipboard
  round-trips and an upload lands in `upload_dir`.
- **Unit tests**: keymap table completeness (every `e.code` in the viewer table maps on each OS), coordinate
  mapping with HiDPI/multi-monitor rectangles, the I420 converter against a reference, and the ctrl protocol
  against recorded viewer sessions.
- **Regression for Android**: `tests/run.mjs` and the phone build stay green. `libremodroid.so` for Android
  must keep exporting the same JNI methods, which is checked with `llvm-nm` in the phone job.
- **Manual (I can't automate these)**: real Windows 10/11 (Intel, AMD, NVIDIA encoders), Apple Silicon and
  Intel Macs (permission flow), GNOME-on-Xorg / KDE / XFCE tray behaviour.

---

## 15. Milestones

Each milestone builds in CI and leaves Android working.

| # | Delivers | Notes |
|---|---|---|
| **M1** Shared library | `android/native` → `rtc/`; `host.c` + `rd_rtc.h`; `jni.c` as adapter; `embed` net mode; `common/plat` Win32 port; `libremodroid` builds for Linux, Windows and macOS; rtc tests on all three | No user-visible change; phone verified on a real device afterwards |
| **M2** Desktop core + Linux headless | `common/wsc.c` + `enroll.c` refactor (router agent switched over); `desktop/core`; software video; X11 capture and XTest input; `remodroid-desktopd`; `platform` in server/Worker; viewer mouse/keyboard mode; Xvfb e2e test | First end-to-end desktop control, testable on this machine |
| **M3** Windows | DXGI + MF fused source, SendInput, clipboard, system CAs, tray app + pairing dialog, zip | |
| **M4** macOS | SCK + VideoToolbox (H.264/HEVC), CGEvent, NSPasteboard, menu bar app + pairing window + permissions, .app | |
| **M5** Linux app and polish | GTK pairing window + SNI tray, notifications, autostart, .deb/.rpm; multi-monitor picker; files on desktops; touch-viewer zoom/pan; panel "Computer" enrollment; README/download grids; `arch.md` section | |
| **M6** Sound | The computer's sound: WASAPI loopback, ScreenCaptureKit audio (so macOS 13+), the PulseAudio/PipeWire monitor via `dlopen`; `desktop/core/audio.c` downmixes, resamples to 48 kHz and encodes Opus (vendored libopus) onto the same track as the phone's (`rtc_audio`); `desktop.audio` setting, the viewer's Sound switch; Xvfb e2e plays a tone into a null sink | Done |
| **M7** Later | Wayland (xdg-desktop-portal ScreenCast + PipeWire via `dlopen`, uinput), VA-API (done), remote cursor shape, unattended mode (Windows service using `WTSGetActiveConsoleSessionId`, macOS LaunchDaemon), signed installers | |

---

## 16. Risks and open decisions

**Risks**

- **Wayland**: Ubuntu, Fedora and Debian GNOME default to Wayland. v1 needs "GNOME on Xorg" or KDE/XFCE on
  X11, and the app says so clearly when it detects a Wayland session (`XDG_SESSION_TYPE`). M6 removes this
  limit.
- **Media Foundation from C**: verbose COM-through-vtables code, and hardware MFTs are asynchronous and
  event-driven with vendor quirks. The Microsoft software MFT and minih264 provide two fallbacks.
- **Software encoding cost**: minih264 at 1080p60 needs about one fast core. The defaults on the software
  path are therefore 1080p30, and `native` is capped at 30 fps.
- **libev on Windows** (select backend, sockets as fds) is a known but less-used configuration. M1 runs the
  full rtc suite on a Windows runner before anything else depends on it.

**Decisions to confirm** (my recommendation first)

- **D1. Linux UI toolkit.** GTK 3 + GIO (the desktop's own libraries, nothing bundled), with a
  StatusNotifierItem tray. *Alternative*: raw Xlib for both window and tray (truly no GTK, but a hand-drawn
  text-entry window, and no tray on GNOME).
- **D2. Software H.264.** Vendor minih264e (CC0, one header) as the universal fallback and the Linux v1
  encoder. *Alternative*: load Cisco's OpenH264 binary at runtime (better quality, but downloaded separately
  for licensing reasons).
- **D3. Unattended access** (login screen, UAC prompts, reboot without logging in) is deferred to M6.
  *Alternative*: build the Windows service and macOS LaunchDaemon in M3/M4 (substantially more work and
  more security review).
- **D4. Moving `android/native` → `rtc/`.** Recommended, for clarity. *Alternative*: leave it in place and
  have the desktop build reach into `android/native/`.
- **D5. Signing**: whether to set up Apple Developer ID and Windows code signing now (needs your
  certificates as CI secrets) or ship unsigned builds first.
