#!/bin/bash
# Install or update RemoDroid on this Linux computer, and set up what it needs.
#
#   ./update_linux.sh                 the latest release
#   ./update_linux.sh 0.6.2           that release
#   ./update_linux.sh file            a .deb, .rpm or .AppImage you already have
#   ./update_linux.sh --appimage      the AppImage (~/Applications) even where a package would do
#   ./update_linux.sh --force         reinstall even when this version is installed
#
# Run it as yourself, in your desktop session (sudo is asked for when needed). It installs the .deb
# (apt) or .rpm (dnf, zypper, yum), or else the AppImage, as ~/Applications/RemoDroid.AppImage, a link
# to the versioned file. An install that is already an AppImage stays one. Then:
#   - /dev/uinput for the virtual gamepads: a udev rule giving the person at the computer access
#     (what Steam's steam-devices rules do), and the uinput module loaded at boot
#   - RemoDroid restarted the way it ran: the remodroid-desktopd user service, or the tray app
# Pairing and settings (~/.config/remodroid) stay. RemoDroid captures X11 sessions; on Wayland, pick
# an X11 ("Xorg") session at the login screen.
set -euo pipefail

REPO=yinyue123/RemoDroid

say() { printf '\033[1m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[33m!!\033[0m %s\n' "$*" >&2; }
die() { printf '\033[31mxx\033[0m %s\n' "$*" >&2; exit 1; }

[ "$(uname -s)" = Linux ] || die "this is for Linux"
[ "$(id -u)" != 0 ] || die "run it as yourself, not root: it asks for sudo when it needs it"

force=0 want="" mode=""
for a in "$@"; do
    case "$a" in
    -f | --force) force=1 ;;
    --appimage) mode=appimage ;;
    -h | --help) sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) want="$a" ;;
    esac
done

case "$(uname -m)" in
x86_64 | amd64) arch=x86_64 deb=amd64 rpm=x86_64 ;;
aarch64 | arm64) arch=aarch64 deb=arm64 rpm=aarch64 ;;
*) die "no RemoDroid build for $(uname -m)" ;;
esac

APPDIR="$HOME/Applications"
LINK="$APPDIR/RemoDroid.AppImage"
SUDO=sudo

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# ---------------- how it's installed now, and how it will be ----------------
have_pkg=""
if command -v dpkg-query >/dev/null && dpkg-query -W -f='${Status}' remodroid-desktop 2>/dev/null | grep "install ok installed" >/dev/null; then
    have_pkg=deb
elif command -v rpm >/dev/null && rpm -q remodroid-desktop >/dev/null 2>&1; then
    have_pkg=rpm
fi
installed=""
case "$have_pkg" in
deb) installed=$(dpkg-query -W -f='${Version}' remodroid-desktop | sed 's/-[0-9]*$//') ;;
rpm) installed=$(rpm -q --qf '%{VERSION}' remodroid-desktop) ;;
esac
if [ -z "$installed" ] && [ -L "$LINK" ]; then
    installed=$(readlink "$LINK" | sed -n 's/^RemoDroid-\(.*\)-linux-[a-z0-9_]*\.AppImage$/\1/p')
    [ -n "$mode" ] || mode=appimage
fi

file="" ver=""
if [ -n "$want" ] && [ -f "$want" ]; then
    file=$(cd "$(dirname "$want")" && pwd)/$(basename "$want")
    case "$file" in
    *.deb) mode=deb ;;
    *.rpm) mode=rpm ;;
    *.AppImage) mode=appimage ;;
    *) die "$want: expected a .deb, .rpm or .AppImage" ;;
    esac
    ver=$(basename "$file" | sed -n -e 's/^remodroid-desktop_\(.*\)-[0-9]*_[a-z0-9]*\.deb$/\1/p' \
        -e 's/^remodroid-desktop-\(.*\)-[0-9]*\.[a-z0-9_]*\.rpm$/\1/p' \
        -e 's/^RemoDroid-\(Computer-\)\{0,1\}\(.*\)-linux-[a-z0-9_]*\.AppImage$/\2/p')
else
    if [ -n "$want" ]; then
        ver="${want#v}"
    else
        say "Looking up the latest release"
        url=$(curl -fsSL -o /dev/null -w '%{url_effective}' "https://github.com/$REPO/releases/latest") ||
            die "can't reach github.com (set https_proxy if you need a proxy)"
        ver="${url##*/tag/v}"
        [ "$ver" != "$url" ] && [ -n "$ver" ] || die "no release found at $url"
    fi
fi

if [ -z "$mode" ]; then
    if command -v apt-get >/dev/null && command -v dpkg >/dev/null; then mode=deb
    elif command -v dnf >/dev/null || command -v zypper >/dev/null || command -v yum >/dev/null; then mode=rpm
    else mode=appimage
    fi
fi
say "Installed: ${installed:-none}   wanted: ${ver:-?} ($mode)"

# ---------------- what was running, to start it again ----------------
unit_on=0 app_on=0 app_args="--background"
systemctl --user is-active --quiet remodroid-desktopd.service 2>/dev/null && unit_on=1
pid=$(pgrep -u "$(id -u)" -f '(^|/)remodroid-desktop( |$)' | head -n 1 || true)
if [ -n "$pid" ]; then
    app_on=1
fi

# ---------------- install ----------------
if [ -n "$file" ] || [ "$installed" != "$ver" ] || [ "$force" = 1 ]; then
    if [ -z "$file" ]; then
        case "$mode" in
        deb) asset="remodroid-desktop_${ver}-1_${deb}.deb" ;;
        rpm) asset="remodroid-desktop-${ver}-1.${rpm}.rpm" ;;
        appimage) asset="RemoDroid-Computer-${ver}-linux-${arch}.AppImage" ;;
        esac
        say "Downloading $asset"
        curl -fL --retry 3 --progress-bar -o "$tmp/$asset" "https://github.com/$REPO/releases/download/v$ver/$asset" ||
            die "download failed: https://github.com/$REPO/releases/download/v$ver/$asset"
        file="$tmp/$asset"
    fi

    if [ "$app_on" = 1 ] || [ "$unit_on" = 1 ]; then
        say "Stopping RemoDroid"
        [ "$unit_on" = 1 ] && systemctl --user stop remodroid-desktopd.service || true
        pkill -u "$(id -u)" -f '(^|/)remodroid-desktop( |$)' 2>/dev/null || true
        pkill -u "$(id -u)" -f "^$LINK( |\$)" 2>/dev/null || true
        sleep 1
    fi

    case "$mode" in
    deb)
        say "Installing the package (sudo)"
        cp "$file" "$tmp/remodroid-desktop.deb" # apt reads it as the _apt user: not from a private home
        chmod 644 "$tmp/remodroid-desktop.deb"
        chmod 755 "$tmp"
        $SUDO apt-get install -y --allow-downgrades "$tmp/remodroid-desktop.deb"
        ;;
    rpm)
        say "Installing the package (sudo)"
        if command -v dnf >/dev/null; then $SUDO dnf install -y "$file"
        elif command -v zypper >/dev/null; then $SUDO zypper --non-interactive install --allow-unsigned-rpm "$file"
        else $SUDO yum install -y "$file"
        fi
        ;;
    appimage)
        name="RemoDroid-${ver}-linux-${arch}.AppImage"
        mkdir -p "$APPDIR"
        prev=""
        [ -L "$LINK" ] && prev=$(readlink "$LINK")
        install -m 755 "$file" "$APPDIR/$name.part"
        mv "$APPDIR/$name.part" "$APPDIR/$name"
        ln -sfn "$name" "$LINK"
        # keep this build and the one it replaced
        for f in "$APPDIR"/RemoDroid-*-linux-*.AppImage; do
            [ -f "$f" ] || continue
            case "$(basename "$f")" in "$name" | "$prev") ;; *) rm -f "$f" ;; esac
        done
        # a menu entry, as the packages have
        mkdir -p "$HOME/.local/share/applications"
        cat >"$HOME/.local/share/applications/remodroid-desktop.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=RemoDroid
GenericName=Remote desktop
Comment=Control this computer from your RemoDroid panel
Exec="$LINK"
Icon=remodroid
Terminal=false
Categories=Network;RemoteAccess;
StartupNotify=false
EOF
        ;;
    esac
else
    say "RemoDroid $ver is already installed (--force reinstalls)"
fi

# ---------------- /dev/uinput, for the virtual gamepads ----------------
RULE=/etc/udev/rules.d/60-remodroid-uinput.rules
if [ ! -w /dev/uinput ]; then
    say "Letting RemoDroid make virtual gamepads (/dev/uinput; sudo)"
    printf '%s\n' '# RemoDroid: virtual gamepads for the person at this computer (as Steam'"'"'s steam-devices rules)' \
        'KERNEL=="uinput", SUBSYSTEM=="misc", TAG+="uaccess", OPTIONS+="static_node=uinput"' |
        $SUDO tee "$RULE" >/dev/null
    printf 'uinput\n' | $SUDO tee /etc/modules-load.d/remodroid-uinput.conf >/dev/null
    $SUDO modprobe uinput 2>/dev/null || true
    $SUDO udevadm control --reload-rules 2>/dev/null || true
    $SUDO udevadm trigger --name-match=uinput 2>/dev/null || true
    sleep 1
    if [ -w /dev/uinput ]; then
        say "Virtual gamepads: ready"
    else
        warn "/dev/uinput is still not writable: log out and in again (gamepads only; the rest works)"
    fi
fi

# ---------------- start it again ----------------
case "${XDG_SESSION_TYPE:-}" in
wayland) warn "this is a Wayland session: RemoDroid captures X11 sessions; pick an Xorg session at the login screen" ;;
esac

if [ "$mode" = appimage ]; then bin="$LINK"; else bin=$(command -v remodroid-desktop || echo /usr/bin/remodroid-desktop); fi
if [ "$unit_on" = 1 ]; then
    say "Restarting the remodroid-desktopd service"
    systemctl --user daemon-reload
    systemctl --user restart remodroid-desktopd.service
elif [ -n "${DISPLAY:-}" ]; then
    say "Starting RemoDroid"
    [ "$app_on" = 1 ] || app_args="" # first start: show the window, for pairing
    setsid -f "$bin" $app_args >/dev/null 2>&1 </dev/null
else
    say "Installed. Start RemoDroid from your desktop's menu (no DISPLAY here)"
fi

log="$HOME/.config/remodroid/desktop.log"
for _ in $(seq 1 15); do
    sleep 1
    [ -f "$log" ] && tail -n 20 "$log" | grep "connected to the server" >/dev/null && break
done
[ -f "$log" ] && tail -n 5 "$log" | sed 's/^/    /'
say "Done: RemoDroid ${ver:-} ($mode)"
