#!/bin/sh
# Test machines: update RemoDroid (server, agent, or both) on another machine over SSH. This
# computer downloads the release package and copies it there, so the machine itself needs no
# access to GitHub (e.g. a server in mainland China).
#
#   back/push-update.sh root@47.107.89.35 -p 36000           ssh options after the address
#   REMODROID_VERSION=0.3.4 back/push-update.sh root@host    a specific release (default: latest)
#
# Log in as root. deb, rpm, apk and OpenWrt ipk; the edited server.ini / agent.ini is kept.
set -eu
[ $# -gt 0 ] || { echo "usage: $0 root@host [ssh options]" >&2; exit 2; }
REPO=yinyue123/RemoDroid
VERSION=${REMODROID_VERSION:-latest}

say() { printf '\033[38;5;173m▸\033[0m %s\n' "$*"; }
ok()  { printf '\033[32m✓\033[0m %s\n' "$*"; }
die() { printf '\033[31m✗\033[0m %s\n' "$*" >&2; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
# ssh runs commands with a short PATH: Alpine's apk and rc-service live in /sbin
P='export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:$PATH;'

say "looking at $1"
INFO=$(ssh "$@" "$P sh -s" <<'EOF'
[ "$(id -u)" = 0 ] || { echo notroot; exit 0; }
k=tar
if command -v opkg >/dev/null 2>&1; then k=ipk
elif command -v apt-get >/dev/null 2>&1; then k=deb
elif command -v dnf >/dev/null 2>&1 || command -v yum >/dev/null 2>&1; then k=rpm
elif command -v apk >/dev/null 2>&1; then k=apk; fi
p=""; for x in server agent; do [ -x /usr/bin/remodroid-$x ] && p="$p $x"; done
a=""; [ $k = ipk ] && a=$(opkg print-architecture | awk '{print $3, $2}' | sort -rn | awk '{print $2}' | tr '\n' ' ')
echo "$(uname -m)|$k|$p|$a"
EOF
) || die "could not reach $1 over ssh"
[ "$INFO" != notroot ] || die "log in to $1 as root"
M=$(echo "$INFO" | cut -d'|' -f1)
K=$(echo "$INFO" | cut -d'|' -f2)
PARTS=$(echo "$INFO" | cut -d'|' -f3)
ARCHS=$(echo "$INFO" | cut -d'|' -f4)
[ -n "$PARTS" ] || die "RemoDroid isn't installed on $1 yet"
case $M in
    x86_64|amd64) DEB=amd64 RPM=x86_64 APK=x86_64 ;;
    aarch64|arm64) DEB=arm64 RPM=aarch64 APK=aarch64 ;;
    armv7*|armv8l) DEB=armhf RPM=armv7hl APK=armv7 ;;
    i?86) DEB=i386 RPM=i686 APK=x86 ;;
    *) DEB=none RPM=none APK=none ;;
esac

if [ "$VERSION" = latest ]; then API=https://api.github.com/repos/$REPO/releases/latest
else API=https://api.github.com/repos/$REPO/releases/tags/v$VERSION; fi
URLS=$(curl -fsSL "$API" | sed -n 's/.*"browser_download_url": *"\([^"]*\)".*/\1/p')
[ -n "$URLS" ] || die "no release found at $API"
pick() { echo "$URLS" | grep -E "$1" | head -n1; }

for part in $PARTS; do
    URL=""
    case $K in
        ipk) [ "$part" = agent ] && for a in $ARCHS; do URL=$(pick "/remodroid-agent_[^/]*_${a}\.ipk$"); [ -n "$URL" ] && break; done ;;
        deb) URL=$(pick "/remodroid-${part}_[^/]*_${DEB}\.deb$") ;;
        rpm) URL=$(pick "/remodroid-${part}-[^/]*\.${RPM}\.rpm$") ;;
        apk) URL=$(pick "/remodroid-${part}_[^/]*_${APK}\.apk$") ;;
    esac
    [ -n "$URL" ] || die "no $K package of the $part for $M in the release"
    F=$(basename "$URL")
    say "downloading $F"
    curl -fsSL -o "$TMP/$F" "$URL" || die "could not download $URL"
    say "copying it to $1 and updating the $part"
    ssh "$@" "cat > /tmp/$F" < "$TMP/$F" || die "could not copy $F to $1"
    ssh "$@" "$P PART=$part KIND=$K F=/tmp/$F sh -s" <<'EOF' || die "updating the $part on $1 failed"
set -e
ini=/etc/remodroid/$PART.ini
[ -f "$ini" ] && cp -p "$ini" "$F.ini"  # the packages keep an edited ini; a lost login is too costly to trust that
case $KIND in
    ipk) opkg install "$F" >/dev/null ;;
    deb) DEBIAN_FRONTEND=noninteractive apt-get install -y -q -o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold "$F" >/dev/null ;;
    rpm) if command -v dnf >/dev/null 2>&1; then dnf install -y -q "$F" >/dev/null; else yum install -y -q "$F" >/dev/null; fi ;;
    apk) apk add -q --allow-untrusted "$F" ;;
esac
if [ -f "$F.ini" ] && ! cmp -s "$F.ini" "$ini"; then
    [ -f "$ini" ] && cp -p "$ini" "$ini.new"
    cp -p "$F.ini" "$ini"
    echo "  put your $ini back (the package's copy is $ini.new)"
fi
if [ -d /run/systemd/system ]; then systemctl daemon-reload; systemctl restart "remodroid-$PART"
elif [ -x "/etc/init.d/remodroid-$PART" ] && [ -f /etc/rc.common ]; then "/etc/init.d/remodroid-$PART" restart
elif command -v rc-service >/dev/null 2>&1; then rc-service "remodroid-$PART" restart >/dev/null 2>&1 || rc-service "remodroid-$PART" start
else echo "  restart remodroid-$PART by hand"; fi
rm -f "$F" "$F.ini"
EOF
    ok "the $part on $1 is now $F"
done
