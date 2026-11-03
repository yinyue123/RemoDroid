# syntax=docker/dockerfile:1
#
# Hermes Agent (NousResearch/hermes-agent) as one hqnode container (browser/hermes/hqnode/PLAN.md), for
# the OECT (RK3566, linux/arm64; amd64 builds too).
#
#   hermes-gateway     `hermes gateway run`: the messaging gateway (Telegram, …), cron, the API server
#   hermes-dashboard   `hermes dashboard` on :9119, off until enabled
#
# hqnode:debian-13 rather than upstream's nousresearch/hermes-agent: hqnode wants systemd as PID 1 (that
# image runs s6-overlay), and Docker Hub isn't reachable from the OECT. Hermes goes in with upstream's own
# install script, which brings its pinned toolchain (uv, Python 3.14, Node, ripgrep, FFmpeg) through `pm`.
# No browser and no computer-use tools: Chromium wouldn't fit beside the remote browser in 3.8 GB.
#
# /opt/hermes/agent is the checkout and its venv, /opt/hermes/tools the pinned toolchain
# (HERMES_RUNTIME_DIR, as in upstream's image); both are the image's and a reinstall replaces them.
# /data/hermes is HERMES_HOME (.env, config.yaml, sessions, memories, skills, logs) and survives, so
# `hermes update` is never run in here: rebuild and reinstall. What the installer writes into a fresh
# HERMES_HOME is kept as /opt/hermes/home-template: its installs/ (the dependency environment, which
# Hermes looks for under HERMES_HOME) is linked from /data/hermes, the rest (bundled skills, config.yaml)
# copied in without overwriting anything, at each start (hermes-seed-home, hermes-gateway.service).
#
# Build context: this directory (browser/hermes/hqnode/build.sh, or .github/workflows/hermes-image.yml).
#   HERMES_CHANNEL   stable (default) or main
#   HERMES_COMMIT    a hermes-agent commit to pin, instead of the channel's head

FROM ghcr.io/yinyue123/hqnode:debian-13
ARG HERMES_CHANNEL=stable
ARG HERMES_COMMIT=

# What the installer and the agent's tools expect from the system: a compiler for the odd wheel built from
# source, git and ssh for the tools, tmux for its background processes.
RUN set -eux; apt-get update; \
    apt-get install -y --no-install-recommends ca-certificates curl git openssh-client xz-utils procps tmux \
        build-essential python3 python3-venv libffi-dev libatomic1 file less; \
    rm -rf /var/lib/apt/lists/*

RUN set -eux; useradd -r -m -d /var/lib/hermes -s /bin/bash hermes; \
    install -d -o hermes -g hermes /opt/hermes

# The installer, as the user that runs it. The real HERMES_HOME is on /data, which doesn't exist until
# the container does, so the build's becomes the template. The install dir must not exist yet: the
# installer removes an empty one and clones in its place, so it goes under a parent hermes owns.
USER hermes
ENV HOME=/var/lib/hermes HERMES_RUNTIME_DIR=/opt/hermes/tools
RUN set -eux; cd /var/lib/hermes; \
    curl -fsSL https://hermes-agent.nousresearch.com/install.sh -o install.sh; \
    HERMES_HOME=/opt/hermes/home-template HERMES_INSTALL_DIR=/opt/hermes/agent \
        bash install.sh --non-interactive --skip-browser --skip-computer-use \
        --channel "$HERMES_CHANNEL" ${HERMES_COMMIT:+--commit "$HERMES_COMMIT"}; \
    rm -rf install.sh /opt/hermes/home-template/logs /opt/hermes/home-template/telemetry \
        /opt/hermes/home-template/.env; \
    test -x /var/lib/hermes/.local/bin/hermes
USER root
ENV HOME=/root

# `hermes` for everyone, always with the HERMES_HOME on /data, so `runuser -u hermes -- hermes model`
# needs nothing else. The smoke test runs it against a scratch home; it fails the build when the install
# didn't produce a working CLI.
COPY hermes-wrapper /usr/local/bin/hermes
COPY hermes-seed-home /usr/local/bin/hermes-seed-home
COPY units/ /etc/systemd/system/
COPY hermes.env.example /etc/hermes/
RUN set -eux; chmod 0755 /usr/local/bin/hermes /usr/local/bin/hermes-seed-home; \
    runuser -u hermes -- hermes-seed-home /tmp/hermes-smoke; \
    runuser -u hermes -- env HERMES_HOME=/tmp/hermes-smoke hermes --version; \
    runuser -u hermes -- env HERMES_HOME=/tmp/hermes-smoke hermes gateway --help >/dev/null; \
    runuser -u hermes -- env HERMES_HOME=/tmp/hermes-smoke hermes dashboard --help >/dev/null; \
    rm -rf /tmp/hermes-smoke; \
    runuser -u hermes -- git -C /opt/hermes/agent rev-parse HEAD > /etc/hermes/commit; \
    systemctl enable hermes-gateway.service

# Inherited from the system image, restated because a wrong one turns every stop into 30 s and a kill.
STOPSIGNAL SIGRTMIN+3
CMD ["/sbin/init"]
