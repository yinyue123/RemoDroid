# RemoDroid

Low-latency remote control of an Android phone from an iPhone (Safari), for cloud gaming.
The phone sits on a USB cable to an OpenWrt router; the iPhone connects through a small
server on AWS. Design and decisions: [`plan.md`](plan.md).

<a href="https://remodroid.pages.dev/"><img src="wizard/button.svg" alt="Install RemoDroid with the wizard: remodroid.pages.dev" width="318" height="64"></a>

```
iPhone Safari ──WebRTC (UDP)──► AWS: remodroid-server (HTTP, WebSocket, TURN)
      ▲                                 ▲ ws:// control + keepalive
      │                                 │
      └──── media relayed by the router ── OpenWrt: remodroid-agent ──USB (built-in ADB)── Android phone
                                                                                          remodroid-server.jar
                                                                                          libremodroid.so (C WebRTC)
```

## What's here

| Path | What | Language |
|---|---|---|
| `server/` | Control server: Basic auth + cookies, REST API, viewer/agent WebSockets, built-in TURN relay, optional HTTPS with automatic Let's Encrypt (ACME) certificates | C + libev + mbedTLS |
| `agent/` | Router agent: built-in ADB client (usbfs), phone supervisor, UDP relay, WebSocket client, CLI | C + libev |
| `android/native/` | Phone WebRTC stack: ICE + TURN client, DTLS-SRTP, SCTP data channels, H.265/H.264 RTP, congestion control | C |
| `android/java/` | Phone server: screen capture, MediaCodec, input injection, clipboard, screen power, files, logcat | Java (app_process) |
| `web/` | iPhone client (PWA): video, sound, multi-touch, floating-ball menu (lock, reconnect, full screen), photo upload, clipboard, all settings; `panel.html` (bottom tabs: Devices, Agents with enroll and settings, Server settings one item at a time) and `wizard.html` (first-run setup) | HTML/CSS/JS |
| `worker/` | The same panel on Cloudflare Workers + a Durable Object, with Cloudflare TURN, for people without a server | JS |
| `wizard/` | The install site (Cloudflare Pages, remodroid.pages.dev) and `install_server.sh` / `install_agent.sh` | HTML, sh |
| `common/` | Shared C: WebSocket framing, tunnel frames, STUN, JSON envelope, ini, utilities | C |
| `third_party/` | Vendored libraries, compiled in statically: libev, inih, cJSON, picohttpparser, crypt_blowfish, mbedTLS, libsrtp2, usrsctp | C |
| `deploy/` | Example `server.ini` / `agent.ini`, systemd units, OpenRC and procd scripts | |
| `packaging/`, `.github/workflows/` | nFPM package definitions (deb, rpm, apk, ipk) and the CI build/release workflow | |
| `tests/` | End-to-end tests (see below) | Node, Python |
| `demo/` | The original mock panel (offline, no server) | HTML |

## Build

Host build (development and tests):

```sh
make            # build/host/remodroid-server, build/host/remodroid-agent
make rtc-tests  # build/host/rtc/rtc_test, rtp_dump (phone stack on the host)
```

Static release binaries for routers and AWS (Zig as the cross compiler, musl, everything linked in):

```sh
make dist                   # inside the build container (build/Dockerfile, needs podman/docker)
./build/dist.sh mipsel      # or directly, with `zig` on PATH; targets: x86 x86_64 armv7 aarch64 mipsel mips
```

Results land in `build/out/<arch>/`: about 0.8 MB per binary (1.2 MB on MIPS).

Phone files (`remodroid-server.jar` + `libremodroid.so` per ABI):

```sh
ANDROID_JAR=.../platforms/android-34/android.jar D8=.../build-tools/34.0.0/d8 NDK=.../android-ndk-r27c \
  ./android/build.sh        # build/android/remodroid-server.jar, build/android/<abi>/libremodroid.so
```

## Test

| Command | Covers |
|---|---|
| `make test` | Control plane end to end: real server + agent, a protocol-faithful fake adbd (RSA auth checked with OpenSSL), fake phone; auth, REST, WebSockets, ADB push/shell/sync, tunnel, UDP relay, commands, `agent.ini` edits, CLI, reconnects (48 checks) |
| `QEMU=qemu-mipsel node tests/run.mjs build/mipsel` | The same, on the static MIPS router binaries under user-mode emulation |
| `python android/native/test/rtc_interop.py` | Phone WebRTC stack ↔ aiortc: ICE, DTLS-SRTP, SCTP, H.264 decoded frames, data channels |
| `python android/native/test/rtp_depack.py` | H.265 and H.264 packetizers: bit-exact round trip through an independent depacketizer + FFmpeg |
| `python tests/e2e_webrtc.py [--relay]` | Full Path A: viewer → server → agent → tunnel → phone stack; `--relay` forces the built-in TURN |
| `python tests/e2e_browser.py` | The real web client in headless Chrome against the full stack: video, touch, menu, protocol |
| `node tests/setup.mjs build/host` | First-run wizard (setup token, saving, restart on the new port), login, enrollment codes with the real agent's `enroll`, router settings through the panel, rename, remove |
| `node tests/worker.mjs build/host` | The Cloudflare Worker under `wrangler dev` (after `cd worker && npm install`) with the real agent: setup, login, enrollment, viewer WebSocket and signaling, commands, rename, remove |
| `node tests/tls.mjs build/host` | HTTPS, redirect, `Secure` cookies, viewer and agent over `wss://` (trusted and untrusted CA), certificate reload on SIGHUP; with `PEBBLE_DIR` set, ACME issuance, reuse and automatic renewal against Pebble (Let's Encrypt's test CA) |

The Python tests need `aiortc` (and `playwright` + Chromium for the browser test).

## Install

The install site, [remodroid.pages.dev](https://remodroid.pages.dev) (`wizard/` in this repository), walks
through the same steps with copy buttons.

1. **Server**, one of:
   - **Your own Linux server:** `curl -fsSL https://remodroid.pages.dev/install_server.sh | sudo sh`. It
     installs the package, starts the server and prints `http://<ip>:5580/wizard.html?token=…`. The setup
     wizard (`web/wizard.html`) asks for the web port, a **domain** (automatic HTTPS from Let's Encrypt),
     the admin login and the STUN/TURN port. It writes `/etc/remodroid/server.ini`, and the server restarts
     on your ports. Afterwards the same page edits the settings behind the login. `remodroid-server -t` checks
     the file without starting anything.
   - **No server: Cloudflare.** Make a TURN key (dashboard → Realtime → TURN Server). Then deploy `worker/`
     (`cd worker && npm install && npx wrangler deploy`, or import your fork with root directory `worker`).
     Open `https://remodroid.<you>.workers.dev` and its wizard asks for the login and the TURN key. The
     Worker is the same panel (login, routers, signaling) on a Durable Object with built-in storage.
2. **Agent** on the router: `wget -qO- https://remodroid.pages.dev/install_agent.sh | sh` (OpenWrt),
   or `curl … | sudo sh` (Debian, Ubuntu, CentOS).
3. **Connect:** on the panel, open **Agents → Add** (`web/panel.html`). Paste the command it shows
   on the router: `remodroid enroll --panel https://your.panel --code X5SX-K3M7`. The code is single-use and
   valid for 10 minutes. The agent then shows as online; tap it to edit its settings. The **Server** tab
   changes one server setting at a time (domain, ports, login, STUN/TURN list).
4. **Phone:** enable USB debugging, plug it into the router, and accept "Allow USB debugging" once.
5. **iPhone:** open the panel in Safari, log in, then Share → Add to Home Screen. Opened from the Home
   Screen it runs full screen (Safari on iPhone can't hide its bars for a normal page).

Firewall for a self-hosted server: TCP 5580 (setup), then your web port (80) and 443 with a domain,
UDP 3478 and UDP 49152–49252 for TURN.

On the router, `remodroid-agent devices | status | shell … | push | pull | logcat | key | restart-server`
talk to the running agent (no adb needed).

## Install from packages

Each GitHub release (and every CI run, as artifacts) has static, dependency-free packages:

| Distro | Package | Install |
|---|---|---|
| Debian / Ubuntu | `remodroid-{server,agent}_<ver>-1_{amd64,i386,armhf,arm64}.deb` | `apt install ./remodroid-agent_*.deb` |
| CentOS / RHEL / Fedora | `remodroid-{server,agent}-<ver>-1.{x86_64,i686,armv7hl,aarch64}.rpm` | `dnf install ./remodroid-agent-*.rpm` |
| Alpine | `remodroid-{server,agent}_<ver>-r1_{x86_64,x86,armv7,aarch64}.apk` | `apk add --allow-untrusted ./remodroid-agent_*.apk` |
| OpenWrt | `remodroid-{server,agent}_<ver>-1_<opkg arch>.ipk` (x86_64, i386_pentium4, arm_cortex-a7/a9/a15, aarch64_generic/cortex-a53/cortex-a72, mipsel_24kc, mips_24kc) | `opkg install ./remodroid-agent_*.ipk` |
| Anything else | `remodroid-<ver>-linux-<arch>.tar.gz` | unpack and copy |

Packages install `/usr/bin/remodroid-*`, `/etc/remodroid/*.ini` (kept on upgrade), the web client
and phone files under `/usr/share/remodroid/`, and a service for the distro's init system
(systemd, OpenRC or procd). Edit the ini file, then enable the service. Pick the OpenWrt
package whose arch matches `opkg print-architecture` on your router.

## Status

Verified on this machine: everything in the test table above, including real Chrome as the viewer.
Not yet verified (needs your hardware, plan milestone M0): the Java phone server on a real
Android phone (capture, encoder settings, input injection, clipboard, screen power), USB ADB on the
router, Safari on your iPhone (H.265 support, hardware decoding), and real-network congestion control.
Sound (M4) works like scrcpy: the phone's output is captured (Android 12+), encoded to Opus and sent as a
WebRTC audio track, and the phone's own speaker goes quiet while you're connected. The first tap on the
picture unmutes it in Safari; the menu's **Sound** switch hands it back to the phone.
