#!/bin/sh
# Installs and starts the RemoDroid server, then prints the link to its setup wizard.
#
#   curl -fsSL https://remodroid.pages.dev/install_server.sh | sudo sh
#
# Picks the package for this distro and CPU from the latest GitHub release
# (REMODROID_VERSION=0.2 for a specific one). Debian/Ubuntu, CentOS/RHEL/Fedora,
# Alpine, or any Linux through the plain tarball.
set -eu
REPO=yinyue123/RemoDroid
VERSION=${REMODROID_VERSION:-latest}

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
[ -n "$URLS" ] || die "no release found at $API"
pick() { echo "$URLS" | grep -E "$1" | head -n1; }

if command -v apt-get >/dev/null 2>&1 && [ -n "$DEB" ]; then KIND=deb URL=$(pick "/remodroid-server_[^/]*_${DEB}\.deb$")
elif { command -v dnf >/dev/null 2>&1 || command -v yum >/dev/null 2>&1; } && [ -n "$RPM" ]; then KIND=rpm URL=$(pick "/remodroid-server-[^/]*\.${RPM}\.rpm$")
elif command -v apk >/dev/null 2>&1 && [ -n "$APK" ]; then KIND=apk URL=$(pick "/remodroid-server_[^/]*_${APK}\.apk$")
else KIND=tar URL=$(pick "/remodroid-[0-9][^/]*-linux-${ARCH}\.tar\.gz$")
fi
[ -n "$URL" ] || die "no $KIND package for $ARCH in the release"

if [ -n "${REMODROID_DRY_RUN:-}" ]; then echo "$KIND $URL"; exit 0; fi  # testing: show the choice only
UPDATE=""
[ -x /usr/bin/remodroid-server ] && UPDATE=1  # already installed: this run updates it
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
FILE="$TMP/$(basename "$URL")"
say "downloading $(basename "$URL")"
fetch "$URL" "$FILE"

say "installing"
case $KIND in
    deb) DEBIAN_FRONTEND=noninteractive apt-get install -y -q -o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold "$FILE" >/dev/null ;;  # keep your edited ini
    rpm) if command -v dnf >/dev/null 2>&1; then dnf install -y -q "$FILE" >/dev/null; else yum install -y -q "$FILE" >/dev/null; fi ;;
    apk) apk add -q --allow-untrusted "$FILE" ;;
    tar)
        tar -C "$TMP" -xzf "$FILE"
        D=$(find "$TMP" -maxdepth 1 -type d -name 'remodroid-*' | head -n1)
        install -m 0755 "$D/bin/remodroid-server" /usr/bin/remodroid-server
        mkdir -p /usr/share/remodroid /etc/remodroid
        rm -rf /usr/share/remodroid/web && cp -r "$D/web" /usr/share/remodroid/web
        [ -f /etc/remodroid/server.ini ] || install -m 0640 "$D/etc/server.ini.example" /etc/remodroid/server.ini
        if [ -d /run/systemd/system ]; then
            install -m 0644 "$D/etc/remodroid-server.service" /etc/systemd/system/remodroid-server.service
            systemctl daemon-reload
        fi
        ;;
esac
if [ -n "$UPDATE" ]; then ok "updated to $(basename "$URL")"; else ok "installed $(basename "$URL")"; fi

# restart, not just start: on an update the old version is still running
say "${UPDATE:+re}starting the service"
if [ -d /run/systemd/system ]; then
    systemctl daemon-reload >/dev/null 2>&1 || true
    systemctl enable remodroid-server >/dev/null 2>&1 || true
    systemctl restart remodroid-server
elif command -v rc-service >/dev/null 2>&1; then
    rc-update add remodroid-server default >/dev/null 2>&1 || true
    rc-service remodroid-server restart >/dev/null 2>&1 || rc-service remodroid-server start
else
    pkill -x remodroid-serve 2>/dev/null && sleep 1  # the kernel keeps 15 characters of the name
    nohup /usr/bin/remodroid-server -c /etc/remodroid/server.ini >/var/log/remodroid-server.log 2>&1 &
fi

# where the wizard is: the port from server.ini, the address the internet sees
PORT=$(sed -n 's/^[[:space:]]*listen[[:space:]]*=[[:space:]]*[^;]*:\([0-9][0-9]*\).*/\1/p' /etc/remodroid/server.ini | head -n1)
PORT=${PORT:-5580}
IP=$(fetch https://cloudflare.com/cdn-cgi/trace 2>/dev/null | sed -n 's/^ip=//p' || true)
[ -n "$IP" ] || IP=$(hostname -I 2>/dev/null | cut -d' ' -f1)
[ -n "$IP" ] || IP=your-server-ip
case $IP in *:*) IP="[$IP]" ;; esac

i=0
while [ ! -s /var/lib/remodroid/setup-token ] && [ $i -lt 10 ] && ! grep -q '^[[:space:]]*[A-Za-z0-9._-]*[[:space:]]*=[[:space:]]*\$2' /etc/remodroid/server.ini 2>/dev/null; do
    sleep 1; i=$((i + 1))
done
echo
if [ -s /var/lib/remodroid/setup-token ]; then
    ok "the server is running. Open the setup wizard:"
    echo
    printf '    \033[1mhttp://%s:%s/wizard.html?token=%s\033[0m\n' "$IP" "$PORT" "$(cat /var/lib/remodroid/setup-token)"
    echo
    echo "  Open TCP $PORT in the firewall / security group first. The wizard sets your"
    echo "  domain (free HTTPS), login and STUN/TURN; then add agents on its Agents tab."
else
    ok "the server is running (already set up): http://$IP:$PORT/"
fi
