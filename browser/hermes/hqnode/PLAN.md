# Hermes Agent on the OECT, as an hqnode container

[Hermes Agent](https://github.com/NousResearch/hermes-agent) (Nous Research, MIT) is a self-hosted agent
with a messaging gateway (Telegram, Discord, Slack, Signal, email…), memory, skills, cron jobs and an
OpenAI-compatible API. This plan runs it on the OECT in its own hqnode container, beside `browser-oect`
and `surveil`, the same way `browser/hqnode` runs the remote browser: an image on `hqnode:debian-13`
with systemd units, built in CI and sideloaded, created through the panel, configured by pushing files
into `/data`.

Nothing here has been installed yet. The environment below is from the box itself, 2026-11-02.

## 1. The OECT today

| | | What it means for Hermes |
|---|---|---|
| Access | `root@192.168.10.15` port 22. From this workstation only through the host's netns: `nsenter -t 1 -n ssh root@192.168.10.15` (the proxy TUN eats plain ssh, as with the router). Port 36000 is closed. | the scripts take `SSH='nsenter -t 1 -n ssh'` for that |
| System | Armbian 26.08 (Debian 13 trixie), kernel 6.1.141-rk35xx-ophub, aarch64, "OEC BOX WXY4" | arm64 image; glibc |
| CPU / RAM | 4 × A55, **3.8 GiB** RAM, ~2.6 GiB available. Swap (since 2026-11-03, through hqnode's tuning): 2 GiB zram zstd (prio 150) + 2 GiB `/var/hqnode/swapfile` (prio 10), swappiness 180; containers get 384 MiB of swap each | room for one more ~1 GiB container, not for another Chromium |
| Disk | `/` 16 G on the SSD (13 G free), `/var/hqnode` 103 G on the SSD (68 G free), `/boot` on eMMC | disk is no constraint |
| hqnode | agent 0.1.0.gf6f7fbe6d8d0, `hqnoded` active, SSH gateway on :2222, panel-managed. Defaults: 2 GiB memory_max, 512 pids. Advanced options on (`CAP_NET_ADMIN`, macvlan on eth0, USB serial) | panel `create` as for the browser |
| Containers | `browser-oect` (d1300c7e471d, ~950 MB, 1.5 G limit, kill-switched), `surveil` (c5abc9784dd9, ~920 MB incl. cache) | Hermes must not starve the browser |
| Images | `hqnode:debian-13` and `:alpine-3.24` cached; browser images are `localhost/…` sideloads (browser-image.yml → fetch-image.sh) | same route for Hermes |
| Network | eth0 192.168.10.15/24 behind the home router. Direct: **GitHub, Docker Hub, api.telegram.org time out**; ghcr.io, pypi, openrouter.ai, Nous Portal answer; api.anthropic.com answers **403** (China egress) | all of Hermes' traffic goes through a proxy |
| Proxies | `sing-box-reality.service`: SOCKS5 inbounds 1081 hk, 1082 hk2, 1083 us, 1084 nl, 1085 tw, 1086 us2 (0.0.0.0), and `socks-browser` 1090 (127.0.0.1, browser only → us). All reach GitHub (hk2 fastest, 0.4 s) | a dedicated inbound for Hermes, below |
| Container egress | pasta: a container's default gateway `192.168.10.1` **is the host's loopback**. From `surveil`: `socks5h://192.168.10.1:1082` → GitHub 200; `127.0.0.1:1082` refused; direct times out | containers use `192.168.10.1:<port>` |
| Kill switch | `rd-killswitch` (nftables) scoped to the browser container's pasta cgroup only | Hermes is not affected |
| Python on the host | 3.13.5, git; no podman/docker | the build happens in CI, not on the box |

## 2. Upstream, as of 2026-11

- Install script `https://hermes-agent.nousresearch.com/install.sh`: clones `NousResearch/hermes-agent`,
  bootstraps `uv` and Hermes' own `pm` toolchain (Python **3.14**, Node, ripgrep, FFmpeg, optionally
  Chromium/Playwright), links `~/.local/bin/hermes`. Flags used here: `--non-interactive`,
  `--skip-browser`, `--skip-computer-use`, `--channel stable`, `--commit SHA`; env `HERMES_INSTALL_DIR`.
  linux-arm64 glibc is supported.
- Official image `nousresearch/hermes-agent` (amd64/arm64, Debian 13, s6-overlay as PID 1, user
  `hermes` uid 10000, state in `/opt/data`). **Not used**: Docker Hub isn't reachable from the OECT,
  hqnode containers want systemd as PID 1, and the panel's catalogue is built around our own images.
- Recommended: 1 GB RAM minimum, 2–4 GB with browser automation; 1–2 cores; a few GB of disk.
- State lives in `HERMES_HOME`: `.env` (keys), `config.yaml`, `SOUL.md`, `sessions/`, `memories/`,
  `skills/`, `logs/`.
- Ports: 8642 API server (opt-in, `API_SERVER_ENABLED`), 9119 dashboard (`hermes dashboard`).
- Terminal backends: local, docker, ssh, modal, daytona, vercel_sandbox (`TERMINAL_ENV`).

## 3. Decisions

| Question | Choice | Why |
|---|---|---|
| Where | a new hqnode container `hermes-oect` | the container is the agent's sandbox: whatever it runs in a shell stays inside it, with its own CPU/memory/disk/traffic limits, and a reinstall resets everything but `/data` |
| Image | `localhost/hermes-hqnode:arm64`, `FROM ghcr.io/yinyue123/hqnode:debian-13`, Hermes installed by its own install script at build time into `/opt/hermes` | systemd units like the browser's; the installer is upstream's supported path, so `pm`'s toolchain is what they test |
| Build | GitHub Actions on an arm64 runner (`.github/workflows/hermes-image.yml`, manual), an oci-archive artifact sideloaded by `fetch-image.sh` | as the browser's; no podman/docker here or on the OECT. Not encrypted: Hermes is MIT and the image holds no keys |
| Hermes version | `--channel stable`, pinned with `HERMES_COMMIT=` when a build is known good | a reinstall shouldn't silently move to `main` |
| Browser tools | **off** (`--skip-browser --skip-computer-use`) | Chromium would need another 1–2 GB; the box has `browser-oect` for browsing, and Hermes' web search/extract tools work without a local browser |
| Terminal backend | `TERMINAL_ENV=local` | "local" is inside the container; no docker socket is given to it |
| State | `HERMES_HOME=/data/hermes` | `/data` survives reinstalls, like the browser's pairing |
| Egress | a new sing-box inbound `mixed-hermes` on host `127.0.0.1:1091`, HTTP+SOCKS, routed to **us**; the container uses `http://192.168.10.1:1091` | HTTP proxying works for every client Hermes runs (httpx, aiohttp, git, npm, uv) without SOCKS extras; the US exit is one Anthropic/OpenAI/Google serve (HK isn't); loopback-only so the LAN can't use it |
| Limits | 2 cores, 1 GiB memory, 8 G root, 8 G `/data`, 256 G/month | leaves the browser its 1.5 G; Hermes without a browser idles at a few hundred MB, Python 3.14 + Node + the venv are ~2 GB on disk |
| Ports | 22 and 9119, both host-only mappings | ssh for `hermes setup`; the dashboard reached through an ssh tunnel, never published |
| Messaging | Telegram first (`TELEGRAM_BOT_TOKEN`, `TELEGRAM_ALLOWED_USERS`) | the simplest gateway; others are a `.env` line each |

**Yours to decide before step 4:** the LLM provider and model (OpenRouter, Nous Portal, Anthropic,
OpenAI, or a custom OpenAI-compatible endpoint) and its key; whether Telegram is the platform, and your
Telegram user id for the allowlist.

## 4. Steps

### 4.1 Host: the proxy inbound (on the OECT, once)

Add to `/etc/sing-box/reality.json`, then restart. Keep a copy first, as for the earlier edits.

```jsonc
// inbounds
{ "type": "mixed", "tag": "mixed-hermes", "listen": "127.0.0.1", "listen_port": 1091 }
// route.rules, next to the socks-browser rules (before the domain/ip "direct" rules)
{ "inbound": ["mixed-hermes"], "ip_is_private": true, "action": "reject" },
{ "inbound": ["mixed-hermes"], "outbound": "us" }
```

```sh
cp /etc/sing-box/reality.json /etc/sing-box/reality.json.prev-hermes
# edit, then
sing-box check -c /etc/sing-box/reality.json && systemctl restart sing-box-reality
curl -sS -x http://127.0.0.1:1091 -o /dev/null -w '%{http_code}\n' https://api.anthropic.com   # 404, not 403
```

The private-address reject keeps the agent from using the proxy to reach the LAN. (The container can
still reach the LAN directly through pasta; a kill switch like the browser's can close that later, see 6.)

### 4.2 Build the image (CI) and sideload it

```sh
gh workflow run hermes-image.yml                       # the stable channel
gh workflow run hermes-image.yml -f commit=<sha>       # or a pinned hermes-agent commit
SSH='nsenter -t 1 -n ssh' ./browser/hermes/hqnode/fetch-image.sh
```

The job builds `browser/hermes/hqnode/Dockerfile` natively on `ubuntu-24.04-arm`; the build fails unless
`hermes --version`, `hermes gateway --help` and `hermes dashboard --help` work. It saves an oci-archive
named `localhost/hermes-hqnode:arm64` with the hermes-agent commit beside it; `fetch-image.sh` downloads
it, checks its sha256 and sideloads it through the machine's hqnode API.

### 4.3 Create (through the panel)

```sh
export SSH='nsenter -t 1 -n ssh' NODE_SSH=root@192.168.10.15
ARCHIVE=hermes-arm64.tar MACHINE=oect ./browser/hermes/hqnode/create.sh
```

`created_by: panel`, so it is listed and deletable there. NODE_SSH sends the create through the machine's
own link to the panel (a create from an archive outlasts Cloudflare's 100 s).

### 4.4 Configure and start

```sh
cp browser/hermes/hqnode/hermes.env.example browser/hermes/hqnode/hermes.env   # git-ignored
$EDITOR browser/hermes/hqnode/hermes.env    # provider key, Telegram token + your user id, dashboard password
MACHINE=oect ./browser/hermes/hqnode/deploy.sh
```

`deploy.sh` refuses an `.env` with no provider key, or a Telegram token without an allowlist. It writes
`/data/hermes/.env` (0600, owned by `hermes`) through the host's mount of `/data` (never `hqnode exec`
stdin, which isn't forwarded), tries the provider, Telegram and GitHub through the proxy from inside,
restarts `hermes-gateway` and shows its last log lines.

Then pick the model once (it writes `/data/hermes/config.yaml`), and restart:

```sh
$SSH -t $NODE_SSH "hqnode exec hermes-oect runuser -u hermes -- hermes model"
$SSH $NODE_SSH "hqnode exec hermes-oect systemctl restart hermes-gateway"
```

### 4.5 Check it

1. Message the Telegram bot: it answers; a message from another account is refused.
2. `hqnode stats hermes-oect`: memory well under 1 GiB idle; `browser-oect` unchanged.
3. Dashboard (optional, `systemctl enable --now hermes-dashboard` in the container):
   `hqnode ports hermes-oect` gives the 9119 mapping `127.100.0.N:PORT`;
   `nsenter -t 1 -n ssh -L 9119:127.100.0.N:PORT root@192.168.10.15`, then http://127.0.0.1:9119
   with the basic-auth user/password from `hermes.env`.
4. Reinstall survives: `MACHINE=oect ./browser/hermes/hqnode/reinstall.sh`, and the bot still remembers.

### 4.6 Updates

Not `hermes update` (it would write into the image's `/opt/hermes`, which a reinstall throws away):
rebuild in CI, `fetch-image.sh`, then `MACHINE=oect ./browser/hermes/hqnode/reinstall.sh`.

## 5. Files here

| File | What it is |
|---|---|
| `Dockerfile` | `hqnode:debian-13` + Hermes via its install script, user `hermes`, the units, smoke test |
| `hermes-wrapper` | `/usr/local/bin/hermes`: the CLI with `HERMES_HOME=/data/hermes` |
| `units/hermes-gateway.service` | `hermes gateway run` as `hermes`, `.env` loaded, enabled |
| `units/hermes-dashboard.service` | `hermes dashboard` on :9119, not enabled |
| `hermes.env.example` | what `/data/hermes/.env` holds; copy to `hermes.env` (git-ignored) |
| `../../../.github/workflows/hermes-image.yml` | the arm64 CI build, an oci-archive artifact kept 3 days |
| `fetch-image.sh` | download that artifact and sideload it onto the machine |
| `create.sh` | panel create with the sizes above (`browser/hqnode/create.sh`, adapted) |
| `deploy.sh` | check and push `hermes.env`, the egress checks, restart, the next commands |
| `reinstall.sh` | the panel's reinstall from the sideloaded image; `/data` kept |
| `build.sh` | a local build, where podman/docker work |

The scripts source `browser/hqnode/panel.sh` rather than carry a third copy of it.

## 6. Risks and open points

- **The install script at build time** is the one part not tried yet: its stages, `HERMES_INSTALL_DIR`
  and the non-interactive path are from its source, not a run. The first CI build is the test; the
  Dockerfile fails loudly if `hermes` isn't on PATH afterwards.
- **Memory:** 1 GiB + 1.5 GiB (browser) + surveil on 3.8 GiB. The 4 GiB of swap (zram first, then the SSD file) and
  earlyoom are behind it; watch `hqnode stats` for zram filling up after Hermes is in.
- **Agent with a shell:** `TERMINAL_ENV=local` means it can run anything as `hermes` in its container,
  reach the LAN directly via pasta, and spend the provider budget. The allowlist (`TELEGRAM_ALLOWED_USERS`)
  is the gate; a kill switch scoped to this container's pasta cgroup (`browser/host/rd-killswitch`, a second
  config) allowing only `127.0.0.1:1091` and DNS would close the LAN.
- **Provider and region:** through the US exit, Anthropic/OpenAI accept the connection; their account
  terms on where you may use them are yours to check.
- `hermes update` must not be used inside the container (4.6).
