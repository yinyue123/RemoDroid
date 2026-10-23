# syntax=docker/dockerfile:1
#
# The remote browser as one hqnode container (browser/remote-browser-plan.md): CloakBrowser on a
# portrait Xorg screen, the controller bar, and the RemoDroid desktop agent that streams it to the
# iPhone, for the OECT (RK3566, linux/arm64; amd64 builds too, without the Rockchip encoder).
#
#   rd-xorg             Xorg :0, the dummy screen + the inputtest touch device     /run/rd-touch.sock
#   rd-video            the MPP device nodes hqnode makes ("rkmpp") handed to group video
#   rd-shell            the bar, the window manager, the browser supervisor        user browser
#   remodroid-desktopd  capture → Rockchip MPP H.265 → WebRTC; touch and keys in   user rdagent
#
# debian-13 rather than Alpine: CloakBrowser is a glibc build. systemd is PID 1, so the services
# are units and Environment= works (no OpenRC scrubbing, unlike the sh0u image). /data holds what a
# reinstall keeps: browser.conf and the agent's pairing. The browser profile is on tmpfs and never
# reaches it.
#
# Build from the repository root (browser/hqnode/build.sh does):
#   CLOAKBROWSER       the free CloakBrowser build from GitHub Releases (the default): Chromium 146,
#                      146.0.7680.177.5 for amd64 and .4 for arm64 (.5 has no arm64 tarball), checked
#                      against the release's SHA256SUMS. "chromium" installs Debian's instead, which
#                      has no fingerprint control (--fingerprint* are ignored).
#   CLOAKBROWSER_URL   another .tar.gz of it (a newer build), with its sha256 in CLOAKBROWSER_SHA256.
#                      The binary's licence allows no redistribution: don't push an image with it to a
#                      public registry.
#   MPP_COMMIT         the rockchip-linux/mpp commit to build.

# ── the agent, the bar and Rockchip MPP, built for the target ───────────────
FROM debian:13 AS build
ARG MPP_COMMIT=20959ec3dabb313b8c33ca0b7b09c06ffd2a9caa
ARG TARGETARCH
RUN set -eux; apt-get update; \
    apt-get install -y --no-install-recommends build-essential cmake git ca-certificates pkg-config \
        libx11-dev libxext-dev libxtst-dev libxrandr-dev libxdamage-dev libxfixes-dev libxft-dev libfontconfig-dev libva-dev libxi-dev
# MPP: only the library and its encoder test; Debian has no package of it. arm64 only: elsewhere
# there is no Rockchip encoder, and the agent uses VA-API or software.
RUN set -eux; mkdir -p /opt/mpp/lib /opt/mpp/bin; [ "$TARGETARCH" = arm64 ] || exit 0; \
    git clone https://github.com/rockchip-linux/mpp.git /src/mpp; cd /src/mpp; git checkout "$MPP_COMMIT"; \
    cmake -S . -B b -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/mpp -DBUILD_TEST=ON -DRKPLATFORM=ON \
        -DHAVE_DRM=ON; \
    cmake --build b -j"$(nproc)"; cmake --install b; \
    cp b/test/mpi_enc_test /opt/mpp/bin/ 2>/dev/null || true
COPY . /src/remodroid
WORKDIR /src/remodroid
RUN set -eux; make -j"$(nproc)" desktop browser OPT='-O2 -g0'; \
    if [ "$TARGETARCH" = arm64 ]; then make browser-mpptest MPP_INC=/src/mpp/inc OPT='-O2 -g0'; \
        cp build/host/browser/mpptest /opt/mpp/bin/; fi

# ── the container ────────────────────────────────────────────────────────────
FROM ghcr.io/yinyue123/hqnode:debian-13
ARG TARGETARCH
ARG CLOAKBROWSER=free
ARG CLOAKBROWSER_URL=
ARG CLOAKBROWSER_SHA256=

# Xorg (its inputtest driver is in xserver-xorg-core) with the dummy screen; the fonts a page expects, and
# the ones CloakBrowser asks for (anti-bot scripts hash emoji and CJK drawn on a canvas, and a set no real
# browser has gives it away); the agent's and the bar's libraries; xauth for the display's cookie.
RUN set -eux; apt-get update; \
    apt-get install -y --no-install-recommends xserver-xorg-core xserver-xorg-video-dummy xauth x11-utils xinput \
        fonts-noto-core fonts-noto-cjk fonts-noto-color-emoji \
        fonts-freefont-ttf fonts-unifont fonts-ipafont-gothic fonts-wqy-zenhei fonts-tlwg-loma-otf \
        libx11-6 libxext6 libxtst6 libxrandr2 libxdamage1 libxfixes3 libxft2 libfontconfig1 \
        libnss3 libgbm1 libasound2t64 libatk-bridge2.0-0t64 libcups2t64 libxkbcommon0 libpango-1.0-0 \
        libxcomposite1 libdrm2 libcairo2 libva2 libva-drm2 libxi6 libdbus-1-3 curl ca-certificates; \
    if [ "$CLOAKBROWSER" = chromium ]; then apt-get install -y --no-install-recommends chromium; fi; \
    rm -rf /var/lib/apt/lists/*

# CloakBrowser into /opt/cloakbrowser, its binary as /opt/cloakbrowser/chrome
RUN set -eux; url=$CLOAKBROWSER_URL sum=$CLOAKBROWSER_SHA256; \
    if [ -z "$url" ] && [ "$CLOAKBROWSER" = free ]; then \
        r=https://github.com/CloakHQ/CloakBrowser/releases/download; \
        case $TARGETARCH in \
        amd64) url=$r/chromium-v146.0.7680.177.5/cloakbrowser-linux-x64.tar.gz \
               sum=4a12bcde95fa1bb1beef2b41ab5e5c27c36be78e3be3d0dac8c64d705216670e ;; \
        arm64) url=$r/chromium-v146.0.7680.177.4/cloakbrowser-linux-arm64.tar.gz \
               sum=8b71ce53b4fd131327331a31fba3835d71882d19bfaabde78dd0f5390bd16f45 ;; \
        *) echo "no CloakBrowser build for $TARGETARCH: CLOAKBROWSER=chromium" >&2; exit 1 ;; \
        esac; \
    fi; \
    if [ -n "$url" ]; then \
        curl -fsSL "$url" -o /tmp/cb.tgz; \
        [ -z "$sum" ] || echo "$sum  /tmp/cb.tgz" | sha256sum -c -; \
        mkdir -p /opt/cloakbrowser; tar xzf /tmp/cb.tgz -C /opt/cloakbrowser; rm /tmp/cb.tgz; \
        [ -x /opt/cloakbrowser/chrome ] || ln -sf "$(find /opt/cloakbrowser -maxdepth 2 -type f -name 'chrome*' -perm -u+x | head -1)" /opt/cloakbrowser/chrome; \
    else \
        mkdir -p /opt/cloakbrowser; ln -sf /usr/bin/chromium /opt/cloakbrowser/chrome; \
    fi

# browser and rdagent share the X cookie (group rdx); only rdagent may open the encoder (group video)
RUN set -eux; groupadd -r rdx; \
    useradd -r -m -d /var/lib/browser -s /usr/sbin/nologin -G rdx browser; \
    useradd -r -m -d /var/lib/rdagent -s /usr/sbin/nologin -G rdx,video rdagent

COPY --from=build /src/remodroid/build/host/desktop/libremodroid.so /usr/lib/remodroid/libremodroid.so
COPY --from=build /src/remodroid/build/host/desktop/remodroid-desktopd /usr/bin/remodroid-desktopd
COPY --from=build /src/remodroid/build/host/browser/rd-shell /usr/bin/rd-shell
COPY --from=build /opt/mpp/lib/ /usr/local/lib/
COPY --from=build /opt/mpp/bin/ /usr/local/bin/
COPY browser/hqnode/rd-xorg-light.conf browser/hqnode/rd-xorg-small.conf browser/hqnode/rd-xorg-native.conf /etc/X11/
COPY browser/hqnode/rd-xorg-prepare browser/hqnode/rd-video-prepare /usr/lib/rd-browser/
COPY browser/hqnode/units/ /etc/systemd/system/
COPY browser/hqnode/browser.conf.example browser/hqnode/desktop.ini.example /etc/rd-browser/
RUN set -eux; ldconfig; mkdir -p /etc/X11/rd-xorg.conf.d; chmod 0755 /usr/lib/rd-browser/*; \
    systemctl enable rd-xorg.service rd-video.service rd-shell.service remodroid-desktopd.service

# Inherited from the system image, restated because a wrong one turns every stop into 30 s and a kill.
STOPSIGNAL SIGRTMIN+3
CMD ["/sbin/init"]
