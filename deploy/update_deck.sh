#!/bin/bash
# Install or update RemoDroid on the Steam Deck, from another computer on the same network.
#
#   ./update_deck.sh                   the latest release
#   ./update_deck.sh 0.6.2             that release
#   ./update_deck.sh x.AppImage        an AppImage you already have (a CI build, say)
#   ./update_deck.sh --force           reinstall even when the Deck has this version
#   DECK=deck@steamdeck ./update_deck.sh   another address (default deck@192.168.10.52)
#
# Needs key login to the Deck (ssh-copy-id deck@…). Where a proxy's TUN device swallows the LAN, it
# retries the ssh from PID 1's network namespace (nsenter, as root). The AppImage is downloaded here and
# copied over, so the Deck needs no GitHub access. On the Deck: ~/Applications/RemoDroid-<ver>-linux-
# x86_64.AppImage, the RemoDroid.AppImage link pointed at it (Desktop mode's autostart starts that
# link), and the agent restarted as the user unit remodroid-desktop in the mode the Deck is in, Game
# or Desktop. The build it replaces stays for going back; older ones are removed. Pairing and settings
# (~/.config/remodroid) stay.
set -euo pipefail

REPO=yinyue123/RemoDroid
DECK=${DECK:-deck@192.168.10.52}

say() { printf '\033[1m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[33m!!\033[0m %s\n' "$*" >&2; }
die() { printf '\033[31mxx\033[0m %s\n' "$*" >&2; exit 1; }

# ---------------- on the Deck: put the AppImage in place and restart the agent ----------------
# (this script, sent over ssh as `bash -s -- --install NAME`; NAME is already in ~/Applications)
install_here() {
    local name=$1 dir="$HOME/Applications" link="$HOME/Applications/RemoDroid.AppImage" log="$HOME/.config/remodroid/desktop.log"
    [ -f "$dir/$name" ] || die "$dir/$name is missing"
    chmod +x "$dir/$name"
    local prev=""
    [ -L "$link" ] && prev=$(readlink "$link")

    # what the running agent was started with: its display and desktop (Game mode: gamescope)
    local pid display="" desktop="" session=""
    pid=$(pgrep -u "$(id -u)" -f '/usr/bin/remodroid-desktop( |$)' | head -n 1 || true)
    if [ -n "$pid" ] && [ -r "/proc/$pid/environ" ]; then
        display=$(tr '\0' '\n' <"/proc/$pid/environ" | sed -n 's/^DISPLAY=//p')
        desktop=$(tr '\0' '\n' <"/proc/$pid/environ" | sed -n 's/^XDG_CURRENT_DESKTOP=//p')
        session=$(tr '\0' '\n' <"/proc/$pid/environ" | sed -n 's/^XDG_SESSION_TYPE=//p')
    fi
    if [ -z "$desktop" ]; then
        if pgrep -x plasmashell >/dev/null; then desktop=KDE; else desktop=gamescope; fi
    fi
    if [ -z "$display" ]; then
        local p
        p=$(pgrep -u "$(id -u)" -x plasmashell | head -n 1 || true)
        [ -n "$p" ] && display=$(tr '\0' '\n' <"/proc/$p/environ" | sed -n 's/^DISPLAY=//p')
    fi
    display=${display:-:0}
    session=${session:-x11}

    say "Stopping the agent"
    systemctl --user stop remodroid-desktop.service 2>/dev/null || true
    systemctl --user reset-failed remodroid-desktop.service 2>/dev/null || true
    # one started by Desktop mode's autostart rather than the unit
    pkill -u "$(id -u)" -f '/usr/bin/remodroid-desktop( |$)' 2>/dev/null || true
    pkill -u "$(id -u)" -f "^$link( |\$)" 2>/dev/null || true
    sleep 1

    ln -sfn "$name" "$link"
    local lines=0
    [ -f "$log" ] && lines=$(wc -l <"$log")
    say "Starting RemoDroid ($name; $desktop, DISPLAY=$display)"
    systemd-run --user --quiet --unit remodroid-desktop -E LANG="${LANG:-en_US.UTF-8}" -E XDG_CURRENT_DESKTOP="$desktop" \
        -E XDG_SESSION_TYPE="$session" -E DISPLAY="$display" "$link" --background

    local ok=0
    for _ in $(seq 1 20); do
        sleep 1
        if [ -f "$log" ] && tail -n +"$((lines + 1))" "$log" | grep "connected to the server" >/dev/null; then ok=1; break; fi
        systemctl --user is-active --quiet remodroid-desktop.service || break
    done
    [ -f "$log" ] && tail -n +"$((lines + 1))" "$log" | sed 's/^/    /' | tail -n 8
    if [ "$ok" = 1 ]; then
        say "RemoDroid is running and connected"
    elif systemctl --user is-active --quiet remodroid-desktop.service; then
        warn "running, but not connected to the server yet: see $log"
    else
        warn "it didn't stay up: journalctl --user -u remodroid-desktop; going back to $prev"
        if [ -n "$prev" ] && [ -f "$dir/$prev" ]; then
            ln -sfn "$prev" "$link"
            systemd-run --user --quiet --unit remodroid-desktop -E LANG="${LANG:-en_US.UTF-8}" -E XDG_CURRENT_DESKTOP="$desktop" \
                -E XDG_SESSION_TYPE="$session" -E DISPLAY="$display" "$link" --background
        fi
        exit 1
    fi

    # keep this build and the one it replaced
    local f
    for f in "$dir"/RemoDroid-*-linux-*.AppImage; do
        [ -f "$f" ] || continue
        case "$(basename "$f")" in "$name" | "$prev") ;; *) rm -f "$f" ;; esac
    done
}

if [ "${1:-}" = "--install" ]; then
    install_here "$2"
    exit 0
fi

# ---------------- here: which build ----------------
force=0 want=""
for a in "$@"; do
    case "$a" in
    -f | --force) force=1 ;;
    -h | --help) sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) want="$a" ;;
    esac
done

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

file="" ver=""
if [ -n "$want" ] && [ -f "$want" ]; then
    file="$want"
    name=$(basename "$file")
    ver=$(printf '%s' "$name" | sed -n 's/^RemoDroid-\(Computer-\)\{0,1\}\(.*\)-linux-x86_64\.AppImage$/\2/p')
    [ -n "$ver" ] || die "$name: expected RemoDroid-…-linux-x86_64.AppImage"
else
    if [ -n "$want" ]; then
        ver="${want#v}"
    else
        say "Looking up the latest release"
        url=$(curl -fsSL -o /dev/null -w '%{url_effective}' "https://github.com/$REPO/releases/latest") ||
            die "can't reach github.com"
        ver="${url##*/tag/v}"
        [ "$ver" != "$url" ] && [ -n "$ver" ] || die "no release found at $url"
    fi
fi
target="RemoDroid-$ver-linux-x86_64.AppImage"

# ---------------- the Deck over ssh ----------------
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=8)
if ! "${SSH[@]}" "$DECK" true 2>/dev/null; then
    if [ "$(id -u)" = 0 ] && command -v nsenter >/dev/null && nsenter -t 1 -n "${SSH[@]}" "$DECK" true 2>/dev/null; then
        SSH=(nsenter -t 1 -n "${SSH[@]}")
    else
        die "can't log in to $DECK with a key (ssh-copy-id $DECK, or set DECK=user@address)"
    fi
fi

installed=$("${SSH[@]}" "$DECK" 'readlink ~/Applications/RemoDroid.AppImage 2>/dev/null || true')
say "Deck has: ${installed:-nothing}   wanted: $target"
if [ "$installed" = "$target" ] && [ "$force" = 0 ]; then
    if "${SSH[@]}" "$DECK" 'systemctl --user is-active --quiet remodroid-desktop.service || pgrep -f "/usr/bin/remodroid-desktop( |$)" >/dev/null'; then
        say "The Deck already runs $target (--force reinstalls)"
        exit 0
    fi
    say "$target is installed but not running: starting it"
    "${SSH[@]}" "$DECK" bash -s -- --install "$target" <"$0"
    exit 0
fi

if [ -z "$file" ]; then
    asset="RemoDroid-Computer-$ver-linux-x86_64.AppImage"
    say "Downloading $asset"
    curl -fL --retry 3 --progress-bar -o "$tmp/$asset" "https://github.com/$REPO/releases/download/v$ver/$asset" ||
        die "download failed: https://github.com/$REPO/releases/download/v$ver/$asset"
    file="$tmp/$asset"
fi
head -c 4 "$file" | grep -q ELF || die "$file isn't an AppImage"

say "Copying to the Deck"
"${SSH[@]}" "$DECK" "mkdir -p ~/Applications && cat > ~/Applications/$target.part && mv ~/Applications/$target.part ~/Applications/$target" <"$file"
"${SSH[@]}" "$DECK" bash -s -- --install "$target" <"$0"
