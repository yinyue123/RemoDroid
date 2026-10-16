#!/bin/sh
# Installs and starts the RemoDroid agent on the router the phone is plugged into.
#
#   wget -qO- https://remodroid.pages.dev/install_agent.sh | sh                    (OpenWrt, Alpine)
#   curl -fsSL https://remodroid.pages.dev/install_agent.sh | sudo sh              (Debian, Ubuntu, CentOS)
#   … | sh -s -- --panel https://your.panel --code XXXX-XXXX                       (and enroll right away)
#
# Picks the package for this system and CPU from the latest GitHub release
# (REMODROID_VERSION=0.2 for a specific one): OpenWrt ipk, deb, rpm, apk, or the tarball.
set -eu
REPO=yinyue123/RemoDroid
VERSION=${REMODROID_VERSION:-latest}
ENROLL=""
while [ $# -gt 0 ]; do
    case $1 in
        --panel|--code|--name|--ca-file) [ $# -ge 2 ] || { echo "$1 needs a value" >&2; exit 2; }; ENROLL="$ENROLL $1 $2"; shift 2 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done

say() { printf '\033[38;5;173m▸\033[0m %s\n' "$*"; }
ok()  { printf '\033[32m✓\033[0m %s\n' "$*"; }
die() { printf '\033[31m✗\033[0m %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || [ -n "${REMODROID_DRY_RUN:-}" ] || die "run as root (… | sudo sh)"
fetch() { # url [file]
    if command -v curl >/dev/null 2>&1; then
        if [ -n "${2:-}" ]; then curl -fsSL -o "$2" "$1"; else curl -fsSL "$1"; fi
    elif command -v wget >/dev/null 2>&1; then
        if [ -n "${2:-}" ]; then wget -qO "$2" "$1"; else wget -qO- "$1"; fi
    else
        die "needs curl or wget"
    fi
}

case "$(uname -m)" in
    x86_64|amd64) ARCH=x86_64 DEB=amd64 RPM=x86_64 APK=x86_64 ;;
    aarch64|arm64) ARCH=aarch64 DEB=arm64 RPM=aarch64 APK=aarch64 ;;
    armv7*|armv8l) ARCH=armv7 DEB=armhf RPM=armv7hl APK=armv7 ;;
    i?86) ARCH=x86 DEB=i386 RPM=i686 APK=x86 ;;
    mips) ARCH=mips DEB= RPM= APK= ;;
    mipsel) ARCH=mipsel DEB= RPM= APK= ;;
    *) die "unsupported CPU: $(uname -m)" ;;
esac

say "fetching the release list"
if [ "$VERSION" = latest ]; then API=https://api.github.com/repos/$REPO/releases/latest
else API=https://api.github.com/repos/$REPO/releases/tags/v$VERSION; fi
URLS=$(fetch "$API" | sed -n 's/.*"browser_download_url": *"\([^"]*\)".*/\1/p')
[ -n "$URLS" ] || die "no release found at $API (is the router's clock right? HTTPS needs it)"
pick() { echo "$URLS" | grep -E "$1" | head -n1; }

URL=""
if command -v opkg >/dev/null 2>&1; then
    KIND=ipk
    # the first architecture opkg accepts that has a package (most specific first)
    for a in $(opkg print-architecture | awk '{print $3, $2}' | sort -rn | awk '{print $2}'); do
        URL=$(pick "/remodroid-agent_[^/]*_${a}\.ipk$")
        [ -n "$URL" ] && break
    done
elif command -v apt-get >/dev/null 2>&1 && [ -n "$DEB" ]; then KIND=deb URL=$(pick "/remodroid-agent_[^/]*_${DEB}\.deb$")
elif { command -v dnf >/dev/null 2>&1 || command -v yum >/dev/null 2>&1; } && [ -n "$RPM" ]; then KIND=rpm URL=$(pick "/remodroid-agent-[^/]*\.${RPM}\.rpm$")
elif command -v apk >/dev/null 2>&1 && [ -n "$APK" ]; then KIND=apk URL=$(pick "/remodroid-agent_[^/]*_${APK}\.apk$")
fi
if [ -z "$URL" ]; then KIND=tar URL=$(pick "/remodroid-[0-9][^/]*-linux-${ARCH}\.tar\.gz$"); fi
[ -n "$URL" ] || die "no package for this system ($ARCH) in the release"

if [ -n "${REMODROID_DRY_RUN:-}" ]; then echo "$KIND $URL"; exit 0; fi  # testing: show the choice only
UPDATE=""
[ -x /usr/bin/remodroid-agent ] && UPDATE=1  # already installed: this run updates it
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
FILE="$TMP/$(basename "$URL")"
say "downloading $(basename "$URL")"
fetch "$URL" "$FILE"

say "installing"
case $KIND in
    ipk) opkg update >/dev/null 2>&1 || true; opkg install "$FILE" >/dev/null ;;
    deb) DEBIAN_FRONTEND=noninteractive apt-get install -y -q -o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold "$FILE" >/dev/null ;;  # keep your edited ini
    rpm) if command -v dnf >/dev/null 2>&1; then dnf install -y -q "$FILE" >/dev/null; else yum install -y -q "$FILE" >/dev/null; fi ;;
    apk) apk add -q --allow-untrusted "$FILE" ;;
    tar)
        tar -C "$TMP" -xzf "$FILE"
        D=$(find "$TMP" -maxdepth 1 -type d -name 'remodroid-*' | head -n1)
        install -m 0755 "$D/bin/remodroid-agent" /usr/bin/remodroid-agent
        mkdir -p /usr/share/remodroid /etc/remodroid
        cp -r "$D/phone/." /usr/share/remodroid/
        [ -f /etc/remodroid/agent.ini ] || install -m 0600 "$D/etc/agent.ini.example" /etc/remodroid/agent.ini
        if [ -d /run/systemd/system ]; then
            install -m 0644 "$D/etc/remodroid-agent.service" /etc/systemd/system/remodroid-agent.service
            systemctl daemon-reload
        elif [ -f /etc/rc.common ]; then
            install -m 0755 "$D/etc/remodroid-agent.init" /etc/init.d/remodroid-agent
        fi
        ;;
esac
[ -e /usr/bin/remodroid ] || ln -s remodroid-agent /usr/bin/remodroid
if [ -n "$UPDATE" ]; then ok "updated to $(basename "$URL")"; else ok "installed $(basename "$URL")"; fi

# restart, not just start: on an update the old version is still running
say "${UPDATE:+re}starting the service"
if [ -d /run/systemd/system ]; then
    systemctl daemon-reload >/dev/null 2>&1 || true
    systemctl enable remodroid-agent >/dev/null 2>&1 || true
    systemctl restart remodroid-agent
elif [ -x /etc/init.d/remodroid-agent ] && [ -f /etc/rc.common ]; then
    /etc/init.d/remodroid-agent enable && /etc/init.d/remodroid-agent restart
elif command -v rc-service >/dev/null 2>&1; then
    rc-update add remodroid-agent default >/dev/null 2>&1 || true
    rc-service remodroid-agent restart >/dev/null 2>&1 || rc-service remodroid-agent start
else
    pkill -x remodroid-agent 2>/dev/null && sleep 1
    nohup /usr/bin/remodroid-agent -c /etc/remodroid/agent.ini >/var/log/remodroid-agent.log 2>&1 &
fi
sleep 1

echo
if [ -n "$ENROLL" ]; then
    # shellcheck disable=SC2086
    exec /usr/bin/remodroid-agent enroll $ENROLL
fi
if [ -n "$UPDATE" ] && grep -q '^[[:space:]]*device_secret[[:space:]]*=[[:space:]]*[^[:space:];]' /etc/remodroid/agent.ini 2>/dev/null; then
    ok "the agent is running the new version and reconnects to your panel by itself."
    exit 0
fi
ok "the agent is running. Connect it to your panel with the command from"
echo "  its Agents tab → Add, for example:"
echo
printf '    \033[1mremodroid enroll --panel https://your.panel --code XXXX-XXXX\033[0m\n'
echo
