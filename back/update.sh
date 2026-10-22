#!/bin/sh
# Test machines: update RemoDroid on a relay server and on this computer from one release.
#
#   back/update.sh                       sz's server, then this computer's desktop agent
#   back/update.sh root@host -p 36000    another server (ssh options after the address)
#   REMODROID_VERSION=0.4.4 back/update.sh   a specific release (default: latest)
#   SKIP_SERVER=1 back/update.sh         only this computer
#
# Run as root. Everything runs in PID 1's namespaces: a sandboxed shell can have its own
# network (e.g. through a proxy tunnel), and an agent started from there would reach the
# server through it. The server goes through back/push-update.sh. Here, the AppImage lands in
# the desktop user's ~/Applications, RemoDroid.AppImage points at it (what the autostart entry
# runs), and the agent restarts as that user with the desktop session's display and bus.
set -eu
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

say() { printf '\033[38;5;173m▸\033[0m %s\n' "$*"; }
ok()  { printf '\033[32m✓\033[0m %s\n' "$*"; }
die() { printf '\033[31m✗\033[0m %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || die "run as root"
if [ "$(readlink /proc/self/ns/net)" != "$(readlink /proc/1/ns/net)" ] ||
   [ "$(readlink /proc/self/ns/mnt)" != "$(readlink /proc/1/ns/mnt)" ]; then
    say "moving into PID 1's namespaces"
    exec nsenter -t 1 -n -m -- "$(readlink -f "$0")" "$@"
fi

REPO=yinyue123/RemoDroid
VERSION=${REMODROID_VERSION:-latest}
HERE=$(dirname "$(readlink -f "$0")")
[ $# -gt 0 ] || set -- sz

# ---------------- the server ----------------
if [ -z "${SKIP_SERVER:-}" ]; then
    "$HERE/push-update.sh" "$@"
fi

# ---------------- this computer ----------------
NET=$(readlink /proc/1/ns/net)
OLD=$(pgrep -o -f 'remodroid-desktop' || true)
# the desktop session: a process with a display, of the agent's user when one runs, in the
# host's network (an agent started from a sandbox may be running elsewhere)
WANT_UID=""
[ -n "$OLD" ] && WANT_UID=$(stat -c %u "/proc/$OLD")
SESSION=""
for d in /proc/[0-9]*; do
    u=$(stat -c %u "$d" 2>/dev/null) || continue
    if [ -n "$WANT_UID" ]; then [ "$u" = "$WANT_UID" ] || continue
    else [ "$u" -ge 1000 ] && [ "$u" -lt 60000 ] || continue; fi
    [ "$(readlink "$d/ns/net" 2>/dev/null)" = "$NET" ] || continue
    case $(cat "$d/comm" 2>/dev/null) in remodroid-deskt*) continue ;; esac
    tr '\0' '\n' < "$d/environ" 2>/dev/null | grep -qE '^(DISPLAY|WAYLAND_DISPLAY)=' || continue
    SESSION=${d#/proc/}
    break
done
[ -n "$SESSION" ] || die "no desktop session found (log in on this computer first)"
U=$(stat -c %u "/proc/$SESSION")
USER_NAME=$(getent passwd "$U" | cut -d: -f1)
USER_HOME=$(getent passwd "$U" | cut -d: -f6)
say "desktop session of $USER_NAME (pid $SESSION)"

case $(uname -m) in
    x86_64|amd64) ARCH=x86_64 ;;
    aarch64|arm64) ARCH=aarch64 ;;
    *) die "no AppImage for $(uname -m)" ;;
esac
if [ "$VERSION" = latest ]; then API=https://api.github.com/repos/$REPO/releases/latest
else API=https://api.github.com/repos/$REPO/releases/tags/v$VERSION; fi
URL=$(curl -fsSL "$API" | sed -n 's/.*"browser_download_url": *"\([^"]*\)".*/\1/p' |
      grep -E "/RemoDroid-[^/]*-linux-$ARCH\.AppImage$" | head -n1)
[ -n "$URL" ] || die "no $ARCH AppImage in the release at $API"
F=$(basename "$URL")
DIR=$USER_HOME/Applications
install -d -o "$U" -g "$(id -g "$U")" "$DIR"
say "downloading $F"
curl -fsSL -o "$DIR/$F.part" "$URL" || die "could not download $URL"
chown "$U:$(id -g "$U")" "$DIR/$F.part"
chmod 0755 "$DIR/$F.part"
mv -f "$DIR/$F.part" "$DIR/$F"
ln -sfn "$F" "$DIR/RemoDroid.AppImage"
chown -h "$U:$(id -g "$U")" "$DIR/RemoDroid.AppImage"

if pgrep -u "$U" -f remodroid-desktop >/dev/null; then
    say "stopping the running agent"
    pkill -TERM -u "$U" -f remodroid-desktop || true
    i=0
    while pgrep -u "$U" -f remodroid-desktop >/dev/null && [ $i -lt 20 ]; do sleep 0.5; i=$((i + 1)); done
    pkill -KILL -u "$U" -f remodroid-desktop 2>/dev/null || true
fi

say "starting $F"
LOG=$USER_HOME/.config/remodroid/desktop.log
LINES=$(cat "$LOG" 2>/dev/null | wc -l)
set --
for v in $(tr '\0' '\n' < "/proc/$SESSION/environ" |
           grep -E '^(DISPLAY|WAYLAND_DISPLAY|XAUTHORITY|XDG_RUNTIME_DIR|DBUS_SESSION_BUS_ADDRESS|XDG_SESSION_TYPE|XDG_CURRENT_DESKTOP|LANG)='); do
    set -- "$@" "$v"
done
nsenter -t "$SESSION" -n -m -- runuser -u "$USER_NAME" -- env -i HOME="$USER_HOME" USER="$USER_NAME" \
    LOGNAME="$USER_NAME" PATH=/usr/local/bin:/usr/bin:/bin "$@" \
    setsid -f "$DIR/RemoDroid.AppImage" --background </dev/null >/dev/null 2>&1

i=0
while [ $i -lt 30 ]; do
    sleep 0.5; i=$((i + 1))
    tail -n +"$((LINES + 1))" "$LOG" 2>/dev/null | grep -q "connected to the server" && break
done
NEW=$(pgrep -n -u "$U" -f 'remodroid-desktop --background' || true)
[ -n "$NEW" ] || die "the agent didn't start; see $LOG"
[ "$(readlink "/proc/$NEW/ns/net")" = "$NET" ] || die "the agent (pid $NEW) is not in the host's network namespace"
tail -n +"$((LINES + 1))" "$LOG" | grep -q "connected to the server" ||
    die "the agent (pid $NEW) runs but hasn't reached the server; see $LOG"
ok "the desktop agent is now $F (pid $NEW), connected to the server"
