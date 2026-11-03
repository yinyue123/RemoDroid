#!/bin/bash
# Create the Hermes Agent container on the OECT — in the panel, because that is the only place a
# container can be created and remain manageable (a `hqnode run` one comes out created_by: cli,
# invisible to the panel's list and undeletable through it; sh0u's create.sh tells that story).
# browser/hqnode/create.sh with Hermes' sizes and no devices.
#
#     ARCHIVE=hermes-arm64.tar MACHINE=oect NODE_SSH=root@192.168.10.15 ./browser/hermes/hqnode/create.sh
#     … CONTAINER_PASS=… ./create.sh -y                                           non-interactive
#
# The panel never pulls: fetch-image.sh puts the image the hermes-image workflow built on the machine
# first, and ARCHIVE names the file it left in the sideload directory (IMAGE_REF instead for an image
# already in `hqnode images`).
#
# NODE_SSH sends the create through the machine's own link to the panel rather than straight to the
# panel's web address: a create from an archive outlasts the 100 s Cloudflare allows a request there.
# SSH='nsenter -t 1 -n ssh' where plain ssh to the LAN doesn't work (a proxy's TUN in the way).
#
# After this, ./deploy.sh: the keys and the checks.

set -uo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'

HERE="$(cd "$(dirname "$0")" && pwd)"
# The remote browser's copy of the panel library: one more caller isn't a reason for a third copy.
# shellcheck source=../../hqnode/panel.sh
. "$HERE/../../hqnode/panel.sh"
trap panel_cleanup EXIT

IMAGE_REF="${IMAGE_REF:-localhost/hermes-hqnode:arm64}"
# A file in the machine's sideload directory to build from instead of IMAGE_REF (fetch-image.sh).
ARCHIVE="${ARCHIVE:-}"
SSH_USERNAME="${SSH_USERNAME:-hermes}"
MACHINE="${MACHINE:-}"
CONTAINER_NAME="${CONTAINER_NAME:-}"
read -r -a SSH_CMD <<< "${SSH:-ssh}"

# The OECT has four A55 cores and 3.8 GB, with the remote browser (1.5 GB) and surveil beside it.
# Hermes has no browser of its own and waits on the model most of the time: two cores and 1 GB.
CPU_CORES="${CPU_CORES:-2}"
MEM="${MEM:-1024M}"
# The image is Python 3.14, Node and the venv: about 2 GB. 8G leaves room for what its tools install.
DISK="${DISK:-8G}"
# /data/hermes is HERMES_HOME: .env, config.yaml, sessions, memories, skills, logs. Without data_bytes
# there is no /data, and a reinstall forgets all of it.
DATA="${DATA:-8G}"
NET_QUOTA="${NET_QUOTA:-256G}"
# 22 for `hermes model` and the like; 9119 the dashboard, host-only, reached through an ssh tunnel. The
# gateway connects out (Telegram long-polls), so nothing has to be reachable from outside.
PORTS="${PORTS:-22,9119}"
DEVICES="${DEVICES-}"

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
    echo -e "${BLUE}=== hqnode: create the Hermes Agent container (through the panel) ===${NC}\n"
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
    cname=$(ask "container name" "${CONTAINER_NAME:-hermes-${mname}}")
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

    # The panel ties the create to this request: a client that hangs up cancels it on the panel while
    # the machine finishes anyway, leaving a container the panel never recorded. Formatting the disks
    # and unpacking the image takes 90-100 s on the OECT, past Cloudflare's 100 s in front of the panel
    # (HTTP 520). With NODE_SSH the request goes through the machine's own link to the panel instead
    # (hqnode's /v1/portal relay: the same body, 40 minutes, not cancelled by the caller).
    local created=0
    if [[ -n "${NODE_SSH:-}" ]]; then
        PANEL_BODY=$(printf '%s' "$body" | "${SSH_CMD[@]}" -o BatchMode=yes "$NODE_SSH" \
            'curl -sS --noproxy "*" --max-time 2700 --unix-socket /run/hqnode.sock -H "Authorization: Bearer $(cat /etc/hqnode/cli-token)" -H "Content-Type: application/json" -X POST -d @- -w "\n%{http_code}" http://localhost/v1/portal/containers')
        PANEL_STATUS=${PANEL_BODY##*$'\n'}; PANEL_BODY=${PANEL_BODY%$'\n'*}
        panel_ok && created=1
    elif PANEL_MAX_TIME=2400 panel_api POST "/machines/${mid}/containers" "$body"; then
        created=1
    fi
    if (( ! created )); then
        echo -e "${RED}✗ not created: $(panel_err)${NC}"
        case "$PANEL_BODY" in
            *catalog*|*image*)
                echo -e "${YELLOW}  the image has to be on the machine first: ./browser/hermes/hqnode/fetch-image.sh${NC}" ;;
        esac
        exit 1
    fi
    local cid created_by
    cid=$(printf '%s' "$PANEL_BODY" | sed -n 's/.*"cid":"\([^"]*\)".*/\1/p')
    created_by=$(printf '%s' "$PANEL_BODY" | sed -n 's/.*"created_by":"\([^"]*\)".*/\1/p')
    echo -e "${GREEN}✓ ${cname}  cid ${cid}  created_by ${created_by:-?}${NC}"

    cat <<EOF

Next:

    MACHINE=${mname} CONTAINER_NAME=${cname} NODE_SSH=${NODE_SSH:-root@192.168.10.15} ./browser/hermes/hqnode/deploy.sh

It pushes hermes.env to /data/hermes/.env, starts the gateway, and checks the proxy and the provider.
EOF
}

main "$@"
