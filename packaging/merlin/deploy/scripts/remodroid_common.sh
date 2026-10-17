#!/bin/sh
# Fixed paths; no shell evaluation of config or submitted values.
export PATH=/koolshare/bin:/koolshare/scripts:$PATH
RD_HOME=/koolshare/remodroid
RD_CONF=/jffs/configs/remodroid
RD_RUN=/tmp/remodroid
RD_BIN=$RD_HOME/bin/remodroid-agent
RD_INI=$RD_CONF/agent.ini
RD_PID=$RD_RUN/agent.pid
RD_LOG=$RD_RUN/agent.log
umask 077
mkdir -p "$RD_RUN"
rd_pid() {
    [ -r "$RD_PID" ] || return 1
    p=$(cat "$RD_PID")
    case "$p" in ''|*[!0-9]*) return 1;; esac
    [ "$(readlink "/proc/$p/exe" 2>/dev/null)" = "$(readlink -f "$RD_BIN")" ] || return 1
    kill -0 "$p" 2>/dev/null || return 1
    printf '%s' "$p"
}
rd_value() {
    awk -v section="$1" -v key="$2" '
    {sub(/\r$/, "")}
    /^[ \t]*\[/ {s=$0; sub(/^[ \t]*\[/,"",s); sub(/\].*$/, "",s); next}
    s==section && index($0,"=") {k=$0; sub(/=.*/,"",k); gsub(/^[ \t]+|[ \t]+$/,"",k);
      if(k==key){v=substr($0,index($0,"=")+1); sub(/[ \t]+;.*/,"",v); gsub(/^[ \t]+|[ \t]+$/,"",v); if(v ~ /^;/) v=""; print v; exit}}
    ' "$RD_INI"
}
rd_stop() {
    p=$(rd_pid) || return 0
    kill -TERM "$p" || return 1
    i=0
    while kill -0 "$p" 2>/dev/null && [ "$i" -lt 5 ]; do sleep 1; i=$((i+1)); done
    if rd_pid >/dev/null; then kill -KILL "$p" || return 1; fi
    rm -f "$RD_PID"
}
rd_start() {
    rd_pid >/dev/null && return 0
    [ -r "$RD_INI" ] || { echo 'Save the config first'; return 1; }
    case "$(rd_value server url)" in ws://?*|wss://?*) ;; *) echo 'Enter a ws:// or wss:// server address'; return 1;; esac
    [ -n "$(rd_value server device_id)" ] && [ -n "$(rd_value server device_secret)" ] || {
        echo 'Enter the device ID and device secret'; return 1;
    }
    [ -z "$(pidof remodroid-agent 2>/dev/null)" ] || { echo 'Another remodroid-agent process is already running; stop the manually started instance first'; return 1; }
    nohup /koolshare/scripts/remodroid_watch.sh </dev/null >/dev/null 2>&1 9>&- &
    sleep 1
    rd_pid >/dev/null || { echo 'The agent exited right after starting; check the logs'; return 1; }
}
