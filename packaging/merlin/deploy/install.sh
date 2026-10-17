#!/bin/sh
set -eu
export PATH=/koolshare/bin:/koolshare/scripts:$PATH
HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
[ -d /koolshare/webs ] && [ -f /koolshare/scripts/base.sh ] || {
    echo '需要 Koolshare / KoolCenter 软件中心，原版梅林不能直接安装此包。'; exit 1;
}
[ -x /koolshare/bin/jq ] || { echo '软件中心缺少 jq，安装中止。'; exit 1; }
# Execute only the help command, before touching an existing installation.
"$HERE/remodroid/bin/remodroid-agent" -h >/dev/null 2>&1 || {
    echo '程序与此固件不兼容，请检查 ARM 架构。RT-AC86U 使用 armv7 包。'; exit 1;
}
FREE=$(df -k /jffs | awk 'END {print $4}')
NEED=$(du -sk "$HERE" | awk '{print $1}')
[ "$FREE" -gt "$((NEED + 2048))" ] || { echo 'JFFS 空间不足，需要为系统保留 2MB。'; exit 1; }
if [ -x /koolshare/scripts/remodroid_control.sh ]; then
    /koolshare/scripts/remodroid_control.sh stop >/dev/null
fi
mkdir -p /koolshare/remodroid /jffs/configs/remodroid
chmod 700 /jffs/configs/remodroid
cp -R "$HERE/remodroid/." /koolshare/remodroid/
cp "$HERE"/scripts/*.sh /koolshare/scripts/
cp "$HERE"/webs/* /koolshare/webs/
cp "$HERE"/res/* /koolshare/res/
cp "$HERE"/init.d/*.sh /koolshare/init.d/
chmod 755 /koolshare/scripts/remodroid*.sh /koolshare/scripts/uninstall_remodroid.sh /koolshare/init.d/*remodroid.sh /koolshare/remodroid/bin/remodroid-agent
# Preserve credentials and USB trust on install, upgrade, and uninstall/reinstall.
if [ ! -f /jffs/configs/remodroid/agent.ini ]; then
    cp /koolshare/remodroid/agent.ini.example /jffs/configs/remodroid/agent.ini
fi
chmod 600 /jffs/configs/remodroid/agent.ini
# Installation never enables or starts the agent.
dbus set softcenter_module_remodroid_name=remodroid
dbus set softcenter_module_remodroid_title=RemoDroid
dbus set softcenter_module_remodroid_description='Android 远程连接代理：配置、启停、状态与日志'
dbus set "softcenter_module_remodroid_version=$(cat /koolshare/remodroid/version)"
dbus set softcenter_module_remodroid_install=1
sync
echo 'RemoDroid 安装完成。请在软件中心打开插件，填写配置后启动。'
