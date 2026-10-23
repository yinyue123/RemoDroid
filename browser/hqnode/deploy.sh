#!/bin/bash
# Configure the container create.sh made: push /data/browser.conf, pair the RemoDroid agent, and check
# that the four services run and the encoder is there.
#
#     MACHINE=oect PAIR_CODE=XXXX-XXXX ./browser/hqnode/deploy.sh
#
#   CONF_FILE   what becomes /data/browser.conf (default browser/hqnode/browser.conf: copy the
#               example, fill in the proxy, keep it out of git)
#   PAIR_CODE   a one-time code from the RemoDroid panel (Agents → Add → Computer); PANEL its URL.
#               Without it the pairing already on /data is kept.
#
# Files go in through the host's mount of /data, never through `hqnode exec`'s stdin, which the agent
# does not forward (sh0u's deploy.sh: a zero-byte config and exit status 0 the whole way).

set -uo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'

HERE="$(cd "$(dirname "$0")" && pwd)"
MACHINE="${MACHINE:-oect}"
CONTAINER_NAME="${CONTAINER_NAME:-browser-${MACHINE}}"
NODE_SSH="${NODE_SSH:-$MACHINE}"
CONF_FILE="${CONF_FILE:-$HERE/browser.conf}"
PAIR_CODE="${PAIR_CODE:-}"
PANEL="${PANEL:-}"
PAIR_NAME="${PAIR_NAME:-$CONTAINER_NAME}"

node_ssh() { ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=20 "$NODE_SSH" "$@"; }

# push_into_data <hcid> <local file> <name under /data>: written host-side into the container's /data,
# owned by the container's root (host uid = the data directory's owner), size checked.
push_into_data() {
    local hcid="$1" src="$2" name="$3" want got
    want=$(wc -c < "$src" | tr -d ' ')
    node_ssh "cat > /tmp/.rdb-push" < "$src" || return 1
    got=$(node_ssh "
        d=\$(awk -v c='$hcid' '\$2 ~ (\"/containers/\" c \"/data\$\") {print \$2}' /proc/mounts | head -1)
        [ -n \"\$d\" ] || d=/var/hqnode/containers/$hcid/data
        [ -d \"\$d\" ] || { echo no-data-dir; exit 1; }
        u=\$(stat -c %u \"\$d\")
        install -o \"\$u\" -g \"\$u\" -m 0644 /tmp/.rdb-push \"\$d/$name\" && wc -c < \"\$d/$name\"
        rm -f /tmp/.rdb-push") || return 1
    got=$(printf '%s' "$got" | tr -d ' \n')
    [[ "$got" == "$want" ]] || { echo -e "${RED}  /data/${name}: ${got} bytes written of ${want}${NC}" >&2; return 1; }
}

main() {
    echo -e "${BLUE}=== deploy ${CONTAINER_NAME} on ${MACHINE} ===${NC}\n"
    local line hcid
    line=$(node_ssh "hqnode list 2>/dev/null | awk -v n='$CONTAINER_NAME' '\$2==n'") || {
        echo -e "${RED}can't reach ${NODE_SSH}${NC}"; exit 1; }
    [[ -n "$line" ]] || { echo -e "${RED}no container ${CONTAINER_NAME} on the machine — run create.sh${NC}"; exit 1; }
    echo "  $line"
    hcid=$(printf '%s' "$line" | awk '{print $1}')
    ex() { node_ssh "hqnode exec '$hcid' $*"; }

    echo -e "\n${BLUE}--- /data/browser.conf ---${NC}"
    if [[ ! -f "$CONF_FILE" ]]; then
        echo -e "${RED}no ${CONF_FILE}${NC}"
        echo -e "${YELLOW}  cp $HERE/browser.conf.example $CONF_FILE  then set proxy, timezone, lang${NC}"
        exit 1
    fi
    if grep -q '^proxy *= *socks5://203\.0\.113\.10' "$CONF_FILE"; then
        echo -e "${RED}${CONF_FILE} still has the example proxy${NC}"; exit 1
    fi
    push_into_data "$hcid" "$CONF_FILE" browser.conf.new || exit 1
    ex "sh -c 'mv /data/browser.conf.new /data/browser.conf; chmod 644 /data/browser.conf'" || exit 1
    echo "  pushed ($(wc -l < "$CONF_FILE") lines)"

    if [[ -n "$PAIR_CODE" ]]; then
        echo -e "\n${BLUE}--- pairing ---${NC}"
        [[ -n "$PANEL" ]] || { echo -e "${RED}PANEL=https://your.panel is needed with PAIR_CODE${NC}"; exit 1; }
        ex "systemctl stop remodroid-desktopd" >/dev/null 2>&1
        ex "runuser -u rdagent -- /usr/bin/remodroid-desktopd -c /data/remodroid/desktop.ini pair --panel '$PANEL' --code '$PAIR_CODE' --name '$PAIR_NAME'" 2>&1 | sed 's/^/  /'
    fi

    echo -e "\n${BLUE}--- services ---${NC}"
    ex "systemctl restart rd-xorg rd-video rd-shell remodroid-desktopd" 2>&1 | sed 's/^/  /'
    sleep 5
    ex "systemctl is-active rd-xorg rd-video rd-shell remodroid-desktopd" 2>&1 | paste -sd' ' | sed 's/^/  /'

    echo -e "\n${BLUE}--- checks ---${NC}"
    ex "sh -c 'ls -l /dev/mpp_service /dev/dma_heap 2>&1; journalctl -b -u rd-video -o cat | tail -3'" 2>&1 | sed 's/^/  /'
    ex "runuser -u rdagent -- mpptest -n 60 2>&1 | tail -2" 2>&1 | sed 's/^/  encoder: /'
    ex "sh -c 'DISPLAY=:0 XAUTHORITY=/run/rd-xorg.auth xdpyinfo 2>/dev/null | grep dimensions || grep -E \"\\(EE\\)\" /run/rd-xorg.log | tail -3'" 2>&1 | sed 's/^/  /'
    ex "journalctl -b -u remodroid-desktopd -o cat | grep -E 'video:|touch:|online|paired' | tail -4" 2>&1 | sed 's/^/  agent: /'
    cat <<EOF

The computer shows up in the RemoDroid panel as ${PAIR_NAME}. Open it from the iPhone's Home Screen
app; the stats should say H.265 and Rockchip MPP.
EOF
}

main "$@"
