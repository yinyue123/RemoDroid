#!/bin/bash
# Configure the container create.sh made: push /data/hermes/.env (the keys, the proxy, who may talk to
# the bot), restart the gateway, and check that it runs and that the proxy and the provider answer.
#
#     MACHINE=oect NODE_SSH=root@192.168.10.15 ./browser/hermes/hqnode/deploy.sh
#     SSH='nsenter -t 1 -n ssh' …            where plain ssh to the LAN doesn't work
#
#   ENV_FILE   what becomes /data/hermes/.env (default browser/hermes/hqnode/hermes.env: copy the
#              example, fill it in; it is git-ignored)
#   FRPC_FILE  what becomes /data/frpc.toml, the dashboard at hermes.11100.top (default
#              browser/hermes/hqnode/frpc.toml, git-ignored; skipped when there is none)
#
# The model itself is chosen once with `hermes model`, which writes /data/hermes/config.yaml (see the
# end of the output). Files go in through the host's mount of /data, never through `hqnode exec`'s
# stdin, which the agent does not forward (sh0u's deploy.sh: a zero-byte config and exit status 0 the
# whole way).

set -uo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'

HERE="$(cd "$(dirname "$0")" && pwd)"
MACHINE="${MACHINE:-oect}"
CONTAINER_NAME="${CONTAINER_NAME:-hermes-${MACHINE}}"
NODE_SSH="${NODE_SSH:-root@192.168.10.15}"
ENV_FILE="${ENV_FILE:-$HERE/hermes.env}"
FRPC_FILE="${FRPC_FILE:-$HERE/frpc.toml}"
read -r -a SSH_CMD <<< "${SSH:-ssh}"

node_ssh() { "${SSH_CMD[@]}" -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=20 "$NODE_SSH" "$@"; }

# push_into_data <hcid> <local file> <name under /data>: written host-side into the container's /data,
# owned by the container's root (host uid = the data directory's owner), mode 0600 since it holds keys,
# size checked.
push_into_data() {
    local hcid="$1" src="$2" name="$3" want got
    want=$(wc -c < "$src" | tr -d ' ')
    node_ssh "umask 077; cat > /tmp/.rdh-push" < "$src" || return 1
    got=$(node_ssh "
        d=\$(awk -v c='$hcid' '\$2 ~ (\"/containers/\" c \"/data\$\") {print \$2}' /proc/mounts | head -1)
        [ -n \"\$d\" ] || d=/var/hqnode/containers/$hcid/data
        [ -d \"\$d\" ] || { echo no-data-dir; exit 1; }
        u=\$(stat -c %u \"\$d\")
        install -o \"\$u\" -g \"\$u\" -m 0600 /tmp/.rdh-push \"\$d/$name\" && wc -c < \"\$d/$name\"
        rm -f /tmp/.rdh-push") || return 1
    got=$(printf '%s' "$got" | tr -d ' \n')
    [[ "$got" == "$want" ]] || { echo -e "${RED}  /data/${name}: ${got} bytes written of ${want}${NC}" >&2; return 1; }
}

# env_val KEY: its value in ENV_FILE, empty when unset or commented out
env_val() { sed -n "s/^$1=//p" "$ENV_FILE" | tail -1; }

check_env() {
    local ok=1
    if [[ -z "$(env_val OPENROUTER_API_KEY)$(env_val ANTHROPIC_API_KEY)$(env_val OPENAI_API_KEY)" ]]; then
        echo -e "${RED}  no provider key (OPENROUTER_API_KEY, ANTHROPIC_API_KEY or OPENAI_API_KEY)${NC}"; ok=0
    fi
    # The agent has a shell: a bot anyone can talk to is a shell anyone can use.
    if [[ -n "$(env_val TELEGRAM_BOT_TOKEN)" && -z "$(env_val TELEGRAM_ALLOWED_USERS)$(env_val GATEWAY_ALLOWED_USERS)" ]]; then
        echo -e "${RED}  TELEGRAM_BOT_TOKEN without TELEGRAM_ALLOWED_USERS: anyone could use the bot${NC}"; ok=0
    fi
    if grep -q '^GATEWAY_ALLOW_ALL_USERS=\(1\|true\)' "$ENV_FILE"; then
        echo -e "${RED}  GATEWAY_ALLOW_ALL_USERS is on${NC}"; ok=0
    fi
    [[ -n "$(env_val HTTPS_PROXY)" ]] || echo -e "${YELLOW}  no HTTPS_PROXY: from the OECT most providers and Telegram time out${NC}"
    ((ok))
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

    echo -e "\n${BLUE}--- /data/hermes/.env ---${NC}"
    if [[ ! -f "$ENV_FILE" ]]; then
        echo -e "${RED}no ${ENV_FILE}${NC}"
        echo -e "${YELLOW}  cp $HERE/hermes.env.example $ENV_FILE  then the provider key and the Telegram bot${NC}"
        exit 1
    fi
    check_env || exit 1
    push_into_data "$hcid" "$ENV_FILE" hermes.env.new || exit 1
    ex "sh -c 'install -d -o hermes -g hermes -m 0700 /data/hermes && install -o hermes -g hermes -m 0600 /data/hermes.env.new /data/hermes/.env && rm /data/hermes.env.new'" || exit 1
    echo "  pushed ($(grep -c '^[A-Z]' "$ENV_FILE") settings)"

    if [[ -f "$FRPC_FILE" ]]; then
        echo -e "\n${BLUE}--- /data/frpc.toml ---${NC}"
        if grep -q 'REPLACE-WITH-THE-FRP-TOKEN' "$FRPC_FILE"; then
            echo -e "${RED}  ${FRPC_FILE} still has the example token${NC}"; exit 1
        fi
        if [[ -z "$(env_val HERMES_DASHBOARD_BASIC_AUTH_PASSWORD)" ]]; then
            echo -e "${RED}  frpc publishes the dashboard: set HERMES_DASHBOARD_BASIC_AUTH_PASSWORD first${NC}"; exit 1
        fi
        push_into_data "$hcid" "$FRPC_FILE" frpc.toml || exit 1
        ex "sh -c 'systemctl enable --now hermes-dashboard && systemctl restart frpc'" 2>&1 | grep -v cgroup | sed 's/^/  /'
        sleep 5
        ex "journalctl -u frpc -o cat --no-pager | tail -3" 2>&1 | sed 's/^/  /'
    fi

    echo -e "\n${BLUE}--- the way out ---${NC}"
    local proxy; proxy=$(env_val HTTPS_PROXY)
    local u
    for u in https://openrouter.ai/api/v1/models https://api.anthropic.com/v1/models https://api.telegram.org https://github.com; do
        ex "curl -sS -m 15 ${proxy:+-x '$proxy'} -o /dev/null -w '%{http_code} %{time_total}s  $u\n' '$u'" 2>&1 | sed 's/^/  /'
    done

    echo -e "\n${BLUE}--- the gateway ---${NC}"
    ex "systemctl restart hermes-gateway" 2>&1 | sed 's/^/  /'
    sleep 10
    ex "systemctl is-active hermes-gateway" 2>&1 | sed 's/^/  hermes-gateway: /'
    ex "journalctl -b -u hermes-gateway -o cat --no-pager | tail -8" 2>&1 | sed 's/^/  /'
    if ! ex "test -s /data/hermes/config.yaml" >/dev/null 2>&1; then
        echo -e "\n${YELLOW}No /data/hermes/config.yaml yet: choose the model once, then restart the gateway:${NC}"
        cat <<EOF

    ${SSH_CMD[*]} -t ${NODE_SSH} "hqnode exec ${CONTAINER_NAME} runuser -u hermes -- hermes model"
    ${SSH_CMD[*]} ${NODE_SSH} "hqnode exec ${CONTAINER_NAME} systemctl restart hermes-gateway"
EOF
    fi
    cat <<EOF

Then message the bot. The dashboard, if wanted:
    ${SSH_CMD[*]} ${NODE_SSH} "hqnode exec ${CONTAINER_NAME} systemctl enable --now hermes-dashboard"
    ${SSH_CMD[*]} ${NODE_SSH} "hqnode ports ${CONTAINER_NAME}"       # the 9119 row: 127.100.0.N:PORT
    ${SSH_CMD[*]} -N -L 9119:127.100.0.N:PORT ${NODE_SSH}            # then http://127.0.0.1:9119
EOF
}

main "$@"
