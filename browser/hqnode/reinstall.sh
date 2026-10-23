#!/bin/bash
# Roll the container onto a newer build of the image: pull it on the machine (the panel never pulls),
# then the panel's own reinstall call. /data survives — browser.conf and the agent's pairing — and the
# browser profile was on tmpfs anyway.
#
#     MACHINE=oect ./browser/hqnode/reinstall.sh          ask first
#     MACHINE=oect ./browser/hqnode/reinstall.sh -y       don't
#     MACHINE=oect ./browser/hqnode/reinstall.sh --dry    say what it would send

set -uo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'

HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=panel.sh
. "$HERE/panel.sh"
trap panel_cleanup EXIT

MACHINE="${MACHINE:-oect}"
CONTAINER_NAME="${CONTAINER_NAME:-browser-${MACHINE}}"
NODE_SSH="${NODE_SSH:-$MACHINE}"
IMAGE_REF="${IMAGE_REF:-docker.io/yinyue123/hub:remote-browser-hqnode}"

MODE=go
case "${1:-}" in
    -y|--yes) MODE=yes ;;
    --dry)    MODE=dry ;;
    "")       ;;
    *)        echo "usage: $0 [-y|--dry]" >&2; exit 2 ;;
esac

node_ssh() { ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=20 "$NODE_SSH" "$@"; }

main() {
    echo -e "${BLUE}=== reinstall ${CONTAINER_NAME} (${MACHINE}) ===${NC}\n"
    panel_login || exit 1
    local mid cid
    mid=$(panel_machine_id "$MACHINE")
    [[ -n "$mid" ]] || { echo -e "${RED}no machine called ${MACHINE} on the panel${NC}"; exit 1; }
    cid=$(panel_container_cid "$mid" "$CONTAINER_NAME")
    [[ -n "$cid" ]] || { echo -e "${RED}no ${CONTAINER_NAME} on the panel${NC}"; exit 1; }
    echo "  machine ${mid}   container ${cid}   image ${IMAGE_REF}"

    if [[ "$MODE" == dry ]]; then
        echo "  ssh ${NODE_SSH} hqnode pull ${IMAGE_REF}"
        echo "  POST /machines/${mid}/containers/${cid}/reinstall {\"ref\":\"${IMAGE_REF}\",\"confirm\":true}"
        exit 0
    fi

    echo -e "\n${BLUE}--- pull ---${NC}"
    node_ssh "hqnode pull '$IMAGE_REF'" 2>&1 | sed 's/^/  /'
    node_ssh "hqnode images | awk -v r='$IMAGE_REF' '\$2==r'" 2>&1 | sed 's/^/  /'

    if [[ "$MODE" != yes ]]; then
        local confirm; read -r -p $'\nreinstall from this image? /data is kept (y/n): ' confirm
        [[ "$confirm" =~ ^[Yy]$ ]] || { echo -e "${YELLOW}cancelled${NC}"; exit 0; }
    fi
    if ! panel_api POST "/machines/${mid}/containers/${cid}/reinstall" \
        "{\"ref\":\"$(panel_json_str "$IMAGE_REF")\",\"confirm\":true}"; then
        echo -e "${RED}✗ not reinstalled: $(panel_err)${NC}"; exit 1
    fi
    echo -e "${GREEN}✓ the panel took it${NC}"

    echo -e "\n${BLUE}--- waiting for the services ---${NC}"
    local i out=""
    for i in $(seq 1 30); do
        sleep 5
        out=$(node_ssh "hqnode exec '$cid' systemctl is-active rd-xorg rd-shell remodroid-desktopd" 2>/dev/null | paste -sd' ')
        [[ "$out" == "active active active" ]] && break
    done
    echo "  rd-xorg rd-shell remodroid-desktopd: ${out:-?}"
}

main "$@"
