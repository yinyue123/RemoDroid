#!/bin/sh
. /koolshare/scripts/remodroid_common.sh
RPC=
case "${1:-}" in '' ) ACTION=status;; *[!0-9]* ) ACTION=$1;; *) RPC=$1; ACTION=${2:-status};; esac
OK=true
MESSAGE=
CONFIG=
reply() {
    pid=$(rd_pid || true)
    auto=false; [ ! -f "$RD_CONF/autostart" ] || auto=true
    logs=$(tail -n 150 "$RD_LOG" 2>/dev/null || true)
    data=$(/koolshare/bin/jq -cn --argjson ok "$OK" --arg message "$MESSAGE" \
      --arg pid "$pid" --argjson autostart "$auto" --arg logs "$logs" --arg config "$CONFIG" \
      --arg version "$(cat "$RD_HOME/version" 2>/dev/null)" \
      '{ok:$ok,message:$message,running:($pid!=""),pid:$pid,autostart:$autostart,logs:$logs,config:$config,version:$version}')
    if [ -n "$RPC" ]; then
        # httpdb wraps the callback as response.result. Base64 avoids JSON/HTML injection.
        encoded=$(printf '%s' "$data" | /koolshare/bin/base64_encode)
        curl -sS --max-time 5 -X POST --data-binary "$encoded" "http://127.0.0.1/_resp/$RPC" >/dev/null
    else
        printf '%s\n' "$data"
    fi
    [ "$OK" = true ]
}
fail() { OK=false; MESSAGE=$1; reply; exit 1; }
exec 9>"$RD_RUN/control.lock"
flock -n 9 || fail 'Another operation is in progress; try again shortly'
case "$ACTION" in
    status) ;;
    config) CONFIG=$(cat "$RD_INI") ;;
    enroll)
        payload=$(dbus get remodroid_enrollment)
        dbus remove remodroid_enrollment
        rd_pid >/dev/null && fail 'Stop the agent before pairing with a panel'
        [ "${#payload}" -le 2048 ] || fail 'Pairing command too long'
        enrollment=$(printf '%s' "$payload" | base64 -d) || fail 'Invalid pairing command encoding'
        panel=$(printf '%s' "$enrollment" | /koolshare/bin/jq -er '.panel | select(type == "string")') || fail 'Invalid panel address'
        code=$(printf '%s' "$enrollment" | /koolshare/bin/jq -er '.code | select(type == "string")') || fail 'Invalid pairing code'
        [ "${#panel}" -le 240 ] && printf '%s\n' "$panel" | grep -Eq '^https?://[][A-Za-z0-9.:_-]+/?$' || fail 'The panel address must be an http:// or https:// address with no path'
        [ "${#code}" -le 64 ] && printf '%s\n' "$code" | grep -Eq '^[A-Za-z0-9-]+$' || fail 'Invalid pairing code format'
        # Stage enrollment in RAM. Never execute the pasted command as shell code.
        cp "$RD_INI" "$RD_RUN/enroll.ini" || fail 'Cannot prepare the config'
        # This firmware has no timeout applet. Bound network waits with a watchdog.
        "$RD_BIN" -c "$RD_RUN/enroll.ini" enroll --panel "$panel" --code "$code" > "$RD_RUN/enroll.log" 2>&1 9>&- &
        enroll_pid=$!
        (sleep 15; kill -TERM "$enroll_pid" 2>/dev/null) </dev/null >/dev/null 2>&1 9>&- &
        guard_pid=$!
        enroll_result=0
        wait "$enroll_pid" || enroll_result=$?
        kill "$guard_pid" 2>/dev/null || true
        wait "$guard_pid" 2>/dev/null || true
        if [ "$enroll_result" != 0 ]; then
            MESSAGE="Enrollment failed; check the panel address and that the code hasn't expired. $(tail -n 5 "$RD_RUN/enroll.log")"
            rm -f "$RD_RUN/enroll.ini"
            fail "$MESSAGE"
        fi
        awk -f "$RD_HOME/normalize.awk" "$RD_RUN/enroll.ini" > "$RD_RUN/normalized.ini" || fail 'The config returned by pairing is invalid'
        cp "$RD_INI" "$RD_CONF/agent.ini.bak" || fail 'Cannot back up the existing config'
        cp "$RD_RUN/normalized.ini" "$RD_CONF/agent.ini.new" || fail 'Cannot save the paired config'
        chmod 600 "$RD_CONF/agent.ini.new"
        mv "$RD_CONF/agent.ini.new" "$RD_INI" || fail 'Cannot save the paired config'
        rm -f "$RD_RUN/enroll.ini" "$RD_RUN/normalized.ini"
        CONFIG=$(cat "$RD_INI")
        MESSAGE=$(rd_start 2>&1) || fail "Enrollment saved, but starting failed: $MESSAGE"
        MESSAGE='Enrollment saved and the agent started; check the logs for panel connection status' ;;
    save)
        payload=$(dbus get remodroid_payload)
        dbus remove remodroid_payload
        [ "${#payload}" -le 24000 ] || fail 'Config too large (16KB max)'
        printf '%s' "$payload" | base64 -d > "$RD_RUN/input.ini" || fail 'Invalid config encoding'
        [ -s "$RD_RUN/input.ini" ] || fail 'Config is empty or the encoding is invalid'
        [ "$(wc -c < "$RD_RUN/input.ini")" -le 16384 ] || fail 'Config too large (16KB max)'
        if ! awk -f "$RD_HOME/normalize.awk" "$RD_RUN/input.ini" > "$RD_RUN/normalized.ini"; then
            rm -f "$RD_RUN/input.ini" "$RD_RUN/normalized.ini"
            fail 'Invalid INI: check section names, duplicate fields, and line lengths'
        fi
        rm -f "$RD_RUN/input.ini"
        cp "$RD_INI" "$RD_CONF/agent.ini.bak" || fail 'Cannot back up the existing config'
        cp "$RD_RUN/normalized.ini" "$RD_CONF/agent.ini.new" || fail 'Cannot write the config; check free space'
        chmod 600 "$RD_CONF/agent.ini.new"
        mv "$RD_CONF/agent.ini.new" "$RD_INI" || fail 'Cannot save the config'
        rm -f "$RD_RUN/normalized.ini"
        CONFIG=$(cat "$RD_INI")
        MESSAGE='Config saved; restart the running agent for it to take effect' ;;
    start) MESSAGE=$(rd_start 2>&1) || fail "$MESSAGE"; MESSAGE='Agent started; check the logs for panel connection status' ;;
    stop) rd_stop || fail 'Stop failed; refresh the status'; MESSAGE='Agent stopped' ;;
    restart) rd_stop || fail 'Stop failed'; MESSAGE=$(rd_start 2>&1) || fail "$MESSAGE"; MESSAGE='Agent restarted' ;;
    auto_on) touch "$RD_CONF/autostart"; MESSAGE='Start on boot enabled' ;;
    auto_off) rm -f "$RD_CONF/autostart"; MESSAGE='Start on boot disabled' ;;
    boot) [ ! -f "$RD_CONF/autostart" ] || { MESSAGE=$(rd_start 2>&1) || fail "$MESSAGE"; } ;;
    clearlog) : > "$RD_LOG"; MESSAGE='Logs cleared' ;;
    *) fail 'Unsupported operation' ;;
esac
reply
