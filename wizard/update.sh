#!/bin/sh
# Updates whatever RemoDroid has on this machine (server, agent, or both) to the latest release.
#
#   curl -fsSL https://remodroid.pages.dev/update.sh | sudo sh       (cloud server, Debian, Ubuntu, CentOS)
#   wget -qO- https://remodroid.pages.dev/update.sh | sh             (OpenWrt, Alpine)
#
# Runs install_server.sh / install_agent.sh for each part that is installed; they keep
# server.ini / agent.ini and restart the service. REMODROID_VERSION=0.2 for a specific release.
set -eu
SITE=${REMODROID_SITE:-https://remodroid.pages.dev}

say() { printf '\033[38;5;173m▸\033[0m %s\n' "$*"; }
ok()  { printf '\033[32m✓\033[0m %s\n' "$*"; }
die() { printf '\033[31m✗\033[0m %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || die "run as root (… | sudo sh)"
fetch() { # url
    if command -v curl >/dev/null 2>&1; then curl -fsSL "$1"
    elif command -v wget >/dev/null 2>&1; then wget -qO- "$1"
    else die "needs curl or wget"; fi
}

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

update() { # server|agent
    ini=/etc/remodroid/$1.ini
    # the packages keep an edited ini, but a lost login or domain is too costly to trust that
    [ -f "$ini" ] && cp -p "$ini" "$TMP/$1.ini"
    say "updating the $1"
    fetch "$SITE/install_$1.sh" > "$TMP/install_$1.sh" || die "could not download $SITE/install_$1.sh"
    sh "$TMP/install_$1.sh"
    if [ -f "$TMP/$1.ini" ] && ! cmp -s "$TMP/$1.ini" "$ini"; then
        [ -f "$ini" ] && cp -p "$ini" "$ini.new"
        cp -p "$TMP/$1.ini" "$ini"
        say "put your $ini back (the package's copy is $ini.new); restarting with it"
        if [ -d /run/systemd/system ]; then systemctl restart "remodroid-$1"
        elif [ -x "/etc/init.d/remodroid-$1" ] && [ -f /etc/rc.common ]; then "/etc/init.d/remodroid-$1" restart
        elif command -v rc-service >/dev/null 2>&1; then rc-service "remodroid-$1" restart
        else die "restart remodroid-$1 by hand so it reads $ini"; fi
    fi
    echo
}

FOUND=""
[ -x /usr/bin/remodroid-server ] && { FOUND=1; update server; }
[ -x /usr/bin/remodroid-agent ] && { FOUND=1; update agent; }
[ -n "$FOUND" ] || die "RemoDroid isn't installed here. Install it with $SITE/install_server.sh or install_agent.sh"
ok "RemoDroid is up to date."
