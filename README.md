# RemoDroid

Low-latency remote control of an Android phone from an iPhone (Safari), for cloud gaming.
The phone sits on a USB cable to an OpenWrt router; the iPhone connects through a small
server on AWS. Design and decisions: [`plan.md`](plan.md).

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
| `server/` | Control server: Basic auth + cookies, REST API, viewer/agent WebSockets, built-in TURN relay | C + libev |
| `agent/` | Router agent: built-in ADB client (usbfs), phone supervisor, UDP relay, WebSocket client, CLI | C + libev |
| `android/native/` | Phone WebRTC stack: ICE + TURN client, DTLS-SRTP, SCTP data channels, H.265/H.264 RTP, congestion control | C |
| `android/java/` | Phone server: screen capture, MediaCodec, input injection, clipboard, screen power, files, logcat | Java (app_process) |
| `web/` | iPhone client (PWA): video, multi-touch, floating-ball menu, photo upload, clipboard, all settings | HTML/CSS/JS |
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

The Python tests need `aiortc` (and `playwright` + Chromium for the browser test).

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

## Deploy

1. **AWS**: install the `remodroid-server` package (or copy the binary, `web/` and `deploy/server.ini.example` →
   `/etc/remodroid/server.ini`); hash passwords with `remodroid-server hashpw`, make a device secret
   with `remodroid-server gensecret`. Open TCP 80 and UDP 3478 + the relay port range. Install
   `deploy/remodroid-server.service`.
2. **Router**: copy `remodroid-agent` for the router's CPU to `/usr/bin/`, the phone files to
   `/usr/share/remodroid/` (`remodroid-server.jar`, `<abi>/libremodroid.so`), `deploy/agent.ini.example`
   → `/etc/remodroid/agent.ini`, and `deploy/remodroid-agent.init` → `/etc/init.d/remodroid-agent`;
   `enable` + `start` (or just `opkg install` the remodroid-agent ipk).
3. **Phone**: enable USB debugging, plug it into the router, accept "Allow USB debugging" once.
4. **iPhone**: open `http://<aws-host>/`, log in, then Share → Add to Home Screen.

On the router, `remodroid-agent devices | status | shell … | push | pull | logcat | key | restart-server`
talk to the running agent (no adb needed).

## Status

Verified on this machine: everything in the test table above, including real Chrome as the viewer.
Not yet verified (needs your hardware, plan milestone M0): the Java phone server on a real
Android phone (capture, encoder settings, input injection, clipboard, screen power), USB ADB on the
router, Safari on your iPhone (H.265 support, hardware decoding), and real-network congestion control.
Audio (M4) is not implemented yet.
