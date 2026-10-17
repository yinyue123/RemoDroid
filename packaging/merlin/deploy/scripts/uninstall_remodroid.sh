#!/bin/sh
set -eu
export PATH=/koolshare/bin:/koolshare/scripts:$PATH
/koolshare/scripts/remodroid_control.sh stop >/dev/null
rm -f /jffs/configs/remodroid/autostart
rm -f /koolshare/init.d/V99remodroid.sh /koolshare/init.d/T01remodroid.sh
rm -f /koolshare/webs/Module_remodroid.asp /koolshare/res/remodroid.js /koolshare/res/remodroid.css /koolshare/res/icon-remodroid.png
rm -rf /koolshare/remodroid
rm -f /koolshare/scripts/remodroid_common.sh /koolshare/scripts/remodroid_control.sh /koolshare/scripts/remodroid_watch.sh
for k in name title description version install; do dbus remove "softcenter_module_remodroid_$k"; done
dbus remove remodroid_payload
dbus remove remodroid_enrollment
echo '已卸载。配置与 ADB 密钥保留在 /jffs/configs/remodroid，便于重新安装。'
rm -f /koolshare/scripts/uninstall_remodroid.sh
