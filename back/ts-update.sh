#!/bin/sh
# Test machines: update RemoDroid everywhere from one build, over Tailscale. It runs on a machine on
# the tailnet (.github/workflows/deploy.yml starts it there) and pushes the packages to:
#
#   server  a relay server in an hqnode container: remodroid-server and the viewer (web/)
#   sx      a Linux computer agent installed from the .deb
#   win11   a Windows VM on that machine (the zip, through the QEMU guest agent's gput.py / gexec.py)
#   deck    a Steam Deck (the AppImage)
#   pc      another AppImage computer
#
#   back/ts-update.sh                          the latest release, to server sx win11 deck
#   back/ts-update.sh server deck              only these
#   REMODROID_VERSION=0.5.8 back/ts-update.sh  one release
#   PKG_DIR=dir back/ts-update.sh              packages already here (a CI build's packages artifact)
#   back/ts-update.sh --stdin server           a tar of the packages (and ts-update.conf) on stdin
#
# Where the machines are isn't in the repo (it's public): it comes from ts-update.conf, NAME=value
# lines, next to this script or in the tar on stdin (deploy.yml writes it from the repository's
# secrets; it wins over the one here). The settings:
#
#   SERVER, SERVER_PORT, SERVER_CT   ssh to the server's host (user@host, port) and its container
#   JUMP                             user@host that reaches the home LAN (192.168.*): the others there go through it
#   SX, SX_USER                      ssh to the .deb computer, and its desktop's user
#   WIN_VM, WIN_USER                 the VM's libvirt name on SX, and the Windows user running RemoDroid
#   DECK                             ssh to the Steam Deck as its desktop's user
#   PC, PC_USER                      ssh to another AppImage computer, and its desktop's user (when not the login)
#
# This machine's key (KEY, default ~/.ssh/id_ed25519) has to be authorized on each of them. In
# ~/.ssh/authorized_keys here, deploy.yml's key runs only this, with the targets as the command:
#   command="/root/remodroid/ts-update.sh --ssh",no-pty,no-port-forwarding,no-agent-forwarding,no-X11-forwarding ssh-ed25519 …
set -eu
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

REPO=yinyue123/RemoDroid
VERSION=${REMODROID_VERSION:-latest}
KEY=${KEY:-$HOME/.ssh/id_ed25519}
SERVER="" SERVER_PORT=22 SERVER_CT="" JUMP="" SX="" SX_USER="" WIN_VM="" WIN_USER="" DECK="" PC="" PC_USER=""
TARGETS_DEFAULT="server sx win11 deck"
HERE=$(dirname "$(readlink -f "$0")")

say() { printf '\033[38;5;173m▸\033[0m %s\n' "$*"; }
ok()  { printf '\033[32m✓\033[0m %s\n' "$*"; }
bad() { printf '\033[31m✗\033[0m %s\n' "$*" >&2; }
die() { bad "$*"; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# NAME=value lines, only the settings above and only plain values: the one on stdin comes from
# whoever holds deploy.yml's key, and mustn't be able to run anything
conf() {
    while IFS= read -r l || [ -n "$l" ]; do
        case $l in ''|'#'*) continue ;; esac
        k=${l%%=*} v=${l#*=}
        case $k in SERVER|SERVER_PORT|SERVER_CT|JUMP|SX|SX_USER|WIN_VM|WIN_USER|DECK|PC|PC_USER) ;; *) die "$1: unknown setting $k" ;; esac
        case $v in *[!A-Za-z0-9@._-]*) die "$1: $k isn't a plain value" ;; esac
        eval "$k=\$v"
    done < "$1"
}
[ -f "$HERE/ts-update.conf" ] && conf "$HERE/ts-update.conf"

# ---------------- what to do, and the packages ----------------
FROM=""
case "${1:-}" in
    --stdin) FROM=stdin; shift ;;
    --ssh)   FROM=stdin; set -f; set -- ${SSH_ORIGINAL_COMMAND:-}; set +f ;;
esac
[ $# -gt 0 ] || set -- $TARGETS_DEFAULT
for t in "$@"; do
    case $t in server|sx|win11|deck|pc) ;; *) die "unknown target $t (server sx win11 deck pc)" ;; esac
done

PKG=$TMP/pkg
mkdir -p "$PKG"
if [ "$FROM" = stdin ]; then
    tar -x -C "$PKG" || die "no tar of packages on stdin"
    [ -f "$PKG/ts-update.conf" ] && { conf "$PKG/ts-update.conf"; rm -f "$PKG/ts-update.conf"; }
elif [ -n "${PKG_DIR:-}" ]; then
    PKG=$PKG_DIR
else
    if [ "$VERSION" = latest ]; then API=https://api.github.com/repos/$REPO/releases/latest
    else API=https://api.github.com/repos/$REPO/releases/tags/v$VERSION; fi
    say "the release at $API"
    curl -fsSL "$API" | sed -n 's/.*"browser_download_url": *"\([^"]*\)".*/\1/p' > "$TMP/urls" || die "could not read $API"
    for u in $(grep -E '/(remodroid-[0-9][^/]*-linux-x86_64\.tar\.gz|RemoDroid-[0-9][^/]*-linux-x86_64\.AppImage|remodroid-desktop_[^/]*_amd64\.deb|RemoDroid-[0-9][^/]*-windows-x86_64\.zip|SHA256SUMS)$' "$TMP/urls"); do
        say "downloading $(basename "$u")"
        curl -fsSL -o "$PKG/$(basename "$u")" "$u" || die "could not download $u"
    done
fi
if [ -f "$PKG/SHA256SUMS" ]; then
    (cd "$PKG" && for f in *; do
        [ "$f" = SHA256SUMS ] && continue
        want=$(awk -v n="$f" '$2 == n { print $1 }' SHA256SUMS)
        [ -z "$want" ] || [ "$(sha256sum "$f" | cut -d' ' -f1)" = "$want" ] || { echo "checksum mismatch: $f" >&2; exit 1; }
    done) || die "the packages don't match SHA256SUMS"
fi
pick() { # the package for a pattern, or nothing
    for f in "$PKG"/$1; do [ -f "$f" ] && { echo "$f"; return; }; done
}

# ---------------- reaching the machines ----------------
SSH_OPTS="-o BatchMode=yes -o ConnectTimeout=15 -o ServerAliveInterval=15 -o StrictHostKeyChecking=accept-new"
[ -f "$KEY" ] && SSH_OPTS="$SSH_OPTS -i $KEY"
# rsh [-p port] host command…: ssh to a machine; one on the home LAN (192.168.*) goes through JUMP.
# (Options before the host: Alpine's ssh doesn't look for them after it.)
rsh() {
    port=22
    [ "$1" = -p ] && { port=$2; shift 2; }
    h=$1; shift
    case $h in
        *@192.168.*) [ -n "$JUMP" ] || { bad "set JUMP in ts-update.conf to reach $h"; return 1; }
            ssh $SSH_OPTS -p "$port" -o ProxyCommand="ssh $SSH_OPTS -W %h:%p $JUMP" "$h" "$@" ;;
        *) ssh $SSH_OPTS -p "$port" "$h" "$@" ;;
    esac
}
# run [-p port] host script-text file args…: upload the file and the script (as a file, so nothing
# in it can read the rest of it from stdin), run the script with the uploaded file's path, clean up
run() {
    pp=""
    [ "$1" = -p ] && { pp="-p $2"; shift 2; }
    h=$1 sc=$2 f=$3; shift 3
    r=/tmp/rd-$(basename "$f")
    printf '%s\n' "$sc" > "$TMP/run.sh"
    rsh $pp "$h" "cat > '$r'" < "$f" &&
    rsh $pp "$h" "cat > /tmp/rd-run.sh" < "$TMP/run.sh" &&
    rsh $pp "$h" "sh /tmp/rd-run.sh '$r' $*; e=\$?; rm -f /tmp/rd-run.sh '$r'; exit \$e" < /dev/null
}

# ---------------- the scripts that run there ----------------
# The server, on its container's host as root: $1 the uploaded tarball, $2 the container. The new server is tested
# against the live server.ini before it replaces the old one; the old one and the viewer are kept
# (three backups of each).
SERVER_SH=$(cat <<'EOF'
set -eu
F=$1 CT=$2
P=$(pgrep -f '^/usr/bin/remodroid-server' || true)
[ -n "$P" ] && [ "$(echo "$P" | wc -l)" = 1 ] || { echo "not one remodroid-server on $(hostname): $P" >&2; exit 1; }
nsenter -t "$P" -U -m -- sh -c 'rm -rf /tmp/rd-up && mkdir -p /tmp/rd-up && tar -xzf - -C /tmp/rd-up' < "$F"
rm -f "$F"
hqnode exec "$CT" sh -c '
set -e
d=$(ls -d /tmp/rd-up/remodroid-*/ | head -n1)
"$d/bin/remodroid-server" -t -c /etc/remodroid/server.ini
ts=$(date +%Y%m%d%H%M%S)
cp -p /usr/bin/remodroid-server /usr/bin/remodroid-server.bak.$ts
tar -C /usr/share/remodroid -czf /usr/share/remodroid/web.bak.$ts.tgz web
cp "$d/bin/remodroid-server" /usr/bin/remodroid-server.new
mv -f /usr/bin/remodroid-server.new /usr/bin/remodroid-server
cp -r "$d/web/." /usr/share/remodroid/web/
rm -rf /tmp/rd-up
ls -t /usr/bin/remodroid-server.bak.* | tail -n +4 | xargs -r rm -f
ls -t /usr/share/remodroid/web.bak.*.tgz | tail -n +4 | xargs -r rm -f
rc-service remodroid-server restart >/dev/null
sleep 2
rc-service remodroid-server status
'
EOF
)

# A Linux computer agent, as root or as its user: $1 the uploaded .deb or .AppImage, $2 the user.
# It restarts with systemd-run --user in the desktop session's environment: a process started from
# ssh dies with the ssh session (logind).
AGENT_SH=$(cat <<'EOF'
set -eu
F=$1 U=$2
uid=$(id -u "$U")
home=$(getent passwd "$U" | cut -d: -f6)
case $F in
    *.deb)
        DEBIAN_FRONTEND=noninteractive dpkg -i "$F" >/dev/null
        rm -f "$F"
        RUN=/usr/bin/remodroid-desktop ;;
    *.AppImage)
        n=$(basename "$F"); n=${n#rd-}
        mkdir -p "$home/Applications"
        chmod 0755 "$F"
        mv -f "$F" "$home/Applications/$n"
        ln -sfn "$n" "$home/Applications/RemoDroid.AppImage"
        [ "$(id -u)" = 0 ] && chown -h "$U:" "$home/Applications" "$home/Applications/$n" "$home/Applications/RemoDroid.AppImage"
        RUN=$home/Applications/RemoDroid.AppImage ;;
esac
as_user() {
    if [ "$(id -u)" = 0 ]; then runuser -u "$U" -- env XDG_RUNTIME_DIR=/run/user/$uid DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$uid/bus "$@"
    else env XDG_RUNTIME_DIR=/run/user/$uid DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$uid/bus "$@"; fi
}
# the desktop session: a process of the user's with a display that isn't the agent
S=""
for d in /proc/[0-9]*; do
    [ "$(stat -c %u "$d" 2>/dev/null)" = "$uid" ] || continue
    case $(cat "$d/comm" 2>/dev/null) in remodroid-*) continue ;; esac
    tr '\0' '\n' 2>/dev/null < "$d/environ" | grep -q '^DISPLAY=' && { S=${d#/proc/}; break; }
done
[ -n "$S" ] || { echo "no desktop session of $U (log in at its screen)" >&2; exit 1; }
set --
for v in $(tr '\0' '\n' < "/proc/$S/environ" | grep -E '^(DISPLAY|WAYLAND_DISPLAY|XAUTHORITY|XDG_SESSION_TYPE|XDG_CURRENT_DESKTOP|LANG)='); do
    set -- "$@" "--setenv=$v"
done
as_user systemctl --user stop remodroid-desktop 2>/dev/null || true
pkill -TERM -u "$uid" -x remodroid-deskt 2>/dev/null || true
i=0
while pgrep -u "$uid" -x remodroid-deskt >/dev/null && [ $i -lt 20 ]; do sleep 0.5; i=$((i + 1)); done
pkill -KILL -u "$uid" -x remodroid-deskt 2>/dev/null || true
LOG=$home/.config/remodroid/desktop.log
N=$(cat "$LOG" 2>/dev/null | wc -l)
as_user systemd-run --user --quiet --unit=remodroid-desktop --collect "$@" "$RUN" --background
i=0
while [ $i -lt 30 ]; do
    sleep 0.5; i=$((i + 1))
    tail -n +"$((N + 1))" "$LOG" 2>/dev/null | grep -q "connected to the server" && break
done
pgrep -u "$uid" -x remodroid-deskt >/dev/null || { echo "the agent didn't start; see $LOG" >&2; exit 1; }
tail -n +"$((N + 1))" "$LOG" | grep -q "connected to the server" || { echo "the agent runs but hasn't reached the server; see $LOG" >&2; exit 1; }
EOF
)

# The Windows VM, on sx as root: $1 the uploaded zip, $2 the VM, $3 the Windows user. The zip goes
# into the guest as a new folder under the user's Programs\RemoDroid, and the scheduled task
# RemoDroid (the user's, interactive) is pointed at it and started.
WIN_SH=$(cat <<'EOF'
set -eu
F=$1 VM=$2 WU=$3
virsh domstate "$VM" 2>/dev/null | grep -q running || { echo "the $VM VM isn't running (virsh start $VM)" >&2; exit 1; }
N=$(basename "$F" .zip); N=${N#rd-}
D="C:\\Users\\$WU\\AppData\\Local\\Programs\\RemoDroid\\$N"
python3 /root/gput.py "$F" "$D.zip" >/dev/null
rm -f "$F"
cat > /tmp/rd-win.ps1 <<PS
\$ErrorActionPreference = 'Stop'
Stop-ScheduledTask RemoDroid -ErrorAction SilentlyContinue
Get-Process RemoDroid, remodroid-desktopd -ErrorAction SilentlyContinue | Stop-Process -Force  # not remodroid-input: the RemoDroidInput service
Start-Sleep 1
Expand-Archive -Force -Path '$D.zip' -DestinationPath '$D'
Remove-Item '$D.zip'
\$exe = Get-ChildItem '$D' -Recurse -Filter RemoDroid.exe | Select-Object -First 1 -ExpandProperty FullName
if (-not \$exe) { 'no RemoDroid.exe in the zip'; exit 1 }
Set-ScheduledTask RemoDroid -Action (New-ScheduledTaskAction -Execute \$exe -Argument '--background') | Out-Null
\$log = 'C:\\Users\\$WU\\AppData\\Roaming\\RemoDroid\\desktop.log'
\$n = (Get-Content \$log -ErrorAction SilentlyContinue | Measure-Object -Line).Lines
Start-ScheduledTask RemoDroid
for (\$i = 0; \$i -lt 30; \$i++) {
  Start-Sleep -Milliseconds 500
  if (Get-Content \$log | Select-Object -Skip \$n | Select-String 'connected to the server') { 'connected'; exit 0 }
}
'not connected'
PS
OUT=$(python3 /root/gexec.py @/tmp/rd-win.ps1 2>&1 || true)
rm -f /tmp/rd-win.ps1
echo "$OUT" | grep -q '^connected' || { echo "$OUT" | tail -n 5 >&2; exit 1; }
EOF
)

# ---------------- the targets ----------------
need() { for n in "$@"; do eval "[ -n \"\$$n\" ]" || { bad "set $n in ts-update.conf"; return 1; }; done; }
t_server() {
    need SERVER SERVER_CT || return 1
    f=$(pick 'remodroid-[0-9]*-linux-x86_64.tar.gz'); [ -n "$f" ] || { bad "no remodroid-*-linux-x86_64.tar.gz"; return 1; }
    say "server: $(basename "$f") → $SERVER_CT on $SERVER"
    run -p "$SERVER_PORT" "$SERVER" "$SERVER_SH" "$f" "$SERVER_CT"
}
t_agent() { # name, ssh host, user there, file
    say "$1: $(basename "$4") → $2"
    run "$2" "$AGENT_SH" "$4" "$3"
}
t_sx() {
    need SX SX_USER || return 1
    f=$(pick 'remodroid-desktop_*_amd64.deb'); [ -n "$f" ] || { bad "no remodroid-desktop_*_amd64.deb"; return 1; }
    t_agent sx "$SX" "$SX_USER" "$f"
}
t_deck() {
    need DECK || return 1
    f=$(pick 'RemoDroid-[0-9]*-linux-x86_64.AppImage'); [ -n "$f" ] || { bad "no RemoDroid-*-linux-x86_64.AppImage"; return 1; }
    t_agent deck "$DECK" "${DECK%%@*}" "$f"
}
t_pc() {
    need PC || return 1
    f=$(pick 'RemoDroid-[0-9]*-linux-x86_64.AppImage'); [ -n "$f" ] || { bad "no RemoDroid-*-linux-x86_64.AppImage"; return 1; }
    t_agent pc "$PC" "${PC_USER:-${PC%%@*}}" "$f"
}
t_win11() {
    need SX WIN_VM WIN_USER || return 1
    f=$(pick 'RemoDroid-[0-9]*-windows-x86_64.zip'); [ -n "$f" ] || { bad "no RemoDroid-*-windows-x86_64.zip"; return 1; }
    say "win11: $(basename "$f") → the $WIN_VM VM on $SX"
    run "$SX" "$WIN_SH" "$f" "$WIN_VM" "$WIN_USER"
}

FAILED=""
for t in "$@"; do
    if "t_$t"; then ok "$t updated"; else bad "$t failed"; FAILED="$FAILED $t"; fi
done
[ -z "$FAILED" ] || die "not updated:$FAILED"
ok "all updated: $*"
