#!/bin/sh
. /koolshare/scripts/remodroid_common.sh
# One managed instance. The agent handles its own connection retries.
"$RD_BIN" -c "$RD_INI" >>"$RD_LOG" 2>&1 </dev/null &
p=$!
printf '%s\n' "$p" > "$RD_PID"
while kill -0 "$p" 2>/dev/null; do
    if [ -f "$RD_LOG" ] && [ "$(wc -c < "$RD_LOG")" -gt 262144 ]; then
        # Truncate in place: the agent keeps its existing append-only descriptor.
        tail -n 150 "$RD_LOG" > "$RD_RUN/last.log"
        cat "$RD_RUN/last.log" > "$RD_LOG"
        rm -f "$RD_RUN/last.log"
    fi
    sleep 5
done
[ "$(cat "$RD_PID" 2>/dev/null)" != "$p" ] || rm -f "$RD_PID"
