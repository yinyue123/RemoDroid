# RemoDroid architecture, start to end

This walks through the whole system in the order things happen: the router boots, the phone is found, the
phone server starts, you open Safari, WebRTC connects, and you tap the screen. Every step names the file and
function that does it, and §14 shows every packet format byte by byte.

Companion documents: [`plan.md`](plan.md) (design and API), [`webrtc.md`](webrtc.md) (WebRTC options, RFCs,
scenarios).

Contents

1. [Five corrections to start with](#1-five-corrections-to-start-with)
2. [The five programs](#2-the-five-programs)
3. [Start-up: from power-on to "phone ready"](#3-start-up-from-power-on-to-phone-ready)
4. [You open Safari: HTTP login and the viewer WebSocket](#4-you-open-safari-http-login-and-the-viewer-websocket)
5. [Signaling: how a WebRTC session starts](#5-signaling-how-a-webrtc-session-starts)
6. [What SDP, ICE, STUN, TURN, DTLS, SRTP, RTP, RTCP and SCTP are for](#6-what-sdp-ice-stun-turn-dtls-srtp-rtp-rtcp-and-sctp-are-for)
7. [A video packet's journey through the agent](#7-a-video-packets-journey-through-the-agent)
8. [What happens when you tap the screen](#8-what-happens-when-you-tap-the-screen)
9. [Commands: the four ways an instruction reaches the phone](#9-commands-the-four-ways-an-instruction-reaches-the-phone)
10. [The Java classes and how they work together](#10-the-java-classes-and-how-they-work-together)
11. [Server internals: HTTP, auth, hub, TURN](#11-server-internals-http-auth-hub-turn)
12. [Third-party libraries: what each one is for](#12-third-party-libraries-what-each-one-is-for)
13. [Threads and event loops](#13-threads-and-event-loops)
14. [Packet reference, byte by byte](#14-packet-reference-byte-by-byte)
15. [Glossary of abbreviations](#15-glossary-of-abbreviations)
16. [Suggested reading order](#16-suggested-reading-order)

---

## 1. Five corrections to start with

These are the places where the obvious mental model differs from what the code does.

1. **The agent does not enable ADB.** You enable *USB debugging* on the phone once (Developer options), which
   makes the phone's `adbd` daemon listen on USB. The agent is an ADB *client* (the role the `adb` command
   plays on a PC), written from scratch in `agent/adb.c` + `agent/usb.c`. No `adb` binary is involved.
2. **Java is not injected through JNI.** The agent *copies* two files to the phone over ADB (a `.jar` with
   Java bytecode and a `.so` with the C WebRTC stack), then *runs* the jar as an ordinary shell command
   (`app_process`). The running Java code then loads the `.so` itself (`System.load`). JNI is just the
   function-call bridge between that Java code and that C code inside one process.
3. **WebRTC runs on the phone, not on the router.** The router never decrypts or understands media. It only
   carries opaque UDP payloads between the internet and the phone through a TCP-like ADB stream (the
   "tunnel").
4. **The WebSockets never carry media.** They carry small JSON messages: logins, status, and the WebRTC
   *negotiation* (SDP offer/answer and ICE candidates). Video, touches, clipboard and files go over WebRTC.
5. **There are two "servers".** `remodroid-server` is the C program on AWS. The *phone server* is the Java
   process `io.remodroid.Server` on the phone. This document always says which one.

---

## 2. The five programs

```
 iPhone                    AWS                         Router (OpenWrt)            Android phone
┌───────────────┐  HTTP   ┌──────────────────┐  WS    ┌──────────────────┐  USB   ┌──────────────────────────┐
│ Safari        │◄───────►│ remodroid-server │◄──────►│ remodroid-agent  │◄──────►│ adbd (Android's daemon)  │
│ web/app.js    │  WS     │  http/auth/hub   │ ws://  │  adb client      │  ADB   │   ▲ localabstract:       │
│               │         │  TURN relay      │        │  phone supervisor│        │   │ remodroid            │
│ RTCPeer-      │         └────────▲─────────┘        │  UDP relay       │        │ ┌─┴──────────────────┐   │
│ Connection    │                  │ UDP (TURN)       │  CLI socket      │        │ │ app_process:       │   │
│               │◄─────────────────┴──────────────────┤                  │        │ │ io.remodroid.Server│   │
│               │   WebRTC media: UDP, end-to-end     │  (relays UDP     │        │ │  Java  ⇄ JNI ⇄ C   │   │
│               │   encrypted, phone ⇄ iPhone         │   payloads only) │        │ │  libremodroid.so   │   │
└───────────────┘                                     └──────────────────┘        │ └────────────────────┘   │
                                                                                  └──────────────────────────┘
```

| Program | Runs on | Language | Source | Job |
|---|---|---|---|---|
| Web client | iPhone Safari | JS/HTML/CSS | `web/` | Login page, video, touch input, floating-ball menu; the WebRTC *answerer* |
| `remodroid-server` | AWS (any Linux server) | C + libev | `server/` | HTTP(S) + login page + cookies (Basic auth for routers), static files, viewer/agent WebSockets, signaling relay, REST API, TURN relay, setup wizard and router enrollment API |
| Worker (alternative to the server) | Cloudflare | JS | `worker/` | The same panel on a Durable Object, with Cloudflare TURN |
| `remodroid-agent` | Router | C + libev | `agent/` | ADB client over USB, installs and supervises the phone server, UDP relay ("tunnel"), WebSocket to AWS, local CLI |
| Phone server (Java) | Phone, as shell user | Java 8 bytecode (dex) | `android/java/` | Screen capture + hardware encoder, input injection, clipboard, screen power, files, shell, logcat |
| Phone WebRTC stack | Phone, same process | C (JNI library) | `rtc/` | ICE/TURN client, DTLS, SRTP, RTP packetizing, RTCP feedback, congestion control, SCTP data channels, SDP |

Shared C code used by the server, agent and phone stack lives in `common/` (buffers, base64, SHA, WebSocket
framing, tunnel framing, STUN encoding, JSON envelope, ini reading).

### The connections

| # | Between | Transport | Carries | Format |
|---|---|---|---|---|
| ① | Safari ⇄ server | HTTP | Login, web files, REST `/api/v1/…` | §14.1 |
| ② | Safari ⇄ server | WebSocket `/ws/viewer` | Device list, events, session request, **signaling** | §14.2 + JSON §14.1 |
| ③ | Agent ⇄ server | WebSocket `/ws/agent` | hello/status/events, commands, **signaling** | §14.2 + JSON |
| ④ | Agent ⇄ phone `adbd` | USB bulk transfers | ADB packets (shell, file push, streams) | §14.4 |
| ⑤ | Agent ⇄ phone server | One ADB stream to `localabstract:remodroid` | Tunnel frames: control JSON + UDP payloads | §14.3 |
| ⑥ | Router UDP socket ⇄ Safari / TURN | UDP over the internet | STUN, TURN, DTLS, SRTP, SCTP — all from the phone's stack | §14.5–14.13 |

④ and ⑤ are the same USB cable: ⑤ is one of the streams multiplexed inside ④.

---

## 3. Start-up: from power-on to "phone ready"

### 3.1 The agent starts (`agent/main.c: main`)

1. It reads `/etc/remodroid/agent.ini` (`agent_reload` → `common/conf.c`, which uses **inih**).
2. It creates the libev loop (`EV_DEFAULT`). Everything in the agent runs on this single thread.
3. `adb_new()` loads or creates the RSA key (`[agent] adbkey`, default `/etc/remodroid/adbkey`). It is a
   2048-bit RSA key made with **mbedTLS**, and the phone uses it to recognise this router.
4. `phone_init()` sets up the phone supervisor timers (`agent/phone.c`).
5. `transport_start()` (`agent/usb.c`) starts looking for a phone over USB, or over TCP if
   `[phone] adb = tcp:HOST:5555`.
6. `cli_server_start()` opens the local Unix socket `/var/run/remodroid.sock`, used by `remodroid-agent status`
   and the other CLI commands.
7. `wsc_start()` (`agent/wsc.c`) connects to the server (§3.7).
8. `ev_run()` handles events until it is killed.

### 3.2 Finding the phone: USB scan, hotplug, the "device list"

An Android phone with USB debugging on exposes a USB *interface* with three identifying numbers:

| sysfs file | Value | Meaning |
|---|---|---|
| `bInterfaceClass` | `ff` | Vendor-specific |
| `bInterfaceSubClass` | `42` | ADB |
| `bInterfaceProtocol` | `01` | ADB protocol |

`scan_usb()` reads `/sys/bus/usb/devices/*:*` (one directory per interface) and picks the first interface
with `ff/42/01`. If `[phone] serial` is set, the device's `serial` file must also match. From sysfs it learns:

- the bus and device numbers, which give the device node `/dev/bus/usb/BBB/DDD`;
- the interface number;
- the bulk IN and OUT endpoint addresses (`find_endpoints`);
- the max packet size;
- the link speed.

`try_usb()` then:

1. `open("/dev/bus/usb/BBB/DDD")`. This is Linux **usbfs**, so no libusb is needed.
2. `ioctl(USBDEVFS_CLAIMINTERFACE)` to take the ADB interface.
3. Submits one bulk IN transfer (`USBDEVFS_SUBMITURB`). Completed transfers are collected with
   `USBDEVFS_REAPURBNDELAY` when libev reports the fd writable (`usb_on_io`).
4. Calls `adb_attach()`, which starts the ADB handshake (§3.3).

**Hotplug:** the agent also opens a `NETLINK_KOBJECT_UEVENT` socket, where the kernel announces "a USB device
was added or removed". On an add it rescans after a short delay; a periodic rescan covers missed events. When
the cable is pulled, the reaped transfers fail, `adb_on_transport_closed()` runs, and the state goes
to `offline`.

**About the "device list":**
- **The agent handles one phone at a time:** the first matching one, or the one named by `serial`.
  `remodroid-agent devices` (`agent/cli.c`, `"devices"`) returns an array with zero or one entry, built from
  the ADB banner (`agent_adb_json`).
- **The server's device list** (`GET /api/v1/devices`, or the `devices` message on `/ws/viewer`) is a list of
  *routers*, one per line of `[devices]` in `server.ini`. Each entry contains whatever its agent last reported
  about its phone (`hub_device_json`).

### 3.3 The ADB handshake (`agent/adb.c`)

ADB is a small packet protocol: a 24-byte header plus an optional payload (§14.4). The handshake:

```
agent                                                     phone adbd
  │ CNXN  version=0x01000001 maxdata=262144                  │
  │       "host::features=shell_v2,cmd,stat_v2"              │
  │─────────────────────────────────────────────────────────►│
  │                     AUTH  type=1 (TOKEN)  20 random bytes │
  │◄─────────────────────────────────────────────────────────│
  │ AUTH type=2 (SIGNATURE)  RSA-PKCS#1-v1.5-SHA1(token)      │
  │   = 256 bytes, signed with the router's private key      │
  │─────────────────────────────────────────────────────────►│
  │  key already trusted? ─ yes ─► CNXN "device::ro.product…" │  state → online
  │◄─────────────────────────────────────────────────────────│
  │  no ─► AUTH type=1 (TOKEN) again                          │
  │◄─────────────────────────────────────────────────────────│
  │ AUTH type=3 (RSAPUBLICKEY) "<base64 key> remodroid@…\0"   │  state → unauthorized
  │─────────────────────────────────────────────────────────►│  phone shows "Allow USB debugging?"
  │                       (you tap Allow) CNXN "device::…"    │  state → online
  │◄─────────────────────────────────────────────────────────│
```

`handle_auth()` implements the left side. The public key is sent in Android's own format, built by
`build_pubkey()`: a struct with the modulus as little-endian words plus a Montgomery constant, then base64.
"Always allow" stores the key in the phone's `/data/misc/adb/adb_keys`, so the prompt appears only once.

The `unauthorized` state is reported to the server as event `adb.unauthorized`. The web client shows "Allow
USB debugging on the phone". If the server connection isn't up yet, the event waits in a queue of up to 32
messages (`W.pending` in `wsc.c`).

**Streams.** After `CNXN`, either side opens numbered *streams* to named *services*:

```
OPEN(local_id, 0, "service-name\0")   → the other side answers OKAY(its_id, local_id), or CLSE if refused
WRTE(local_id, remote_id, data)       → the receiver must answer OKAY before the next WRTE (flow control)
CLSE(local_id, remote_id)             → closes the stream
```

The services RemoDroid uses:

| Service string | What it does | Used by |
|---|---|---|
| `shell,v2,raw:CMD` (or `shell:CMD`) | Runs `CMD` with `sh`, as the `shell` user (uid 2000). v2 wraps stdout, stderr and the exit code in small packets (§14.4.2) | `adb_shell` |
| `sync:` | File transfer sub-protocol: `STAT`, `SEND`, `RECV`, `DATA`, `DONE` (§14.4.3) | `adb_push`, `adb_pull` |
| `localabstract:remodroid` | `adbd` connects to the Unix socket `@remodroid` on the phone and splices the bytes | the tunnel (§3.6) |

### 3.4 Starting the phone server (`agent/phone.c`)

When ADB goes online, `phone_adb_state(ADB_ONLINE)` calls `start_server()`:

1. **Ask the phone what it is.** Runs `getprop ro.product.model; getprop ro.build.version.release; getprop
   ro.build.version.sdk; getprop ro.product.cpu.abi` through `adb_shell`. `got_props()` gets four lines,
   for example `Pixel 7`, `14`, `34`, `arm64-v8a`.
2. **Copy the files** (`adb_push`, the sync service). It skips a file if the size and mtime already match:
   - `/usr/share/remodroid/remodroid-server.jar` → `/data/local/tmp/remodroid/remodroid-server.jar`
     (`.jar` with `classes.dex` inside; produced by `javac --release 8` + `d8`).
   - `/usr/share/remodroid/<abi>/libremodroid.so` → `/data/local/tmp/remodroid/libremodroid.so` (the
     NDK-built C stack for the phone's CPU ABI).
3. **Launch it** (`launch_server`) with this shell command over a long-running `shell` stream:
   ```sh
   pkill -f io.remodroid.Server >/dev/null 2>&1;
   CLASSPATH=/data/local/tmp/remodroid/remodroid-server.jar \
     exec app_process /system/bin io.remodroid.Server \
       socket=remodroid lib=/data/local/tmp/remodroid/libremodroid.so log=info
   ```
   - **`app_process`** is Android's own Java launcher, the same program that starts the Zygote. It starts an
     Android Runtime (ART) and calls `main()` of the named class from the classpath. No APK is installed and
     no app appears on the phone.
   - It runs as the **`shell` user**, because that's who ADB shells run as. That user is allowed to do what
     RemoDroid needs: inject input events, create virtual displays that mirror the screen, read and set the
     clipboard through hidden APIs, and change screen power. This is the approach scrcpy uses.
   - Its stdout and stderr flow back through the shell stream into a 500-line ring buffer in the agent
     (`server_output` → `log_line`), shown by `diag`.
4. **Supervise it.**
   - If the shell stream ends, `server_exited()` sends the event `server.exited` and restarts the server with
     exponential backoff (1, 2, 4 … 30 s, in `schedule_restart`).
   - If the tunnel stops answering pings for 15 s, the server is also restarted.

### 3.5 Inside the phone process: Java → `System.load` → JNI → C thread

`android/java/src/io/remodroid/Server.java: main()`:

```
Server.main(args)
 ├─ Looper.prepareMainLooper()                 some framework calls require a Looper
 ├─ System.load(".../libremodroid.so")         ← the C library enters the process here
 │     └─ JNI_OnLoad() (android/native/jni.c)   registers the native methods of io.remodroid.Native,
 │                                             caches method IDs of the static callbacks onCtrl, onSession…
 ├─ Native.listener = server                   where C → Java callbacks go
 ├─ ScreenEncoder.hasHardwareHevc()            scans MediaCodecList for a hardware H.265 encoder
 ├─ Native.start("abstract:remodroid", direct=false, h265, maxKbps, logLevel)
 │     └─ n_start() → pthread_create(thread_main)
 │           thread_main: AttachCurrentThread (gets a JNIEnv for callbacks)
 │                        ev_loop_new()       a separate libev loop for the WebRTC stack
 │                        rtc_new()           → net_new(): listen() on the abstract Unix socket "@remodroid"
 │                        ev_run()            ← all ICE/DTLS/SCTP/RTP work happens on this thread
 ├─ startCleanup()                             spawns a second app_process (CleanUp) as a crash guard
 ├─ device.watchClipboard(...)                 Binder listener for clipboard changes
 ├─ schedule reportStatus every 30 s
 └─ Looper.loop()                              the main thread then sleeps here
```

**How Java and C call each other** (`jni.c`):

- **Java → C** (for example `Native.video(...)` from the encoder thread, or `Native.dcSend(...)` from a Session
  thread):
  - The JNI function copies the arguments into a `cmd_t`.
  - It appends that to a mutex-protected queue and wakes the C loop with `ev_async_send`.
  - The C loop thread runs it in `on_wake → run_cmd`.
  - The WebRTC objects are therefore only ever touched by one thread, and no other locking is needed.
  - If more than 4 MB of video is already queued, delta frames are dropped to avoid building up latency.
- **C → Java** (for example "a data-channel message arrived"):
  - The C loop thread calls `CallStaticVoidMethod(Native.onMessage, …)` with its attached `JNIEnv`.
  - `Native.onMessage` forwards to `Server.onMessage`, which forwards to `Session.message`.
  - These run **on the C thread**, so the Java side keeps them short or hands the work to its own executors.

### 3.6 The tunnel opens

About 0.5 s after launching, the agent's retry timer calls `open_tunnel()`, which runs
`adb_open("localabstract:remodroid")`:

1. The phone's `adbd` connects to the abstract Unix socket `@remodroid`.
2. `net.c: on_accept()` in the C stack accepts it. This connection is the tunnel.
3. Both ends call their `link_up` handlers:
   - agent → phone: `TUN_CTRL {"t":"config","d":{"phone":{…},"video":{…},"files":{…}}}`, the settings from
     `agent.ini` (`send_config`).
   - phone → agent: `TUN_CTRL {"t":"ready","d":{"proto":1,"version":"0.1.0","android":"14","sdk":34,
     "model":"…","codecs":["h265","h264"],"max_fps":120}}` (`Server.onLink`).
4. The agent marks the server `running` and sends the event `server.started` to AWS.
5. Every 5 s the agent sends `TUN_PING`, and the phone answers `TUN_PONG`.

The tunnel is a **byte stream** (an ADB stream behaves like TCP), so messages are framed by the length prefix
of the tunnel frame (§14.3).

### 3.7 Agent ⇄ server WebSocket (`agent/wsc.c` ⇄ `server/http.c`, `server/hub.c`)

**The upgrade request:**

```
GET /ws/agent HTTP/1.1
Host: aws.example.com
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Key: <16 random bytes, base64>
Sec-WebSocket-Version: 13
Authorization: Basic base64("router1:<device_secret>")     ← first time
Cookie: rd_session=<32 hex>                                 ← later reconnects
```

**What the server does with it:**

1. It first tries the cookie (`auth_cookie_session`, with `want_device=1`).
2. Otherwise it checks Basic auth against `[devices]` in `server.ini`: a **bcrypt** hash, checked with
   **crypt_blowfish**.
3. On success it answers `101 Switching Protocols` with
   `Sec-WebSocket-Accept: base64(SHA1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))`.
4. For a Basic login it also sends `Set-Cookie`. The agent keeps the cookie, so reconnects skip the
   bcrypt work.

**The first messages:**

```
agent → {"t":"hello","d":{"proto":1,"version":"0.1.0","arch":"mips","phone":{…}}}
agent → {"t":"status","d":{…}}
server→ {"t":"welcome","d":{"proto":1,"server_time":…,"ping_s":20,"timeout_s":60,"heartbeat_s":30}}
```

**Keepalive:**
- The agent sends a WebSocket ping every `ping_s` and gives up after `timeout_s` without traffic.
- It sends a fresh `status` every `heartbeat_s`, and immediately on changes (debounced by 0.2 s).
- After a disconnect it reconnects with exponential backoff, capped at `reconnect_max_seconds`.

---

## 4. You open Safari: HTTP login and the viewer WebSocket

### 4.1 Login

```
Safari → GET /?device=r1 HTTP/1.1                            (no cookie yet)
server → 302 Location: /login?next=/%3Fdevice%3Dr1           (login_redirect)
Safari → GET /login                                          → web/login.html (public, like setup.css)
Safari → POST /login  {"user":"me","password":"password"}    (fetch from the page, Origin checked)
server   auth_login_request() → auth_password(): bcrypt-check "password" against [users] me = $2b$10$…
         auth_new_session(): 16 random bytes → 32 hex chars, kept in memory with the creation time
server → 200 {"user":"me"}  Set-Cookie: rd_session=3f9c…e1; HttpOnly; SameSite=Strict; Path=/; Max-Age=2592000
Safari → the page goes on to ?next= (only a path on this site); every request carries Cookie: rd_session=3f9c…e1
```

Wrong password: `401`; locked out: `429`. People never see a Basic auth prompt; only routers use Basic
(§3.7). A viewer whose WebSocket keeps failing asks `/api/v1/me`, and a `401` sends it to the login page.
The login page itself checks `/api/v1/me` first and moves straight on when the cookie is still good.

- **Cookie lifetime:** `Max-Age` = `[auth] session_days` × 86400. The default is 30 days.
- **Cookie attributes:** `HttpOnly` means JavaScript can't read the cookie. `SameSite=Strict` means other
  sites can't send requests that carry it.
- **Lockout:** after `max_failed_logins` failures from one IP, that IP is locked out for `lockout_seconds` (or `lockout_minutes` when the former is unset)
  (HTTP 429).
- **Sessions are in memory only:** if the server restarts, everyone logs in again.
- **Plain HTTP by default:** the password and the cookie travel in clear text. Setting `[acme] domains`
  turns on HTTPS with an automatic certificate (§11.5); cookies then get `Secure`. `webrtc.md` §2 explains why
  HTTPS matters, including the WebRTC fingerprint.

### 4.2 The viewer WebSocket (`web/app.js: connectWs`)

1. `new WebSocket("ws://host/ws/viewer")` sends the cookie automatically. The server also checks that the
   `Origin` header matches the `Host` header (or `allowed_origins`), so another website can't open this
   socket with your cookie.
2. The server sends `{"t":"devices","d":{"devices":[{id,name,online,phone:{attached,adb_state,server,…},session}]}}`.
3. `maybeStart()` checks, in order:
   - router online;
   - phone attached;
   - ADB authorized;
   - phone server `running`;
   - no other viewer connected.

   It shows an overlay explaining whichever step is missing. When all pass, it calls `startSession()`.
4. Later changes arrive as `device.update` (status) and `device.event` (toasts such as "Phone unplugged").

---

## 5. Signaling: how a WebRTC session starts

**Signaling** is the exchange of two kinds of information before any media can flow:

- the **SDP offer and answer**: which codecs, which encryption keys (as fingerprints), ICE credentials;
- the **ICE candidates**: the IP:port addresses each side might be reachable at.

WebRTC doesn't define how to deliver them. RemoDroid relays them as JSON through two WebSockets and the tunnel.

```
 Safari (app.js)          remodroid-server (hub.c)       agent (wsc.c/phone.c)        phone C stack (rtc.c)
   │ session.request id=3      │                              │                              │
   │ {device,quality,          │                              │                              │
   │  force_relay}             │                              │                              │
   │──────────────────────────►│ new sid "s_7c1e…"            │                              │
   │                           │ TURN credentials (REST)      │                              │
   │                           │ session.start{sid,           │                              │
   │                           │  ice_servers,quality}        │                              │
   │                           │─────────────────────────────►│ resolve turn: host names     │
   │                           │                              │ (the phone may have no DNS)  │
   │                           │                              │ TUN_CTRL session.start       │
   │                           │                              │─────────────────────────────►│ start_session():
   │ ack id=3 {sid}            │                              │                              │  ice_new (ufrag, pwd)
   │◄──────────────────────────│                              │                              │  sdp_build_offer()
   │ session.ice{ice_servers}  │                              │                              │  ice_gather()
   │◄──────────────────────────│                              │                              │
   │ new RTCPeerConnection     │                              │  TUN_CTRL signal{offer}      │
   │                           │   signal{type:offer,sdp}     │◄─────────────────────────────│
   │      signal{offer}        │◄─────────────────────────────│                              │
   │◄──────────────────────────│                              │                              │
   │ setRemoteDescription      │                              │                              │
   │ setCodecPreferences       │                              │   signal{candidate} (trickle)│
   │ createAnswer              │◄─────────────────────────────┼──────────────────────────────│ host/srflx/relay
   │ setLocalDescription       │                              │                              │
   │ signal{answer}            │                              │                              │
   │──────────────────────────►│ (adds sid)──────────────────►│─────────────────────────────►│ sdp_parse_answer()
   │ signal{candidate}…        │                              │                              │ ice_set_remote()
   │──────────────────────────►│─────────────────────────────►│─────────────────────────────►│ ice_add_remote()
   │                                                                                         │
   │◄═══════════ ICE checks (STUN) ═══ DTLS handshake ═══ SRTP video + SCTP channels ═══════►│
```

What each step does in the code:

1. **`session.request`** (`app.js: startSession`). The server's `viewer_session_request()`:
   - checks that the device exists, is online and isn't busy. If another viewer holds it, the request fails with `busy`
     unless it carries `"takeover":true` (the **Take over** button on the "In use" screen): then that viewer's session
     is ended with reason `taken_over` (the phone gets `session.end`, the old browser `session.ended`) and this one goes ahead;
   - makes a session ID (`s_` + 16 hex);
   - computes TURN credentials (`ice_servers()`, §11.4);
   - sends `session.start` to the agent;
   - answers the viewer with an `ack` carrying the sid, followed by `session.ice` with the same ICE servers.
2. **The agent** (`phone_on_server_msg`):
   - If the phone server isn't ready, it immediately answers `session.ended {reason:"phone_unavailable"}`.
   - Otherwise it replaces `turn:aws.example.com:3478` with `turn:203.0.113.10:3478` (`resolve_ice_hosts`),
     because on Path A the phone has no network or DNS of its own.
   - It then wraps the message in a `TUN_CTRL` frame.
3. **The phone** (`rtc.c: start_session`):
   - picks random SSRCs;
   - creates the ICE agent (random `ufrag` of 8 characters and `pwd` of 24);
   - builds the SDP offer (`sdp.c`, the actual text is shown in §5.1);
   - sends it up as `signal`;
   - starts gathering candidates;
   - tells Java `onSession(CONNECTING)`.
4. **Safari** (`app.js: onSignal`):
   - `setRemoteDescription(offer)`;
   - `preferCodecs()`, which reorders codecs so that H.265 is used only if Safari can hardware-decode it;
   - `createAnswer()` and `setLocalDescription()`;
   - sends the answer back, then its candidates (`onicecandidate`).
5. **The phone** parses the answer (`sdp_parse_answer`). It extracts:
   - the chosen codec and payload type;
   - RTX;
   - which feedback types and header extensions Safari accepted;
   - Safari's ufrag, pwd and fingerprint;
   - its `a=setup` value;
   - the SCTP port.

   ICE checks start as soon as local and remote candidates pair up.
6. **Ending.**
   - Viewer side: `session.end` goes viewer → server → agent → phone (`end_session`).
   - Phone side: `session.ended` goes phone → agent → server → viewer.
   - The server keeps one session per device.

### 5.1 The offer the phone sends (from `sdp_build_offer`)

```
v=0                                                   SDP version
o=- 4611686018427387904 2 IN IP4 127.0.0.1            origin: session id, version (unused values are allowed)
s=-                                                   session name (unused)
t=0 0                                                 timing: permanent
a=group:BUNDLE 0 1 2                                  all m-sections share ONE transport (one ICE/DTLS)
a=extmap-allow-mixed                                  one- and two-byte header extensions may mix
a=msid-semantic: WMS remodroid
m=video 9 UDP/TLS/RTP/SAVPF 96 97 98 99 100 101       video section; port 9 = "see ICE"; PT list in preference order
c=IN IP4 0.0.0.0                                      placeholder address (ICE decides)
a=rtcp:9 IN IP4 0.0.0.0
a=ice-ufrag:Xq7f2LbA                                  ICE username fragment (8 chars)
a=ice-pwd:9QmT1cVZr0Hh3sKpW6yNa2Eu                    ICE password (24 chars), keys MESSAGE-INTEGRITY
a=ice-options:trickle                                 candidates follow separately
a=fingerprint:sha-256 4A:9C:…:E1                      SHA-256 of the phone's DTLS certificate
a=setup:active                                        the phone will be the DTLS client (see webrtc.md §11.7)
a=mid:0                                               media id used by BUNDLE
a=extmap:1 …abs-send-time                             header extension id 1
a=extmap:2 …transport-wide-cc-extensions-01           header extension id 2
a=extmap:3 …playout-delay                             header extension id 3
a=sendonly                                            the phone only sends video
a=msid:remodroid video0
a=rtcp-mux                                            RTP and RTCP on the same port
a=rtcp-rsize                                          reduced-size RTCP allowed
a=rtpmap:96 H265/90000                                PT 96 = H.265, 90 kHz RTP clock
a=rtcp-fb:96 nack / nack pli / ccm fir / transport-cc / goog-remb   accepted feedback kinds (fb_lines)
a=fmtp:96 level-id=123;profile-id=1;tier-flag=0;tx-mode=SRST        Main profile, level 4.1
a=rtpmap:97 rtx/90000                                 PT 97 = retransmissions…
a=fmtp:97 apt=96                                      …of PT 96
a=rtpmap:98 H264/90000                                + fmtp profile-level-id=640c2a (Constrained High 4.2)
a=rtpmap:99 rtx/90000  a=fmtp:99 apt=98
a=rtpmap:100 H264/90000                               + fmtp profile-level-id=42e02a (Constrained Baseline 4.2)
a=rtpmap:101 rtx/90000 a=fmtp:101 apt=100
a=ssrc-group:FID 305419896 2271560481                 media SSRC + its RTX SSRC
a=ssrc:305419896 cname:remodroid  (+ msid lines)
m=application 9 UDP/DTLS/SCTP webrtc-datachannel      data channel section
a=ice-ufrag / ice-pwd / fingerprint / setup:active    same transport values (BUNDLE)
a=mid:1
a=sctp-port:5000                                      SCTP port inside DTLS
a=max-message-size:262144                             largest data-channel message the phone accepts
m=audio 9 UDP/TLS/RTP/SAVPF 111                       the phone's sound (Android 12+; left out otherwise)
a=mid:2  a=sendonly  a=msid:remodroid audio0          same stream id as the video: one MediaStream in Safari
a=rtpmap:111 opus/48000/2                             Opus, 48 kHz, stereo
a=fmtp:111 minptime=10;useinbandfec=1;stereo=1;sprop-stereo=1
a=ssrc:<audio ssrc> cname:remodroid  (+ msid line)
```

If the phone has no hardware H.265 encoder, PTs 96 and 97 are left out.

**Sound** (`AudioCapture.java`, `rtc_audio`). The phone records the `REMOTE_SUBMIX` source, the same
one scrcpy uses: while it records, Android plays media into the submix instead of the speaker, so the
sound moves to the iPhone. It needs a context that says it is `com.android.shell` (uid 2000, which holds
`CAPTURE_AUDIO_OUTPUT`), built from `ActivityThread` on the main thread. MediaCodec encodes Opus;
each 20 ms packet becomes one RTP packet (PT 111, 48 kHz clock, marker on the first). No RTCP sender
reports are sent for it, so the browser plays it at once instead of holding it for lip sync. Capture
runs only while the viewer took the track, `[phone] audio` is on and the viewer hasn't switched Sound off
(`audio.set` on the ctrl channel); stopping gives the sound back to the phone's speaker.

---

## 6. What SDP, ICE, STUN, TURN, DTLS, SRTP, RTP, RTCP and SCTP are for

Think of WebRTC as a stack. Each layer solves one problem:

```
            video frames                         touches, ctrl JSON, files
                │                                         │
     ┌──────────▼──────────┐                  ┌───────────▼───────────┐
     │ RTP  (+ RTCP)       │                  │ SCTP  (+ DCEP)        │   reliability, ordering, many channels
     │ cut frames into     │                  │ data channels         │
     │ packets, timestamps │                  │ "ctrl" "input" "file" │
     └──────────┬──────────┘                  └───────────┬───────────┘
     ┌──────────▼──────────┐                  ┌───────────▼───────────┐
     │ SRTP                │◄── keys from ────│ DTLS                  │   encryption
     │ encrypt + auth each │    DTLS          │ handshake, then       │
     │ RTP/RTCP packet     │                  │ encrypts SCTP         │
     └──────────┬──────────┘                  └───────────┬───────────┘
                └──────────────┬──────────────────────────┘
                     ┌─────────▼─────────┐
                     │ ICE (+STUN, TURN) │   find a working UDP path; keep it alive
                     └─────────┬─────────┘
                               ▼
                       UDP socket (on the router via the tunnel, or on the phone in Path B)
```

| Protocol | Problem it solves | In RemoDroid | Code |
|---|---|---|---|
| **SDP** | "What will we send, how, and how do I recognise you?" A text description of codecs, payload types, ICE credentials, the DTLS fingerprint and data-channel parameters | Phone writes the offer, Safari writes the answer; carried by signaling | `rtc/sdp.c` |
| **ICE** | Both sides are behind NATs. Collect every address each side might be reachable at (*candidates*), test every pair with STUN messages (*connectivity checks*), pick the best working one (*nomination*), and keep checking it (*consent*) | The phone is the controlling agent and nominates | `ice.c` |
| **STUN** | A tiny request/response protocol. (a) "What is my public IP:port?" (Binding to a STUN server gives a *server-reflexive* candidate). (b) The message format of ICE connectivity checks, authenticated with the ICE password | Server answers Binding on UDP 3478; ICE checks between phone and Safari | `common/stun.c` |
| **TURN** | When no direct path works (symmetric NAT, firewall), a server on the internet relays the packets. The phone *allocates* a relay port on the TURN server, which gives a *relay* candidate | Built into `remodroid-server`; phone is the client | `server/turn.c`, `ice.c` |
| **DTLS** | TLS over UDP. Authenticates the peer (the certificate hash must equal the SDP `a=fingerprint`), agrees keys, and encrypts the SCTP data | Phone is the DTLS client (mbedTLS) | `dtls.c` |
| **SRTP** | Encrypts and authenticates each RTP/RTCP packet with keys *exported* from the DTLS handshake (cheaper per packet than DTLS records) | AES-128-CM + HMAC-SHA1-80 via libsrtp | `rtc.c: make_srtp` |
| **RTP** | Media packets: sequence number (loss detection), timestamp (when to display), SSRC (which stream), payload type (which codec), header extensions | H.265/H.264 frames split to ≤1100-byte payloads | `rtp.c` |
| **RTCP** | Control packets beside RTP: Sender/Receiver Reports (loss, jitter, RTT), NACK ("resend these"), PLI/FIR ("send a keyframe"), transport-cc and REMB (bandwidth feedback) | Phone sends SR; Safari sends RR + feedback | `rtp.c: rtp_on_rtcp`, `cc.c` |
| **SCTP** | A message transport with many independent streams, each reliable or not, ordered or not. WebRTC runs it *inside* DTLS | usrsctp in no-threads mode; three channels | `sctp.c` |
| **DCEP** | Tiny protocol on top of SCTP to open a named data channel ("ctrl") and agree its reliability | Phone opens `ctrl`, `input`, `file` | `sctp.c` |

**One UDP port, many protocols.** Thanks to BUNDLE and rtcp-mux, everything uses one UDP socket. The receiver
tells packets apart by their **first byte** (RFC 7983, `rtc.c: ice_data` and `ice.c: deliver`):

| First byte | Protocol |
|---|---|
| 0–3 | STUN (top two bits 00) |
| 20–63 | DTLS (record content types 20–23) |
| 64–79 | TURN ChannelData (channel numbers 0x4000–0x4FFF) |
| 128–191 | RTP or RTCP (version 2 = top bits 10). RTCP when the second byte is 192–223 (packet types 200–206 with the top bit set) |

### 6.1 Order of events on the wire

**Candidate gathering** (`ice_gather`):

1. The phone opens one UDP socket. In Path A this sends `TUN_UDP_OPEN`, and the agent creates a real
   socket on the router.
2. The agent reports the router's LAN addresses for that socket with `TUN_UDP_BOUND`. These become **host**
   candidates.
3. STUN Binding → a **srflx** candidate (the router's public IP).
4. TURN Allocate → a **relay** candidate.
5. Each candidate is trickled to Safari as soon as it is known.

**ICE checks** (`on_tick`, every 20 ms):
- Pairs are tested in priority order, with retransmits at 100/200/400/800 ms and then 1.6 s.
- Safari tests the same pairs from its side. A check that arrives from an unknown address creates a
  **prflx** candidate. This is how the phone learns the iPhone's LAN address, which Safari hides behind a
  `.local` name.
- After the first success the phone waits 120 ms for a better pair, then repeats the check with
  `USE-CANDIDATE`. That repeat is the nomination.

**DTLS** (`ice_state_cb`):
- When ICE becomes `connected`, the phone, as the DTLS client, sends a ClientHello.
- The flights are: ClientHello → ServerHello + Certificate + ServerKeyExchange + CertificateRequest +
  ServerHelloDone → Certificate + ClientKeyExchange + CertificateVerify + ChangeCipherSpec + Finished →
  ChangeCipherSpec + Finished.
- Each side checks the other's certificate hash against the SDP fingerprint (`dtls.c: verify`).

**Keys:**
- When the handshake finishes, mbedTLS exports 60 bytes: client key 16 | server key 16 | client salt 14 |
  server salt 14.
- `dtls_up` builds the outbound SRTP context from the phone's (client's) half and the inbound context from
  Safari's half.

**Media and data:**
- `rtp_new` and `cc_new` start; `sctp_new` connects SCTP inside DTLS.
- The phone opens the three channels with DCEP. Because it is the DTLS client, it uses even stream IDs
  0, 2 and 4.
- Java gets `onSession(CONNECTED)`. `Session` starts the encoder, and the first keyframe goes out.

**Afterwards:**
- Every 4–6 s a consent check on the chosen pair. No answer for 10 s → `disconnected`; 30 s → `failed`, which
  ends the session.
- TURN refreshes before the allocation's 600 s lifetime runs out.

---

## 7. A video packet's journey through the agent

### 7.1 Path A (default): through the router

Phone → iPhone, one RTP packet:

```
PHONE ─────────────────────────────────────────────────────────────────────────────────────────────
 1  MediaCodec (hardware)  encodes one frame of the mirrored screen      ScreenEncoder.drain()
 2  Native.video(buf, …, csd if keyframe)  copy → queue → ev_async       jni.c n_video
 3  rtc_video(): timestamp = (pts − first_pts) × 90 kHz                  rtc.c
 4  rtp_send_frame(): split Annex-B into NAL units; each NAL ≤ 1100 B
    goes alone, bigger ones are cut into FU fragments (§14.9)            rtp.c
 5  build_header(): RTP header + abs-send-time + transport-cc seq        rtp.c (§14.8)
    keep a copy in the 1024-packet history (for NACK)
 6  srtp_protect(): encrypt payload, append 10-byte auth tag             rtc.c rtp_out (libsrtp)
 7  ice_send() → selected pair: direct, or via TURN (ChannelData +4 B)   ice.c send_path
 8  net_udp_send() → tunnel socket → TUN_UDP_SEND frame                  net.c (§14.3)
    {sock_id, destination IP:port, payload}; appended to the tunnel
    output buffer; dropped if > 4 MB is already queued
 9  write() to the abstract Unix socket → adbd                           net.c flush
USB ───────────────────────────────────────────────────────────────────────────────────────────────
10  adbd wraps the bytes in WRTE packets (24-byte header, §14.4) and sends them as USB bulk IN transfers
ROUTER ────────────────────────────────────────────────────────────────────────────────────────────
11  usbfs completes the URB → usb_in_done → adb_on_packet(WRTE)          usb.c, adb.c
    agent answers OKAY (lets adbd send the next WRTE)
12  stream on_data → tunnel_data → link_data: append to P.tin,           phone.c
    tun_parse() loop pulls whole frames out of the byte stream
13  handle_frame(TUN_UDP_SEND) → udp_send(): sendto(dest, payload)       phone.c
INTERNET ──────────────────────────────────────────────────────────────────────────────────────────
14  UDP datagram from the router's public IP → Safari (or → TURN server → Safari)
```

The reverse direction (RTCP feedback, STUN, DTLS, SCTP from Safari) is the mirror image:

1. `recvfrom` on the router socket → `on_udp` → `TUN_UDP_RECV` frame, with the source address.
2. `adb_write` → WRTE packets → USB bulk OUT → adbd → the Unix socket.
3. `net.c: on_frame` → `rtc.c: net_udp` → `ice_on_udp` → `deliver` → `ice_data`, which demultiplexes by the
   first byte.

**What the router does per packet:**
- It parses an 11-byte tunnel header (`af = 4`, see §14.3).
- It makes one `sendto` or `recvfrom` call.
- It doesn't decrypt anything and doesn't touch the RTP header.

**When the router can't keep up:**
- Packets from the internet are dropped once 1 MB is waiting for the phone (`LINK_BACKLOG_MAX`).
- On the phone, if more than 1 MB is waiting to go to the router, delta frames are skipped and a keyframe is
  requested (`rtc_video`).
- Dropping is better than queueing for a game, because a queued packet arrives late and is useless anyway.

**Overhead added to a 1,134-byte SRTP packet on its way through the router:**

| Hop | Added |
|---|---|
| Tunnel frame | 11 B |
| ADB WRTE header | 24 B per WRTE, and one WRTE may carry several frames |
| USB | Bulk transfer overhead |
| Internet | UDP 8 B + IPv4 20 B, the same as if the phone were on the internet itself |

### 7.2 Path B: the phone uses its own Wi-Fi or mobile data

With `[phone] path = wifi`, Java calls `Native.setDirect(true)`, and `net_udp_open` creates a **real UDP socket
on the phone**:
- The host candidates are the phone's own interface addresses (`report_local_addrs`).
- Steps 8–13 disappear: media leaves the phone's radio directly.
- The router and the tunnel still carry ADB, the control messages and signaling.

---

## 8. What happens when you tap the screen

```
SAFARI ────────────────────────────────────────────────────────────────────────────────────────────
 1  pointerdown on <video>                                                app.js down()
 2  map the browser pointerId → a small pointer number 0–9 (allocPid)
 3  normalise the position to the video rectangle: nx = (clientX − left) / width (0..1)
 4  encTouch(): 14 bytes  01 00 00 01 | x u16 | y u16 | pressure u16 | ts u32   (§14.14)
 5  st.ch.input.send(bytes)  → the "input" data channel
 6  Safari's SCTP puts it in a DATA chunk (stream 2, PPID 53 binary)      (§14.12)
 7  DTLS encrypts the SCTP packet into an application_data record         (§14.7)
 8  UDP to the selected ICE pair (the router's socket, or via TURN)
ROUTER ────────────────────────────────────────────────────────────────────────────────────────────
 9  recvfrom → TUN_UDP_RECV frame → adb_write → WRTE → USB bulk OUT       phone.c on_udp
PHONE: C thread ───────────────────────────────────────────────────────────────────────────────────
10  adbd → Unix socket → net.c on_conn_read → on_frame(UDP_RECV)
11  ice_on_udp → deliver (from a validated pair) → rtc.c ice_data
12  first byte 23 (DTLS application_data) → dtls_input → mbedTLS decrypt → dtls_data
13  sctp_input → usrsctp_conninput → usrsctp sends a SACK back (same path, reversed)
    → receive_cb → stream 2 = "input", PPID 53 → sctp_msg
14  cb_message → JNI CallStaticVoidMethod(Native.onMessage, "input", true, byte[14])
PHONE: Java (still on the C thread) ───────────────────────────────────────────────────────────────
15  Native.onMessage → Server.onMessage → Session.message("input") → Controller.handle(bytes)
16  x = 0x8000/65535 × displayWidth (display size in its current rotation)
17  touch(): pointer slot bookkeeping, then MotionEvent.obtain(downTime, now, ACTION_DOWN or
    ACTION_POINTER_DOWN | index<<8, pointerCount, props, coords, …, SOURCE_TOUCHSCREEN)
18  Device.inject(ev) → InputManager.injectInputEvent(ev, ASYNC) through reflection (hidden API)
ANDROID ───────────────────────────────────────────────────────────────────────────────────────────
19  InputDispatcher delivers the touch to the app/game under the finger, exactly like a real finger
20  the app redraws → SurfaceFlinger composes the screen → the mirrored virtual display →
    MediaCodec → the next video frame carries the result back (§7)
```

- **Moves** are the same message with action 2, sent on every `pointermove`.
- **Lifting** sends action 1.
- **Multi-touch:** a second finger becomes `ACTION_POINTER_DOWN` with the pointer index in bits 8–15.
  `Controller` keeps up to 10 pointers.

Touches take the shortest path that exists in the system. There is no JSON and no thread hop in Java. The only
work is the binary decode, `MotionEvent.obtain` and one Binder call into `system_server`.

---

## 9. Commands: the four ways an instruction reaches the phone

> I've read "instructment" as *instructions / commands*. If you meant Android's **instrumentation** framework
> (`am instrument`, used for UI tests): RemoDroid doesn't use it. It injects events directly from a
> shell-user process (§8 step 18), which is faster and needs no test APK.

| Path | Used for | Works without a video session? | Code |
|---|---|---|---|
| **A. `ctrl` data channel** (viewer ⇄ phone, JSON over WebRTC) | Everything during a session: `key`, `nav` (back/home/recents…), `text`, `clip.set`, `quality.set`, `screen.power`, `rotate`, `shell.exec`, `logcat.start/stop`, `file.*`, `ping` | No | `app.js: sendCtrl` → `Session.control()` |
| **B. REST command** (viewer → server → agent) | `POST /api/v1/devices/{id}/cmd {"cmd":"phone.wake"}`, and `server.restart`, `phone.sleep`, `phone.reboot`, `adb.reconnect`, `agent.reload`, `agent.config` (settings) | Yes | `api.c` → `hub_forward_cmd` → agent `wsc.c` `"cmd"` → `phone_run_cmd()` |
| **C. Tunnel CTRL** (agent ⇄ phone server) | `config` (settings), `cmd` (wake/sleep), `session.*`/`signal` from the server; phone → agent: `ready`, `status`, `event`, `signal`, `session.ended` | Yes | `phone.c: send_ctrl / handle_ctrl` ⇄ `rtc.c: net_ctrl` → `Server.onCtrl` |
| **D. Router CLI** (`remodroid-agent shell …` on the router) | `devices`, `status`, `shell`, `logcat`, `push`, `pull`, `key`, `restart-server`, `reconnect`, `reload` | Yes (ADB directly) | `cli.c` → Unix socket → the running agent → `adb_shell` / `adb_push` |

**Example A**, pressing Home in the menu:

```
viewer → ctrl:  {"t":"nav","id":12,"d":{"button":"home"}}
phone:          Session.control → device.press(KEYCODE_HOME) → two KeyEvents (down, up) injected
phone → ctrl:   {"t":"ack","id":12,"d":{}}
```

**Example B**, waking the phone with no session:

```
Safari  POST /api/v1/devices/router1/cmd  {"cmd":"phone.wake","args":{}}       (cookie auth)
server  → agent WS: {"t":"cmd","id":7,"d":{"cmd":"phone.wake","args":{}}}      (waits ≤ timeout)
agent   phone_run_cmd: tunnel up? → TUN_CTRL {"t":"cmd","d":{"cmd":"phone.wake"}} → Server.onCtrl → device.wake()
        tunnel down but ADB up? → adb shell "input keyevent KEYCODE_WAKEUP"
agent   → {"t":"ack","id":7}  → server → HTTP 200 to Safari
```

**Settings flow:**
1. The web menu sends `agent.config`, for example `{"kbps":12000}`.
2. The agent maps web names to ini keys through the `CFG` table, so `kbps` becomes `[video] max_bitrate_kbps`.
3. It rewrites `agent.ini`, keeping its comments, and reloads it.
4. It sends `config` to the phone, and `Server.onCtrl` applies it.

---

## 10. The Java classes and how they work together

```
                         ┌────────────────────────────── one process: app_process (uid shell) ─────────────────────────────┐
                         │                                                                                                  │
   agent (tunnel CTRL) ──┼─► C stack ──onCtrl/onLink/onSession/onChannelOpen/onMessage/onKeyframe/onBitrate──► Native ───► Server
                         │      ▲                                                                                    │      │
                         │      │ sendCtrl / dcSend / video / setMaxKbps / setDirect / stats (queued to the C thread)  │      │ owns
                         │      │                                                                                    │      ▼
                         │      └──────────────── Native ◄──────────────────────────┐                     Config, Device, scheduler
                         │                                                          │                             │
                         │                         per viewer session:  Session ────┤ creates on CONNECTED        │
                         │                                               │  │  │    │                             │
                         │                         ScreenEncoder ◄───────┘  │  └──► Files ──► Shell (media scan)  │
                         │                         (encoder thread)         │                                     │
                         │                         Controller ◄─────────────┘ (input bytes)                       │
                         │                              │                                                         │
                         │                              └──────────► Device ◄──── Hidden (reflection helpers) ─────┘
                         │                                                                                                  │
                         │   CleanUp: a SECOND app_process, started by Server; restores the panel / locks if Server dies    │
                         └──────────────────────────────────────────────────────────────────────────────────────────────────┘
```

| Class | Lives | Responsibility | Talks to |
|---|---|---|---|
| `Server` | Whole process | `main()`: loads the `.so`, starts the native stack, starts `CleanUp`, watches the clipboard, sends `status` every 30 s. Implements `Native.Listener`: on `CONNECTED` creates a `Session`; on `ENDED` tells it to stop and schedules the lock after the grace period. Handles `config` and `cmd` from the agent | `Native`, `Session`, `Device`, `Config` |
| `Native` | Static | The JNI declarations (`start`, `sendCtrl`, `dcSend`, `video`, …) and the static callbacks the C code calls, forwarded to the listener (`Server`) | `jni.c` |
| `Session` | One per viewer | On connect: wake the phone, cancel a pending lock, configure and start `ScreenEncoder`. On `ctrl` channel open: send `session.ready` (screen size, codec, caps). Dispatches `ctrl` JSON (§9 A), `input` bytes (→ `Controller`), `file` chunks (→ `Files`). Sends `stats` each second, `clip.changed`, `screen.changed`. On end: stop the encoder and logcats, turn the panel back on | `ScreenEncoder`, `Controller`, `Files`, `Shell`, `Device`, `Native` |
| `ScreenEncoder` | Own thread | Picks a hardware encoder (`MediaCodecList`). Creates a MediaCodec input `Surface` and a mirrored virtual display that draws into it (`DisplayManager.createVirtualDisplay` on Android 14+, `SurfaceControl.createDisplay` before). Drains encoded frames → `Native.video`. Puts the codec config (VPS/SPS/PPS) in front of keyframes. Rebuilds itself on rotation or size change. Applies `setBitrate` and `requestKeyframe` without a restart | `Device` (display info), `Native` |
| `Controller` | Inside `Session` | Decodes 14-byte touch / 13-byte scroll messages, keeps up to 10 pointer slots, builds `MotionEvent`s | `Device.inject` |
| `Device` | Whole process | The phone through hidden APIs: display size and rotation, `inject` (InputManager), key presses, `text`, panel power (`SurfaceControl.setDisplayPowerMode`, or `DisplayControl` on 14+), wake/sleep, clipboard get/set/listen (`IClipboard` + a Binder `ClipListener`), battery from `/sys` | `Hidden` |
| `Files` | Inside `Session` | `file.push` (upload to `/sdcard/DCIM/RemoDroid`, SHA-256 check, media scan), `file.pull`, `file.list`, `file.delete`; 16 KB binary chunks on the `file` channel | `Native.dcSend`, `Shell` |
| `Shell` | Static | `shell.exec` with a timeout and output limit, `runQuiet`, `Logcat` streaming in batches | `Runtime.exec` |
| `Config` | Whole process | Typed settings from the agent's `config` message (`[phone]`, `[video]`, `[files]`) | — |
| `Hidden` | Static | Reflection helpers: `ServiceManager.getService` + `Stub.asInterface`, find a method by name and parameter count (signatures differ between Android releases) | — |
| `Ln` | Static | Logging to stderr → shell stream → agent ring buffer → `diag` | — |
| `CleanUp` | Separate process | Reads `panel_off`, `panel_on`, `lock_on_exit=…` lines on stdin. When stdin closes (the server died, even from a crash or `kill -9`), it turns the panel back on and optionally locks the phone. Started with `setsid` so it survives adbd's SIGHUP | `Device` |

**Why reflection everywhere?** The jar is compiled against the public `android.jar`, but input injection, screen
power, virtual displays mirroring the real screen and clipboard listeners are hidden or system APIs. They exist
in the phone's framework at runtime, so `Hidden` and `Device` look them up by name.

---

## 11. Server internals: HTTP, auth, hub, TURN

`remodroid-server` is one thread with one libev loop (`server/main.c`). It handles:
- TCP port 80 for HTTP and WebSockets;
- UDP port 3478 for STUN/TURN;
- one UDP socket per TURN allocation;
- timers for pings and session sweeps.

### 11.1 HTTP (`http.c`)

1. `on_accept` → a `conn_t`.
2. `on_read` appends to `c->in`, then `process_http` parses the request with **picohttpparser**
   (`phr_parse_request`).
3. `handle_request` routes:

| Route | Auth | Handler |
|---|---|---|
| `GET /.well-known/acme-challenge/*` | None (checked first, on either listener) | `acme_challenge` (§11.5) |
| anything on plain HTTP while HTTPS is up | None | 308 redirect to `https://` (`[tls] plain_http = redirect`) |
| `GET /ws/agent` | Device cookie, or Basic against `[devices]` | `ws_upgrade` → `hub_agent_open` |
| `GET /ws/viewer` | User cookie + Origin check | `ws_upgrade` → `hub_viewer_open` |
| `/api/v1/*` | User cookie (else 401 JSON) | `api_handle` (`api.c`): `me`, `logout`, `devices`, `devices/{id}`, `/adb`, `/cmd`, `/diag` |
| `POST /login` | Name + password (JSON) + Origin check → `Set-Cookie` | `auth_login_request` |
| `GET /login` | None: the login page (`web/login.html`); also `setup.css`, icons, manifest | `serve_static` |
| any other `GET` | Cookie, or 302 to `/login?next=…` | `serve_static` (files from `web_root`, `sendfile`) |

**After the upgrade:**
- the same `conn_t` switches to `CONN_WS`;
- `process_ws` reassembles frames (`common/ws.c`), parses the JSON with **cJSON**, and calls `hub_on_message`.

**Freeing connections:** a closed connection is freed by an `ev_check` reaper after the current callback
returns. Nothing ever frees an object while a caller further up the stack still uses it.

### 11.2 Auth (`auth.c`)

- **Passwords:** `[users]` and `[devices]` hold bcrypt hashes (`$2b$10$…`, cost 10), created with
  `remodroid-server hashpw`. `auth_check_password` uses crypt_blowfish (`crypt_rn`) and compares the result
  in constant time.
- **Sessions:** a linked list of `{id (32 hex), user/device name, is_device, created}`. They are valid for
  `session_days`, swept periodically, and `logout` deletes one.
- **Lockout:** a failure counter per client IP. After `max_failed_logins` failures, logins from that IP
  (the login page and routers' Basic auth alike) get 429 for `lockout_seconds` (or `lockout_minutes`).

### 11.3 Hub (`hub.c`): who is connected, and relaying

**State:**
- One `device_t` per `[devices]` entry: its agent connection, the last `hello`, the last `status`, the active
  `vsession_t`.
- Viewers are just `conn_t`s with role `VIEWER`.

**Relaying:**
- `viewer_message` / `agent_message` implement the session and signaling relay of §5.
- `status` and `event` from agents are broadcast to all viewers as `device.update` / `device.event`.
- `hub_forward_cmd` turns a REST request into a `cmd` message with an id. It parks the HTTP connection
  (`busy`) until the agent's `ack`/`error` with that id arrives or the request times out (`pend_timeout`).
- An agent that reconnects replaces its old connection.
- A disconnecting agent or viewer ends its session.

### 11.4 TURN (`turn.c`) and its credentials

**Issuing credentials** (`hub.c: ice_servers`). On each session request the server computes:

```
username   = "<now + credential_ttl_minutes×60>:<user>"            e.g. "1792180800:me"
credential = base64( HMAC-SHA1( [turn] shared_secret, username ) )
```

Nothing is stored. The TURN code can recompute the password from the username alone, which is why this scheme
(the "TURN REST API", also used by coturn) works for both the built-in relay and an external one.

**Allocating** (phone side `ice.c`, server side `turn.c: handle_request`):

```
phone  Allocate  REQUESTED-TRANSPORT=UDP, LIFETIME=600                       (no credentials)
server 401 Unauthorized  REALM="remodroid"  NONCE="<32 hex>"
phone  Allocate  + USERNAME, REALM, NONCE, MESSAGE-INTEGRITY
         key for MESSAGE-INTEGRITY = MD5("1792180800:me" ":" "remodroid" ":" password)
server authenticate(): nonce current? expiry in the future? recompute password → key → check HMAC
       open_relay(): bind a UDP socket in relay_min_port..relay_max_port
       Allocate success  XOR-RELAYED-ADDRESS=public_ip:49160  XOR-MAPPED-ADDRESS=<router public ip:port>
       LIFETIME=600
phone  CreatePermission  XOR-PEER-ADDRESS=<Safari's address>                  (when pairing with it)
phone  ChannelBind  CHANNEL-NUMBER=0x4000  XOR-PEER-ADDRESS=<Safari>         (once the relay pair is chosen)
phone  Refresh  LIFETIME=600                                                  (before expiry)
```

**Relaying** (`on_packet`, `on_relay`):

| Direction | Framing | What the server does |
|---|---|---|
| Phone → Safari | ChannelData `40 00 <len> <data>` (or a Send indication) | Strips the header and `sendto`s the data from the relay socket to the peer |
| Safari → relay port | Plain UDP datagram | Dropped unless the source IP has a permission. Otherwise wrapped as ChannelData (or a Data indication) and sent to the phone's address (the router's public IP:port) |

STUN Binding requests on port 3478 are answered too, which gives the phone its srflx candidate.

**Notes:**
- `public_ip` matters on EC2: the instance only sees its private IP, but the relayed address must be the
  public one.
- The nonce rotates every hour, and an old nonce gets `438 Stale Nonce`.
- `max_allocations` limits abuse.

### 11.5 HTTPS and automatic certificates (`tls.c`, `acme.c`)

**HTTPS** (`tls.c`): a second listener (`[tls] listen`, default `0.0.0.0:443` when ACME is on). Each accepted
connection gets an mbedTLS context; `conn_read` / `conn_write` in `http.c` go through `tls_read` / `tls_write`
instead of `read` / `write`, so HTTP, the API and both WebSockets work unchanged on top. Static files go through
the output buffer instead of `sendfile()`. A new certificate replaces the config for new connections, and open
connections keep the old one until they close.

**ACME** (`acme.c`, RFC 8555), from `[acme] domains = remo.example.com`:

```
worker thread                                   Let's Encrypt                     event loop
 GET  directory ─────────────────────────────►  newNonce, newAccount, newOrder URLs
 HEAD newNonce  ─────────────────────────────►  Replay-Nonce
 POST newAccount {termsOfServiceAgreed}  (JWS signed with account.key, "jwk")  → account URL ("kid")
 POST newOrder {identifiers:[dns names]}  ───►  order: authorizations[], finalize URL
 for each authorization:
   POST-as-GET authz ────────────────────────►  http-01 challenge: token, url
   publish token → keyAuthorization = token + "." + base64url(SHA-256(JWK))  ──►  challenge table
   POST challenge {} ────────────────────────►  CA fetches http://<domain>/.well-known/acme-challenge/<token>
                                                 ─────────────────────────────────►  served from the table
   poll authz until "valid"
 new key.pem (P-256), CSR with the names  ───►  POST finalize {csr}
 poll order until "valid", download chain ───►  cert.pem
 write files, ev_async ─────────────────────────────────────────────────────────►  tls_load(): new certificate
```

Every POST is a JWS: `{"protected": b64url({"alg":"ES256","nonce","url","jwk"|"kid"}), "payload": b64url(json),
"signature": b64url(r‖s)}`. A `badNonce` answer is retried with the fresh nonce. On start, a stored certificate
that covers the domains is loaded straight away. A check runs every 6 hours, and renewal starts when a third of the
lifetime is left. Failures retry after 10 minutes, doubling up to 12 hours.

### 11.6 Setup wizard and enrollment (`setup.c`, `enroll.c`)

**Setup.**
- With no `[users]` the server is in setup mode: it serves only `wizard.html` and `/api/v1/setup`.
- The setup API is unlocked by the token in `<state_dir>/setup-token`. The installer prints the link with it.
- Saving rewrites `server.ini` line by line with `rd_conf_write` (comments kept), then re-execs
  `/proc/self/exe` on the new ports. All sockets are close-on-exec.

**Settings after setup** (`settings_api`, the panel's Server tab, `web/panel.html#server`):
- `GET /api/v1/settings` returns the current values; the STUN/TURN list comes back as
  `[{url, username, has_credential}]`, never with its passwords.
- `PATCH /api/v1/settings` carries only the fields that changed. A new name or password needs
  `current_password`. A list entry without a password keeps the one it had (matched by `was`, its old URL).
- Only a change to a listener (web/HTTPS port, domain, built-in TURN) restarts the server. Logins survive
  that restart: `auth_save_sessions` writes them to `<state_dir>/sessions` (0600) just before the re-exec.

**Enrollment:**

```
web/panel.html   POST /api/v1/enroll/codes            → {code: "X5SX-K3M7", expires_in: 600}
router           remodroid enroll --panel https://… --code X5SX-K3M7
                 POST /api/v1/enroll {code, name, arch} (no login: the code is the proof)
                 ← {device_id, device_secret, ws_path}  → agent.ini [server] url/device_id/device_secret
                 the CLI asks the running agent to reload; it connects with wss:// (https panel) or ws://
web/panel.html   GET /api/v1/enroll/codes/X5SX-K3M7     → {state: "used", device, online: true}
```

The router's settings are then read and written from the panel with the `agent.config.get` and
`agent.config` commands. The Cloudflare Worker (`worker/src/hub.js`) implements the same endpoints.

---

## 12. Third-party libraries: what each one is for

All of them are vendored in `third_party/` and compiled into the binaries (static builds, nothing to install).

| Library | What it is | Server | Agent | Phone `.so` | Used for |
|---|---|:-:|:-:|:-:|---|
| **libev** 4.33 | Event loop: waits on many sockets and timers at once and calls a function when one is ready | ✓ | ✓ | ✓ | The single-threaded design of every C program. Each `ev_io` / `ev_timer` / `ev_async` / `ev_signal` / `ev_check` is one "call me when…" |
| **cJSON** | JSON parser and printer | ✓ | ✓ | ✓ | All control messages (`common/msg.c` envelope), config pushes, stats |
| **inih** | `.ini` file parser | ✓ | ✓ | | `server.ini`, `agent.ini` (`common/conf.c`) |
| **picohttpparser** | Fast, zero-copy HTTP header parser | ✓ | ✓ | | Server: every request. Agent: the WebSocket upgrade response (`wsc.c`) |
| **crypt_blowfish** | bcrypt (`$2b$`) password hashing from Openwall | ✓ | | | `auth.c`: checking user and device passwords; `hashpw` |
| **mbedTLS** 3.6.2 | Crypto and (D)TLS library | ✓ | ✓ | ✓ | All: SHA-1, SHA-256, HMAC, MD5, base64 (`common/util.c`) for WebSocket accept keys, TURN credentials and STUN integrity. Server: HTTPS (TLS 1.2), and the ACME client (ECDSA P-256 keys, ES256 signatures, certificate requests, HTTPS to the CA). Agent: RSA-2048 key generation and signing for ADB auth, `wss://` client. Phone: DTLS 1.2 with `use_srtp`, ECDSA P-256 self-signed certificate, key export, random numbers. Config: `common/rd_mbedtls_config.h` (agent, server), `rtc/mbedtls_rtc_config.h` (phone; the desktop library uses both through `desktop/mbedtls_desktop_config.h`) |
| **libsrtp** 2.6.0 | Cisco's SRTP library | | | ✓ | Encrypting every RTP/RTCP packet (`srtp_protect`, `srtp_protect_rtcp`) and decrypting incoming RTCP; uses its own built-in AES and HMAC |
| **usrsctp** 0.9.5.0 | The FreeBSD SCTP stack as a user-space library | | | ✓ | Data channels. Run with `usrsctp_init_nothreads`: we feed it packets (`usrsctp_conninput`), it gives packets back through a callback (`conn_output` → DTLS), and our libev timer drives its timers (`usrsctp_handle_timers`) |

What is **not** used:
- **No WebRTC library:** not libwebrtc, libdatachannel or libjuice. ICE, TURN client, SDP, RTP, RTCP and
  congestion control are RemoDroid's own code.
- **No media libraries:** no FFmpeg. The phone's hardware encoder does all encoding.
- **No USB or adb tools:** no libusb, no `adb` binary. Linux usbfs is used directly.
- **No scrcpy code:** its approach was a reference only.
- **Java and the web client have no dependencies:** only the Android framework, and plain JS respectively.

Test-only tools, which are not in the product: aiortc (Python WebRTC) as the reference peer, Playwright +
headless Chrome, OpenSSL in the fake adbd, and FFmpeg to check decoded frames.

---

## 13. Threads and event loops

| Program | Threads | Notes |
|---|---|---|
| `remodroid-server` | 1 (+1 during ACME) | One libev loop. The CPU-heavy part is bcrypt (cost 10 takes tens of milliseconds, more on a small CPU), which briefly blocks the loop; cookies avoid repeating it. A certificate order runs on a short-lived worker thread so the loop never waits for the CA |
| `remodroid-agent` | 1 | One libev loop for USB, ADB streams, the tunnel, UDP sockets, the WebSocket and the CLI. `getaddrinfo` for TURN host names is the one blocking call |
| Phone process | Several | **main** (Looper, idle) · **native loop** (pthread with its own libev: all WebRTC, all `Native.on*` callbacks, so `Controller.handle` runs here) · **encoder** (MediaCodec drain → `Native.video`) · **Server.sched** (status, lock timer, clipboard) · **Session.exec** (ctrl commands, encoder (re)configuration, stats) · logcat and shell readers · Binder threads (clipboard listener) |
| `CleanUp` process | 1 | Blocks on stdin |

On the phone, every Java → C call is queued to the native loop (§3.5), so the WebRTC stack itself is
single-threaded like the server and the agent.

---

## 14. Packet reference, byte by byte

Conventions:
- Offsets are in bytes from the start of the structure.
- `u8/u16/u32` are unsigned integers of 1/2/4 bytes; `i16` is signed.
- **BE** means big-endian (network order, most significant byte first). That is the default for every
  internet protocol below. **ADB is little-endian (LE).**
- Bit fields are listed from the most significant bit.
- Example bytes are hex.

### 14.1 JSON envelope (WebSockets, `ctrl` channel, tunnel CTRL)

```json
{"t":"session.request","id":3,"sid":"s_7c1e04aa9b3d52f0","d":{"device":"router1"}}
```

| Key | Meaning |
|---|---|
| `t` | Message type (`hello`, `status`, `signal`, `ack`, `error`, `cmd`, …) |
| `id` | Request number chosen by the sender. The reply (`ack` / `error`) carries the same `id`. Omitted for one-way messages |
| `sid` | Stream-session ID. Present on `session.*` and `signal` |
| `d` | The payload object. Errors: `{"code":"busy","msg":"device is already in a session"}` |

### 14.2 WebSocket frame (RFC 6455, `common/ws.c`)

```
 bit:  0   1   2   3   4 5 6 7 │ 0     1 2 3 4 5 6 7
      FIN RSV1 RSV2 RSV3 opcode │ MASK  payload len (7 bits)
      [extended length: 2 bytes if len=126, 8 bytes if len=127]
      [masking key: 4 bytes, only if MASK=1]
      payload (XOR-ed with the masking key, byte i ^ key[i % 4], if masked)
```

| Field | Meaning |
|---|---|
| FIN | 1 = last fragment of a message |
| RSV1–3 | Must be 0, since no extensions are negotiated |
| opcode | `1` text, `2` binary, `8` close, `9` ping, `A` pong, `0` continuation |
| MASK | 1 on frames from client to server. Browsers and the agent are clients; the server's frames are unmasked |

Example, the agent sending `{"t":"status",…}` (180 bytes):
```
81            FIN=1, opcode 1 (text)
FE 00 B4      MASK=1, len=126 → the next 2 bytes hold the length: 0x00B4 = 180
9A 3C 11 E0   masking key
…180 masked bytes…
```

### 14.3 Tunnel frame (agent ⇄ phone, `common/tunnel.h`)

```
off  size  field
 0   u16   len       BE, number of bytes AFTER this field
 2   u8    type      01 UDP_OPEN · 02 UDP_SEND · 03 UDP_RECV · 04 UDP_CLOSE · 05 UDP_BOUND · 20 PING · 21 PONG · 30 CTRL
 3   u8    sock_id   1..255: which UDP socket on the router (0 for CTRL/PING/PONG)
 4   u8    af        4 = IPv4 address follows, 6 = IPv6, 0 = no address and no port
 5   4|16  addr      IPv4 or IPv6 address (absent when af = 0)
 …   u16   port      BE (absent when af = 0)
 …   …     payload   the UDP payload (UDP_SEND/RECV) or one JSON message (CTRL)
```

| Type | Direction | addr:port means | Payload |
|---|---|---|---|
| `UDP_OPEN` | phone → agent | `0.0.0.0:0` (only `af` matters: IPv4 or IPv6 socket) | — |
| `UDP_SEND` | phone → agent | Destination | The datagram to send |
| `UDP_RECV` | agent → phone | Source of the received datagram | The datagram |
| `UDP_CLOSE` | phone → agent | — | — |
| `UDP_BOUND` | agent → phone | A LAN address + port of the router socket (one frame per interface) | — |
| `PING` / `PONG` | both | — | — |
| `CTRL` | both | — | JSON (§14.1) |

Example, the phone sending a 1,134-byte SRTP packet to Safari at 198.51.100.7:51022 on socket 1:
```
04 77         len = 1 + 1 + 1 + 4 + 2 + 1134 = 1143 = 0x0477
02            type UDP_SEND
01            sock_id 1
04            af = IPv4
C6 33 64 07   198.51.100.7
C7 4E         port 51022
80 E0 …       the 1,134-byte SRTP packet (§14.8)
```

Example, a CTRL message: `00 1D 30 00 00 7B 22 74 22 3A …` (len 29, type 30, sock 0, af 0, then
`{"t":"cmd",…}`).

### 14.4 ADB (`agent/adb.c`, AOSP `protocol.txt`)

#### 14.4.1 Packet header: 24 bytes, all fields u32 **little-endian**

```
off  field        meaning
 0   command      4 ASCII letters read as a LE u32: "CNXN" = 0x4E584E43, "AUTH", "OPEN", "OKAY", "WRTE", "CLSE", "SYNC", "STLS"
 4   arg0         depends on the command (see below)
 8   arg1         depends on the command
12   data_length  payload bytes following the header
16   data_crc32   payload checksum; 0 here (optional since protocol version 0x01000001)
20   magic        command XOR 0xFFFFFFFF (a sanity check)
24   payload      data_length bytes
```

| Command | arg0 | arg1 | Payload |
|---|---|---|---|
| `CNXN` | Protocol version (`0x01000001`) | Max payload size (agent asks 262144) | Banner: `host::features=shell_v2,cmd,stat_v2` / `device::ro.product.name=…;features=…` |
| `AUTH` | 1 TOKEN · 2 SIGNATURE · 3 RSAPUBLICKEY | 0 | 20-byte token / 256-byte signature / `base64key user@host\0` |
| `OPEN` | Sender's local stream id | 0 | Service name + `\0` |
| `OKAY` | Sender's local id | Receiver's id | — (stream accepted, or "ready for the next WRTE") |
| `WRTE` | Sender's local id | Receiver's id | Stream data |
| `CLSE` | Sender's local id (0 if refusing an OPEN) | Receiver's id | — |

Example, the agent writing 1,145 bytes of tunnel data into stream 1 (the phone's end is 5):
```
57 52 54 45   "WRTE"
01 00 00 00   arg0 = 1  (agent's local id)
05 00 00 00   arg1 = 5  (phone's id)
79 04 00 00   data_length = 0x0479 = 1145
00 00 00 00   crc (unused)
A8 AD AB BA   magic = ~0x45545257
…1145 bytes…  (one or more tunnel frames, possibly cut at any byte: it's a stream)
```

On USB, the header and the payload are sent as **two separate bulk transfers** (`usb_send`). A transfer that is
an exact multiple of the endpoint's max packet size ends with a zero-length packet. Only one WRTE per stream
may be in flight until the other side's OKAY arrives, which is ADB's flow control.

#### 14.4.2 shell v2 packets (inside WRTE payloads of a `shell,v2,raw:` stream)

```
off  size  field
 0   u8    id      0 stdin · 1 stdout · 2 stderr · 3 exit code · 4 close stdin · 5 window size
 1   u32   length  LE
 5   …     data    for id 3: one byte, the exit code
```

Example: `01 06 00 00 00 50 69 78 65 6C 0A` = stdout "Pixel\n". Then `03 01 00 00 00 00` = exit code 0.

#### 14.4.3 sync packets (inside a `sync:` stream; used by push/pull)

```
off  size  field
 0   4     id      ASCII: "STAT" "SEND" "RECV" "DATA" "DONE" "OKAY" "FAIL" "QUIT"
 4   u32   value   LE: a length, or a time/mode for some ids
 8   …     data
```

Push:
1. `SEND` + len + `"/data/local/tmp/remodroid/remodroid-server.jar,33188"` (path, comma, mode in decimal:
   33188 = 0100644).
2. `DATA` + len + ≤64 KB, repeated.
3. `DONE` + mtime.
4. The phone answers `OKAY` (or `FAIL` + message).

Before pushing, `STAT` + path returns mode/size/mtime, so unchanged files are skipped.

### 14.5 STUN and ICE checks (RFC 8489, RFC 8445, `common/stun.c`)

```
off  size  field
 0   u16   type          bits: 00 | M11..M7 C1 M6..M4 C0 M3..M0  (method M, class C)
 2   u16   length        bytes of attributes (excluding this 20-byte header); always a multiple of 4
 4   u32   magic cookie  21 12 A4 42
 8   12    transaction ID  random; a response echoes it
20   …     attributes:   u16 type | u16 length | value | padding to a multiple of 4
```

Message types used:

| Type | Method / class |
|---|---|
| `0001` | Binding request |
| `0101` | Binding success |
| `0111` | Binding error |
| `0003` / `0103` / `0113` | Allocate request / success / error |
| `0004` | Refresh |
| `0008` | CreatePermission |
| `0009` | ChannelBind |
| `0016` | Send indication |
| `0017` | Data indication |

Attributes:

| Code | Name | Value |
|---|---|---|
| `0006` | USERNAME | ICE: `"<receiver's ufrag>:<sender's ufrag>"`. TURN: `"1792180800:me"` |
| `0008` | MESSAGE-INTEGRITY | 20-byte HMAC-SHA1 over the message so far. Key: ICE = the receiver's `ice-pwd`; TURN = MD5(user:realm:pass) |
| `8028` | FINGERPRINT | CRC-32 of the message XOR `0x5354554E`; always last |
| `0020` | XOR-MAPPED-ADDRESS | `00` · family (`01` v4, `02` v6) · port XOR `0x2112` · address XOR the magic cookie (v6: XOR cookie + transaction ID) |
| `0024` | PRIORITY | u32: the prflx priority the sender would have |
| `8029` / `802A` | ICE-CONTROLLED / ICE-CONTROLLING | u64 random tie-breaker |
| `0025` | USE-CANDIDATE | Empty: "this pair is nominated" (controlling side only) |
| `0009` | ERROR-CODE | 2 reserved bytes · class (hundreds) · number · reason text. 401 = `00 00 04 01 "Unauthorized"` |
| `0014` / `0015` | REALM / NONCE | TURN long-term credential parameters |
| `0019` | REQUESTED-TRANSPORT | `11 00 00 00` (17 = UDP) |
| `000D` | LIFETIME | u32 seconds |
| `0016` | XOR-RELAYED-ADDRESS | The relay address (XOR-encoded like 0020) |
| `0012` | XOR-PEER-ADDRESS | The peer (Safari) address |
| `0013` | DATA | Payload of a Send/Data indication |
| `000C` | CHANNEL-NUMBER | u16 channel (`4000`–`4FFF`) + 2 reserved bytes |

Example, the phone's nominating check to Safari (`ice.c: send_check`). Safari's ufrag is `uQ3n`, the phone's is
`Xq7f2LbA`:
```
00 01 00 4C                        Binding request, 76 bytes of attributes
21 12 A4 42                        magic cookie
9F 0B 4E 77 1A C2 66 38 D0 5E 2B 11   transaction ID
00 06 00 0D  75 51 33 6E 3A 58 71 37 66 32 4C 62 41  00 00 00    USERNAME "uQ3n:Xq7f2LbA" + 3 pad
00 24 00 04  6E FF FF FF           PRIORITY = 110<<24 | 65535<<8 | 255 (type pref 110 = prflx; ice.c cand_prio)
80 2A 00 08  <8 random bytes>      ICE-CONTROLLING tie-breaker
00 25 00 00                        USE-CANDIDATE (empty)
00 08 00 14  <20 bytes HMAC-SHA1>  MESSAGE-INTEGRITY, key = Safari's ice-pwd
80 28 00 04  <4 bytes CRC>         FINGERPRINT
```

Safari answers `01 01 …` (Binding success), with the same transaction ID, an XOR-MAPPED-ADDRESS, and integrity
keyed with the **phone's** pwd.

### 14.6 TURN ChannelData (RFC 8656 §12)

```
off  size  field
 0   u16   channel number   4000–4FFF (first byte 0x40–0x4F tells it apart from STUN/DTLS/RTP)
 2   u16   length           of the data only
 4   …     data             (padding to 4 bytes only over TCP; none over UDP)
```

Example: `40 00 04 6E | 80 E0 …` is 1,134 bytes of SRTP on channel 0x4000. Without a channel binding, the same
data travels in a Send indication: a 20-byte STUN header + XOR-PEER-ADDRESS (12) + DATA header (4) + padding.

### 14.7 DTLS record (RFC 6347)

```
off  size  field
 0   u8    content type     14 ChangeCipherSpec · 15 Alert · 16 Handshake · 17 Application data
 1   u16   version          FE FD = DTLS 1.2 (FE FF = 1.0, used in some first ClientHellos)
 3   u16   epoch            0 before ChangeCipherSpec, 1 after (keys changed)
 5   u48   sequence number  per epoch, for replay detection
11   u16   length           of the fragment
13   …     fragment
```

**Handshake fragments** have their own 12-byte header:

```
type (u8) | length (u24) | message_seq (u16) | fragment_offset (u24) | fragment_length (u24)
```

The fragment offset and length exist because a 1,500-byte certificate (or Chrome's large ClientHello) is cut
into several records. Types:

| Type | Message |
|---|---|
| 1 | ClientHello |
| 2 | ServerHello |
| 11 | Certificate |
| 12 | ServerKeyExchange |
| 13 | CertificateRequest |
| 14 | ServerHelloDone |
| 15 | CertificateVerify |
| 16 | ClientKeyExchange |
| 20 | Finished |

The `use_srtp` extension (type `000E`) in the hellos carries the SRTP profile list: `0001` =
SRTP_AES128_CM_HMAC_SHA1_80, the only one the phone offers.

**Application data** (epoch 1) carries SCTP packets:

```
17 FE FD | 00 01 | 00 00 00 00 00 2A | 00 51 | explicit nonce (8) | encrypted SCTP | GCM tag (16)
```

With an AES-GCM suite (ECDHE-ECDSA-AES128-GCM-SHA256 is what Chrome and mbedTLS normally agree on) each record
adds 13 + 8 + 16 = 37 bytes.

### 14.8 RTP and SRTP (RFC 3550, RFC 3711, RFC 8285)

```
 byte 0:  V V P X C C C C     V = version (2) · P = padding present · X = header extension present · CC = CSRC count
 byte 1:  M T T T T T T T     M = marker (last packet of a frame) · PT = payload type (7 bits)
 2   u16  sequence number     +1 per packet; gaps = loss (NACK)
 4   u32  timestamp           90 kHz clock; all packets of one frame share it
 8   u32  SSRC                which stream (random, from the SDP a=ssrc)
12   …    CSRC list (CC × 4 bytes; always 0 here)
     if X: u16 0xBEDE (one-byte extension profile) | u16 length in 32-bit words | elements | zero padding
           element: ID (4 bits) | L (4 bits, = data length − 1) | data
     payload
SRTP: payload encrypted with AES-128 in counter mode; + 10-byte HMAC-SHA1-80 tag at the end (header stays readable)
```

The extensions the phone writes (`build_header`, ids from the offer):

| ID | Name | Data | Meaning |
|---|---|---|---|
| 1 | abs-send-time | 3 bytes | Send time in seconds, 6.18 fixed point (24 bits, wraps every 64 s). Used by receive-side estimation (REMB) |
| 2 | transport-cc | 2 bytes | Transport-wide sequence number, counting all packets including RTX. Safari reports each one's arrival time (§14.11) |
| 3 | playout-delay | 3 bytes | min (12 bits) + max (12 bits) in 10 ms units; `00 00 00` = "render immediately". Sent on keyframe packets |

A keyframe's first packet (H.265, all three extensions):
```
90           V=2, P=0, X=1, CC=0
60           M=0, PT=96 (H.265)           (the frame's last packet has E0: M=1)
1A 40        sequence 6720
5D 3A 10 C4  timestamp
12 34 56 78  SSRC 305419896
BE DE 00 03  extension profile, 3 words (12 bytes) follow
12 4B 9F 02  ID 1, L=2 → 3 bytes abs-send-time 4B 9F 02
21 00 C7     ID 2, L=1 → 2 bytes transport-cc seq 0x00C7
32 00 00 00  ID 3, L=2 → 3 bytes playout-delay min 0 / max 0
00           zero padding to the 4-byte boundary (4 + 3 + 4 + 1 = 12 bytes)
40 01 0C 01 …  payload: the VPS NAL unit (type 32), sent alone because it is small
… 10-byte SRTP auth tag
```

Size budget (`rtp.c`):
- the payload is at most 1,100 bytes (`MAX_PAYLOAD`);
- the header is 12, plus 12 or 16 of extensions;
- the SRTP tag adds 10, for a total of about 1,134–1,138 bytes;
- this leaves room for TURN (+4), IPv6 (+40) and the tunnel without IP fragmentation.

### 14.9 H.265 and H.264 payloads (RFC 7798, RFC 6184)

The encoder produces **Annex-B**: NAL units separated by start codes `00 00 00 01`. `annexb_split` removes the
start codes. AUD and SEI units are dropped, and each NAL unit is then sent alone or fragmented.

**H.265 NAL unit header**, 2 bytes:

```
F (1) | Type (6) | LayerId (6) | TID (3)
```

| Bytes | Type | Meaning |
|---|---|---|
| `40 01` | 32 | VPS |
| `42 01` | 33 | SPS |
| `44 01` | 34 | PPS |
| `26 01` | 19 | IDR_W_RADL (keyframe slice) |
| `02 01` | 1 | TRAIL_R (normal slice) |

**H.265 Fragmentation Unit** (FU, used when a NAL unit is longer than 1,100 bytes):

```
PayloadHdr (2 bytes): F | Type = 49 | LayerId | TID   → 62 01
FU header  (1 byte):  S (start) | E (end) | FuType (6 bits) = the original type
then a piece of the NAL unit body (without its own 2-byte header)
```

Example, a 30 KB IDR slice in 28 packets:
- first packet: `62 01 93` (S=1, type 19);
- middle packets: `62 01 13`;
- last packet: `62 01 53` (E=1).

The receiver rebuilds the header from PayloadHdr and FuType.

**H.264 NAL unit header**, 1 byte:

```
F (1) | NRI (2, importance) | Type (5)
```

| Byte | Type | Meaning |
|---|---|---|
| `67` | 7 | SPS |
| `68` | 8 | PPS |
| `65` | 5 | IDR slice |
| `41` / `61` | 1 | Non-IDR slice |

**H.264 FU-A:**

```
FU indicator (1 byte): F | NRI | Type = 28   → 7C (for NRI = 3)
FU header    (1 byte): S | E | R | original Type   → 85 first, 05 middle, 45 last (for type 5)
```

### 14.10 RTX retransmission (RFC 4588)

A resent packet uses the RTX payload type and SSRC, its own sequence number, and the original sequence number
(OSN) as the first 2 payload bytes:

```
90 | 61 (PT 97) | rtx seq | same timestamp | RTX SSRC | same extensions (new transport-cc seq) | OSN (2) | original payload | SRTP tag
```

### 14.11 RTCP (RFC 3550, RFC 4585, RFC 5104, transport-cc draft, REMB draft)

Common header, 4 bytes:

```
V (2) = 2 | P (1) | RC / FMT (5) | PT (8) | length (u16) = packet length in 32-bit words minus 1
```

| PT | FMT | Name | Sender | Body after the header |
|---|---|---|---|---|
| 200 | RC | **SR** Sender Report | Phone, each second | sender SSRC · NTP time (8) · RTP time (4) · packet count (4) · octet count (4) |
| 201 | RC | **RR** Receiver Report | Safari | sender SSRC · report blocks |
| 202 | SC | **SDES** | Phone | CNAME `remodroid` |
| 205 | 1 | **Generic NACK** | Safari | sender SSRC · media SSRC · {PID u16, BLP u16}… |
| 205 | 15 | **transport-cc** feedback | Safari | sender SSRC · media SSRC · base seq (2) · status count (2) · reference time (3, ×64 ms) · feedback count (1) · status chunks · arrival deltas |
| 206 | 1 | **PLI** picture loss | Safari | sender SSRC · media SSRC (nothing else) |
| 206 | 4 | **FIR** full intra request | Safari | sender SSRC · 0 · {SSRC (4), seq nr (1), 3 reserved}… |
| 206 | 15 | **REMB** | Safari | sender SSRC · 0 · `52 45 4D 42` ("REMB") · num SSRC (1) · exponent (6 bits) + mantissa (18 bits) · SSRCs. Bitrate = mantissa × 2^exponent |

**Report block** (24 bytes, in RR/SR):

| Field | Size |
|---|---|
| SSRC | 4 |
| fraction lost | 1 (loss since the last report, /256) |
| cumulative lost | 3 |
| extended highest sequence | 4 |
| jitter | 4 (90 kHz units) |
| LSR | 4 (middle 32 bits of the last SR's NTP time) |
| DLSR | 4 (delay since that SR, in 1/65536 s) |

The phone computes **RTT = now − LSR − DLSR** (`handle_rr_block`) and passes the loss to `cc_on_loss`.

**NACK example:**

```
81 CD 00 03 | sender SSRC | 12 34 56 78 | 1A 41 00 02
```

- `81`: V=2, FMT=1. `CD`: PT 205. `00 03`: length 3 → 16 bytes.
- `1A 41 00 02`: PID 0x1A41, BLP 0x0002 → packets 0x1A41 and 0x1A43 are lost (BLP bit *i* means PID+1+*i*).

**PLI example:**

```
81 CE 00 02 | sender SSRC | 12 34 56 78
```

PT 206, FMT 1. The phone then asks MediaCodec for a sync frame, at most once per 300 ms.

**transport-cc chunks** (2 bytes each), which `cc_on_twcc` decodes:

| First bit | Chunk type | Layout |
|---|---|---|
| 0 | Run length | status symbol (2 bits) + run length (13 bits) |
| 1 | Status vector | next bit 0: fourteen 1-bit symbols; 1: seven 2-bit symbols |

Symbols:

| Symbol | Meaning | Delta that follows |
|---|---|---|
| `00` | Not received | none |
| `01` | Received, small delta | 1 byte, 250 µs units |
| `10` | Received, large or negative delta | 2 bytes, signed |

The congestion controller compares send-time gaps with arrival-time gaps (`cc.c`).

In a compound packet, Safari sends RR + feedback back to back. With `rtcp-rsize`, a lone feedback packet is
allowed. Everything is SRTCP-protected: the RTCP packet, then 4 bytes of E-flag + SRTCP index, then the 10-byte
tag.

### 14.12 SCTP inside DTLS (RFC 9260, RFC 8261)

**Common header** (12 bytes):

| Field | Size | Value |
|---|---|---|
| source port | u16 | `13 88` (5000) |
| destination port | u16 | `13 88` |
| verification tag | u32 | Chosen during the INIT handshake |
| checksum | u32 | CRC-32c |

Then one or more **chunks**:

```
type (u8) | flags (u8) | length (u16, header + value, before padding) | value | padding to 4 bytes
```

Chunk types:

| Type | Name |
|---|---|
| 0 | DATA |
| 1 | INIT |
| 2 | INIT ACK |
| 3 | SACK |
| 4 | HEARTBEAT |
| 5 | HEARTBEAT ACK |
| 6 | ABORT |
| 7 | SHUTDOWN |
| 10 | COOKIE ECHO |
| 11 | COOKIE ACK |
| 130 | RE-CONFIG (stream reset; browsers use it to close a channel) |

The association starts with INIT → INIT ACK → COOKIE ECHO → COOKIE ACK. As in every WebRTC implementation,
both sides call connect (the phone's `usrsctp_connect` in `sctp_new`), and SCTP resolves the simultaneous open.

**DATA chunk** (16-byte header):

```
off  size  field
 0   u8    type 0
 1   u8    flags   bit 3 I (send SACK immediately) · bit 2 U (unordered) · bit 1 B (first piece) · bit 0 E (last piece)
 2   u16   length  16 + user data
 4   u32   TSN     transmission sequence number (per association; SACKs acknowledge it)
 8   u16   stream identifier   = the data channel (ctrl 0, input 2, file 4)
10   u16   stream sequence number  (ordering within the stream)
12   u32   PPID    payload protocol: 50 DCEP · 51 text · 53 binary · 56/57 empty text/binary
16   …     user data
```

**SACK:**

```
cumulative TSN ack (u32) | advertised receive window (u32) | number of gap blocks (u16) | number of duplicates (u16) | …
```

### 14.13 DCEP: opening a data channel (RFC 8832)

`DATA_CHANNEL_OPEN` (PPID 50, on the new channel's stream):

```
off  size  field
 0   u8    message type   03 = OPEN (02 = ACK, which is the whole message)
 1   u8    channel type   00 reliable ordered · 80 reliable unordered · 01/81 partial (max retransmits) · 02/82 partial (lifetime)
 2   u16   priority
 4   u32   reliability parameter  (retransmit count or lifetime in ms; 0 for reliable)
 8   u16   label length
10   u16   protocol length
12   …     label, then protocol
```

Example: the phone opens `ctrl` on stream 0:

```
03 00 00 00 00 00 00 00 00 04 00 00 63 74 72 6C
```

- `03 00`: OPEN, reliable ordered.
- `00 00`: priority 0.
- `00 00 00 00`: reliability 0.
- `00 04 00 00`: label length 4, protocol length 0.
- `63 74 72 6C`: "ctrl".

Safari answers `02` on the same stream.

### 14.14 Input messages (`input` channel, plan.md A.7)

**Touch**, 14 bytes:

```
off  size  field
 0   u8    type      01 = touch
 1   u8    action    0 down · 1 up · 2 move · 3 cancel
 2   u8    pointer   0–9 (app.js allocPid)
 3   u8    flags     bit 0: last message of a batch (the web client always sets 1)
 4   u16   x         BE, 0..65535 across the video width
 6   u16   y         BE, 0..65535 across the video height
 8   u16   pressure  0..65535 (65535 if unknown; 0 on up)
10   u32   ts        viewer clock in ms (for latency statistics)
```

Example, a finger down in the centre: `01 00 00 01 80 00 80 00 FF FF 00 01 E2 40`.

| Bytes | Field |
|---|---|
| `01` | touch |
| `00` | down |
| `00` | pointer 0 |
| `01` | flags |
| `80 00` | x = 32768 → 0.5 × width |
| `80 00` | y = 0.5 × height |
| `FF FF` | full pressure |
| `00 01 E2 40` | ts 123456 ms |

**Scroll**, 13 bytes:

```
off  size  field
 0   u8    type 02
 1   u16   x
 3   u16   y
 5   i16   h     horizontal, 1/256 of a wheel notch
 7   i16   v     vertical, 1/256 of a wheel notch
 9   u32   ts
```

**How the touch above looks on the wire from Safari to the phone (relayed), layer by layer:**

| Layer | Bytes | Running total |
|---|---|---|
| Touch message | 14 | 14 |
| SCTP DATA chunk header (16) + padding (2) | +18 | 32 |
| SCTP common header | +12 | 44 |
| DTLS record header (13) + GCM nonce (8) + tag (16) | +37 | 81 → the UDP payload Safari sends |
| TURN ChannelData (relayed only: added by the TURN server on its way to the router) | +4 | 85 |
| UDP + IPv4 headers on the internet | +28 | 109 direct, 113 relayed |
| On the router: tunnel frame header (`UDP_RECV`, IPv4) | +11 | 92 (or 96 if relayed) |
| ADB WRTE header (shared if several frames are batched) | +24 | |

### 14.15 File chunks (`file` channel, plan.md A.8)

```
off  size  field
 0   u32   xfer     BE, transfer id from file.push / file.pull
 4   u32   seq      BE, chunk number from 0
 8   …     payload  up to 16 KB
```

The control side runs over `ctrl`:
1. `file.push {name,size,sha256,…}` → `ack {xfer}`.
2. The chunks.
3. `file.done {xfer}` → the phone checks the SHA-256 → `ack {path}`.

---

## 15. Glossary of abbreviations

| Term | Meaning |
|---|---|
| **ABI** | Application Binary Interface: the phone CPU type for native code (`arm64-v8a`, `armeabi-v7a`) |
| **ADB / adbd** | Android Debug Bridge; `adbd` is its daemon on the phone |
| **AES-CM / GCM** | AES in Counter Mode (SRTP encryption) / Galois-Counter Mode (encryption + authentication in one) |
| **af** | Address family: IPv4 or IPv6 |
| **Annex-B** | H.264/H.265 byte-stream format with `00 00 00 01` start codes |
| **ART / app_process** | Android Runtime / the launcher that starts a Java `main()` on Android |
| **BLP** | Bitmask of following Lost Packets (in a NACK) |
| **BUNDLE** | All media sections share one transport |
| **bcrypt** | Slow, salted password hash (`$2b$10$…`: version 2b, cost 2^10 rounds) |
| **candidate** | One IP:port where a side may be reachable (host, srflx, prflx, relay) |
| **cc / GCC** | Congestion control / Google Congestion Control |
| **CNAME** | Canonical name of an RTP sender (in SDES) |
| **CSD** | Codec-Specific Data: the parameter sets (VPS/SPS/PPS) MediaCodec emits first |
| **DCEP** | Data Channel Establishment Protocol |
| **DTLS** | Datagram TLS: TLS for UDP |
| **FEC** | Forward Error Correction (redundant packets) |
| **FIR / PLI** | Full Intra Request / Picture Loss Indication: both ask for a keyframe |
| **FU / FU-A** | Fragmentation Unit: one NAL unit split over several RTP packets (H.265 / H.264) |
| **ICE** | Interactive Connectivity Establishment |
| **IDR** | Instantaneous Decoder Refresh: a keyframe that needs no earlier frame |
| **JNI** | Java Native Interface: calling C from Java and back |
| **JSEP** | JavaScript Session Establishment Protocol: browser rules for offers/answers |
| **libev** | The event-loop library (see §12) |
| **MID** | Media identification (`a=mid`), names an m-section for BUNDLE |
| **MTU** | Maximum Transmission Unit: the largest packet a link carries without fragmenting |
| **NACK** | Negative acknowledgement: "these packets are missing" |
| **NAL unit** | Network Abstraction Layer unit: one piece of an H.264/H.265 stream (a slice, an SPS, …) |
| **NAT** | Network Address Translation (your router/carrier rewriting addresses) |
| **nomination** | The controlling ICE agent's choice of the pair to use |
| **NTP time** | 64-bit timestamp: seconds since 1900 + fraction, used in SR |
| **OSN** | Original Sequence Number (in an RTX packet) |
| **PPID** | SCTP Payload Protocol Identifier |
| **prflx / srflx** | Peer-reflexive / server-reflexive candidate: an address learned from an incoming check / from a STUN server |
| **PT** | RTP Payload Type: the number that maps to a codec in the SDP |
| **REMB** | Receiver Estimated Maximum Bitrate |
| **REST credentials** | TURN username/password derived from a shared secret and an expiry time |
| **RR / SR** | Receiver Report / Sender Report |
| **RTCP** | RTP Control Protocol |
| **RTP** | Real-time Transport Protocol |
| **RTT** | Round-trip time |
| **RTX** | Retransmission stream (RFC 4588) |
| **SCTP** | Stream Control Transmission Protocol |
| **SDP** | Session Description Protocol |
| **SEI / AUD** | Supplemental info / access unit delimiter NAL units (dropped before sending) |
| **sid** | RemoDroid's session id (`s_` + 16 hex) |
| **SPS / PPS / VPS** | Sequence / Picture / Video Parameter Set: decoder setup |
| **SRTP / SRTCP** | Secure RTP / RTCP |
| **SSRC** | Synchronization Source: 32-bit id of an RTP stream |
| **STUN** | Session Traversal Utilities for NAT |
| **TSN** | Transmission Sequence Number (SCTP) |
| **TURN** | Traversal Using Relays around NAT |
| **twcc / transport-cc** | Transport-wide congestion control feedback |
| **ufrag / pwd** | ICE username fragment and password from the SDP |
| **URB** | USB Request Block: one USB transfer submitted through usbfs |
| **usbfs** | Linux interface `/dev/bus/usb/BBB/DDD` for talking to USB devices from user space |

---

## 16. Suggested reading order

1. **The happy path, top to bottom:**
   - `web/app.js` (`connectWs`, `startSession`, `startPeer`, `onSignal`, `down`);
   - `server/hub.c` (`viewer_session_request`, `agent_message`);
   - `agent/phone.c` (`phone_on_server_msg`, `handle_ctrl`, `udp_send`, `on_udp`);
   - `rtc/rtc.c` (`start_session`, `on_signal`, `ice_state_cb`, `dtls_up`, `ice_data`).
2. **The phone process:** `Server.java` → `jni.c` → `Session.java` → `ScreenEncoder.java` → `Controller.java`.
3. **ADB:** `agent/usb.c: scan_usb, try_usb` → `agent/adb.c: adb_attach, handle_auth, adb_open, adb_shell,
   adb_push` → `agent/phone.c: start_server … launch_server`.
4. **One protocol at a time:** `common/stun.c` + `rtc/ice.c`, then `dtls.c`, `rtp.c` + `cc.c`,
   `sctp.c`, `sdp.c`.
5. **The server's edges:** `server/http.c: handle_request`, `server/auth.c`, `server/turn.c`.
6. **Watching it happen:**
   - Run `python tests/e2e_webrtc.py` (add `--relay` to force TURN) with `log_level = trace`.
   - Every tunnel CTRL message, ADB packet (`adb ->` / `adb <-`), ICE state and SCTP event is logged.
   - The web client's Logs tab shows the protocol log from the browser side.
