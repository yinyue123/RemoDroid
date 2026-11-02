# AirPlay from the remote browser

How the remote browser (`browser-oect` on the OECT) plays to an AirPlay speaker, here the HomePod mini
"Main Bedroom" at 192.168.10.111. This file also covers what went wrong on the way there, and how each
problem was found and fixed (2026-11-01).

## The path of the sound

```
Chromium (user browser)
  │  PulseAudio protocol, PULSE_SERVER=unix:/run/pulse/native
  ▼
pipewire-pulse ──► PipeWire graph ──┬─► "browser" (null sink, "This phone")
                                    │      └─ monitor recorded by remodroid-desktopd ─► the phone
                                    └─► raop_output.Main-Bedroom.local (module-raop-sink)
                                           │  from lan0, 192.168.10.44 (user pulse only)
                                           ▼
                                        HomePod mini 192.168.10.111
                                           TCP 7000  RTSP: the session (control requests)
                                           UDP       RTP audio, control sync, timing
```

- **Chromium** connects to a PulseAudio server. Since the move to PipeWire it is `pipewire-pulse`, on the same
  socket that PulseAudio used (`/run/pulse/native`), so Chromium and the desktop agent didn't change.
- **The sink you pick decides where the sound goes.** The Sound page in rd-shell sets the default sink with
  `pactl set-default-sink`, and moves playing streams there too:
  - "This phone" is the null sink `browser`, whose monitor the desktop agent records and sends to the phone;
  - each AirPlay speaker is a sink of its own.
- **Finding speakers:** `libpipewire-module-raop-discover` browses `_raop._tcp` over Avahi, and avahi-daemon
  listens on `lan0` only. For each speaker found it loads a `libpipewire-module-raop-sink`, which appears as
  one more sink.

### The network: lan0

The container keeps its normal pasta network: the browser's own traffic goes out through the proxy and the
host's kill switch. AirPlay needs the LAN directly (multicast DNS, and UDP both ways), so the container also
gets `lan0`:

- **The link:** a macvlan on the OECT's `eth0` (hqnode: Network card, or `hqnode link`).
- **Its address:** 192.168.10.44/24, gateway 192.168.10.2. hqnode's agent sets these over netlink when the
  container starts, from the address and gateway on the container's Network card.
- **Who can use it:** `rd-lan` (`browser/hqnode/rd-lan`) narrows it down.
  - The address has no route in the main table.
  - Table 100 (or the table hqnode created) holds the LAN, multicast and the gateway.
  - Only the users `pulse` (the sound server) and `avahi` look there: `ip rule uidrange … lookup <table>`.
  - There is no "from 192.168.10.44" rule, so the browser can't bind to the address and bypass the kill
    switch.
- **IPv6 is off on lan0**, and its `rp_filter` is 0: a speaker's packets arrive on lan0, while the main table
  would reach the LAN through `tap0`.

### An AirPlay 1 (RAOP) session

The HomePod speaks AirPlay 2, but it still accepts AirPlay 1 senders ("RAOP", what iTunes used). A session
looks like this:

| Step | Request (RTSP over TCP 7000) | What it does |
|---|---|---|
| 1 | `OPTIONS *` | Asks what the speaker supports. The speaker checks the sender's `User-Agent` here. |
| 2 | `ANNOUNCE` + SDP | Describes the stream: Apple Lossless framing of 16-bit PCM, 44.1 kHz, stereo. |
| 3 | `SETUP` | Exchanges UDP ports. The sender gives its control and timing ports; the speaker answers with its `server_port` (audio) and `control_port`. The HomePod answers `timing_port=0`. |
| 4 | `RECORD` | Starts playback. The speaker may report its own `Audio-Latency`. |
| 5 | `SET_PARAMETER volume` | Sets the volume, in dB. |
| … | `POST /feedback` every 2 s | **Keep-alive. The HomePod drops a sender that doesn't send it.** |
| end | `TEARDOWN` | Ends the session. |

Alongside the RTSP connection there are three UDP flows:

- **Audio:** RTP packets of about 8 ms each (PipeWire's `rtp.ptime` 7.76 ms) to the speaker's `server_port`.
- **Control (sync):** once a second the sender says "RTP time T plays at NTP time N". It sends T minus the
  latency, which tells the speaker how far behind to play. `raop.latency.ms` sets this value.
- **Timing:** the speaker sends NTP-like timing requests to the sender's timing port, and the sender
  answers, so the two clocks agree. The HomePod sends these even before it answers `SETUP`.

## What went wrong, and the fixes

### 1. PulseAudio: SETUP never finished (500 after 32 s)

PulseAudio's RAOP sink opens its timing socket only after the speaker answers `SETUP`. But the HomePod asks
the sender's timing port *before* answering `SETUP`, and waits for a reply. Each side waited for the other;
after 32 s the HomePod answered `500`.

- **Fix:** a patch, `browser/hqnode/pulse/raop-early-timing.patch`. It answers timing requests from
  `ANNOUNCE` onwards.
- **Second problem:** the HomePod then answered `timing_port=0`. The same patch uses the port the timing
  requests came from instead.

### 2. PulseAudio: the speaker vanished from the list

PulseAudio's RAOP sink unloads itself when the connection drops or the sink goes idle, but
`module-raop-discover` still remembers the speaker. So it never adds the speaker again, and the speaker is
gone from the Sound page until the discovery module reloads.

**Fix:** `raop-discover-autoreconnect.patch` (`autoreconnect=1`).

That fix was built, but PulseAudio was replaced before it was installed, for the next problem.

### 3. PipeWire: 403 Forbidden

We moved to PipeWire's RAOP modules, as in the NixOS PipeWire wiki's raop-discover setup. Its sink connected,
but the HomePod answered `OPTIONS` with **403**, and nothing played.

**Finding it:** I sent `OPTIONS` to 192.168.10.111:7000 by hand and changed one header at a time. Only the
`User-Agent` mattered:

| User-Agent | Answer |
|---|---|
| `PipeWire/1.4.2` | 403 |
| `iTunes/11.0.4 (Windows; N)` | 200 |
| `AirPlay/381.13` | 200 |

PipeWire builds it in `module-raop-sink.c` as `DEFAULT_USER_NAME "/" PACKAGE_VERSION`, which is
`PipeWire/1.4.2`.

**Live fix:** in `libpipewire-module-raop-sink.so`, I replaced the string `PipeWire/1.4.2` with
`AirPlay/381.13`. Both are 14 bytes, so nothing else in the file moves. After that, `OPTIONS`, `ANNOUNCE`,
`SETUP`, `RECORD` and `SET_PARAMETER` all answered 200.

### 4. The HomePod wasn't on the Sound page

rd-shell's Sound page only lists sinks named `browser` or `raop_output.*`. Those are PulseAudio's names.
PipeWire calls them `raop_sink.<host>.<ip>.<port>`, for example
`raop_sink.Main-Bedroom.local.192.168.10.111.7000`. The page skipped them, so the only choice was
"This phone".

**Live fix (before the image had the proper one):** a `stream.rules` entry in the discovery module's config renames the HomePod's sink to
`raop_output.Main-Bedroom.local`, described as "Main Bedroom".

- Every rule that matches a speaker creates a sink of its own. So the catch-all rule matches
  `raop.ip = "!192.168.10.111"`; otherwise the HomePod appeared twice.
- **Proper fix, in the image:** the Sound page also accepts `raop_sink.*` names.

### 5. Music stopped after about 15 seconds

The HomePod closed the RTSP connection about 15 s into playback. The log showed:

- `got connection error -32 (Broken pipe)`, then `mod.raop-sink: error -32`;
- after that, `error sending control packet: -9` (EBADF) every second;
- the sink still showed `RUNNING`, but there was no connection to 192.168.10.111:7000 any more.

**Cause:** PipeWire 1.4.2 starts the 2-second `POST /feedback` keep-alive only for one encryption type
(`module-raop-sink.c`, `rtsp_record_reply`):

```c
// feedback timer is only needed for auth_setup	encryption
if (impl->encryption == CRYPTO_AUTH_SETUP && !impl->feedback_timer) {
```

The HomePod mini's Bonjour record gives no RSA (`et=1`) and no MFi auth-setup (`et=4`), so its encryption
type is `none` and PipeWire never sends the keep-alive. The HomePod treats a sender that goes quiet on RTSP
as gone.

**Ruled out:** forcing `raop.encryption.type = "auth_setup"`. The HomePod mini answers `POST /auth-setup`
with **404**, and PipeWire then drops the speaker.

**Live fix:** I changed one instruction in `libpipewire-module-raop-sink.so` (arm64, Debian 1.4.2), so that
the timer starts for every encryption type:

| | Address | Instruction | Word |
|---|---|---|---|
| before | `0xe240` | `b.eq 0xe414` (only when encryption == 2, `auth_setup`) | `0x54000ea0` |
| after | `0xe240` | `b 0xe414` (always) | `0x14000075` |

- `0xe414` is the code that creates the feedback timer: `pw_loop_add_timer(…, rtsp_do_post_feedback, …)`.
- I found it from the string `record status: %d`, which `rtsp_record_reply` logs.
- The patched file's SHA-256 is `157348aff30c73ab4425a3567f5b6d4d642248b026b136ac40196f221732eb0d`. It also
  carries the User-Agent change.

**Checked:**
- A 50-second test tone kept its connection for the whole test, with a `200` reply to each keep-alive (31
  of them).
- Then YouTube Music in Chromium played to "Main Bedroom", and every keep-alive was answered.

### Latency: `raop.latency.ms`

`raop.latency.ms` is set on every speaker: 1000 at first, 500 since you said starting a track felt slow.
Everything you do (play, seek, a new track) is heard about `raop.latency.ms` + 250 ms later. **It is
applied:**
- It becomes the latency in the control sync packets (`rtptime - latency`). That is how long the speaker
  buffers before playing.
- The module's default is 1500 ms, with a floor of 250 ms.

The node's `sess.latency.msec = 250` is a different setting: PipeWire's own send buffer, fixed at 250 ms.

- **If playback drops out**, raise `raop.latency.ms`.
- **If the delay after pressing play is too long**, lower it.

## Where things are, in the image

The live fixes in `browser-oect` (2026-11-02) became the image's own:

| What | Where |
|---|---|
| The two patches, built in the image's `pipewire-raop` stage from Debian's source and options | `browser/hqnode/pipewire/patches/raop-user-agent.patch`, `raop-feedback-always.patch` |
| PipeWire config: "browser" sink (the default at start), raop-discover with `raop.latency.ms = 500` | `browser/hqnode/pipewire/pipewire.conf.d/rd.conf` → `/etc/rd-browser/pipewire/…` |
| pipewire-pulse on `/run/pulse/native` | `browser/hqnode/pipewire/pipewire-pulse.conf.d/rd.conf` |
| Services, as user `pulse` with `XDG_RUNTIME_DIR=/run/rd-pw`, `XDG_CONFIG_HOME=/etc/rd-browser` | `units/rd-pipewire.service`, `rd-pipewire-pulse.service`, `rd-wireplumber.service` |
| The Sound page and the menu's Volume row | `browser/rd-shell/rd-shell.c` (it knows `raop_sink.*` and `raop_output.*`) |

PulseAudio, its patches and `rd-pulse` are gone. The live rename rule (`raop_output.Main-Bedroom.local`) isn't
in the image: the Sound page reads PipeWire's own names now.

## Checking it

Inside the container (`hqnode exec browser-oect …`), with `PULSE_SERVER=unix:/run/pulse/native`:

```sh
runuser -u browser -- pactl list short sinks            # the speakers, and RUNNING / SUSPENDED
runuser -u browser -- pactl list sink-inputs            # Chromium's stream, and which sink it's on
ss -tn | grep 192.168.10.111:7000                      # the RTSP session: one line while playing
journalctl -u rd-pipewire | grep mod.raop               # options/setup/record status, errors
```

With `PIPEWIRE_DEBUG=2,mod.raop*:4` on `rd-pipewire`, the log shows each RTSP reply, the timing syncs, and
one `reply status: 200` every 2 s for the keep-alive.
