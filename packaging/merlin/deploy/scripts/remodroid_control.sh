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
flock -n 9 || fail '另一个操作正在执行，请稍后重试'
case "$ACTION" in
    status) ;;
    config) CONFIG=$(cat "$RD_INI") ;;
    save)
        payload=$(dbus get remodroid_payload)
        dbus remove remodroid_payload
        [ "${#payload}" -le 24000 ] || fail '配置过大（最多 16KB）'
        printf '%s' "$payload" | base64 -d > "$RD_RUN/input.ini" || fail '配置编码无效'
        [ -s "$RD_RUN/input.ini" ] || fail '配置为空或编码无效'
        [ "$(wc -c < "$RD_RUN/input.ini")" -le 16384 ] || fail '配置过大（最多 16KB）'
        if ! awk -f "$RD_HOME/normalize.awk" "$RD_RUN/input.ini" > "$RD_RUN/normalized.ini"; then
            rm -f "$RD_RUN/input.ini" "$RD_RUN/normalized.ini"
            fail 'INI 格式错误：请检查节名、重复字段和每行长度'
        fi
        rm -f "$RD_RUN/input.ini"
        cp "$RD_INI" "$RD_CONF/agent.ini.bak" || fail '无法备份原配置'
        cp "$RD_RUN/normalized.ini" "$RD_CONF/agent.ini.new" || fail '无法写入配置，请检查剩余空间'
        chmod 600 "$RD_CONF/agent.ini.new"
        mv "$RD_CONF/agent.ini.new" "$RD_INI" || fail '无法保存配置'
        rm -f "$RD_RUN/normalized.ini"
        CONFIG=$(cat "$RD_INI")
        MESSAGE='配置已保存；运行中的程序需要重启后生效' ;;
    start) MESSAGE=$(rd_start 2>&1) || fail "$MESSAGE"; MESSAGE='代理进程已启动；面板连接情况请查看日志' ;;
    stop) rd_stop || fail '停止失败，请刷新状态'; MESSAGE='代理已停止' ;;
    restart) rd_stop || fail '停止失败'; MESSAGE=$(rd_start 2>&1) || fail "$MESSAGE"; MESSAGE='代理已重启' ;;
    auto_on) touch "$RD_CONF/autostart"; MESSAGE='已启用开机启动' ;;
    auto_off) rm -f "$RD_CONF/autostart"; MESSAGE='已关闭开机启动' ;;
    boot) [ ! -f "$RD_CONF/autostart" ] || { MESSAGE=$(rd_start 2>&1) || fail "$MESSAGE"; } ;;
    clearlog) : > "$RD_LOG"; MESSAGE='日志已清空' ;;
    *) fail '不支持的操作' ;;
esac
reply
