#!/bin/bash
# The panel, as a library — sourced by create.sh and deploy.sh.
#
# COPIED VERBATIM from sh0u's quadlet/hqnode/panel.sh (itself dkdad's quadlet/hqnode-charge/panel.sh). It is the same
# panel and the same three calls, and this repository cannot reach across to
# that one, so it carries its own copy. Fix a bug here and fix it there too;
# there is no shared home for it yet and inventing one for two callers would be
# worse than the duplication.
#
# --- original header follows ---
# The panel, as a library — sourced by create.sh and update.sh.
#
# It exists because of the rule at the top of deploy.md, and because that rule
# turned out to be wider than the sentence that stated it. "Containers are made
# in the panel" was written after tw's was made with `hqnode run` and came out
# `created_by: "cli"`, invisible to the list and undeletable through it. The
# same thing is true one layer down, and it bit the same node on the same day:
#
#     hqnode routes           www.ncku.edu.tw → 672e3d3098d9    the agent had it
#     the panel's container   access.domains: []                 the panel did not
#
# push_routes used to POST to the agent's own socket (`http://unix/v1/routes`).
# The agent obeys — the name is served, the handshake works, a client cannot
# tell the difference — but the panel keeps its own inventory and has no call
# that adopts what it did not write. So the domain does not appear on the
# container's page, nobody can manage it there, and the next thing that
# reconciles from the panel's side does not know the name exists.
#
# Hence: **domains are added in the panel too**, and this file is the door.
# Everything here is HTTPS to PANEL_URL from wherever the script is run. There
# is no ssh in this file and there should not be — the agent socket is a
# tenant-proof back door, not the front one.
#
# Two calls do the work, and they are not interchangeable:
#
#     PATCH /machines/{mid}/containers/{cid}   {"domains":[…]}
#         attaches the names. The list is *replaced*, which is why
#         panel_add_domains merges rather than assigns — see there.
#
#     PUT   /machines/{mid}/containers/{cid}/routes/{domain}
#         configures one attached name. It answers 404 for a name that is not
#         attached yet, so the PATCH always comes first.
#
# Read off the panel's own front-end, which is the only specification there is:
#     save: put(`${base}/routes/${domain}`,
#               {http:{enabled,port}, https:{enabled,mode,port}})
#
# JSON is read with grep and sed rather than jq, for the same reason the rest
# of this directory avoids `timeout` and sshpass: the script has to run on a
# stock macOS with nothing installed. The three extractions it needs are all
# single-level and are tested against the real payload; anything deeper than
# that belongs in a language with a parser, not here.

PANEL_URL="${PANEL_URL:-https://hqno.de}"
PANEL_USER="${PANEL_USER:-music}"
PANEL_PASS="${PANEL_PASS:-}"

# State, and one rule about it that is easy to get wrong: a function whose
# output is captured with $( … ) runs in a subshell, and every variable it sets
# dies there. So panel_api does not *print* its result — it leaves the body in
# PANEL_BODY and the status in PANEL_STATUS, and its callers read those. A
# version of this file that printed the body and set the status was written
# first, and every status check in it silently compared the empty string.
#
# The same rule is why panel_login and panel_load must be called from the
# script's own shell before anything reads the cache: a login performed inside
# a subshell asks for the password again on the next call.
PANEL_JAR=""
PANEL_STATUS=""
PANEL_BODY=""
PANEL_LOGGED_IN=""
PANEL_MACHINES_JSON=""

panel_cleanup() { [[ -n "$PANEL_JAR" ]] && rm -f "$PANEL_JAR"; }

# Quote a value for a JSON string. Only the two characters that can end the
# string early — a password is chosen by a person, not by this script.
panel_json_str() {
    local s="$1"
    s="${s//\\/\\\\}"
    s="${s//\"/\\\"}"
    printf '%s' "$s"
}

# One request. Nothing is printed: the body lands in PANEL_BODY and the status
# in PANEL_STATUS, so a caller can tell "empty list" from "the panel said no" —
# the panel answers both with a body that has no rows in it. Returns non-zero
# on anything that is not 2xx, so `panel_api … || return 1` reads naturally.
panel_api() {
    local method="$1" path="$2" body="${3:-}"
    # A jar is needed even for the login, which is what fills it. Without this
    # curl is handed `-b "" -c ""`, fails before it opens a connection, and
    # leaves an empty status that reads as "no response" rather than as the
    # programming error it is.
    [[ -z "$PANEL_JAR" ]] && PANEL_JAR=$(mktemp)
    local tmp
    tmp=$(mktemp)
    local -a args=(
        -sS -b "$PANEL_JAR" -c "$PANEL_JAR"
        -X "$method" -H 'Content-Type: application/json'
        -o "$tmp" -w '%{http_code}'
        --connect-timeout 10 --max-time "${PANEL_MAX_TIME:-60}"
    )
    [[ -n "$body" ]] && args+=(-d "$body")
    PANEL_STATUS=$(curl "${args[@]}" "${PANEL_URL}/api/v1${path}" 2>/dev/null)
    PANEL_BODY=$(tr -d '\n' < "$tmp")
    rm -f "$tmp"
    panel_ok
}

panel_ok() { [[ "$PANEL_STATUS" == 2* ]]; }

# The message out of the last error body, for a caller that wants one line
# rather than the whole JSON in the terminal.
panel_err() {
    local msg
    msg=$(printf '%s' "$PANEL_BODY" | sed -n 's/.*"message":"\([^"]*\)".*/\1/p')
    [[ -z "$msg" ]] && msg=$(printf '%s' "$PANEL_BODY" | sed -n 's/.*"error":"\([^"]*\)".*/\1/p')
    printf '%s' "${msg:-HTTP ${PANEL_STATUS:-无响应}}"
}

# Asked for once, on the first call that needs it, and never for a script run
# that turns out to have nothing to send. PANEL_PASS in the environment skips
# the prompt for an unattended run.
panel_login() {
    [[ -n "$PANEL_LOGGED_IN" ]] && return 0
    [[ -z "$PANEL_JAR" ]] && PANEL_JAR=$(mktemp)

    if [[ -z "$PANEL_PASS" ]]; then
        read -r -s -p "${PANEL_URL} 上 ${PANEL_USER} 的密码: " PANEL_PASS
        echo "" >&2
    fi
    if [[ -z "$PANEL_PASS" ]]; then
        echo "  ! 面板密码是空的" >&2
        return 1
    fi

    if ! panel_api POST /auth/login \
        "{\"identifier\":\"$(panel_json_str "$PANEL_USER")\",\"password\":\"$(panel_json_str "$PANEL_PASS")\"}"; then
        echo "  ! 面板登录失败: $(panel_err)" >&2
        return 1
    fi
    PANEL_LOGGED_IN=1
    return 0
}

# GET /machines carries every machine *and* its containers, which is the only
# listing there is: /machines/{mid}/containers is POST-only, and asking for one
# container by id answers "No such endpoint." Fetched once per run, into a
# global the readers below share.
#
# Call it from the script's own shell, not from inside $( … ) — see the note on
# state at the top. The readers call it too, so a caller that forgets still
# gets right answers; it just pays for the request again.
panel_load() {
    [[ -n "$PANEL_MACHINES_JSON" ]] && return 0
    panel_login || return 1
    if ! panel_api GET /machines; then
        echo "  ! 取机器列表失败: $(panel_err)" >&2
        return 1
    fi
    PANEL_MACHINES_JSON="$PANEL_BODY"
    return 0
}

# Every machine the login can see, as "id<TAB>name". The panel is the
# inventory, so a script that needs to pick a node asks it rather than carrying
# a table that goes stale the day a node is added.
panel_list_machines() {
    panel_load || return 1
    printf '%s' "$PANEL_MACHINES_JSON" \
        | grep -o '"id":"mch_[^"]*","name":"[^"]*"' \
        | sed -n 's/"id":"\([^"]*\)","name":"\([^"]*\)"/\1\t\2/p'
}

# A machine id from the name the panel shows. The regions this repo calls nl,
# tw and us2 are named that on the panel as well, so a TARGETS row that carries
# no id still resolves — but the id is worth carrying anyway, because a machine
# can be renamed on a web page and a script should not depend on that.
panel_machine_id() {
    local name="$1"
    panel_load || return 1
    printf '%s' "$PANEL_MACHINES_JSON" \
        | grep -o '"id":"mch_[^"]*","name":"[^"]*"' \
        | grep "\"name\":\"${name}\"$" \
        | sed -n 's/.*"id":"\([^"]*\)".*/\1/p' \
        | head -1
}

# Everything the panel holds about one machine: the record from its own id up
# to where the next machine's begins. `gsub(…,"\n&")` rather than sed's `\n` in
# a replacement, which BSD sed does not take.
panel_machine_chunk() {
    local mid="$1"
    panel_load || return 1
    printf '%s' "$PANEL_MACHINES_JSON" \
        | awk '{gsub(/"id":"mch_/,"\n&"); print}' \
        | grep "^\"id\":\"${mid}\""
}

# A container's id from its name, scoped to one machine — the panel names a
# container <service>-<region> and two machines can hold the same service.
panel_container_cid() {
    local mid="$1" name="$2" chunk
    chunk=$(panel_machine_chunk "$mid") || return 1
    printf '%s' "$chunk" \
        | grep -o '"cid":"[^"]*","name":"[^"]*"' \
        | grep "\"name\":\"${name}\"$" \
        | sed -n 's/.*"cid":"\([^"]*\)".*/\1/p' \
        | head -1
}

# The names already attached, one per line. A container record runs from its
# own cid to the next one, so the `access.domains` matched here is its own.
panel_container_domains() {
    local cid="$1"
    panel_load || return 1
    printf '%s' "$PANEL_MACHINES_JSON" \
        | awk '{gsub(/"cid":"/,"\n&"); print}' \
        | grep "^\"cid\":\"${cid}\"" \
        | sed -n 's/.*"domains":\[\([^]]*\)\].*/\1/p' \
        | tr ',' '\n' \
        | sed -e 's/^ *//' -e 's/"//g' \
        | grep -v '^$'
}

# Attach names, keeping the ones already there.
#
# The PATCH replaces the whole list, so assigning what a script's table happens
# to declare would silently drop any name someone added on the web page — and
# the web page is where a name is *supposed* to be added. So this merges, and
# removing a name stays a deliberate act performed where it can be seen.
#
# Prints nothing and returns 0 when every wanted name is already attached, so a
# second run of the caller is quiet rather than pretending to have done work.
panel_add_domains() {
    local mid="$1" cid="$2"
    shift 2
    local -a wanted=("$@")

    local -a have=()
    local d
    while IFS= read -r d; do [[ -n "$d" ]] && have+=("$d"); done < <(panel_container_domains "$cid")

    # `${a[@]}` on an empty array is an unbound variable under `set -u` in the
    # bash macOS ships. The `+` form expands to nothing at all instead, which
    # is the only spelling that works on both.
    local -a merged=(${have[@]+"${have[@]}"})
    local -a added=()
    local w h found
    for w in "${wanted[@]}"; do
        found=""
        for h in ${merged[@]+"${merged[@]}"}; do [[ "$h" == "$w" ]] && found=1 && break; done
        [[ -n "$found" ]] && continue
        merged+=("$w")
        added+=("$w")
    done

    if ((${#added[@]} == 0)); then
        return 0
    fi

    local list="" sep=""
    for d in "${merged[@]}"; do
        list+="${sep}\"$(panel_json_str "$d")\""
        sep=","
    done

    if ! panel_api PATCH "/machines/${mid}/containers/${cid}" "{\"domains\":[${list}]}"; then
        echo "  ! 挂域名失败: $(panel_err)" >&2
        return 1
    fi
    # The list changed on the panel, so the cached copy is stale.
    PANEL_MACHINES_JSON=""
    printf '%s\n' "${added[@]}"
    return 0
}

# Configure one attached name.
#
# $3 mode      http | tls | both   — which of the two routers answers for it
# $4 tls_mode  sni | managed       — and how the TLS one does
# $5 port      the container port the mode named above dials
#
# The port belongs to whichever half is doing the work, and the two halves read
# a different number: in `managed` the host terminates and forwards plaintext,
# so the container port is the HTTP one; in `sni` the bytes go in still
# encrypted and it is the TLS one. Route.Backend() on the agent side makes the
# same choice, and a body that puts the number in the other field is a route
# that dials a port nothing listens on.
#
# The other half still has to carry a number the panel will accept, so it gets
# the standard one. That is not a guess: it is what the agent itself wrote for
# tw when the name was attached with the defaults (`http_port: 80`,
# `tls_port: 443`), and leaving it alone keeps the two sides identical.
panel_put_route() {
    local mid="$1" cid="$2" domain="$3" mode="$4" tls_mode="$5" port="$6"

    local http_enabled=false https_enabled=false
    case "$mode" in
        http) http_enabled=true ;;
        tls)  https_enabled=true ;;
        both) http_enabled=true; https_enabled=true ;;
        *)    echo "  ! ${domain}: mode 只能是 http/tls/both,给的是 ${mode}" >&2; return 1 ;;
    esac

    local http_port=80 tls_port=443
    if [[ "$tls_mode" == "managed" ]]; then
        http_port="$port"
    else
        tls_port="$port"
    fi

    local body
    body=$(printf '{"http":{"enabled":%s,"port":%s},"https":{"enabled":%s,"mode":"%s","port":%s}}' \
        "$http_enabled" "$http_port" "$https_enabled" "$tls_mode" "$tls_port")

    if ! panel_api PUT "/machines/${mid}/containers/${cid}/routes/$(panel_url_encode "$domain")" "$body"; then
        echo "  ! ${domain}: $(panel_err)" >&2
        return 1
    fi
    return 0
}

# A domain in a path segment. Hostnames are letters, digits, dots and hyphens,
# all of which pass through unchanged — this is here so a name that is not one
# fails visibly at the panel instead of quietly addressing something else.
panel_url_encode() {
    local s="$1"
    case "$s" in
        *[!a-zA-Z0-9.-]*) printf '%s' "$s" | sed 's/[^a-zA-Z0-9.-]/-/g' ;;
        *) printf '%s' "$s" ;;
    esac
}

# What the panel believes is routed to this container, one line per name, for
# printing after a change. A managed name also carries a certificate, and its
# state is shown because that is the half of the route that takes time to
# become true — a `sni` name never has one and gets no column.
#
# Read, never ask. The agent's own loop obtains a managed name's certificate as
# soon as the route exists, and POSTing the panel's manual `issue` here would
# spend one of the five a name gets per week on every run of this script,
# including the runs that changed nothing. "failed" right after a cutover is
# usually not a broken setup either: it is the authority's answer to the
# attempts made while the splitter in front still pointed somewhere else, and
# the row carries a next_try_at that the loop honours.
panel_show_routes() {
    local mid="$1" cid="$2"
    if ! panel_api GET "/machines/${mid}/containers/${cid}/routes"; then
        echo "  ! 读路由失败: $(panel_err)" >&2
        return 1
    fi
    printf '%s' "$PANEL_BODY" \
        | awk '{gsub(/{"domain":"/,"\n&"); print}' \
        | grep '^{"domain":"' \
        | while IFS= read -r row; do
            local domain mode tls_mode cert
            domain=$(printf '%s' "$row"   | sed -n 's/^{"domain":"\([^"]*\)".*/\1/p')
            mode=$(printf '%s' "$row"     | sed -n 's/.*,"mode":"\([a-z]*\)".*/\1/p')
            tls_mode=$(printf '%s' "$row" | sed -n 's/.*"tls_mode":"\([a-z]*\)".*/\1/p')
            cert=$(printf '%s' "$row"     | sed -n 's/.*"cert":{[^}]*"state":"\([a-z]*\)".*/\1/p')
            if [[ -n "$cert" ]]; then
                echo "  ${domain}  ${mode}/${tls_mode}  cert ${cert}"
            else
                echo "  ${domain}  ${mode}/${tls_mode}"
            fi
        done
}
