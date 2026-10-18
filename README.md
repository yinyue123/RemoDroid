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

## Download

The easiest way is the [install site](https://remodroid.pages.dev/), which picks the right file for you.
To download a package yourself, use the grids below. They are made by `doc/downloads.py`, and every
[release](https://github.com/yinyue123/RemoDroid/releases) has the same grids for its own files.

<!-- downloads -->
### Downloads (v0.4)

Pick the row for the machine's CPU (`uname -m`: `x86_64`, `aarch64` = ARM64, `armv7l` = ARMv7, `i686` = x86) and the column for its system. OpenWrt: take the build that `opkg print-architecture` lists. *Any Linux* holds both the server and the agent.

| Server | Debian · Ubuntu | CentOS · Fedora | Alpine | OpenWrt | Any Linux |
|:--|:-:|:-:|:-:|:-:|:-:|
| **x86_64**<br><sub>Intel / AMD 64-bit</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="server .deb x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_amd64.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="server .rpm x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server-0.4-1.x86_64.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="server .apk x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-r1_x86_64.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="server .ipk x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_x86_64.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="server + agent .tar.gz x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-x86_64.tar.gz) |
| **ARM64**<br><sub>aarch64</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="server .deb aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_arm64.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="server .rpm aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server-0.4-1.aarch64.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="server .apk aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-r1_aarch64.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-generic.svg" alt="server .ipk aarch64_generic" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_aarch64_generic.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a53.svg" alt="server .ipk aarch64_cortex-a53" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_aarch64_cortex-a53.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a72.svg" alt="server .ipk aarch64_cortex-a72" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_aarch64_cortex-a72.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="server + agent .tar.gz aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-aarch64.tar.gz) |
| **ARMv7**<br><sub>32-bit ARM</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="server .deb armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_armhf.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="server .rpm armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server-0.4-1.armv7hl.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="server .apk armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-r1_armv7.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a7.svg" alt="server .ipk arm_cortex-a7_neon-vfpv4" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_arm_cortex-a7_neon-vfpv4.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a9.svg" alt="server .ipk arm_cortex-a9_vfpv3-d16" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_arm_cortex-a9_vfpv3-d16.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a15.svg" alt="server .ipk arm_cortex-a15_neon-vfpv4" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_arm_cortex-a15_neon-vfpv4.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="server + agent .tar.gz armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-armv7.tar.gz) |
| **x86**<br><sub>32-bit</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="server .deb x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_i386.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="server .rpm x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server-0.4-1.i686.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="server .apk x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-r1_x86.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="server .ipk i386_pentium4" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_i386_pentium4.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="server + agent .tar.gz x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-x86.tar.gz) |
| **MIPS LE**<br><sub>mipsel</sub> | — | — | — | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="server .ipk mipsel_24kc" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_mipsel_24kc.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="server + agent .tar.gz mipsel" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-mipsel.tar.gz) |
| **MIPS BE**<br><sub>mips</sub> | — | — | — | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="server .ipk mips_24kc" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-server_0.4-1_mips_24kc.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="server + agent .tar.gz mips" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-mips.tar.gz) |

| Agent | Debian · Ubuntu | CentOS · Fedora | Alpine | OpenWrt | Any Linux |
|:--|:-:|:-:|:-:|:-:|:-:|
| **x86_64**<br><sub>Intel / AMD 64-bit</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="agent .deb x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_amd64.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="agent .rpm x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent-0.4-1.x86_64.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="agent .apk x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-r1_x86_64.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="agent .ipk x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_x86_64.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="server + agent .tar.gz x86_64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-x86_64.tar.gz) |
| **ARM64**<br><sub>aarch64</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="agent .deb aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_arm64.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="agent .rpm aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent-0.4-1.aarch64.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="agent .apk aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-r1_aarch64.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-generic.svg" alt="agent .ipk aarch64_generic" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_aarch64_generic.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a53.svg" alt="agent .ipk aarch64_cortex-a53" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_aarch64_cortex-a53.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a72.svg" alt="agent .ipk aarch64_cortex-a72" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_aarch64_cortex-a72.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="server + agent .tar.gz aarch64" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-aarch64.tar.gz) |
| **ARMv7**<br><sub>32-bit ARM</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="agent .deb armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_armhf.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="agent .rpm armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent-0.4-1.armv7hl.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="agent .apk armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-r1_armv7.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a7.svg" alt="agent .ipk arm_cortex-a7_neon-vfpv4" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_arm_cortex-a7_neon-vfpv4.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a9.svg" alt="agent .ipk arm_cortex-a9_vfpv3-d16" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_arm_cortex-a9_vfpv3-d16.ipk)<br>[<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk-a15.svg" alt="agent .ipk arm_cortex-a15_neon-vfpv4" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_arm_cortex-a15_neon-vfpv4.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="server + agent .tar.gz armv7" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-armv7.tar.gz) |
| **x86**<br><sub>32-bit</sub> | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/deb.svg" alt="agent .deb x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_i386.deb) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/rpm.svg" alt="agent .rpm x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent-0.4-1.i686.rpm) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/apk.svg" alt="agent .apk x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-r1_x86.apk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="agent .ipk i386_pentium4" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_i386_pentium4.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="server + agent .tar.gz x86" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-x86.tar.gz) |
| **MIPS LE**<br><sub>mipsel</sub> | — | — | — | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="agent .ipk mipsel_24kc" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_mipsel_24kc.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="server + agent .tar.gz mipsel" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-mipsel.tar.gz) |
| **MIPS BE**<br><sub>mips</sub> | — | — | — | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/ipk.svg" alt="agent .ipk mips_24kc" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-agent_0.4-1_mips_24kc.ipk) | [<img src="https://raw.githubusercontent.com/yinyue123/RemoDroid/main/doc/badges/tar.svg" alt="server + agent .tar.gz mips" height="24">](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-linux-mips.tar.gz) |

Koolshare / KoolCenter offline installers: [Merlin armv7](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-merlin-armv7.tar.gz) · [Merlin aarch64](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/remodroid-0.4-merlin-aarch64.tar.gz). RT-AC86U: choose armv7. These are agent-only plugins, not OpenWrt packages.

Checksums: [SHA256SUMS](https://github.com/yinyue123/RemoDroid/releases/download/v0.4/SHA256SUMS)
<!-- /downloads -->

## What's here

| Path | What | Language |
|---|---|---|
| `server/` | Control server: login page + cookies (Basic auth for routers), REST API, viewer/agent WebSockets, built-in TURN relay, optional HTTPS with automatic Let's Encrypt (ACME) certificates | C + libev + mbedTLS |
| `agent/` | Router agent: built-in ADB client (usbfs), phone supervisor, UDP relay, WebSocket client, CLI | C + libev |
| `android/native/` | Phone WebRTC stack: ICE + TURN client, DTLS-SRTP, SCTP data channels, H.265/H.264 RTP, congestion control | C |
| `android/java/` | Phone server: screen capture, MediaCodec, input injection, clipboard, screen power, files, logcat | Java (app_process) |
| `web/` | iPhone client (PWA): video, sound, multi-touch, floating-ball menu (lock, reconnect, full screen), photo upload, clipboard, all settings; `panel.html` (bottom tabs: Devices, Agents with enroll and settings, Server settings one item at a time) and `wizard.html` (first-run setup) | HTML/CSS/JS |
| `worker/` | The same panel on Cloudflare Workers + a Durable Object, with Cloudflare TURN, for people without a server | JS |
| `wizard/` | The install site (Cloudflare Pages, remodroid.pages.dev) , `install_server.sh` / `install_agent.sh` and `update.sh` | HTML, sh |
| `common/` | Shared C: WebSocket framing, tunnel frames, STUN, JSON envelope, ini, utilities | C |
| `third_party/` | Vendored libraries, compiled in statically: libev, inih, cJSON, picohttpparser, crypt_blowfish, mbedTLS, libsrtp2, usrsctp | C |
| `deploy/` | Example `server.ini` / `agent.ini`, systemd units, OpenRC and procd scripts | |
| `packaging/`, `.github/workflows/` | nFPM package definitions (deb, rpm, apk, ipk) and the CI build/release workflow | |
| `python/`, `skills/`, `.claude/skills/phone/` | AI control: the `remodroid` SDK and `rd` CLI for the phone's UI JSON / screenshot / action API, learned workflows that replay without the model, and the Claude Code skill that uses them ([skill-plan.md](skill-plan.md)) | Python |
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
| `python3 tests/python_sdk.py` | The AI control SDK, `rd` CLI and skill runner against a fake phone API: selectors, in-memory screenshots, hand-over to the AI (exit 3), resume with `--from-step` |
| (in `make test`) | Notification forwarding: the `[notify]` config reaches the phone, a captured notification reaches a viewer over WebSocket, and the server pushes it to a stub Bark endpoint as JSON |
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

To update later, run on the server or router: `curl -fsSL https://remodroid.pages.dev/update.sh | sudo sh`
(or `wget -qO- … | sh` on OpenWrt). It updates whichever of the server and agent are installed to
the latest release, keeps `server.ini` / `agent.ini`, and restarts them.

On the router, `remodroid-agent devices | status | shell … | push | pull | logcat | key | restart-server`
talk to the running agent (no adb needed).

## Install from packages

Each GitHub release (and every CI run, as artifacts) has static, dependency-free packages; see
[Download](#download) for which file fits your machine. Install one with `apt install ./remodroid-*.deb`,
`dnf install ./remodroid-*.rpm`, `apk add --allow-untrusted ./remodroid-*.apk`, `opkg install ./remodroid-*.ipk`,
or unpack the `.tar.gz` and copy its files.

Packages install `/usr/bin/remodroid-*`, `/etc/remodroid/*.ini` (kept on upgrade), the web client
and phone files under `/usr/share/remodroid/`, and a service for the distro's init system
(systemd, OpenRC or procd). Edit the ini file, then enable the service. Pick the OpenWrt
package whose arch matches `opkg print-architecture` on your router.

### ASUS Merlin / Koolshare

Use the dedicated `remodroid-<version>-merlin-aarch64.tar.gz` (64-bit HND models:
RT-AC86U, RT-AX88U, …) or `-merlin-armv7.tar.gz` (32-bit ARM models: RT-AC68U, …)
package in **Software Center → Manual installation**. Match the package to your
router's architecture (`uname -m`: `aarch64` → aarch64, `armv7l` → armv7); the
installer refuses a mismatched package.
It provides a router management page for configuration, start/stop, logs, and optional
autostart. Installation does not start the agent. Stock Asuswrt-Merlin without the
Koolshare/KoolCenter software center is not supported by this installer.
See [Merlin packaging](packaging/merlin/README.md) for building, migration, and limitations.

## AI control

The AI can operate the phone through REST: `GET …/ui` returns the screen as JSON (from an in-process
UiAutomation on the phone; `uiautomator dump` as a fallback), `GET …/screenshot?format=jpeg&max_edge=1280`
returns the image bytes directly (GPU-scaled, compressed in C on the phone, never written to disk), and
`POST …/actions` taps, types, scrolls and waits by selector. It's served by the router agent (`[api] listen` +
`token` in `agent.ini`) and relayed by the server (`remodroid-server gentoken NAME`), both under
`/api/v1/devices/{id}/`. A workflow the AI works out once is saved under `skills/` and replayed by Python
without model calls; when it meets something new it hands the screen back to the AI. Design:
[skill-plan.md](skill-plan.md); how to write skills: [skills/README.md](skills/README.md).

```sh
export PYTHONPATH=python RD_URL=http://192.168.1.1:8790 RD_TOKEN=…
python3 -m remodroid ui                      # the screen as an outline
python3 -m remodroid tap --rid search --wait-change
python3 skills/settings-search/run.py --query wifi
```

## Phone notifications

The phone can forward its own notifications. The helper reads them on the phone (a poll of
`dumpsys notification`, the only notification content the shell user can see), filters out ongoing
and low-importance ones, and sends each new one to any connected client over the viewer WebSocket and
to [Bark](https://github.com/Finb/Bark) when the server has a `bark_url`. From there your usual flow
takes over: you get the push, then call the AI control API (or a skill) to act on it.

Turn it on with `[notify] enabled = true` in the router's `agent.ini` (plus `min_importance` and an
`ignore` list), and set `[notify] bark_url = https://api.day.app/<key>` in the server's `server.ini`
for Bark (or per router under `[device_notify]`). The Bark key stays on the server and is never
logged. WebSocket delivery needs no server config. See the sample configs in `deploy/`.

## Status

Verified on this machine: everything in the test table above, including real Chrome as the viewer.
Not yet verified (needs your hardware, plan milestone M0): the Java phone server on a real
Android phone (capture, encoder settings, input injection, clipboard, screen power), USB ADB on the
router, Safari on your iPhone (H.265 support, hardware decoding), and real-network congestion control.
Sound (M4) works like scrcpy: the phone's output is captured (Android 12+), encoded to Opus and sent as a
WebRTC audio track, and the phone's own speaker goes quiet while you're connected. The first tap on the
picture unmutes it in Safari; the menu's **Sound** switch hands it back to the phone.
