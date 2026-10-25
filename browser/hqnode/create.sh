#!/bin/bash
# Create the remote-browser container on the OECT — in the panel, because that is the only place a
# container can be created and remain manageable (a `hqnode run` one comes out created_by: cli,
# invisible to the panel's list and undeletable through it; sh0u's create.sh tells that story).
#
#     ./browser/hqnode/create.sh                    ask for everything
#     MACHINE=oect CONTAINER_PASS=… ./create.sh -y  non-interactive
#
# It asks for the Rockchip encoder's devices ("devices": ["rkmpp"]: /dev/mpp_service, /dev/dma_heap/*
# and the SoC's compatible string), which needs an hqnode with that option — an older panel refuses
# the field. DEVICES= (empty) creates one without them: the agent then encodes in software.
#
# The panel never pulls, so the image has to be on the machine first:
#
#     ssh -p 36000 root@<oect> 'hqnode pull docker.io/yinyue123/hub:remote-browser-hqnode'
#
# (`context deadline exceeded` there is the CLI's own deadline; the pull goes on. Don't retry.)
# Or from the image fetch-image.sh sideloaded (built by the browser-image workflow):
#
#     ARCHIVE=remote-browser-arm64.tar MACHINE=oect ./browser/hqnode/create.sh
#
# After this, ./deploy.sh: the settings and the pairing.

set -uo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'

HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=panel.sh
. "$HERE/panel.sh"
trap panel_cleanup EXIT

IMAGE_REF="${IMAGE_REF:-docker.io/yinyue123/hub:remote-browser-hqnode}"
# A file in the machine's sideload directory to build from instead of IMAGE_REF (fetch-image.sh).
ARCHIVE="${ARCHIVE:-}"
SSH_USERNAME="${SSH_USERNAME:-browser}"
MACHINE="${MACHINE:-}"
CONTAINER_NAME="${CONTAINER_NAME:-}"

# The OECT has four A55 cores and 2 GB: the container gets all four cores (Chromium rasterizes in
# software) and 1.5 GB, leaving the host, hqnoded and pasta the rest with zram behind them.
CPU_CORES="${CPU_CORES:-4}"
MEM="${MEM:-1536M}"
# The image is about 1.5 GB; 8G leaves room for logs and Chromium's own files outside the profile.
DISK="${DISK:-8G}"
# /data: browser.conf, the agent's pairing and the six users' profiles, tabs, history and favourites
# (cookies and site data grow: a few hundred MB a user is normal). It must exist — without data_bytes
# there is no /data, and a reinstall forgets the pairing and the users.
DATA="${DATA:-4G}"
NET_QUOTA="${NET_QUOTA:-1024G}"
# 22 only: the agent connects out to the panel, and WebRTC goes through TURN when nothing is mapped.
PORTS="${PORTS:-22}"
DEVICES="${DEVICES-rkmpp}"

ASSUME_YES=""
[[ "${1:-}" == "-y" || "${1:-}" == "--yes" ]] && ASSUME_YES=1

to_bytes() {
    local v="$1" n unit
    n="${v%[KkMmGgTt]}"; unit="${v#"$n"}"
    [[ "$n" =~ ^[0-9]+$ ]] || { echo ""; return 1; }
    case "$unit" in
        K|k) echo $((n * 1024)) ;;
        M|m) echo $((n * 1024 * 1024)) ;;
        G|g) echo $((n * 1024 * 1024 * 1024)) ;;
        T|t) echo $((n * 1024 * 1024 * 1024 * 1024)) ;;
        '')  echo "$n" ;;
        *)   echo ""; return 1 ;;
    esac
}

ask() {
    local prompt="$1" default="$2" answer
    [[ -n "$ASSUME_YES" ]] && { printf '%s' "$default"; return; }
    read -r -p "${prompt} [${default}]: " answer
    printf '%s' "${answer:-$default}"
}

main() {
    echo -e "${BLUE}=== hqnode: create the remote-browser container (through the panel) ===${NC}\n"
    panel_login || exit 1

    local -a mids=() mnames=()
    local id name
    while IFS=$'\t' read -r id name; do
        [[ -z "$id" ]] && continue
        mids+=("$id"); mnames+=("$name")
    done < <(panel_list_machines)
    ((${#mids[@]})) || { echo -e "${RED}the panel has no machines${NC}"; exit 1; }

    local mid="" mname=""
    if [[ -n "$MACHINE" ]]; then
        local i
        for i in "${!mnames[@]}"; do
            [[ "${mnames[$i]}" == "$MACHINE" ]] && { mid="${mids[$i]}"; mname="$MACHINE"; break; }
        done
        [[ -n "$mid" ]] || { echo -e "${RED}no machine called ${MACHINE} on the panel${NC}"; exit 1; }
    else
        echo -e "${BLUE}=== which machine ===${NC}"
        local i
        for i in "${!mids[@]}"; do echo -e "${GREEN}$((i+1)).${NC} ${mnames[$i]}  (${mids[$i]})"; done
        local choice
        read -r -p "machine: " choice
        [[ "$choice" =~ ^[0-9]+$ ]] && ((choice >= 1 && choice <= ${#mids[@]})) \
            || { echo -e "${RED}not one of them${NC}"; exit 1; }
        mid="${mids[$((choice-1))]}"; mname="${mnames[$((choice-1))]}"
    fi

    local cname image ssh_user cpu mem disk data quota ports devices
    cname=$(ask "container name" "${CONTAINER_NAME:-browser-${mname}}")
    local source=ref
    if [[ -n "$ARCHIVE" ]]; then
        source=archive; image="$ARCHIVE"
    else
        image=$(ask "image ref"  "$IMAGE_REF")
    fi
    ssh_user=$(ask "gateway user" "$SSH_USERNAME")
    cpu=$(ask   "CPU cores"      "$CPU_CORES")
    mem=$(to_bytes  "$(ask "memory" "$MEM")")   || { echo -e "${RED}memory like 1536M${NC}"; exit 1; }
    disk=$(to_bytes "$(ask "root disk" "$DISK")") || { echo -e "${RED}bad root disk size${NC}"; exit 1; }
    data=$(to_bytes "$(ask "/data" "$DATA")")    || { echo -e "${RED}bad /data size${NC}"; exit 1; }
    quota=$(to_bytes "$(ask "traffic / month" "$NET_QUOTA")") || { echo -e "${RED}bad traffic size${NC}"; exit 1; }
    ports=$(ask "ports" "$PORTS")
    devices=$(ask "devices" "$DEVICES")

    local cpass="${CONTAINER_PASS:-}"
    if [[ -z "$cpass" ]]; then
        read -r -s -p "password for ${ssh_user}@${cname}: " cpass; echo ""
    fi
    [[ -n "$cpass" ]] || { echo -e "${RED}empty password${NC}"; exit 1; }

    local ports_json; ports_json=$(printf '%s' "$ports" | tr -d ' ')
    [[ "$ports_json" =~ ^[0-9]+(,[0-9]+)*$ ]] || { echo -e "${RED}ports like 22,8080${NC}"; exit 1; }
    local devices_json="" d sep=""
    for d in $(printf '%s' "$devices" | tr ',' ' '); do
        devices_json+="${sep}\"$(panel_json_str "$d")\""; sep=","
    done

    local body
    body=$(printf '{"name":"%s","ssh_username":"%s","ssh_password":"%s","'"$source"'":"%s","limits":{"cpu_cores":%s,"mem_bytes":%s,"disk_bytes":%s,"data_bytes":%s,"net_quota_bytes":%s},"ports":[%s]%s}' \
        "$(panel_json_str "$cname")" "$(panel_json_str "$ssh_user")" \
        "$(panel_json_str "$cpass")" "$(panel_json_str "$image")" \
        "$cpu" "$mem" "$disk" "$data" "$quota" "$ports_json" \
        "$([[ -n "$devices_json" ]] && printf ',"devices":[%s]' "$devices_json")")

    echo -e "\n${BLUE}=== to create ===${NC}"
    echo "  machine  ${mname} (${mid})"
    echo "  name     ${cname}    gateway user ${ssh_user}"
    echo "  image    ${image} (${source})"
    echo "  limits   ${cpu} cores / $((mem/1024/1024))M memory / $((disk/1024/1024))M root / $((data/1024/1024))M /data"
    echo "  traffic  $((quota/1024/1024/1024))G/month   ports ${ports_json}   devices ${devices:-none}"
    if [[ -z "$ASSUME_YES" ]]; then
        local confirm; read -r -p $'\ncreate it? (y/n): ' confirm
        [[ "$confirm" =~ ^[Yy]$ ]] || { echo -e "${YELLOW}cancelled${NC}"; exit 0; }
    fi

    if ! panel_api POST "/machines/${mid}/containers" "$body"; then
        echo -e "${RED}✗ not created: $(panel_err)${NC}"
        case "$PANEL_BODY" in
            *catalog*|*image*)
                echo -e "${YELLOW}  the image has to be on the machine first:${NC}"
                echo -e "${YELLOW}  ssh -p 36000 root@<node> 'hqnode pull ${image}'${NC}" ;;
            *devices*|*rkmpp*)
                echo -e "${YELLOW}  this panel or machine can't give the encoder's devices; DEVICES= to create without them${NC}" ;;
        esac
        exit 1
    fi
    local cid created_by
    cid=$(printf '%s' "$PANEL_BODY" | sed -n 's/.*"cid":"\([^"]*\)".*/\1/p')
    created_by=$(printf '%s' "$PANEL_BODY" | sed -n 's/.*"created_by":"\([^"]*\)".*/\1/p')
    echo -e "${GREEN}✓ ${cname}  cid ${cid}  created_by ${created_by:-?}${NC}"

    cat <<EOF

Next:

    MACHINE=${mname} CONTAINER_NAME=${cname} PAIR_CODE=XXXX-XXXX ./browser/hqnode/deploy.sh

It pushes browser.conf, pairs the agent with the panel code, and checks the services.
EOF
}

main "$@"
