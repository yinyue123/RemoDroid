#!/bin/sh
# OpenWrt in a podman container on its own LAN address (macvlan), with LuCI, the agent ipk built
# from this repository (packaging/nfpm-agent.yaml + packaging/luci/) and a RemoDroid server, so you
# can try Services → RemoDroid from any device on your network.
#
#   sudo tests/openwrt.sh 192.168.10.240              OpenWrt 23.05.6 at a free address on your LAN
#   sudo tests/openwrt.sh 192.168.10.240 19.07.10     another release: 19.07.10 21.02.7 22.03.7 23.05.6 24.10.8
#   sudo tests/openwrt.sh stop
#
# Then open http://<ip>/ from another device (the host itself can't reach a macvlan address), log in
# as root / remodroid, go to Services → RemoDroid and paste the pairing command printed at the end.
# The panel is http://<ip>:5580 (admin / remodroid).
#
# Needs root podman, nfpm and internet access (OpenWrt packages, the latest RemoDroid release).
#   PARENT=eth0      LAN interface (default: the one with the default route)
#   NET=mylan        macvlan network to use (default: an existing one on PARENT's subnet, else remodroid-lan)
#   BIN=dir          local x86_64 binaries instead of the latest release's: an unpacked release's bin/,
#                    or build/out/x86_64 (phone files from build/android)
set -eu
cd "$(dirname "$0")/.."
NAME=remodroid-openwrt
say() { printf '\033[1m▸ %s\033[0m\n' "$*"; }
die() { printf '✗ %s\n' "$*" >&2; exit 1; }

[ "${1:-}" ] || die "usage: $0 IP [OPENWRT_VERSION] | stop"
if [ "$1" = stop ]; then
    podman rm -f "$NAME" >/dev/null 2>&1 && echo "removed $NAME" || echo "$NAME isn't running"
    exit 0
fi
IP=$1
VER=${2:-23.05.6}
command -v nfpm >/dev/null || die "nfpm is needed to build the ipk: https://nfpm.goreleaser.com/install/"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# --- a macvlan network on the LAN ---
PARENT=${PARENT:-$(ip route show default | awk '{print $5; exit}')}
GW=$(ip route show default dev "$PARENT" | awk '{print $3; exit}')
SUBNET=$(python3 -c "import ipaddress,sys; print(ipaddress.ip_interface(sys.argv[1]).network)" \
    "$(ip -o -4 addr show dev "$PARENT" | awk '{print $4; exit}')")
python3 -c "import ipaddress,sys; sys.exit(ipaddress.ip_address(sys.argv[1]) not in ipaddress.ip_network(sys.argv[2]))" \
    "$IP" "$SUBNET" || die "$IP isn't in $PARENT's subnet $SUBNET"
ping -c1 -W1 "$IP" >/dev/null 2>&1 && die "$IP answers ping; pick a free address"
if [ -z "${NET:-}" ]; then
    for n in $(podman network ls --filter driver=macvlan --format '{{.Name}}'); do
        [ "$(podman network inspect "$n" --format '{{.NetworkInterface}} {{range .Subnets}}{{.Subnet}}{{end}}')" = "$PARENT $SUBNET" ] && NET=$n && break
    done
fi
if [ -z "${NET:-}" ]; then
    NET=remodroid-lan
    say "creating macvlan network $NET on $PARENT ($SUBNET)"
    podman network create -d macvlan -o parent="$PARENT" --subnet "$SUBNET" --gateway "$GW" "$NET" >/dev/null
fi

# --- the agent ipk, built from this repository ---
if [ -z "${BIN:-}" ]; then
    TAG=$(curl -fsSL https://api.github.com/repos/yinyue123/RemoDroid/releases/latest | sed -n 's/.*"tag_name": *"\([^"]*\)".*/\1/p')
    say "downloading the RemoDroid $TAG x86_64 binaries"
    curl -fsSL "https://github.com/yinyue123/RemoDroid/releases/download/$TAG/remodroid-${TAG#v}-linux-x86_64.tar.gz" | tar -xz -C "$TMP"
    REL="$TMP/remodroid-${TAG#v}-linux-x86_64"
    BIN=$REL/bin PHONE=$REL/phone WEB=$REL/web VERSION=${TAG#v}
else  # an unpacked release (bin/ phone/ web/) or build/out/x86_64 with build/android and web/
    BIN=$(cd "$BIN" && pwd); PHONE=${BIN%/*}/phone WEB=${BIN%/*}/web VERSION=0.0
    [ -d "$PHONE" ] || PHONE=build/android
    [ -d "$WEB" ] || WEB=web
fi
say "building the agent ipk (LuCI page from packaging/luci/)"
sed -e "s|\${PKG_ARCH}|x86_64|g" -e "s|\${VERSION}|$VERSION|g" -e "s|\${BIN}|$BIN|g" -e "s|\${PHONE}|$PHONE|g" \
    packaging/nfpm-agent.yaml > "$TMP/nfpm.yaml"
nfpm package -f "$TMP/nfpm.yaml" -p ipk -t "$TMP/" >"$TMP/nfpm.log" 2>&1 || { cat "$TMP/nfpm.log"; die "nfpm failed"; }
IPK=$(ls "$TMP"/remodroid-agent_*.ipk)

# --- OpenWrt ---
say "starting OpenWrt $VER at $IP (network $NET)"
podman rm -f "$NAME" >/dev/null 2>&1 || true
podman run -d --name "$NAME" --network "$NET" --ip "$IP" "docker.io/openwrt/rootfs:x86-64-$VER" /sbin/init >/dev/null
sleep 3
podman cp "$IPK" "$NAME:/tmp/remodroid-agent.ipk"
podman cp "$BIN/remodroid-server" "$NAME:/usr/bin/remodroid-server"
podman cp "$WEB" "$NAME:/tmp/remodroid-web"
say "installing LuCI and the agent"
podman exec "$NAME" sh -c '
    set -e
    mkdir -p /var/lock
    printf "remodroid\nremodroid\n" | passwd root >/dev/null 2>&1
    opkg update >/tmp/opkg.log 2>&1 || { tail -5 /tmp/opkg.log; exit 1; }
    opkg install luci curl >>/tmp/opkg.log 2>&1 || { tail -5 /tmp/opkg.log; exit 1; }
    opkg install /tmp/remodroid-agent.ipk >>/tmp/opkg.log 2>&1 || { tail -5 /tmp/opkg.log; exit 1; }
    /etc/init.d/uhttpd restart
'

# --- a RemoDroid server in the same container, set up through its wizard API ---
say "starting a RemoDroid server and making a pairing code"
CODE=$(podman exec "$NAME" sh -c '
    set -e
    mkdir -p /tmp/srv/state
    printf "[server]\nstate_dir = /tmp/srv/state\n[http]\nlisten = 0.0.0.0:5590\nweb_root = /tmp/remodroid-web\n" > /tmp/srv/server.ini
    (remodroid-server -c /tmp/srv/server.ini >/tmp/srv/log 2>&1 &)
    i=0; until [ -s /tmp/srv/state/setup-token ] || [ $i -ge 10 ]; do sleep 1; i=$((i+1)); done
    curl -fsS -o /dev/null -H "X-Setup-Token: $(cat /tmp/srv/state/setup-token)" -X POST http://127.0.0.1:5590/api/v1/setup \
        --data "{\"http_port\":5580,\"username\":\"admin\",\"password\":\"remodroid\",\"turn_enabled\":false}"
    i=0; until curl -fs -o /dev/null http://127.0.0.1:5580/login || [ $i -ge 10 ]; do sleep 1; i=$((i+1)); done
    curl -fsS -c /tmp/srv/cookie -o /dev/null -H "Content-Type: application/json" http://127.0.0.1:5580/login \
        --data "{\"user\":\"admin\",\"password\":\"remodroid\"}"
    curl -fsS -b /tmp/srv/cookie -X POST http://127.0.0.1:5580/api/v1/enroll/codes --data "{\"name\":\"OpenWrt test\"}" |
        sed -n "s/.*\"code\": *\"\([^\"]*\)\".*/\1/p"
')
[ -n "$CODE" ] || die "couldn't make a pairing code; see: podman exec $NAME cat /tmp/srv/log"

cat <<EOF

✓ OpenWrt $VER is up at $IP (open these from another device on your LAN, not this host)

  LuCI      http://$IP/   root / remodroid   →  Services → RemoDroid
  Panel     http://$IP:5580/   admin / remodroid

  Pairing command (paste it into LuCI; the code works once, for 10 minutes):
    remodroid enroll --panel http://127.0.0.1:5580 --code $CODE

  A new code: the panel's Agents → Add. Remove everything: sudo $0 stop
EOF
