# Isolated Phone-Layout Browser — Plan

Run **CloakBrowser** (Linux persona, phone layout) on a portrait Xorg screen inside a throwaway **hqnode
container** on an **OECT (RK3566) running Armbian**, and use it from an **iPhone 14 Plus** through
**RemoDroid**. The container runs the RemoDroid **Linux desktop agent** (`remodroid-desktopd`), which:
- streams **hardware H.265** over WebRTC, using the RK3566's own encoder through **Rockchip MPP**;
- takes **real multi-touch** and typing back in.

The site's code never runs on the iPhone. It sees only the container's spoofed fingerprint and the
proxy's IP.

---

## Decisions

| Question | Answer | Why, in one line |
|---|---|---|
| Hardware | **OECT**: RK3566 (4× Cortex-A55), 2 GB RAM, 8 GB eMMC, plus a **64 GB SSD** | Yours; it has a hardware H.265 encoder |
| Storage | **`/var/hqnode` on the 64 GB SSD** (ext4) | The eMMC keeps only Armbian; images, container disks and swap go on the SSD |
| Host OS | **Armbian, vendor kernel 6.1** (rk35xx) | MPP's driver is only in Rockchip's kernel; hqnode needs ≥ 5.19, so not the 5.10 BSP |
| Android over ADB, or desktop Linux? | **Desktop Linux** | An hqnode container can run Xorg and CloakBrowser; it can't run Android (see below) |
| Remote control | **RemoDroid desktop agent** + RemoDroid server/panel + the RemoDroid web viewer | Already built: WebRTC, TURN, login, iPhone full-screen viewer, keyboard sheet |
| Container | **hqnode** (hqnoded arm64), from `ghcr.io/yinyue123/hqnode:debian-13` | glibc, which CloakBrowser (a Chromium build) needs; systemd runs the services |
| Screen | **Xorg + dummy driver, 880×1904 at 2×**: the iPhone 14 Plus's exact shape | Tested; sized for what four A55 cores can render (see Screen size) |
| Touch | **Xorg `inputtest` driver** (chosen); `uinput` kept as the fallback | Tested: real XI2 direct-touch events, with no kernel device and no hqnode change |
| Keyboard | **XTest** (what the desktop agent already does) | Tested on the same Xorg |
| Video | **Hardware H.265 through Rockchip MPP**: a new `desktop/linux/video_mpp.c` | The RK3566 has no VA-API, so the agent's existing hardware path doesn't apply |
| Devices | **hqnode gives the container `/dev/mpp_service` and `/dev/dma_heap/*`**: a new `rkmpp` device option | Tested on a DRM node: hqnode must create the nodes itself (a bind of the host's is refused) |
| Kill switch | **nftables on the OECT host**, matching this container's pasta cgroup | You own the host, so it can do what the container can't |
| Controller bar | **`rd-shell`**: a small C/Xlib program that is the bar and the window manager in one | It keeps the browser below the bar and starts, stops and resets CloakBrowser |
| hqnode changes for v1 | **One: the `rkmpp` device option** | uinput stays a later step |

---

## Android over ADB, or desktop Linux?

**Choose desktop Linux.**

| | Android over ADB | Desktop Linux in hqnode |
|---|---|---|
| Where it runs | A real phone on USB, or an emulator/redroid | The OECT, as one hqnode container |
| In an hqnode container? | **No.** redroid/Waydroid need `binderfs` and `ashmem` in the host kernel and a privileged container; an emulator needs `/dev/kvm` | **Yes.** Xorg, the dummy driver, `inputtest` and Chromium all run unprivileged (tested) |
| CloakBrowser | Doesn't exist for Android. You'd get stock Chrome with the phone's own fingerprint and Google account | Runs as built (arm64), with `--fingerprint-platform` and seeds |
| Throwaway | Wiping a phone between sessions is slow and manual | `hqnode reinstall` or delete-and-create; the profile is on tmpfs anyway |
| New identity | Factory reset, or a second phone | New seed and an empty profile: one tap on the bar |
| RemoDroid code | Done (the phone path) | Mostly done (the Linux desktop agent); needs touch, MPP and the bar |
| Fingerprint realism | **Better**: a real Android device, real mobile UA, real touch | Weaker: Linux + portrait phone-sized screen + touch is a rare combination |

Pick Android only if the sites you care about reject the Linux-phone combination and you're willing to
dedicate a physical phone per identity. That would be RemoDroid as it already runs for phones, with no
container and no CloakBrowser. Everything below is the desktop Linux plan.

---

## Architecture

```
iPhone 14 Plus Safari (RemoDroid web app, Home Screen, full screen)
  ▲ WebRTC H.265 video            │ WebRTC data channel: touches (0x01), keys (0x04), ctrl text
  │                               ▼
remodroid-server on AWS (panel, signaling, TURN) ── WS /ws/agent ──┐
                                                                   │
OECT ── Armbian (vendor 6.1) ── hqnoded ── nftables kill switch ── home network (own VLAN)
  └─ hqnode container (debian-13 arm64, systemd, devices: rkmpp)
       ├─ /dev/mpp_service, /dev/dma_heap/*   made by hqnode for this container; group video inside
       ├─ rd-xorg.service        Xorg :0, dummy 880×1904 + inputtest touch socket /run/rd-touch.sock
       ├─ rd-shell.service       controller bar + mini window manager + CloakBrowser supervisor
       │    └─ CloakBrowser      kiosk, below the bar; SOCKS5 proxy for all traffic incl. WebRTC
       └─ remodroid-desktopd     X11 capture (BGRA) → MPP H.265 → WebRTC; touches → inputtest, keys → XTest
```

Two kinds of traffic leave the container:
- **The browser's**: only to the SOCKS5 proxy. This is enforced by Chromium's own settings, and by the
  host's nftables.
- **RemoDroid's**: the agent's WebSocket to the panel and its WebRTC UDP, direct or through TURN. It never
  carries page traffic.

---

## Components

| Part | Choice | Notes |
|---|---|---|
| Machine | OECT: RK3566, 4× A55, 2 GB RAM, 8 GB eMMC | One container on it; nothing else heavy |
| Host | Armbian (Debian base), vendor kernel 6.1 for rk35xx, cgroup v2, `passt`, zram | Must show `/dev/mpp_service` and `/dev/dma_heap/` |
| Storage | 64 GB SSD, one ext4 partition, mounted at `/var/hqnode` | ext4 because hqnode uses idmapped mounts; the eMMC stays for the OS only |
| Container | hqnode, image `…/hub:remote-browser-hqnode` (arm64) on `hqnode:debian-13`, `devices: ["rkmpp"]` | 4 cores, 1.5 GB RAM, 8 GB root disk, 1 GB `/data` |
| Browser | CloakBrowser Linux arm64 binary | `--fingerprint-platform=linux`, seed per identity |
| Screen | Xorg 21.1 + `xserver-xorg-video-dummy` | 880×1904 (multiples of 16, iPhone 14 Plus shape), Chrome scale 2 → 440×952 CSS px |
| Touch | `inputtest` input driver (in `xserver-xorg-core`), `DeviceType "Touch"`, 5 points | The agent writes touch events to its socket |
| Keyboard | XTest, existing `desktop/linux/input_x11.c` | Typing from the iPhone keyboard sheet arrives as ctrl `text` |
| Video | Existing X11 capture (MIT-SHM + XDamage) → BGRA into an MPP buffer → RK3566 encoder, H.265 | P frames only, CBR, 30 fps; the encoder converts the colours itself, so the CPU does no YUV work |
| Controller bar | `rd-shell` (new, C + Xlib + Xft) | 44 CSS px tall (88 px at 2×; 44 pt on the iPhone) at the top of the screen |
| Remote control | RemoDroid server or Cloudflare worker, already deployed | The container pairs like any computer: `remodroid-desktopd pair` |
| Network | SOCKS5 proxy in CloakBrowser + host nftables | WebRTC only via the proxy; DNS only via the proxy |

---

## The OECT host

1. **Armbian with the vendor kernel** (rk35xx, 6.1). Two other kernels fail:
   - **mainline** has no `mpp_service`;
   - **Rockchip's 5.10 BSP** is below hqnode's 5.19 floor (idmapped mounts on overlay lower layers).

   Check:
   ```
   uname -r                                    # 6.1.x
   ls -l /dev/mpp_service /dev/dma_heap/       # the encoder and its buffer heaps
   stat -fc %T /sys/fs/cgroup                  # cgroup2fs
   ```
2. **Encoder check before anything else.** Build Rockchip's MPP (`github.com/rockchip-linux/mpp`) on the
   host and encode a test frame at the planned size, from BGRA input:
   ```
   mpi_enc_test -t 16777220 -w 880 -h 1904 -f 0x1000b -n 60 -o /tmp/t.h265   # HEVC, BGRA8888
   ```
   - The `-t` and `-f` values are MPP's `MppCodingType` and `MppFrameFormat` numbers; check them against
     `mpp_frame.h` of the commit you build.
   - If BGRA input is refused, the agent has to convert to NV12 first (see Video).
   - If 1904 lines is refused, use `small` (784×1696) and see Risks.
3. **Storage: the 64 GB SSD.** Give it one ext4 partition, mounted at `/var/hqnode` before hqnode
   starts. hqnode keeps everything there: images, each container's `disk.img`, uploads and its swap file.
   ```
   mkfs.ext4 -L hqnode /dev/sda1
   echo 'LABEL=hqnode /var/hqnode ext4 defaults,noatime 0 2' >> /etc/fstab
   mkdir -p /etc/systemd/system/hqnode.service.d
   printf '[Unit]\nRequiresMountsFor=/var/hqnode\n' > /etc/systemd/system/hqnode.service.d/ssd.conf
   ```
   - `RequiresMountsFor` keeps hqnode from starting on the eMMC if the SSD is missing. Otherwise it would
     quietly create a fresh `/var/hqnode` there, and fill the 8 GB.
   - Put the SSD on a powered port, or one the OECT can power; an underpowered SSD drops off under write
     load.
   - Budget:

     | Use | Size |
     |---|---|
     | Image | about 1.5 GB (debian-13 + Xorg + fonts + CloakBrowser + MPP) |
     | Container root disk | 8 GB |
     | `/data` | 1 GB |
     | Swap file | 2 GB |
     | Room for a second image during an upgrade | the rest, roughly 50 GB free |
4. **Memory:**
   - zram swap of 1 GB, set through hqnode's machine settings;
   - behind it, a 2 GB swap file on the SSD (`/var/hqnode/swapfile`, also through hqnode's settings),
     at lower priority than zram. A heavy page then slows down instead of being killed;
   - nothing else resident besides hqnoded and pasta.
5. **hqnode:** install `hqnoded` (arm64 .deb) and enroll the OECT in the panel as a machine.
6. **Network:**
   - Put the OECT on its own VLAN, firewalled from home devices.
   - It needs only outbound access: to the RemoDroid server/TURN and to the proxy.
   - SSH to it through the hqnode gateway or Tailscale.

---

## The hqnode container

Laid out the way `sh0u/quadlet/hqnode` is, in `browser/hqnode/` in this repository:

```
browser/hqnode/
  Dockerfile               hqnode:debian-13 + Xorg + fonts + CloakBrowser + MPP + remodroid-desktop + rd-shell
  rd-xorg-light.conf  rd-xorg-small.conf      one per screen size (below)
  units/rd-xorg.service    Xorg :0
  units/rd-video.service   oneshot: chgrp video on the MPP nodes
  units/rd-shell.service   the bar, After=rd-xorg
  units/remodroid-desktopd.service   the agent, After=rd-xorg rd-video, DISPLAY=:0
  browser.conf.example     copied to /data/browser.conf
  build.sh                 build (and --push) docker.io/yinyue123/hub:remote-browser-hqnode, linux/arm64
  create.sh  deploy.sh  reinstall.sh  panel.sh      the same four as sh0u's, panel.sh copied verbatim
```

**Base image: `hqnode:debian-13`, not Alpine.** CloakBrowser is a glibc Chromium build and won't run on musl.
This also means PID 1 is systemd, so services are unit files. `Environment=` works in them. OpenRC's
environment scrubbing, which shaped the sh0u image, doesn't apply.

**Packages** (Debian 13, arm64):
- `xserver-xorg-core` (ships `inputtest_drv.so`), `xserver-xorg-video-dummy`, `x11-xserver-utils`;
- `xdotool`, for debugging;
- `fonts-noto-core`, `fonts-noto-cjk`, `fonts-noto-color-emoji`;
- CloakBrowser's runtime libraries (`libnss3`, `libgbm1`, `libasound2`, …);
- the `remodroid-desktop` aarch64 build from the release.

**MPP:** Debian has no package, so a builder stage compiles `librockchip_mpp.so.1` from the
`rockchip-linux/mpp` commit pinned in `build.sh`. It's copied into `/usr/lib/aarch64-linux-gnu/`, along with
`mpi_enc_test` for checks.

**Users.** Xorg runs as root inside the container, which hqnode maps to an unprivileged host UID.
- `rd-shell` and CloakBrowser run as user `browser`.
- `remodroid-desktopd` runs as user `rdagent`. Only `rdagent` is in group `video`, so only the agent can
  open the encoder; the browser can't.
- Both users read the X cookie in `/run/rd-xorg.auth`.

**The device nodes at boot.** hqnode creates `/dev/mpp_service` and `/dev/dma_heap/*` owned by the
container's root, mode 0660. `rd-video.service` (oneshot, `Before=remodroid-desktopd`) runs
`chgrp video /dev/mpp_service /dev/dma_heap/*`. That step was tested on a DRM node: container root may
change the group, and a non-root member of it can then open the node.

**What lives where:**

| Path | What | Survives reinstall |
|---|---|---|
| `/data/browser.conf` | proxy, seed, timezone, locale, home URL, screen size, bar options | yes |
| `/data/remodroid/desktop.ini` | the agent's pairing (`[server]`) and settings | yes, so there's no re-pairing |
| `/tmp/profile` | CloakBrowser profile (tmpfs, charged to the container's memory) | no, by design |
| everything else | image content | replaced by the image |

**Sizing** for a 2 GB box: 4 cores, **1.5 GB**. Chromium dominates. Measure in build steps 5 and 8.
- hqnode mounts `/dev/shm` at 64 MB, which Chromium outgrows, so it runs with `--disable-dev-shm-usage`.
- The profile is on tmpfs, so a heavy session's cache counts against the 1.5 GB. That's why the disk cache
  is capped at 50 MB.

**Creating it**, as with sh0u: pull on the machine, then create through the panel (never `hqnode run`):

```
ssh -p 36000 root@<oect> 'hqnode pull docker.io/yinyue123/hub:remote-browser-hqnode'
MACHINE=oect DISK=8G DATA=1G MEM=1536M CPU_CORES=4 ./browser/hqnode/create.sh   # ports: 22; "devices": ["rkmpp"]
MACHINE=oect ./browser/hqnode/deploy.sh   # pushes browser.conf, pairs the agent (one panel code)
```

Inbound UDP isn't required: WebRTC goes through the RemoDroid TURN server when a direct path can't be
made. Behind a home router it usually will be; a mapped UDP port (hqnode `publish`, plus a router port
forward) is an optional latency improvement.

---

## Xorg plan

### Configuration (`browser/hqnode/rd-xorg-light.conf`)

Tested as written, with Xorg 21.1.19:

```
Section "ServerFlags"
  Option "AutoAddDevices" "false"      # no udev in the container; only the devices below
  Option "AutoEnableDevices" "false"
  Option "DontVTSwitch" "true"
EndSection
Section "Device"
  Identifier "dummy"
  Driver "dummy"
  VideoRam 32768
EndSection
Section "Monitor"
  Identifier "mon"
  HorizSync 5.0-1000.0
  VertRefresh 5.0-200.0
  Modeline "880x1904"  141.75  880 944 1040 1200  1904 1907 1917 1973 -hsync +vsync   # cvt 880 1904 60
EndSection
Section "Screen"
  Identifier "screen"
  Device "dummy"
  Monitor "mon"
  DefaultDepth 24
  SubSection "Display"
    Depth 24
    Modes "880x1904"
    Virtual 880 1904
  EndSubSection
EndSection
Section "InputDevice"
  Identifier "touch"
  Driver "inputtest"
  Option "SocketPath" "/run/rd-touch.sock"
  Option "DeviceType" "Touch"
  Option "TouchCount" "5"              # navigator.maxTouchPoints; 5 is what iPhones report
EndSection
Section "ServerLayout"
  Identifier "layout"
  Screen "screen"
  InputDevice "touch"
EndSection
```

`rd-xorg-small.conf` is the same with:

```
  Modeline "784x1696"  113.00  784 848 928 1072  1696 1699 1709 1757 -hsync +vsync   # cvt 784 1696 60
  Modes "784x1696" / Virtual 784 1696
```

### Start

```
Xorg :0 -config /etc/X11/rd-xorg.conf -configdir /etc/X11/rd-xorg.conf.d \
     -noreset -nolisten tcp -novtswitch -keeptty -logfile /run/rd-xorg.log
```

- `rd-xorg.conf.d` is an empty directory in the image.
- `rd-xorg.service` runs Xorg with `Type=simple` and `Restart=always`. Its `ExecStartPre` links
  `rd-xorg-<screen>.conf` to `rd-xorg.conf`, using `screen` from `/data/browser.conf`.
- `rd-shell` and `remodroid-desktopd` are `After=rd-xorg.service` and retry until `:0` answers.

### Screen size: the iPhone 14 Plus, proportionally

The iPhone 14 Plus has a 1284×2778 px panel, a 428×926 CSS viewport and a `devicePixelRatio` of 3. The X
screen keeps that shape (214:463), so the viewer fills the phone's screen with no black bars.

Both sides must also be multiples of 16, because the encoder works in 16×16 blocks. The agent's encoders
refuse other sizes or round them down. So the sizes are the nearest multiples of 16:

| `screen =` | X screen | Chrome scale | CSS viewport (below the bar) | Shape vs iPhone | Pixels | On the OECT |
|---|---|---|---|---|---|---|
| **`light`** (default) | **880×1904** | **2** | 440×952 (440×908) | 0.00% | 1.68 MP | **yes** |
| `small` | 784×1696 | 2 | 392×848 (392×804) | +0.01% | 1.33 MP | if `light` is too heavy |
| `native` | 1280×2768 | 3 | 427×923 (427×879) | +0.05% | 3.54 MP | **no**: 2.1× the pixels to rasterize on A55 cores, and past the encoder's rated rate |

**Why `light` on the OECT:**
- Chromium has no GPU here (see Video), so every frame of the page is rasterized by four A55 cores. That,
  not the encoder, is what limits scrolling.
- The RK3566's encoder is rated for 1080p at 60 fps (124 MP/s). `light` at 30 fps is 50 MP/s.
- The page lays out at 440 CSS px wide, 12 px wider than the phone. Mobile layouts treat both the same.

`small` is the step down if scrolling stutters. Switching means changing `screen` in `browser.conf` and
running `systemctl restart rd-xorg rd-shell`; `rd-shell` reads the same key for Chrome's scale factor and
the bar height. `native` (which needs a 30 Hz modeline: the dummy driver caps the pixel clock at 300 MHz)
is for a stronger machine.

### What was tested, and what it showed

Tested in an Alpine 3.22 rootfs whose `/dev` held only hqnode's nodes (`null zero random urandom tty`),
first as root and then inside a user namespace (`unshare -r`), as hqnode runs containers:

- Xorg starts with **no `/dev/tty0`, no `/dev/dri`, no `/dev/input`**. `xdpyinfo` reports 880×1904,
  784×1696 and 1280×2768 with their configs. Touch works at each size.
- The modeline must be a real CVT line: a hand-made one with short blanking is rejected
  ("monitor doesn't support reduced blanking"), and Xorg silently falls back to 640×480.
- `Virtual` is required too, or the dummy driver picks its own virtual size.
- A config directory of its own keeps packaged `xorg.conf.d` snippets (libinput, …) from adding devices.
- The touch device appears as `Touch mode: direct`, `Max number of touches: 5`. XTest's keyboard and
  pointer are present, and `xdotool type` works.
- Harmless log noise: no VGA arbiter, no GLX software renderer, no ACPI. Chromium renders with its own
  SwiftShader path, so it doesn't need GLX.

That was on x86_64. Debian 13's `xserver-xorg-core` (21.1.16) also ships `inputtest_drv.so`, and the
arm64 package is built from the same source. The first boot in the OECT container (build step 5)
confirms it there.

---

## Touch input plan

### What the viewer sends, and what changes

The phone viewer already sends Android-style touch: message `0x01` with action, pointer id, tool, x, y
(0–65535), pressure. On a desktop the agent currently maps only pointer 0 to the left mouse button
(`desktop/core/input.c`), and the viewer turns touches into mouse gestures for desktops.

The change:
1. **Agent** advertises `"touch": 5` in `session.ready` when a touch backend is configured.
2. **Viewer** (`web/app.js`), when `touch` is present, uses the phone's raw multi-touch path for this
   desktop: every finger is sent as `0x01` and two-finger gestures go to the remote, not to local
   pinch-zoom.
3. **Agent** `input.c`, case `0x01`: with a touch backend, every pointer goes to `dsk_touch(id, action, x, y)`.
   Without one, it keeps today's mouse mapping.

New platform hook in `desktop/core/desk.h`, with two Linux backends:

```c
int  dsk_touch_open(const char *spec, int w, int h);   /* "inputtest:/run/rd-touch.sock" | "uinput:…" */
void dsk_touch(int id, int action, double nx, double ny); /* action: 0 down, 1 up, 2 move, 3 cancel */
void dsk_touch_reset(void);                            /* lift every finger (0x05 reset, disconnect) */
```

Configured in `desktop.ini`:

```
[input]
touch = inputtest:/run/rd-touch.sock
```

### Chosen: `inputtest` (`desktop/linux/touch_inputtest.c`)

`inputtest` is an Xorg input driver that takes events from a Unix socket. With `DeviceType "Touch"` it
creates an XI2 **direct-touch** device: what a touchscreen is, so Chromium treats it as one.

Protocol (`/usr/include/xorg/xf86-input-inputtest-protocol.h`, version 1.1). Every message starts with
`{uint32 length; uint32 type}`:

1. Connect to `/run/rd-touch.sock` and send `CLIENT_VERSION {1, 1}`. Read `SERVER_VERSION`.
2. For each finger event, send `TOUCH {touchid, touch_type, valuators}`:
   - `touch_type` is `XI_TouchBegin` (18), `XI_TouchUpdate` (19) or `XI_TouchEnd` (20).
   - valuators 0 and 1 are x and y, in the device's 0–65535 range. The viewer's normalized coordinates
     scale straight into it.
3. `WAIT_FOR_SYNC` is only for tests.

Tested: a two-finger pinch written this way arrived in `xinput test-xi2` as `TouchBegin` ×2, `TouchUpdate`
×10 and `TouchEnd` ×2 per touch, from the `touch` device, both as root and in a user namespace.

Mapping in the agent: each viewer pointer id gets a touch id that counts up for every down. A `cancel` is
sent as `TouchEnd`. `dsk_touch_reset` ends every open touch, on `0x05` and when the session ends. A lost
socket (Xorg restarted) reconnects on the next down.

Why it wins in hqnode: the driver lives entirely inside the container's Xorg. There's no kernel input
device, nothing appears on the host, and hqnode needs no change.

### Fallback: `uinput` (`desktop/linux/touch_uinput.c`)

This is for two cases: running the agent on the OECT outside hqnode, or if Chromium somehow ignores the
`inputtest` device. It is **not** a "pass `/dev/uinput` into the container" plan, and here is why not:

- **uinput is not namespaced.** A device made through it is a host kernel input device, and its
  `/dev/input/eventN` appears in the *host's* devtmpfs, not in the container.
- **The fd decides nothing.** Whoever can open `/dev/uinput` can create any device, including a keyboard.
  The kernel's console keyboard handler binds to every device with key bits, so a container could type
  into the host's VT login.

The safe shape: the device is created by something trusted, outside the container, and the container
only gets that one event node.

1. **Create (trusted side).** On hqnode this is a new agent option, `input: touch`. Outside hqnode it is the
   desktop agent itself, as `pad_uinput.c` does for gamepads. The creator:
   - opens `/dev/uinput`;
   - sets `EV_ABS`: `ABS_MT_SLOT` (0–4), `ABS_MT_TRACKING_ID`, `ABS_MT_POSITION_X/Y` (0…W−1, 0…H−1) and
     `ABS_X/Y`;
   - sets `EV_KEY` `BTN_TOUCH` and `INPUT_PROP_DIRECT`;
   - names it `rd-touch-<cid>` and calls `UI_DEV_CREATE`. It keeps the fd open for the container's life:
     closing it removes the device.
2. **Hand over (hqnode only).** `UI_GET_SYSNAME` gives `inputN`, and from that `eventN`'s major:minor.
   - Make that node in the container's device tmpfs, owned by the container's root UID, mode 0660, as the
     `rkmpp` option does.
   - Bind it to `/dev/input/event0`.
   - Add a host udev rule `ATTRS{name}=="rd-touch-*", ENV{LIBINPUT_IGNORE_DEVICE}="1"` so nothing on the
     host ever consumes it.
3. **Inject (in the container).** The agent writes `struct input_event` to `/dev/input/event0`. Writing to
   an evdev node injects into that device, and the kernel drops any event type the device didn't declare,
   so the container can only make touches. Each frame is a type-B sequence:
   - for each changed finger: `ABS_MT_SLOT n`, then `ABS_MT_TRACKING_ID id` (or −1 to lift);
   - its `ABS_MT_POSITION_X/Y`;
   - `BTN_TOUCH` 1 on the first finger down and 0 on the last up;
   - `ABS_X/Y` of the first finger;
   - `SYN_REPORT`.
4. **Xorg.** The device must exist before Xorg starts, because `AutoAddDevices` is off:
   ```
   Section "InputDevice"
     Identifier "touch"
     Driver "evdev"
     Option "Device" "/dev/input/event0"
     Option "GrabDevice" "off"    # an EVIOCGRAB by Xorg would drop events injected through another fd
   EndSection
   ```
   (`libinput` also works, and doesn't grab.)

The hqnode part is an agent change with its own security review. It isn't in the v1 build order.

---

## Keyboard

Unchanged from the desktop agent: `0x04` key messages become XTest keycodes (evdev + 8), and text from
the iPhone's keyboard sheet arrives as ctrl `text`, typed as keysyms on a spare keycode.

The iOS keyboard opens from the viewer's existing keyboard button. CloakBrowser in kiosk mode gets what is
typed while one of its fields has focus. The bar's URL field gets it while the bar has focus (below).

---

## Controller bar: `rd-shell`

One small program (C, Xlib + Xft, ~600 lines; `browser/rd-shell/`), started by `rd-shell.service` as
user `browser`. It does three jobs.

### 1. The bar

A strip across the top, **44 CSS px** tall: 88 px at 2×, which is 44 pt on the iPhone, Apple's minimum
tap size. It looks like a phone status bar, and it also makes `screen.availHeight < screen.height`, as on
a phone. Since it is part of the X screen, it's in the video, works from any viewer, and needs no viewer
change.

```
┌──────────────────────────────────────────────────────┐
│ ◀  ▶  ⟳  │ example.com — Page title …          │ ⋯ │   88 px
├──────────────────────────────────────────────────────┤
│                                                      │
│          CloakBrowser (kiosk), 880 × 1816            │
```

| Control | Action |
|---|---|
| ◀ ▶ ⟳ | Back, forward, reload: Alt+Left, Alt+Right, F5 sent with XTest to the browser window |
| Title / URL | Shows the window title (`_NET_WM_NAME`). Tap: becomes a text field with the bar focused. Type on the iPhone keyboard; Enter opens it (below) |
| ⋯ menu | Drops down over the page while open: **Home**, **New identity**, **Restart browser**, **Stop / Start browser**, **Tabs** (Ctrl+Tab, Ctrl+W), **Proxy: ok / down** |

Opening a URL runs CloakBrowser again with the same profile and the URL. Chromium's process singleton hands
it to the running browser as a new tab, and the bar closes the previous tab with Ctrl+W so there's one tab
at a time. One tab also matters for the OECT's memory. This uses no DevTools connection at all, so there's
nothing for a site to detect.

If v2 needs the real URL rather than the title, `rd-shell` can launch CloakBrowser with
`--remote-debugging-pipe` and use only `Target.setDiscoverTargets`, which reports url and title without
attaching to the page. It still calls no `Runtime.*`. That's a choice to make after testing, not part of v1.

### 2. The window manager

It's the only window manager on `:0`, deliberately minimal:

- It selects `SubstructureRedirect` on the root. It places every top-level window below the bar, whatever
  the window asks for: `(0, 88, 880, 1816)` at `light`, `(0, 88, 784, 1608)` at `small`. A
  `_NET_WM_STATE_FULLSCREEN` request (kiosk mode) gets the state set but the same geometry, so the browser
  is "full screen" below the bar.
- It publishes `_NET_SUPPORTING_WM_CHECK`, `_NET_SUPPORTED`, `_NET_WORKAREA` and `_NET_ACTIVE_WINDOW`.
- It draws no frames. Override-redirect windows (menus, `<select>` popups) place themselves as usual.
- Focus: the browser window has it, except while the URL field is being edited.

Fallback if kiosk fights the geometry: drop `--kiosk`, use `--app=<home>` (no tab strip or omnibox), with
the same placement.

### 3. The browser supervisor

It reads `/data/browser.conf`, starts CloakBrowser with the flags below, restarts it if it dies, and runs
the menu actions:

| Action | What it does |
|---|---|
| Restart | Stop the browser, then start it with the same profile and seed |
| New identity | Stop the browser, delete `/tmp/profile`, then start with a new random seed. The new seed is written to `/data/browser.conf` only if `seed_mode = sticky` |
| Stop | Stop the browser; the bar shows "Stopped — tap ⋯ → Start" |
| Proxy check | A TCP connect to the proxy every 10 s; it doesn't fetch anything through it |

---

## The encoder in the container: the `rkmpp` device option

### What the encoder needs

The RK3566's video encoder is driven by Rockchip's MPP library through:
- **`/dev/mpp_service`**: the encoder itself (the vendor kernel's `rk_vcodec` driver);
- **`/dev/dma_heap/*`**: where MPP allocates the frame and bitstream buffers the encoder reads and writes.

The OECT's `/dev/dri` is the display controller and the Mali GPU, and neither encodes video, so VA-API and
the earlier `gpu` idea don't apply here. `/dev/rga` (Rockchip's 2D scaler/converter) is optional; see Video.

### Why hqnode has to make the nodes (tested)

Tested on this machine with a DRM render node (226:128), in a user namespace mapped the way hqnode maps a
container (container root = host UID 100000). Permission checks on a character device are the same
whatever the driver, so this carries over to `mpp_service` and `dma_heap`:

| Way of giving the node | Open `O_RDWR` as container root | Why |
|---|---|---|
| Bind the host's node (owned by host root) | **EACCES** | Its owner isn't mapped in the namespace, and the kernel refuses writes to such files. This is the same reason `manager/devices.go` can't bind the host's `/dev/null` |
| A new node with the same major:minor, on a tmpfs without `nodev`, owned by UID 100000, mode 0660 | **ok**, and the driver answers its ioctl | Owned by a mapped UID |
| The same node, opened by container UID 1000 | EACCES | Not the owner and not in its group |
| … after container root runs `chgrp`, opened by a member of that group | **ok** | |

A node under the host's `/run` (mounted `nodev`) is refused even for real root. hqnode's per-container
device tmpfs is mounted without `nodev`, which is why it works there.

Also checked in hqnode's code:
- hqnode attaches no cgroup device program, and `hqnode.service` sets no `DevicePolicy`, so the cgroup
  won't block the nodes.
- The seccomp filter only refuses btrfs ioctls, so the MPP and DMA-heap ioctls pass.
- `/sys` is mounted in the container (read-only).

What isn't tested yet is `mpp_service` itself under a user namespace. Build step 2 does that.

### The hqnode change

Placed where hqnode already does the same thing for `/dev/null`:

| Where | Change |
|---|---|
| `agent/internal/manager/devices.go` | `prepareDevices` takes the container's device list. Each name maps to a fixed list of host paths. For `rkmpp`: `/dev/mpp_service`, every `/dev/dma_heap/*`, and `/dev/rga` if present. For each, it `stat`s the host node for its type and major:minor, `mknod`s the same in the device tmpfs, and chowns it to the container's mapped root, mode 0660. Nothing outside the list, and never a path from the request |
| `agent/internal/runtime/mounts.go` `populateDev` | Binds each prepared node at its own path (`/dev/mpp_service`, `/dev/dma_heap/<name>`, `/dev/rga`), creating directories as needed. The fixed six stay as they are |
| `agent/internal/store/types.go` `Container` | `Devices []string json:"devices,omitempty"`; accepted values: `"rkmpp"` (and later `"gpu"` for DRM render nodes on x86 machines) |
| `agent/internal/api/containers.go` `createRequest` | `devices`, refused with a clear error if the host lacks the profile's nodes |
| Host report | The agent reports which profiles the machine can offer (`rkmpp` when `/dev/mpp_service` exists), so the panel shows it |
| `server/` + `web/` | The machine owner sets it per container (create and edit), as with ports. A tenant can't turn it on |
| Reinstall | Keeps `devices`. Transfer/migrate refuses a target machine without the profile |

If a node is missing at start (kernel booted without the driver), the container logs it and still starts,
and the agent falls back to software H.264.

### What it costs in isolation

| Exposure | How bad | What limits it |
|---|---|---|
| Rockchip's vendor `mpp_service` and `dma_heap` drivers become reachable from the container | The main new attack surface. Vendor drivers get less review than mainline ones, and a bug in one is a host kernel bug | Only for containers the machine owner marks; this box runs one container, yours |
| DMA-heap buffers aren't charged to the container's memory limit | On a 2 GB box, a runaway allocator could push the host into OOM | The agent allocates a fixed small set of buffers (about 4 × 6.7 MB); only a trusted container gets `rkmpp` |
| The encoder is one shared block | Irrelevant with one container | — |

---

## Video

### The MPP backend (`desktop/linux/video_mpp.c`, new)

It plugs in behind the agent's existing hardware interface (`dsk_hwvideo_open` / `_step` / `_keyframe` /
`_bitrate` / `_close` / `_has_h265` / `_name` in `desk.h`):

- **Selection.** `desktop/linux/video_hw.c` (new, small) chooses the backend:
  - MPP when `/dev/mpp_service` opens and `librockchip_mpp.so.1` loads;
  - otherwise VA-API (`video_vaapi.c`, unchanged);
  - otherwise the existing software fallback.

  `dsk_hwvideo_has_h265` asks the chosen backend.
- **Library.** Loaded with `dlopen`, as libva is, so the same agent binary runs where MPP isn't installed.
- **Encoder setup:**
  - `MPP_CTX_ENC`, with `MPP_VIDEO_CodingHEVC`, or `MPP_VIDEO_CodingAVC` when the viewer only takes H.264;
  - prep: width, height, `hor_stride` (bytes, `w × 4`), `ver_stride`, format `MPP_FMT_BGRA8888`. That's the
    byte order of a 24-bit X11 image, so the capture needs no conversion;
  - rate control: CBR at the viewer's bitrate, `fps_in = fps_out = 30`, a long GOP with
    `MPP_ENC_SET_IDR_FRAME` on request;
  - headers: `MPP_ENC_HEADER_MODE_EACH_IDR`, so every IDR carries its VPS/SPS/PPS.
- **Per frame:**
  1. XDamage says the screen changed.
  2. The MIT-SHM image is copied into one of two MPP DMA buffers (6.7 MB; a `memcpy` is cheap next to
     what the encoder saves).
  3. `encode_put_frame`, then `encode_get_packet`.
  4. The packet's Annex-B NAL units go to the same callback the VA-API path uses (`push_au`).
- **Sizes.** Width and height must be multiples of 16 (`light` and `small` are). MPP doesn't scale, so the
  encode size is the screen size. If the viewer's quality menu asks for a smaller short side, the agent
  ignores that and logs it. Scaling through `/dev/rga` is a later option.
- **If BGRA input is refused** by this encoder (build step 1 checks it), convert BGRA → NV12 before
  encoding: through RGA (`/dev/rga`, already in the `rkmpp` profile), or on the CPU with `yuv.c` as a
  last resort. On A55 cores the CPU route costs about a core at `light`.
- **Fallbacks:** MPP H.264 if the viewer doesn't take H.265, then minih264 in software (Constrained
  Baseline, short side capped at 1080 by `video.c`).

The iPhone viewer already answers with a codec the computer offers. H.265 is used when iOS Safari
negotiates it.

### What the CPU still does

- **Capture:** XDamage plus one MIT-SHM copy per changed frame.
- **One `memcpy` into the MPP buffer.**
- **Chromium's software rendering:** this is the real load, and the reason the default screen is `light`.

**Chromium doesn't get the GPU.** CloakBrowser runs with `--disable-gpu`, so WebGL goes through SwiftShader
and the real GPU never appears in it. On the OECT that also avoids exposing the vendor Mali driver to the
browser. GPU rasterization on the Mali-G52 is a possible later experiment, if scrolling is too slow, and
then only together with CloakBrowser's WebGL spoofing.

---

## Browser settings

```bash
--fingerprint-platform=linux
--fingerprint=<seed>                  # same seed = same identity; new seed = new identity
--proxy-server=socks5://PROXY:PORT    # socks5:// → DNS resolved by the proxy
--host-resolver-rules="MAP * ~NOTFOUND , EXCLUDE PROXY"   # no local DNS lookups, ever
--force-webrtc-ip-handling-policy=disable_non_proxied_udp
--webrtc-ip=<proxy IP or auto>
--kiosk --force-device-scale-factor=2 --touch-events=enabled
--user-data-dir=/tmp/profile          # tmpfs, wiped by "New identity" and every reinstall
--disable-dev-shm-usage               # hqnode's /dev/shm is 64 MB
--disable-gpu                         # WebGL via SwiftShader; no Mali driver in the browser
--renderer-process-limit=2 --disk-cache-size=52428800
--lang=<locale>                       # plus TZ=<zone> in the browser's environment
```

- Timezone, language and locale must match the proxy's exit location. `rd-shell` sets `TZ` and `--lang`
  from `browser.conf`.
- If the proxy needs a username/password, run a local forwarder (`gost` / `microsocks`) on `127.0.0.1` as
  another unit in the image.
- Keep Chromium's sandbox on. If it fails in a nested user namespace, find out why rather than adding
  `--no-sandbox` (step 5).

---

## Network and the kill switch

The container has **no `CAP_NET_ADMIN`**, so there's no firewall inside it, and hqnode has **no
per-container egress allow-list** yet. On the OECT you own the host, so the host does it.

hqnode's network is **pasta**, which turns the container's connections into ordinary sockets owned by a
pasta process on the host. Each container's pasta runs in a cgroup of its own, beside the container's
(`hqnode/docs/security.md`). nftables can match a socket's cgroup, so one set of rules covers exactly this
container's traffic:

```
table inet rd_killswitch {
  set allow4 { type ipv4_addr . inet_proto . inet_service; elements = {
      <proxy IP> . tcp . <proxy port>,  <proxy IP> . udp . <proxy port>,     # SOCKS5 + its UDP relay
      <remodroid server IP> . tcp . 443,  <TURN IP> . udp . 3478 } }      # the agent
  chain out {
    type filter hook output priority 0;
    socket cgroupv2 level 3 "<pasta cgroup of this container>" ip daddr . meta l4proto . th dport @allow4 accept
    socket cgroupv2 level 3 "<pasta cgroup of this container>" drop
  }
}
```

- Find the cgroup path on the OECT with
  `cat /proc/$(pgrep -f "pasta.*<cid>")/cgroup`. `level` is that path's depth.
- `rd-killswitch.service` on the host writes the rules from the same `browser.conf` values. It runs
  `After=hqnode.service`, and again whenever the container restarts, since a new pasta may get a new path.
- If the cgroup match can't be made to hold, the fallback is coarser: drop everything leaving the OECT
  except the same allow-list. The box runs nothing else that needs the internet besides hqnoded's panel
  link, which you add to the list.

| Layer | What it does |
|---|---|
| Browser | Fixed SOCKS5 proxy: Chromium never falls back to direct. DNS is mapped to NOTFOUND except for the proxy. WebRTC is limited to proxied UDP. If the proxy dies, pages fail |
| Host nftables | The container reaches only the proxy, the RemoDroid server and TURN. Nothing on the home network, nothing else on the internet |

---

## Security

- **Container:** hqnode's defaults are stronger than the original Docker flags. User namespace (root inside
  is an unprivileged UID outside), a capability set without `CAP_NET_ADMIN`/`CAP_SYS_ADMIN`, a seccomp
  denylist, and six device nodes.
- **Devices passed in:** `/dev/mpp_service`, `/dev/dma_heap/*` and optionally `/dev/rga`, made by hqnode for
  this container (see "What it costs in isolation"). No `/dev/dri`, no `/dev/uinput`, no `/dev/input`.
- **Users:** Xorg as container root; the browser and bar as `browser`; the agent as `rdagent`, the only
  member of `video`.
- **Profile:** tmpfs only; nothing from a site reaches `/data`.
- **Host:** the OECT on its own VLAN; the host nftables kill switch above; SSH only through the hqnode
  gateway or Tailscale; no ports open on the home router (except an optional WebRTC UDP port).
- **Storage:** everything of hqnode's is on the SSD, which is local to the OECT; no network storage. The
  browser profile never touches it (tmpfs), so a reinstall leaves nothing from a site on disk.
- **Access:** RemoDroid login (cookie, HTTPS). Keep the panel password strong.
- **RemoDroid options for this device** (`desktop.ini`): `clipboard_sync = 0`, `allow_shell = 0`, no
  `[files] upload_dir`. That means no clipboard, no file transfer and no shell.
- **Session end:** "New identity", or `reinstall.sh` for a clean root filesystem.
- **Still exposed:** anything typed or uploaded. Use fake details only.

---

## iPhone viewer

RemoDroid's web app, as it is today:

- Added to the Home Screen, it runs full screen.
- Video fit to the screen. At 880×1904 it fills the iPhone 14 Plus exactly.
- Login and cookie.
- Keyboard sheet for typing.
- Quality menu: bitrate and fps. Resolution stays at the screen size with MPP.

The only viewer change is the **touch** mode in the touch input plan: when `session.ready` says `touch`, use
the phone's multi-touch path for this computer.

---

## RemoDroid changes, by file

| File | Change |
|---|---|
| `desktop/core/desk.h` | `dsk_touch_open`, `dsk_touch`, `dsk_touch_reset` |
| `desktop/core/input.c` | `0x01` → `dsk_touch` for every pointer when a touch backend is open; reset on `0x05` and session end |
| `desktop/core/session.c` | `"touch": N` in `session.ready` |
| `desktop/core/agent.c` | read `[input] touch` |
| `desktop/linux/touch_inputtest.c` | new: the socket client above |
| `desktop/linux/touch_uinput.c` | new, later: the fallback |
| `desktop/linux/video_mpp.c` | new: the MPP H.265/H.264 encoder |
| `desktop/linux/video_hw.c` | new: picks MPP, then VA-API; `video_vaapi.c`'s `dsk_hwvideo_*` become `vaapi_*` behind it |
| `desktop/desktop.mk` | build both on Linux; an `mpptest` like `vatest` |
| `desktop/mac/`, `desktop/win/` | stub `dsk_touch_open` returning "not supported" |
| `web/app.js` | raw multi-touch for desktops that advertise `touch` |
| `browser/rd-shell/` | new: the bar, window manager and supervisor |
| `browser/hqnode/` | new: the arm64 image, `rd-video.service`, and its scripts (`create.sh` sends `"devices": ["rkmpp"]`) |
| `browser/host/` | new: `rd-killswitch.service` and its nftables template, for the OECT host |
| `tests/e2e_desktop.py` | a case on Xorg dummy + inputtest: send a two-finger `0x01` sequence, assert XI2 touch events |

---

## Build order

1. **OECT host**:
   - Armbian with the vendor 6.1 kernel; check `/dev/mpp_service`, `/dev/dma_heap/`, cgroup v2;
   - build MPP and run `mpi_enc_test` at 880×1904 from BGRA. This answers the three hardware unknowns:
     BGRA input, the 1904-line height, and the rate at 30 fps;
   - the 64 GB SSD at `/var/hqnode` with `RequiresMountsFor`, zram plus the SSD swap file, VLAN;
   - install hqnoded and enroll the OECT.
2. **hqnode `rkmpp` option**:
   - the agent part (`devices.go`, `populateDev`, store, create API), with a test on the OECT that opens
     `/dev/mpp_service` as container root and allocates from a DMA heap;
   - then the panel part.
3. **MPP backend**: `video_mpp.c` + `video_hw.c` + `mpptest`. Develop on the OECT host, outside the
   container, on an Xorg dummy screen.
4. **Touch in the agent**: `touch_inputtest.c`, the `input.c`/`session.c` changes, and the e2e case.
   Develop on this machine with the tested config.
5. **Image and container**: Dockerfile (arm64) on `hqnode:debian-13`, created with `rkmpp`. In the
   container, check that:
   - Xorg starts and the touch device is there;
   - `mpptest` as `rdagent` encodes;
   - CloakBrowser starts with its sandbox.

   Pair the agent and watch from the iPhone (view only). The stats must say **H.265, hardware (MPP)**.
6. **Viewer touch mode**: real touch from the iPhone. Check scroll, fling, pinch, long-press.
7. **Keyboard**: the keyboard sheet into page fields.
8. **`rd-shell`**: window manager and placement first, then the bar, then the menu and supervisor.
   Measure RAM and CPU. Decide between `light` and `small`.
9. **Proxy, kill switch and fingerprint**: `rd-killswitch`, proxy, DNS, WebRTC, timezone and locale; the
   tests below.
10. **Later**: RGA scaling for smaller encode sizes; GPU rasterization experiment; uinput, only if
    `inputtest` falls short; an hqnode per-container egress allow-list, to replace the host script.

---

## Testing

| Check | Where | Pass condition |
|---|---|---|
| Kernel / devices | OECT host | 6.1 vendor kernel; `/dev/mpp_service` and `/dev/dma_heap/*` present |
| Storage | `findmnt /var/hqnode`; boot with the SSD unplugged | ext4 on the SSD; without it, hqnode doesn't start (nothing written to the eMMC) |
| Encoder (host) | `mpi_enc_test` at 880×1904, BGRA | Decodable H.265 file; 30 fps or better |
| Devices (container) | `ls -l /dev/mpp_service /dev/dma_heap` | Present, group `video`, nothing else added |
| Encoder access | `sudo -u rdagent mpptest` / `sudo -u browser mpptest` | Agent encodes; browser is refused |
| Encoder (live) | Viewer stats, agent log | H.265 hardware (MPP), steady 30 fps at 880×1904; H.264 when H.265 isn't negotiated |
| Screen | `xdpyinfo` in the container; `innerWidth`, `devicePixelRatio` in the page | 880×1904; 440, 2 |
| Touch device | `xinput list --long touch` | `Touch mode: direct`, 5 touches |
| Touch in the page | A touch-test page, plus `navigator.maxTouchPoints`, `matchMedia('(pointer: coarse)')`, `matchMedia('(hover: none)')` | 5 / true / true |
| IP / DNS | browserleaks.com | Only the proxy IP and proxy-side DNS |
| WebRTC | browserleaks.com/webrtc + a WebRTC test site | Only the proxy IP |
| Kill switch | `curl` to any other address from inside the container; stop the proxy | Refused by nftables (counter rises); with the proxy down, pages fail and the bar shows "Proxy: down" |
| Home network | Connect from the container to a LAN address | Refused |
| Fingerprint | CreepJS, pixelscan.net, FingerprintJS demo | No tampering or inconsistency flags |
| Gestures | Real mobile sites | Scroll, fling, pinch, long-press and carousels work |
| Bar | Back, reload, URL, New identity | New identity gives a new fingerprint ID and an empty profile |
| Performance | `top` / `free` on the host and in the container, viewer stats | No OOM; Chromium keeps up while scrolling; acceptable latency |

---

## Risks / open questions

- **The encoder's limits at portrait sizes.** The RK3566 encoder is rated 1080p60. Whether it takes a
  frame 1904 (or 1696) lines tall, and BGRA input, is checked on day one (step 1).
  - If height is the limit, encode the frame rotated 90° (MPP's prep rotation; 1904×880 is within
    1080p's size) and have the viewer rotate it back. That's a viewer change.
  - If BGRA is the limit, convert with RGA first.
- **Chromium on four A55 cores** without a GPU is the weakest part: heavy pages will scroll slowly. `small`
  and one tab are the levers; GPU rasterization is the later experiment.
- **2 GB of RAM** is tight for Chromium plus Xorg plus the agent. zram is required, and the profile on
  tmpfs counts against it.
- **Vendor kernel:** MPP needs Armbian's vendor 6.1 kernel, and hqnode hasn't run on it yet (it needs 5.19+,
  cgroup v2, idmapped mounts on ext4, `passt`). Step 1 shows whether anything is missing.
- **`mpp_service` in a user namespace** is untested; the DRM-node test only shows the permission side.
- **Device profile:** Linux + portrait phone-sized screen + touch matches few real devices. Strict detectors
  may flag it. This is the one thing Android-over-ADB would do better.
- **Layout by user agent:** sites that choose layout by UA, not width, may still serve desktop pages.
- **Chromium and `inputtest`:** XI2 events were tested, not Chromium itself. Chromium should take a direct
  XI2 touch device as a touchscreen (step 5 checks it). The `uinput` fallback is the answer if it doesn't.
- **Pointer media queries:** XTest's virtual pointer exists on every X server. Check whether Chromium counts
  it as a mouse (`pointer: fine` / `hover: hover`).
- **Kiosk under a minimal window manager:** the fallback is `--app` mode.
- **CloakBrowser on Linux arm64:** confirm the arm64 build, the Linux persona, and which features need a
  Pro key.
- **Proxy UDP:** WebRTC through the proxy requires SOCKS5 UDP support on the proxy.
- **The kill switch's cgroup match:** it depends on pasta's cgroup path per container staying stable.
  `rd-killswitch` re-applies on container restart, and there is the coarser whole-box fallback.
