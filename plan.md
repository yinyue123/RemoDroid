# RemoDroid: Low-Latency Android Cloud Gaming — Project Plan (Draft v12)

> **v2 change:** the phone does all the heavy work (capture, encoding, WebRTC). The ARM device can be a weak OpenWrt router. Its only runtime job is a small C relay that reads framed packets from a TCP connection and sends them as UDP packets.
>
> **v3 change:** a detailed WebRTC section (§5) covering a pure-C stack; the phone's Java part is written from scratch, with scrcpy as a reference only.
>
> **v4 change:** H.265 (HEVC) is the main codec, with H.264 as the automatic fallback (§5.5).
>
> **v5 change:** the router agent keeps a WebSocket connection to the AWS server, used only for control messages and keepalive (§4.2, §4.5). The agent is written in C and uses libev for all I/O.
>
> **v6 change:** no TLS. Plain HTTP and `ws://`, secured with HTTP Basic auth plus session cookies (§4.6).
>
> **v7 change:** logins last 30 days by default; this and other settings live in ini files (§4.7).
>
> **v8 change:** the AWS control server is also written in C with libev (§4.3).
>
> **v9 change:** every library is vendored and linked statically into the binaries; nothing is installed on the router, server or phone (§7).
>
> **v10 change:** clipboard sync (§4.8), remote screen off while connected and lock after disconnect (§4.9), hardware decoding on the iPhone (§5.6).
>
> **v11 change:** adb is built into the agent: a small C ADB client talks to the phone over USB directly, so no adb program is installed on the router (§4.2).
>
> **v12 change:** full API definition (Appendix A): REST, viewer and agent WebSockets, DataChannel control, input and file protocols, tunnel control, and the agent CLI.

## 1. Short answer

Yes, this is doable. scrcpy feels slow remotely mainly because of how its stream is carried: it sends raw video over TCP through adb or an SSH tunnel, with no congestion control or bitrate adaptation. Its capture and encoding are fast. WebRTC over UDP fixes the transport part.

Moving the work to the phone is a good choice. A modern phone has a hardware video encoder and far more CPU than a router, and the encryption WebRTC needs at 20 Mbps costs it almost nothing.

## 2. Corrections and constraints

| Idea | Reality | Approach |
|---|---|---|
| Capture the screen from the "block driver" | A non-rooted phone doesn't allow reading the framebuffer or display driver directly. | Run a program pushed with adb as the **shell user** through `app_process`, the same method scrcpy uses. It creates a virtual display that feeds a **hardware MediaCodec encoder**. No root, APK or consent popup is needed, and the same privilege allows touch input injection. |
| Everything on the phone in C | The screen-capture APIs are hidden **Java** APIs, so they can't be called from pure C. Encoding runs on the hardware encoder, so the language doesn't affect CPU use. | Use a **thin Java layer** for capture, encoder setup and input injection, and **C for WebRTC and the network stack**. The router agent is pure C. |
| UDP over adb | **ADB only carries reliable byte streams** (like TCP), not UDP. | Wrap each UDP packet in a frame inside one ADB stream. The router agent unwraps it and sends real UDP. USB is lossless, so this leg adds only about 1–3 ms. A similar approach has worked before: gnirehtet carries network traffic over adb. |

## 3. Architecture

```
┌──────────────────────── Android phone (does all the work) ──────────────────────────┐
│                                                                                     │
│  Java (app_process, shell uid)            C (own WebRTC stack, see §5)              │
│  ┌──────────────────────────┐  frames     ┌──────────────────────────────────┐      │
│  │ virtual display capture  │ ──────────► │ RTP packetize, SRTP/DTLS, ICE    │      │
│  │ MediaCodec H.265 / H.264 │             │ DataChannel (input, photos)      │      │
│  │ audio capture (Opus)     │ ◄────────── │ congestion control → bitrate     │      │
│  │ input injection          │  commands   │ "UDP socket" = tunnel backend    │      │
│  │ photo → DCIM + scan      │             └───────────────┬──────────────────┘      │
│  └──────────────────────────┘                             │ framed packets          │
└───────────────────────────────────────────────────────────┼─────────────────────────┘
                                                            │ USB (built-in ADB client)
                                              ┌─────────────▼─────────────┐
                                              │ OpenWrt router: agent (C) │
                                              │ built-in ADB over USB     │
                                              │ relay: TCP frame ⇄ UDP    │
                                              │ WebSocket client (ws://)  │
                                              └─────┬───────────────┬─────┘
                        control + keepalive (ws://) │               │ media: UDP (SRTP, already encrypted)
                                              ┌─────▼───────────────▼─────┐        ┌──────────────┐
                                              │ AWS: control server + web │ ◄────► │ iPhone       │
                                              │ coturn TURN (fallback)    │  UDP   │ Safari (PWA) │
                                              └───────────────────────────┘        └──────────────┘
```

The phone is the WebRTC peer. The router relays packets without understanding them: they are already encrypted, so it never decodes video. Input and photos go **directly from the iPhone to the phone** over the WebRTC DataChannel, so they don't depend on the router's WebSocket. The router agent's WebSocket to AWS carries only control messages and keepalives (§4.5).

### 3.1 Two ways to route the UDP traffic

| | **Path A — tunnel through the router** (the v2 request) | **Path B — phone uses its own Wi-Fi** |
|---|---|---|
| Media path | Phone → ADB stream over USB → router agent → UDP | Phone → Wi-Fi → UDP |
| Router work at runtime | Agent relays packets (some CPU use on a weak router) | Nothing; ADB is used only to start and watch the server |
| Latency | +1–3 ms (USB) | Lowest |
| Socket backend in our C stack | Tunnel backend (see §5.4) | Plain UDP socket |
| Works without Wi-Fi on the phone | Yes | No |

**Recommendation:** build Path A because it is the requested design, but put the socket layer behind an interface so Path B is a one-line config change. Use whichever path measures faster on your hardware. If the OpenWrt router is also your home Wi-Fi access point, Path B will most likely win.

## 4. Components

### 4.1 Android server (Java + native C)
- **Java part**, about 1–1.5k lines, **written from scratch** for your phone's Android version, using scrcpy-server (Apache 2.0) **only as a reference** for hidden API signatures. No scrcpy code or binaries are used directly:
  - virtual display capture
  - MediaCodec encoding set up for low latency: **H.265 (HEVC) by default, H.264 as fallback**
  - audio capture (Android 11+)
  - input injection
  - live control of bitrate, resolution, FPS and keyframe requests
- **Native part** (`libremodroid.so` loaded through JNI, or a separate `rtcd` executable connected through a unix socket if JNI loading under `app_process` turns out to be restricted; this gets verified in M0):
  - A **pure-C WebRTC stack** (see §5): our own SDP, RTP, RTCP, congestion control and DataChannel glue on top of libjuice, mbedTLS, libsrtp2 and usrsctp.
  - Packages H.265 or H.264 into RTP, receives Safari's feedback (NACK, PLI, TWCC), and adapts the bitrate.
  - Two socket backends: real UDP (Path B) and the ADB tunnel (Path A).
- Photos received over the DataChannel are written to `/sdcard/DCIM/RemoDroid/`, then a media scan is triggered so they show up in the Gallery.

### 4.2 Router agent (pure C)

One small program **written in C** on the router with three jobs, all in a single thread driven by **libev**. It is built as **one static binary** with musl for the router's CPU (mips/mipsel/arm/aarch64). libev, inih, cJSON and picohttpparser are compiled into it, so nothing has to be installed with `opkg` (§7.1). It needs no TLS library and is supervised by a procd init script.

**Event loop (libev)**

| Watcher | Used for |
|---|---|
| `ev_io` | the usbfs file descriptor (completed USB transfers); the netlink socket (USB hotplug); one per UDP relay socket; the WebSocket's TCP socket; the unix socket for the `remodroid-agent shell/push/...` commands |
| `ev_timer` | WebSocket ping (20 s) and pong timeout (60 s), `status` heartbeat (30 s), reconnect backoff, tunnel `PING` to the phone |
| `ev_signal` | `SIGTERM` (clean shutdown for procd), `SIGHUP` (reload config) |

Rules for the loop:
- **All sockets non-blocking**, including the WebSocket's plain TCP socket.
- **Phone detection is event-driven**: the kernel reports USB plug/unplug through netlink, so there is no polling.
- **Low-latency backpressure:** each outgoing direction has a small bounded queue. If the USB link or a UDP socket can't keep up, **old media packets are dropped instead of queued**. Late video is worse than lost video, and WebRTC recovers from loss with NACK and keyframes. Control (`CTRL`) frames are never dropped.

**1. Built-in ADB client (no adb program on the router)**

The agent talks to the phone's `adbd` **directly over USB**, using the ADB wire protocol. There is no `adb` binary, no adb server process and no libusb. AOSP's own adb can't be used as a library: it is C++ that depends on many internal Android libraries. So we implement only the small part of the protocol we need, about **1.5–2k lines of C**, following AOSP's `protocol.txt`, with AOSP's adb source and scrcpy as references.

| Piece | How |
|---|---|
| USB transport | Linux **usbfs** (`/dev/bus/usb/BBB/DDD`) ioctls: find the interface with class `0xff`, subclass `0x42`, protocol `0x01`; claim it; send and receive bulk transfers with `USBDEVFS_SUBMITURB` / `REAPURBNDELAY`. The usbfs file descriptor is pollable, so it sits in the libev loop like any socket. |
| Hotplug | A netlink `NETLINK_KOBJECT_UEVENT` socket in the libev loop: the kernel reports USB add/remove events, so there is no polling. |
| Messages | 24-byte header + payload: `CNXN`, `AUTH`, `OPEN`, `OKAY`, `WRTE`, `CLSE`. Protocol version `0x01000001`, max payload 256 KB, with per-stream flow control. |
| Authentication | RSA-2048. On first run the agent generates a key and stores it as `/etc/remodroid/adbkey`. The phone shows **"Allow USB debugging?"** once; tick *Always allow*. Signing uses **mbedTLS** (only the bignum/RSA/SHA-1 modules, about 60–100 KB) compiled into the agent. |
| `shell:` service | Runs commands on the phone: start the phone server via `app_process`, read Android version/model, `input keyevent` and so on. |
| `sync:` service | Pushes files (`SEND`/`DATA`/`DONE`): `remodroid-server.jar` and `libremodroid.so` to `/data/local/tmp/`, skipped when the phone already has the same version (checked with `STAT` + size/mtime). |
| Tunnel stream | `OPEN("localabstract:remodroid")` connects **straight to the phone server's socket**. No `adb forward` and no local TCP port; the tunnel frames below travel inside this ADB stream. |

What this gains:
- One file on the router and nothing to install.
- **Less CPU and latency:** with a normal adb, every packet would go agent → TCP → adb server → USB. Now it goes agent → USB directly, which removes one process and two copies per packet on a weak router.

Built-in debugging commands (so you don't miss the `adb` command line). Only one program can claim the USB interface at a time, so these talk to the running agent through a unix socket:
```
remodroid-agent devices                 # list connected phones
remodroid-agent shell "getprop ro.build.version.release"
remodroid-agent push local.file /sdcard/Download/
remodroid-agent logcat                  # stream the phone's logcat
```

**Supervision**
- Detects the phone, pushes and starts the phone server, and restarts it if it exits.
- Recovers after the USB cable is unplugged and re-plugged.
- Screen power, wake and lock are handled by the phone server itself (§4.9).

**2. UDP relay (media, Path A)**
- Unwraps `UDP_SEND` frames from the phone and sends them as real UDP; wraps incoming UDP as `UDP_RECV` frames.
- One UDP socket per tunnel socket ID. It never looks inside the packets.

**3. WebSocket client (control + keepalive only)**
- Keeps one persistent plain `ws://` connection to the AWS control server (no TLS; see §4.6). **No media and no photos go over it**; those travel over WebRTC.
- Carries only small JSON control messages (§4.5) and keepalives.
- Forwards control messages that are meant for the phone (for example WebRTC signaling) to the phone over the ADB tunnel stream as `CTRL` frames, and the phone's replies back up.
- Implementation: a minimal RFC 6455 client written by us (~400 lines: HTTP upgrade with Basic auth and cookie headers, framing, masking, ping/pong) on a plain TCP socket.

**Tunnel frame format** (big-endian, sent inside the `localabstract:remodroid` ADB stream):
```
u16 len | u8 type | u8 sock_id | u8 af | addr (4 or 16 bytes) | u16 port | payload
type: 0x01 UDP_OPEN   0x02 UDP_SEND (phone→router)   0x03 UDP_RECV (router→phone)
      0x04 UDP_CLOSE  0x20 PING   0x21 PONG
      0x30 CTRL       (payload = one JSON control message; sock_id/af/addr/port unused)
```
The phone no longer needs its own TCP connection to AWS: all of its signaling goes through the agent's WebSocket.

### 4.3 AWS
- **Control server**, **written in C with libev**, the same style as the router agent:
  - One single-threaded libev loop: `ev_io` for the listening socket and every client connection, `ev_timer` for WebSocket pings, session expiry sweeps and login-lockout expiry, `ev_signal` for `SIGHUP` (reload `server.ini`) and `SIGTERM`.
  - **HTTP/1.1:** request parsing with **picohttpparser** (one small C file, MIT). Static PWA files are served from a directory with `sendfile()`. Endpoints: `GET /` (Basic auth → cookie), `POST /logout`, `/ws/agent`, `/ws/viewer`.
  - **WebSocket:** the same RFC 6455 framing code as the agent (shared `common/` library), here in server mode.
  - **JSON:** **cJSON** (C, MIT) for the control messages in §4.5.
  - **Password hashing:** bcrypt via openwall **crypt_blowfish** (C, public domain); session IDs from `getrandom()`.
  - **TURN credentials:** HMAC-SHA1 over `<expiry>:<user>` with coturn's shared secret (coturn REST API format). HMAC-SHA1 comes from a small bundled implementation, so the server has no OpenSSL dependency.
  - Built as **one static binary** (musl) with all its libraries compiled in, and run as a systemd service on the EC2 instance. No Docker and no `apt` packages needed (§7.1).
  - Plain HTTP only, no TLS (§4.6).
  - `ws://…/ws/agent`: one connection per router agent, authenticated with HTTP Basic auth, then a session cookie.
  - `ws://…/ws/viewer`: one connection per Safari session, authenticated with the session cookie set at login.
  - Routes control messages between viewers and agents, tracks which devices are online, issues short-lived TURN credentials, and serves the PWA.
- **coturn:** used only when a direct path between the router's public IP and the iPhone isn't possible, for example on carrier NAT (CGNAT). Video passes through without being decoded.

### 4.4 Web client (iPhone Safari)
- TypeScript PWA, added to the Home Screen so it can run fullscreen.
- Uses the REST API, the viewer WebSocket and the DataChannel APIs in Appendix A.
- Video element, multi-touch capture, quality menu, stats overlay (RTT, fps, bitrate, packet loss, decoder), Back/Home/Recents buttons, keyboard, clipboard bar and Paste button (§4.8), and a remote screen on/off toggle (§4.9).
- **Hardware decoding:** see §5.6.
- Photo upload with `<input type="file" accept="image/*">`, which offers both Camera and Library. Photos are sent over the DataChannel in chunks with acknowledgments, so the server isn't involved.

### 4.5 Control protocol (agent ⇄ server WebSocket)

Every message is one JSON text frame with the same envelope:
```json
{"t": "<type>", "id": 42, "sid": "<session id, if any>", "d": { ... }}
```

The full message list is in **Appendix A.5**; device detection events are in A.4, and the router ⇄ phone messages in A.9.

Keepalive:
- **WebSocket ping every 20 s**; the agent reconnects if no pong arrives within 60 s. This also keeps the home router's NAT entry for the TCP connection alive.
- **`status` heartbeat every 30 s**, so the server and the web page always show whether the phone is online.
- **Reconnect** with exponential backoff (1 s → 30 s, with random jitter). Running WebRTC sessions continue during a short WebSocket outage, because media and input don't depend on it.

Not on this WebSocket: video, audio, touch input, quality changes and photos. These go over WebRTC directly between the iPhone and the phone, so they never wait on a TCP path through AWS.

### 4.6 Security without TLS: plain HTTP, Basic auth and cookies

As requested, **no TLS** is used for the web page, the API or the WebSockets: everything is `http://` and `ws://` on the AWS server. The router agent then needs no TLS library. WebRTC itself still encrypts video, audio, input and photos (DTLS-SRTP is mandatory in every browser and can't be turned off).

**Viewer (Safari)**
1. Safari opens `http://<aws-host>/`. The server answers `401` with `WWW-Authenticate: Basic realm="RemoDroid"`, and Safari shows its built-in login prompt.
2. On a correct username/password, the server creates a random 128-bit session ID and sets
   `Set-Cookie: rd_session=<id>; HttpOnly; SameSite=Strict; Path=/; Max-Age=2592000`.
   The login lasts **30 days by default**; change `session_days` in `server.ini` (§4.7).
3. Every later request, including the `ws://…/ws/viewer` upgrade, is authenticated by that cookie. A cookie is needed here because browsers can't add an `Authorization` header to a WebSocket, but they do send cookies with it automatically.
4. On the WebSocket upgrade the server also checks the `Origin` header, so other websites can't open a connection with your cookie.

**Agent (router)**
1. The agent opens `ws://<aws-host>/ws/agent` with `Authorization: Basic base64(<device-id>:<device-secret>)`.
2. The server answers with its own `Set-Cookie` session. On reconnects the agent sends only the cookie, and falls back to Basic auth when the session has expired, so the password crosses the network as rarely as possible.

**Server side**
- Passwords and device secrets are stored hashed (bcrypt).
- Failed Basic auth attempts are rate-limited per IP, with a temporary lockout.
- Sessions can be revoked from the server (logout, or disabling a device).
- TURN credentials stay short-lived (coturn's time-limited REST credentials).

**What this does and doesn't protect.** See the risks in §10.

### 4.7 Configuration (ini files)

Both the AWS server and the router agent read a plain ini file. The server reloads its file on `SIGHUP` or restart. The agent reloads on `SIGHUP` through its libev `ev_signal` watcher. Both use the same ini parser, **inih** (one small C file, BSD license), from the shared `common/` library.

**`/etc/remodroid/server.ini` (AWS)**
```ini
[http]
listen = 0.0.0.0:80

[auth]
; How long a login stays valid. Default 30 days.
session_days = 30
cookie_name = rd_session
; Failed Basic auth attempts per IP before a temporary lockout
max_failed_logins = 5
lockout_minutes = 15

[users]
; username = bcrypt hash (generated with: remodroid-server hashpw)
me = $2b$12$...

[devices]
; device id = bcrypt hash of the device secret
router1 = $2b$12$...

[turn]
urls = turn:<aws-host>:3478?transport=udp
shared_secret = <same as coturn static-auth-secret>
credential_ttl_minutes = 60

[keepalive]
ping_seconds = 20
timeout_seconds = 60
```

**`/etc/remodroid/agent.ini` (router)**
```ini
[server]
url = ws://<aws-host>/ws/agent
device_id = router1
device_secret = <secret>

[keepalive]
ping_seconds = 20
timeout_seconds = 60
heartbeat_seconds = 30
reconnect_max_seconds = 30

[phone]
serial =              ; empty = first phone found on USB
path = auto           ; auto | tunnel (Path A) | wifi (Path B)
screen_off_on_connect = true
lock_on_disconnect = true
lock_grace_seconds = 10
clipboard_sync = true
overheat_c = 45        ; phone.overheat event above this battery temperature

[video]
codec = auto          ; auto | h265 | h264
max_bitrate_kbps = 20000
max_fps = 60

[files]
max_mb = 2048         ; largest file for push/pull
```

**Session lifetime:** the cookie's `Max-Age` is taken from `session_days` (30 days = `Max-Age=2592000`). The server stores each session's login time and checks it against the *current* `session_days`, so changing the value takes effect immediately for existing logins too, not only for new ones.

### 4.8 Clipboard sync

Text clipboard is synced both ways over the WebRTC DataChannel as `clip.set` messages (`{"t":"clip.set","d":{"text":"…"}}`). It doesn't go through AWS. Images are a possible later addition.

**Android side (phone server, shell user)**
- It reads and writes the clipboard through the hidden `IClipboard` service, the same way scrcpy does; the shell user is allowed to.
- It registers a clipboard-change listener. When something is copied on Android, the new text is sent to the iPhone.
- On `clip.set` from the iPhone it sets the Android clipboard. Optionally it also injects a paste (`KEYCODE_PASTE`) into the focused field, using the **"Paste to Android"** button.
- It suppresses echo: text that just arrived from the iPhone isn't sent back.
- Android 12+ may show a small "pasted from your clipboard" notice when the shell reads the clipboard. That's harmless.

**iPhone side (Safari)**
- Safari only lets a page touch the clipboard during a user tap. On plain `http://` the modern `navigator.clipboard` API isn't available at all, because it requires a secure (HTTPS) page. So clipboard sync uses the older methods that still work over HTTP:
  - **Android → iPhone:** when Android copies text, the page shows a small bar, *"Android copied: …  [Copy]"*. One tap copies it to the iPhone clipboard using `document.execCommand('copy')`.
  - **iPhone → Android:** a **Paste** button opens a small text field. A long-press → Paste (or the keyboard's paste) fires a `paste` event, and the text is sent to Android.
- With HTTPS this would be one tap shorter (`navigator.clipboard.readText()`), but it would still need a tap; Safari never allows silent clipboard access.

### 4.9 Remote screen power and locking

The phone server controls the phone's physical screen. scrcpy is used as the reference for the APIs, which differ by Android version.

**Turn the remote screen off after connecting**
1. When a viewer connects, the phone is woken (`KEYCODE_WAKEUP`) so capture starts on a live display.
2. As soon as the first video frame has been sent, the **physical panel is switched off** with `SurfaceControl.setDisplayPowerMode(POWER_MODE_OFF)`. On Android 14+ this uses `DisplayControl`, loaded from `services.jar`. The phone keeps running and capture continues; only the panel is dark. This saves battery and heat and keeps the screen private.
3. The phone is kept awake for the whole session, because a dark panel must not let it fall asleep.
4. The setting is re-applied after events that turn the panel back on, such as rotation or the power button. The web UI gets a **"Screen on/off"** toggle for when you're next to the phone.

**Lock the remote screen after disconnecting**
1. A session counts as ended when the viewer closes it, or when WebRTC loses the connection (ICE disconnected or failed). A **grace period** (default 10 s) avoids locking during a brief network blip; if you reconnect within it, nothing happens.
2. When the grace period expires, the phone server restores the panel power mode, releases the wake lock, and injects `KEYCODE_SLEEP`. The phone goes to sleep and **locks according to its own lock settings**. A PIN, pattern or password must be set on the phone, and "Power button instantly locks" should be on.
3. On the next connection the phone wakes onto the lock screen, and you unlock it remotely by entering the PIN through the stream.
4. **Crash safety:** a tiny separate cleanup process, started with the phone server, waits for the server to exit. If the server crashes or the USB cable is pulled, the cleanup process restores the panel and locks the phone, so it is never left dark and unlocked. scrcpy uses the same pattern.

**Settings:** `screen_off_on_connect`, `lock_on_disconnect`, `lock_grace_seconds` and `clipboard_sync` in the `[phone]` section of `agent.ini` (§4.7). The agent passes them to the phone server when it starts it.

## 5. WebRTC stack on the phone

### 5.1 What "WebRTC" actually consists of

Safari speaks a fixed set of protocols. The phone must implement every layer below:

| # | Layer | Standard | What it does | Hard part |
|---|---|---|---|---|
| 1 | Signaling + SDP | RFC 8866 / JSEP | Offer/answer text describing codecs, ICE credentials, DTLS fingerprint | Not hard; we only need a small fixed subset |
| 2 | ICE / STUN / TURN | RFC 8445 / 8489 / 8656 | Finds a working UDP path (direct, or relayed via AWS) and keeps it alive | Many edge cases (nomination, consent checks, TURN auth) |
| 3 | DTLS 1.2 | RFC 6347 / 5764 | Handshake that proves identity (fingerprint) and derives SRTP keys | **Cryptography, never write it ourselves** |
| 4 | SRTP / SRTCP | RFC 3711 | Encrypts every media packet | Cryptography |
| 5 | RTP packetization | RFC 7798 (H.265), RFC 6184 (H.264), RFC 7587 (Opus) | Splits frames into ≤1200-byte packets (H.265 FU type 49 / H.264 FU-A) | Easy |
| 6 | RTCP feedback | RFC 4585 / 5104, TWCC draft | NACK → retransmit, PLI → keyframe, reports | Medium |
| 7 | Congestion control | GCC (draft-ietf-rmcat-gcc) | Estimates available bandwidth → drives encoder bitrate ("Auto" quality) | **Hardest to tune**; key to low latency |
| 8 | SCTP + DCEP | RFC 8831 / 8832 | DataChannel for touch input and photo upload | SCTP is a full transport protocol |

### 5.2 Options

| Option | Language | Work for us | Maturity | Tunnel support |
|---|---|---|---|---|
| **A. libdatachannel** | C++17 core with C API (its dependencies are C) | Least | High | Patch libjuice's socket layer |
| **B. Pure-C assembly** *(recommended for pure C)* | **100% C** | Medium: we write layers 1, 5, 6, 7, 8-glue (~4–6k lines) | Proven libs for the hard parts | Native: our code owns the sockets |
| C. libpeer | Pure C, embedded-oriented | Low at first | Low: weak congestion control and TURN | Patch needed |
| D. Everything from scratch incl. DTLS/SRTP | C | Very high | — | — |

**Option B in detail** — use C libraries only for crypto and the two big protocols, write the rest:

| Layer | Source |
|---|---|
| 1 Signaling + SDP | **Write** (~400 lines; fixed template, parse only what Safari sends) |
| 2 ICE / STUN / TURN | **libjuice** (C, MPL-2.0), with our tunnel socket backend; or **write** a minimal ICE agent (~1.5k lines) since we control both ends' expectations |
| 3 DTLS | **mbedTLS** (C, Apache-2.0) |
| 4 SRTP | **libsrtp2** (C, BSD) |
| 5 RTP packetizers | **Write** (~500 lines for H.265 + H.264 + Opus) |
| 6 RTCP | **Write** (~800 lines): NACK with a retransmit buffer, PLI/FIR → request keyframe, SR/RR |
| 7 Congestion control | **Write** (~1–1.5k lines): start with loss + RTT based, then delay-based using TWCC feedback (Safari supports `transport-cc`) |
| 8 DataChannel | **usrsctp** (C, BSD) + **write** DCEP (~300 lines) |

Why not option D: DTLS and SRTP are cryptography. Writing them ourselves would be slow and almost certainly insecure, and mbedTLS/libsrtp are small, audited C libraries that fit the "pure C" goal anyway.

### 5.3 Safari specifics to target
- Video: **H.265 Main profile** first in the offer, **H.264** (Constrained Baseline or High, `packetization-mode=1`) second; no B-frames. See §5.5.
- Phone is the **offerer** (ICE controlling); Safari answers.
- RTP header extensions: `transport-cc`, `abs-send-time`; try `playout-delay` (min=0) to shrink Safari's buffer.
- Bundle everything on one UDP 5-tuple (`a=group:BUNDLE`, `rtcp-mux`) → one tunnel socket, fewer TURN allocations.

### 5.4 Tunnel socket
With option B our own code owns the UDP layer, so the tunnel (Path A) is just a second socket backend: `sendto()` writes a `UDP_SEND` frame, incoming `UDP_RECV` frames are fed back in. STUN through the tunnel still discovers the router's public address, so direct peer-to-peer keeps working. Fallback if things go wrong: Go/Pion, which supports pluggable networking natively.

M0 includes a spike: DTLS + SRTP handshake with Safari using option B, through the relay.

### 5.5 Codec strategy: H.265 main, H.264 fallback

**Why H.265:** about 30–50% fewer bits than H.264 for the same picture quality, so better quality at the same upload speed, or the same quality at a lower bitrate. iPhones have hardware H.265 decoding, and most Android phones from the last several years have hardware H.265 encoders.

**How the codec is chosen, automatically, per session:**
1. **Phone checks its encoder:** at startup the Java layer looks for a hardware `video/hevc` encoder (skipping software-only encoders, which are too slow) and tests whether it accepts the low-latency settings. If it doesn't, the phone offers only H.264.
2. **SDP negotiation:** the phone's offer lists H.265 first and H.264 second:
   ```
   m=video 9 UDP/TLS/RTP/SAVPF 96 97 98 99
   a=rtpmap:96 H265/90000
   a=fmtp:96 profile-id=1;level-id=123;tx-mode=SRST
   a=rtpmap:97 rtx/90000
   a=fmtp:97 apt=96
   a=rtpmap:98 H264/90000
   a=fmtp:98 profile-level-id=42e01f;packetization-mode=1;level-asymmetry-allowed=1
   a=rtpmap:99 rtx/90000
   a=fmtp:99 apt=98
   ```
   Safari's answer keeps the codecs it supports. If H.265 remains, we use it; otherwise H.264.
3. **Runtime fallback:** if Safari reports no decoded frames within ~2 s, or the H.265 encoder fails mid-session, the phone renegotiates to H.264 automatically. The quality menu also gets a manual **Codec: Auto / H.265 / H.264** switch.

**Implementation notes:**
- **Parameter sets:** MediaCodec delivers them once at the start (`csd-0`). We store them and send them before every keyframe (VPS/SPS/PPS for H.265, SPS/PPS for H.264), so Safari can recover after packet loss or a resolution change.
- **Packetizer:** H.265 uses a 2-byte NAL header and its own fragmentation unit type (49), so it needs a separate packetizer from H.264. Both share the same RTP, NACK and congestion-control code.
- **Encoder latency:** some phones' H.265 encoders buffer more frames than their H.264 encoders. M0 measures both on your phone; if H.265 adds noticeable delay, "Auto" prefers H.264 on that phone.
- **iOS support:** H.265 over WebRTC works only on recent Safari versions. Older iOS automatically gets H.264. M0 confirms it on your iPhone.

### 5.6 Hardware decoding on the iPhone

Safari decodes WebRTC H.265 and H.264 with the iPhone's **hardware decoder** (VideoToolbox) automatically. No web API turns this on or off, but Safari silently **falls back to software decoding** when the stream is outside what the hardware supports. Software decoding is slower, adds latency and drains the battery. So "enable hardware decoding" means keeping the stream inside the hardware's limits and checking that it stays there:

1. **Stay inside hardware limits:**
   - H.265 **Main** profile (8-bit 4:2:0), level ≤ 5.1.
   - H.264 Constrained Baseline or High, level ≤ 5.1.
   - Width and height multiples of 16 (the encoder setup rounds to these).
   - No B-frames, no unusual chroma formats or 4:4:4.
2. **Ask Safari before choosing a codec:** the web client calls `navigator.mediaCapabilities.decodingInfo({type: "webrtc", video: {contentType: "video/H265", width, height, framerate, bitrate}})` for each codec. Only codecs reported as `powerEfficient: true` (meaning hardware) are kept in the answer, using `setCodecPreferences`. If H.265 isn't hardware-decoded on that iPhone, the session uses H.264 automatically.
3. **Verify during the session:** the stats overlay shows the decoder from `getStats()` (`decoderImplementation` / `powerEfficientDecoder`, where Safari reports them) together with decode time per frame. If decoding turns out to be software, the client asks the phone to switch codec or lower the resolution.
4. **Encoding stays in hardware too:** the phone only uses hardware MediaCodec encoders and skips software ones such as `c2.android.*` and `OMX.google.*` (§5.5).

M0 checks which of these reports your iOS version provides.

## 6. How each requirement is handled

- **Low latency:**
  - Encoder: `KEY_LATENCY=1`, real-time priority, no B-frames, keyframes sent on request, and intra-refresh where supported.
  - Frames are sent as soon as they're encoded.
  - Safari's jitter buffer is kept as small as it allows.
  - Target: **40–80 ms** end to end on a direct path, and about **70–120 ms** through TURN. Choose the AWS region closest to home.
- **Resolution, bitrate and FPS (similar to YouTube):**
  - Presets: Auto / 1080p / 720p / 540p / 360p, 1–20 Mbps, and 30/60/90/120 fps.
  - Bitrate changes are seamless. Resolution changes restart the encoder, causing a glitch of roughly 100–300 ms.
  - Auto mode adjusts bitrate based on network feedback and lowers resolution only when needed.
  - This is easier now because the encoder and WebRTC run in the same process.
- **Input:** touch events normalized to 0–1, multi-touch, and keys, sent over a reliable, ordered DataChannel and injected directly on the phone.
- **Photos:** iPhone camera or library → DataChannel → phone → DCIM → media scan. HEIC is converted to JPEG when needed and EXIF is kept.
- **Clipboard sync:** text both ways over the DataChannel, with one tap on the iPhone side (§4.8).
- **Remote screen:** panel off while connected; sleep and lock after disconnect, with a 10 s grace period (§4.9).
- **Hardware decoding:** H.265 and H.264 decoded by the iPhone's hardware decoder; stream parameters kept within its limits and verified (§5.6).

## 7. Tech stack and static builds

| Part | Language | Libraries (all compiled in statically) | Output |
|---|---|---|---|
| Phone capture/encode/input | Java, written from scratch (scrcpy as reference only) | none (Android framework only) | `remodroid-server.jar` |
| Phone WebRTC | C | libjuice, mbedTLS, libsrtp2, usrsctp, cJSON | `libremodroid.so` (or static `rtcd` executable) |
| Router agent | C | libev, inih, cJSON, picohttpparser, mbedTLS (RSA/SHA-1 subset for ADB auth) | `remodroid-agent` (one static binary) |
| AWS control server | C | libev, inih, cJSON, picohttpparser, crypt_blowfish, small SHA-1/HMAC | `remodroid-server` (one static binary) |
| Web | TypeScript PWA | none at runtime (compiled to plain JS/HTML/CSS) | static files served by `remodroid-server` |

### 7.1 Static build rules
- **Every library is vendored** as source in `third_party/` at a pinned version, built by our Makefiles, and **linked statically** into our binaries. Nothing is installed with `opkg`, `apt` or any package manager for these libraries.
- **Router agent and AWS server:** fully static executables linked against **musl** (`-static`). musl is used instead of glibc because static glibc binaries break DNS lookups (`getaddrinfo`) and other NSS features. Each is a single file you copy and run.
- **Phone:** Android only allows linking against its own system libraries dynamically, and those exist on every phone (`libc`, `libm`, `libdl`, `liblog`). All our third-party libraries are linked statically into `libremodroid.so`, so the phone needs nothing installed. The agent pushes the files automatically.
- **Size control:** build with `-Os -ffunction-sections -fdata-sections -Wl,--gc-sections` and `strip`, and compile mbedTLS with only the features DTLS-SRTP needs, so the router binary stays small. Estimate: agent ~300–500 KB, server ~1 MB, `libremodroid.so` ~1.5–2.5 MB.
- **Reproducible toolchain:** a `Dockerfile` in `build/` pins every toolchain (musl cross-compilers for the router's CPU and the server's CPU, Android NDK, JDK + `d8`). `make dist` inside it produces all binaries. The build machine needs only Docker; the router, server and phone need nothing extra.

### 7.2 Deployment

| Target | What you copy | Runs as |
|---|---|---|
| Router | `remodroid-agent`, `agent.ini`, the phone files (`remodroid-server.jar`, `libremodroid.so`), procd init script | procd service |
| AWS | `remodroid-server`, `server.ini`, `web/` files, systemd unit | systemd service |
| Phone | nothing: enable USB debugging and accept the "Allow USB debugging" prompt once | pushed and started by the agent's built-in ADB client |

### 7.3 External programs (not libraries)
- **adb:** not needed. Its functionality is built into the agent (§4.2).
- **coturn** on AWS, the TURN relay, is still a separate program. See decision 4 in §11.
- **Router kernel:** USB host support is part of the router firmware (kernel modules such as `kmod-usb-core` plus `kmod-usb2` or `kmod-usb3`), so it can't go into our binary. Firmware for routers with a USB port almost always includes it; M0 checks that `/dev/bus/usb` exists.

## 8. Repository layout

```
remodroid/
  android/
    java/          capture, encoder, input (→ server.jar)
    native/        WebRTC core, tunnel backend (→ libremodroid.so / rtcd)
  common/          shared C code: WebSocket framing, control-message JSON (cJSON), ini (inih), logging
  agent/           C + libev router agent (built-in ADB client, UDP relay, WebSocket client) + procd init
  server/          C + libev control server (HTTP, WebSocket, auth, TURN credentials) + systemd unit, coturn.conf
  web/             TypeScript PWA (built to static files)
  third_party/     vendored library sources at pinned versions: libev, inih, cJSON, picohttpparser,
                   crypt_blowfish, libjuice, mbedtls, libsrtp2, usrsctp
  build/           Dockerfile with all toolchains; top-level Makefile → dist/
  docs/
```

## 9. Milestones

| # | Milestone | Done when |
|---|---|---|
| M0 | **Feasibility checks** | a static `remodroid-agent` built with musl runs on the router; its built-in ADB client finds the phone through usbfs, passes RSA auth, pushes a file, runs a shell command and opens a `localabstract:` stream; a minimal from-scratch Java prototype starts from the router through `app_process` and captures + encodes the screen; low-latency encoding and live bitrate change confirmed on the phone for **both H.265 and H.264**, with encoder delay measured for each; Safari on your iPhone decodes an H.265 WebRTC stream **in hardware** (checked with `mediaCapabilities` and `getStats`) from a page served over plain `http://`, including when launched from the Home Screen; turning the panel off with `setDisplayPowerMode` works on your phone's Android version; a JNI `.so` loads under `app_process` (or the separate-executable fallback is chosen); a pure-C DTLS + SRTP handshake with Safari works, with STUN through the relay; ADB-over-USB throughput and router CPU use measured at 10/20 Mbps |
| M1 | **Agent + LAN MVP** | C agent relaying media; phone streams WebRTC (H.265, with automatic H.264 fallback) through the router to Safari on the LAN; touch control works; latency measured for Path A and Path B |
| M2 | **Cloud path** | AWS control server (plain HTTP), Basic-auth login with session cookies, and TURN deployed; agent's WebSocket connects, authenticates with Basic auth then cookie, sends keepalives and reconnects; signaling flows over it; works over 5G |
| M3 | **Quality controls** | Presets, live bitrate, resolution switching, auto mode, stats overlay |
| M4 | **Audio** | Opus audio with tap-to-unmute |
| M5 | **Photo upload, clipboard, screen power** | iPhone photo appears in the Android album; text copied on either side can be pasted on the other; the panel turns off after connecting; the phone locks 10 s after disconnecting and also after a server crash |
| M6 | **Reliability + packaging** | USB replug recovery, auto-reconnect, procd/systemd services, logs; `make dist` builds every static binary in the build container; install = copy files |
| M7 | *Optional* | Gamepad, clipboard sync, multiple devices, recording |

## 10. Risks

- **Router USB power.** Most router USB ports supply 0.5–0.9 A, which can't keep a gaming phone charged. Use a **powered USB hub** or a Y-cable.
- **Router CPU and storage.** The agent moves every media byte between USB and UDP in one libev thread, with no decoding and no separate adb process. On a very weak MIPS router, 20 Mbps may still use noticeable CPU, which M0 measures; Path B avoids this. The agent needs well under 1 MB of flash, plus about 2 MB for the phone files.
- **Built-in ADB compatibility.** Our ADB client implements only the classic USB protocol. If a phone's `adbd` misbehaves with it, AOSP's adb source is the reference for fixes. Don't run a real `adb` server on the router at the same time: only one program can claim the phone's USB interface.
- **Games that detect USB debugging.** Some games and anti-cheat systems refuse to run when Developer Options or USB debugging is on. **Check this for your specific games first.**
- **Secure-screen apps** (banking, DRM video) show black when captured.
- **Phone heat.** Encoding plus charging around the clock. Use charge limiting or bypass charging if the phone has it.
- **AWS data transfer:** about $0.09/GB, roughly **$0.32/hour** at 8 Mbps. This applies only when TURN is used; direct peer-to-peer traffic doesn't go through AWS.
- **iPhone Safari limits:** fullscreen only as a PWA, audio needs a tap, the stream pauses in the background, and clipboard access needs a tap each time (no `navigator.clipboard` on plain HTTP; §4.8).
- **Screen-off API differences.** The hidden API that turns the panel off changed in Android 14, and some vendors reset the panel on certain events. M0 checks it on your phone.
- **Locking needs a lock method set on the phone.** Without a PIN, pattern or password, "lock" only turns the screen off.
- **Security without TLS (§4.6).** This is a real trade-off. Without TLS:
  - The **Basic auth password and session cookies travel in readable form.** Anyone on a network in between (public Wi-Fi, a hotel network, the mobile carrier or ISP) can capture them and then **fully control your phone**, including viewing the screen and injecting touches.
  - The **control messages can be read and changed in transit.** That includes the WebRTC setup messages, which carry the DTLS fingerprint. An attacker who can modify traffic could swap the fingerprint and intercept the video.
  - Passive eavesdroppers still **can't see the video, audio, input or photos**: WebRTC's DTLS-SRTP encryption covers those regardless.
  - Mitigations included: hashed passwords, rate limiting, `HttpOnly` + `SameSite` cookies, `Origin` checks, revocable sessions, short-lived TURN credentials, and the agent accepts tunnel frames only from the phone.
  - **If this changes later:** TLS could be added to the browser side only, using a reverse proxy on AWS, without touching the router agent. The browser side is where an untrusted network like public Wi-Fi is most likely.

## 11. Decisions

**Decided:**
- The ARM device can be a weak OpenWrt router; the phone does all capture, encoding and WebRTC work.
- The router runs one agent **written in C, using libev** for all I/O: **built-in ADB client over USB** (no adb program installed), TCP-frame → UDP relay, and a WebSocket client to AWS.
- The agent's WebSocket carries **only control messages and keepalives**; media, input and photos go over WebRTC.
- **No TLS:** the web page, API and WebSockets use plain HTTP/`ws://`, secured with HTTP Basic auth plus session cookies (§4.6). WebRTC's built-in media encryption stays.
- The **AWS control server is written in C with libev**, sharing WebSocket, JSON and ini code with the agent.
- **Login lasts 30 days** by default, configurable in `server.ini`. Server and agent settings are plain ini files (§4.7).
- The Java part is written from scratch. scrcpy is a reference only and is not used directly.
- Codec: **H.265 is the main codec, H.264 the fallback**, chosen automatically per session plus a manual switch (§5.5).
- **Static builds:** all libraries are vendored and compiled into the binaries; nothing is installed on the router, server or phone (§7.1).
- **Clipboard sync** both ways (§4.8); **remote screen off** while connected and **lock** after disconnect (§4.9); **hardware decoding** on the iPhone (§5.6).

**Still open:**

1. **Default path:** Path A (tunnel through the router), Path B (phone's own Wi-Fi), or support both and pick by measurement *(recommended)*?
2. **WebRTC stack:** pure-C assembly, option B *(recommended)*, or libdatachannel, option A (less work, but C++)? And is Go/Pion acceptable as an emergency fallback?
3. **Audio:** in scope for v1?
4. **coturn** (§7.3), a program rather than a library. Options:
   - **a.** Install it once with `apt install coturn`. This is the simplest option.
   - **b.** Ship a static build we compile ourselves in `build/`.
   - **c.** Build a small UDP TURN relay into `remodroid-server` (~1.5–2k lines of C), so the server is truly one file.

## 12. Information needed

- Router model, CPU architecture, RAM, free flash space, OpenWrt version, USB 2.0 or 3.0
- Phone model and Android version; is the phone on Wi-Fi?
- iOS version
- Games to be played
- Home upload speed and whether the router has a public IPv4 address or is behind carrier NAT (CGNAT)
- Location (for the AWS region)

## Appendix A. API definition

### A.1 Overview: where each API lives

The API is split by path so that the rule "the agent's WebSocket carries only control and keepalive" holds. Anything with real data volume (input, files, logcat, shell output) goes over **WebRTC DataChannels** directly between the iPhone and the phone, and never through AWS.

| # | API | Between | Transport | Needs an active session? | Used for |
|---|---|---|---|---|---|
| A.3 | REST API | Browser → server | HTTP `/api/v1/*` | No | Login, device list, device and ADB status, admin commands |
| A.4 | Viewer WebSocket | Browser ⇄ server | `ws://…/ws/viewer` | No | Live device detection events, session setup, signaling |
| A.5 | Agent WebSocket | Agent ⇄ server | `ws://…/ws/agent` | No | Registration, status, events, commands, signaling, keepalive |
| A.6 | Session control | Browser ⇄ phone | DataChannel `ctrl` (JSON) | Yes | Quality, keys, clipboard, screen power, shell, logcat, file control, stats |
| A.7 | Input | Browser → phone | DataChannel `input` (binary) | Yes | Touch, scroll; lowest-latency path |
| A.8 | File data | Browser ⇄ phone | DataChannel `file` (binary) | Yes | File push/pull chunks, photo upload |
| A.9 | Tunnel control | Agent ⇄ phone | `CTRL` frames in the ADB stream | — | Config, session lifecycle, signaling pass-through, phone status |
| A.10 | Agent CLI | Shell on router → agent | unix socket `/var/run/remodroid.sock` | No | `devices`, `status`, `shell`, `push`, `pull`, `logcat`, … |

**Why three DataChannels:** each DataChannel is a separate SCTP stream. A large file transfer on `file` therefore can't delay a touch event on `input` (no head-of-line blocking between them). All three are reliable and ordered.

### A.2 Common conventions

**JSON envelope**, used by every JSON API (A.4, A.5, A.6, A.9, A.10):
```json
{"t": "<type>", "id": 17, "sid": "s_8f2c…", "d": { }}
```
- `t`: message type (the names below).
- `id`: set by the sender of a request; the reply carries the same `id`. Omitted for events.
- `sid`: session ID, when the message belongs to a viewer session.
- `d`: the payload.

**Replies.** Each request is answered by exactly one of:
```json
{"t": "ack",   "id": 17, "d": { /* result, may be empty */ }}
{"t": "error", "id": 17, "d": {"code": "not_found", "msg": "no such file"}}
```

**Error codes** (also the `error` field of REST error bodies):

| Code | HTTP | Meaning |
|---|---|---|
| `bad_request` | 400 | Missing or invalid field |
| `unauthorized` | 401 | No valid cookie / Basic auth |
| `forbidden` | 403 | Origin check failed, or device disabled |
| `not_found` | 404 | Unknown device, file or path |
| `offline` | 409 | Agent not connected, or phone not attached |
| `busy` | 409 | Device already in a session, or transfer already running |
| `adb_unauthorized` | 409 | Phone hasn't accepted the router's ADB key yet |
| `too_large` | 413 | Output or file over a limit |
| `rate_limited` | 429 | Too many failed logins |
| `timeout` | 504 | Agent or phone didn't answer in time |
| `io_error` | 500 | Read/write failure on the phone |
| `unsupported` | 501 | Feature not supported by this phone or Android version |

**Other conventions**
- **Byte order:** binary messages are big-endian, the same as the tunnel frames.
- **Coordinates:** normalized to `0..65535` across the current video frame, so they don't depend on the stream's resolution.
- **Times:** milliseconds. Timestamps are Unix epoch ms unless marked as a monotonic clock.
- **Versioning:** REST under `/api/v1`. WebSocket and DataChannel peers exchange `proto: 1` in `hello` / `session.ready`; unknown message types are answered with `error`/`unsupported` and otherwise ignored.

### A.3 REST API (browser → server)

All endpoints except `/login` require the `rd_session` cookie (§4.6). Bodies are JSON (`Content-Type: application/json`).

| Method | Path | Request | Response | Notes |
|---|---|---|---|---|
| `GET` | `/login` | Basic auth header | `302 /` + `Set-Cookie` | Without a valid header: `401` + `WWW-Authenticate: Basic` |
| `POST` | `/api/v1/logout` | — | `204` | Deletes the session |
| `GET` | `/api/v1/me` | — | `{"user":"me","session_expires":1767225600000}` | |
| `GET` | `/api/v1/devices` | — | `{"devices":[Device,…]}` | All devices in `server.ini` |
| `GET` | `/api/v1/devices/{id}` | — | `Device` | |
| `GET` | `/api/v1/devices/{id}/adb` | — | `AdbStatus` | Detailed ADB state |
| `POST` | `/api/v1/devices/{id}/cmd` | `{"cmd":"…","args":{…}}` | `{"result":{…}}` | Admin commands, see below |
| `GET` | `/api/v1/devices/{id}/diag` | `?lines=200` | `{"agent_log":[…],"server_stderr":[…]}` | Last lines of the agent log and the phone server's stderr, max 64 KB. For when the phone server won't start and no session (and so no logcat) is possible. |

**`Device` object**
```json
{
  "id": "router1",
  "name": "Living room phone",
  "online": true,
  "last_seen": 1767139200000,
  "agent": {"version": "0.1.0", "uptime_s": 86400, "public_ip": "203.0.113.7", "arch": "mipsel"},
  "phone": {
    "attached": true,
    "serial": "R58N12ABCDE",
    "model": "SM-G991B",
    "android": "14",
    "sdk": 34,
    "battery": {"level": 82, "charging": true, "temp_c": 31.5},
    "screen": "off",
    "locked": false,
    "server": "running",
    "path": "tunnel",
    "codecs": ["h265", "h264"]
  },
  "session": {"active": true, "sid": "s_8f2c…", "since": 1767139000000}
}
```
- `phone.screen`: `on` | `off` (panel off, §4.9) | `asleep`.
- `phone.server`: `stopped` | `starting` | `running` | `crashed`.
- `phone.path`: `tunnel` (Path A) | `wifi` (Path B).

**`AdbStatus` object**
```json
{
  "usb": "connected",
  "auth": "authorized",
  "serial": "R58N12ABCDE",
  "usb_speed": "high",
  "protocol_version": "0x01000001",
  "max_payload": 262144,
  "features": ["shell_v2", "cmd", "stat_v2"],
  "streams_open": 2,
  "tx_bytes": 123456789,
  "rx_bytes": 2345678,
  "errors": 0,
  "connected_since": 1767100000000
}
```
- `usb`: `none` | `connected` | `error`.
- `auth`: `waiting` (handshake in progress) | `unauthorized` (accept the prompt on the phone) | `authorized`.
- `usb_speed`: `full` | `high` | `super`.

**Admin commands** (`POST …/cmd`; forwarded to the agent as `cmd`, A.5)

| `cmd` | `args` | Effect |
|---|---|---|
| `server.restart` | — | Restart the phone server |
| `phone.wake` | — | Wake the screen |
| `phone.sleep` | — | Sleep and lock |
| `phone.reboot` | `{"mode": "normal"}` | Reboot the phone (`normal` \| `recovery` \| `bootloader`) |
| `adb.reconnect` | — | Drop and redo the USB/ADB connection |
| `agent.reload` | — | Reload `agent.ini` |
| `quality.default` | `QualitySettings` (A.6) | Default quality for new sessions |

### A.4 Viewer WebSocket (browser ⇄ server, `ws://…/ws/viewer`)

Authenticated by cookie; the server checks `Origin`.

| Direction | `t` | `d` | Purpose |
|---|---|---|---|
| server → viewer | `devices` | `{"devices":[Device,…]}` | Full snapshot, sent right after connecting |
| server → viewer | `device.update` | `{"id":"router1","patch":{…}}` | Partial `Device` update, e.g. battery or screen state |
| server → viewer | `device.event` | `Event` (below) | **Device detection** and state changes, in real time |
| viewer → server | `session.request` | `{"device":"router1","quality":QualitySettings}` | Ask to connect; reply `ack {"sid":"s_…"}` or `error busy/offline` |
| server → viewer | `session.ice` | `{"ice_servers":[{"urls":"turn:…","username":"…","credential":"…"}]}` | TURN credentials for this session |
| server ⇄ viewer | `signal` | `{"type":"offer"\|"answer"\|"candidate", …}` | WebRTC signaling (the phone sends the offer) |
| viewer → server | `session.end` | `{}` | Leave |
| server → viewer | `session.ended` | `{"reason":"viewer_left"\|"phone_detached"\|"agent_offline"\|"timeout"}` | Session is over |

**`Event` object**, the device detection feed:
```json
{"id": "router1", "kind": "phone.attached", "ts": 1767139200000, "info": {"serial": "R58N12ABCDE", "model": "SM-G991B"}}
```

| `kind` | When |
|---|---|
| `agent.online` / `agent.offline` | The agent's WebSocket connects, or misses keepalives (server-generated) |
| `phone.attached` / `phone.detached` | USB plug/unplug, detected through netlink |
| `adb.unauthorized` / `adb.authorized` | The phone is waiting for the "Allow USB debugging" prompt, or it was accepted |
| `server.started` / `server.exited` | Phone server came up, or exited (`info.code`, `info.signal`) |
| `phone.locked` / `phone.unlocked` | Lock state changed |
| `phone.overheat` | Battery temperature above the limit in `agent.ini` |

### A.5 Agent WebSocket (agent ⇄ server, `ws://…/ws/agent`)

Authenticated with Basic auth, then a cookie (§4.6). Carries **only control and keepalive**.

| Direction | `t` | `d` | Purpose |
|---|---|---|---|
| agent → server | `hello` | `{"proto":1,"version":"0.1.0","arch":"mipsel","phone":{…}}` | First message |
| server → agent | `welcome` | `{"server_time":…,"ping_s":20,"timeout_s":60,"heartbeat_s":30}` | Accepted; keepalive settings |
| agent → server | `status` | `{"phone":{…},"adb":AdbStatus}` | Heartbeat every 30 s and on change; the server builds `Device` from it |
| agent → server | `event` | `Event` without `id` | Device detection and state changes; the server adds `id` and forwards to viewers |
| server → agent | `session.start` | `{"sid":"s_…","ice_servers":[…],"quality":QualitySettings}` | Viewer wants to connect; the agent passes it to the phone (A.9) |
| agent ⇄ server | `signal` | `{"sid":"s_…","type":"offer"\|"answer"\|"candidate",…}` | Signaling pass-through, both ways |
| server → agent | `session.end` | `{"sid":"s_…"}` | Viewer left |
| agent → server | `session.ended` | `{"sid":"s_…","reason":"…"}` | Phone ended the session, e.g. ICE failed |
| server → agent | `cmd` | `{"cmd":"…","args":{…}}` | Admin commands from A.3; reply `ack`/`error` |
| server → agent | `diag` | `{"lines":200}` | Reply `ack {"agent_log":[…],"server_stderr":[…]}`, max 64 KB |

**Keepalive:** a WebSocket ping every 20 s, reconnect if no pong within 60 s, `status` every 30 s, and reconnect backoff 1 s → 30 s with jitter (§4.5).

### A.6 Session control (browser ⇄ phone, DataChannel `ctrl`, JSON)

Opened by the phone right after the WebRTC connection is up. The first message is `session.ready` from the phone.

**Session and quality**

| Direction | `t` | `d` |
|---|---|---|
| phone → viewer | `session.ready` | `{"proto":1,"screen":{"w":1080,"h":2400,"rotation":0},"codec":"h265","caps":{"codecs":["h265","h264"],"max_fps":120,"audio":true}}` |
| viewer → phone | `quality.set` | `QualitySettings` |
| phone → viewer | `quality.state` | `QualitySettings` with the values actually in use (after auto mode or encoder limits) |
| phone → viewer | `screen.changed` | `{"w":2400,"h":1080,"rotation":1}` after rotation or a resolution change |
| phone → viewer | `stats` | every 1 s: `{"fps":60,"kbps":8200,"rtt_ms":38,"loss_pct":0.2,"enc_ms":6,"codec":"h265","path":"tunnel","bwe_kbps":14000}` |
| viewer → phone | `ping` | `{"ts":…}`; reply `pong {"ts":…,"phone_ts":…}` for latency measurement |

**`QualitySettings`**
```json
{"mode": "auto", "preset": "720p", "width": 720, "height": 1600, "fps": 60, "kbps": 6000, "codec": "auto"}
```
- `mode`: `auto` (bitrate/resolution adapt to the network) | `manual`.
- `preset`: `1080p` | `720p` | `540p` | `360p` | `custom`; with `custom`, `width`/`height` are used.
- `codec`: `auto` | `h265` | `h264`; a codec change triggers renegotiation.

**Keys, navigation, text**

| Direction | `t` | `d` | Notes |
|---|---|---|---|
| viewer → phone | `key` | `{"action":"down"\|"up"\|"press","code":66,"meta":0}` | Android `KEYCODE_*`, `META_*` flags |
| viewer → phone | `nav` | `{"button":"back"\|"home"\|"recents"\|"power"\|"volume_up"\|"volume_down"\|"notifications"}` | Convenience wrapper around `key` |
| viewer → phone | `text` | `{"text":"héllo 你好"}` | ASCII is injected as key events; other text goes through the clipboard and a paste |
| viewer → phone | `rotate` | `{"to":"auto"\|"portrait"\|"landscape"}` | |

**Clipboard (§4.8)**

| Direction | `t` | `d` |
|---|---|---|
| both | `clip.set` | `{"text":"…","paste":false}`; with `paste: true` (viewer → phone), it also injects a paste |
| phone → viewer | `clip.changed` | `{"text":"…"}` when Android's clipboard changes |

**Screen power (§4.9)**

| Direction | `t` | `d` |
|---|---|---|
| viewer → phone | `screen.power` | `{"on":true}`: turn the physical panel on or off during the session |
| phone → viewer | `screen.state` | `{"panel":"on"\|"off","locked":true}` |

**Shell**

| Direction | `t` | `d` |
|---|---|---|
| viewer → phone | `shell.exec` | `{"cmd":"pm list packages -3","timeout_ms":10000}` |
| phone → viewer | reply `ack` | `{"exit_code":0,"stdout":"…","stderr":"…","truncated":false,"ms":420}`; output max 1 MB |

The command runs as the shell user, directly on the phone (the phone server is already the shell user), without going through the router.

**Logcat**

| Direction | `t` | `d` |
|---|---|---|
| viewer → phone | `logcat.start` | `{"filter":["*:W","MyGame:V"],"format":"threadtime","tail":500,"buffers":["main","system","crash"]}`; reply `ack {"stream":3}` |
| phone → viewer | `logcat.lines` | `{"stream":3,"lines":["10-16 12:00:01.123  1234  1250 W Tag: msg", …]}`, batched every 100 ms or 64 KB |
| viewer → phone | `logcat.stop` | `{"stream":3}` |
| phone → viewer | `logcat.ended` | `{"stream":3,"reason":"stopped"\|"error"}` |

The phone server runs `logcat` itself. Lines are dropped, with a `{"dropped":N}` marker, if the viewer can't keep up, so logcat never slows the video.

**Files (control part; data goes on `file`, A.8)**

| Direction | `t` | `d` | Reply |
|---|---|---|---|
| viewer → phone | `file.list` | `{"path":"/sdcard/Download"}` | `ack {"entries":[{"name":"a.jpg","size":12345,"mtime":…,"dir":false},…]}` |
| viewer → phone | `file.stat` | `{"path":"…"}` | `ack {"size":…,"mtime":…,"dir":false}` |
| viewer → phone | `file.push` | `{"xfer":7,"path":"/sdcard/DCIM/RemoDroid/IMG_0001.jpg","size":2345678,"sha256":"…","overwrite":false,"media_scan":true}` | `ack` = ready, start sending chunks |
| viewer → phone | `file.pull` | `{"xfer":8,"path":"/sdcard/Download/log.zip"}` | `ack {"size":…,"sha256":"…"}`, then the phone sends chunks |
| either | `file.done` | `{"xfer":7}` | Sent after the last chunk; the receiver verifies size and SHA-256 and replies `ack` or `error io_error` |
| either | `file.cancel` | `{"xfer":7}` | Aborts; a partial upload is deleted |
| phone → viewer | `file.progress` | `{"xfer":7,"bytes":1048576}` | Every 256 KB |
| viewer → phone | `file.delete` / `file.mkdir` | `{"path":"…"}` | `ack` |

- **Photo upload** is a `file.push` to `/sdcard/DCIM/RemoDroid/` with `media_scan: true`, so it appears in the Gallery.
- `path` must be under `/sdcard/` or `/data/local/tmp/`; anything else gets `error forbidden`.

### A.7 Input (browser → phone, DataChannel `input`, binary)

Compact fixed-size messages, because they're the most latency-sensitive traffic. One message per DataChannel send.

**Touch**, 14 bytes:
```
off size field
 0   u8   type       0x01 = touch
 1   u8   action     0 down, 1 up, 2 move, 3 cancel
 2   u8   pointer    pointer ID (0–9), from the browser's Touch.identifier mapping
 3   u8   flags      bit0: last message of a multi-pointer batch
 4   u16  x          0..65535 across the video frame
 6   u16  y          0..65535
 8   u16  pressure   0..65535 (65535 if unknown)
10   u32  ts         viewer monotonic clock, ms (for input-latency stats)
```
The phone server keeps the state of all active pointers and injects a full Android `MotionEvent` (with `ACTION_POINTER_DOWN`/`UP`), so multi-touch works.

**Scroll**, 13 bytes:
```
 0   u8   type       0x02 = scroll
 1   u16  x
 3   u16  y
 5   i16  h          horizontal, 1/256 notch units
 7   i16  v          vertical, 1/256 notch units
 9   u32  ts
```

**Reserved types:** `0x03` gamepad buttons/axes (later milestone), `0x04` mouse.

### A.8 File data (browser ⇄ phone, DataChannel `file`, binary)

```
 0   u32  xfer       transfer ID from file.push / file.pull
 4   u32  seq        chunk number, starting at 0
 8   ...  payload    up to 16 KB
```
- **Flow control:** the sender keeps `bufferedAmount` under 1 MB, using `bufferedAmountLowThreshold` in Safari and the usrsctp send buffer on the phone. Video is never starved, because media doesn't share SCTP.
- **Integrity:** SHA-256 of the whole file is checked at `file.done`.
- **Limits:** max file size set in `agent.ini` (`[files] max_mb`, default 2048).

### A.9 Tunnel control (agent ⇄ phone, `CTRL` frames, JSON)

Inside the ADB stream (§4.2), using the same envelope.

| Direction | `t` | `d` |
|---|---|---|
| agent → phone | `config` | All `[phone]`, `[video]` and `[files]` settings from `agent.ini`, sent right after the stream opens |
| phone → agent | `ready` | `{"proto":1,"version":"0.1.0","android":"14","sdk":34,"model":"…","codecs":["h265","h264"],"max_fps":120}` |
| phone → agent | `status` | `{"battery":{…},"screen":"off","locked":false,"path":"tunnel"}` on change; the agent merges it into its own `status` (A.5) |
| agent → phone | `session.start` | Same `d` as A.5 |
| agent ⇄ phone | `signal` | Signaling pass-through |
| agent → phone | `session.end` | `{"sid":"…"}` |
| phone → agent | `session.ended` | `{"sid":"…","reason":"…"}` |
| agent → phone | `cmd` | `phone.wake`, `phone.sleep`, `server.stop` and so on; reply `ack`/`error` |

Liveness: tunnel `PING`/`PONG` frames every 5 s. If 3 are missed, the agent restarts the phone server.

### A.10 Agent CLI (on the router)

The CLI talks to the running agent through `/var/run/remodroid.sock`. Each request is one JSON line in the A.2 envelope; replies are JSON lines too, so it can also be scripted.

| Command | Does |
|---|---|
| `remodroid-agent devices` | Phones found on USB, with `AdbStatus` |
| `remodroid-agent status` | Agent, WebSocket, phone server and session state |
| `remodroid-agent shell "<cmd>"` | Run a command on the phone through the built-in ADB client; prints output, exits with the command's exit code |
| `remodroid-agent push <local> <remote>` | Copy a file to the phone (ADB `sync:`) |
| `remodroid-agent pull <remote> <local>` | Copy a file from the phone |
| `remodroid-agent logcat [filter…]` | Stream logcat until Ctrl-C |
| `remodroid-agent key <code>` | Inject a key event, e.g. `key 26` for power |
| `remodroid-agent restart-server` | Restart the phone server |
| `remodroid-agent reconnect` | Redo the USB/ADB connection |
| `remodroid-agent reload` | Reload `agent.ini` (same as `SIGHUP`) |

These work **without a WebRTC session and without AWS**, which is useful on the router while setting things up.
