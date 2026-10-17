#!/bin/sh
set -eu
export PATH=/koolshare/bin:/koolshare/scripts:$PATH
HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
[ -d /koolshare/webs ] && [ -f /koolshare/scripts/base.sh ] || {
    echo 'This package needs the Koolshare / KoolCenter software center; stock Asuswrt-Merlin cannot install it directly.'; exit 1;
}
[ -x /koolshare/bin/jq ] || { echo 'The software center is missing jq; aborting.'; exit 1; }
# Match the package architecture to the router BEFORE touching an existing install.
# A 32-bit agent will run under AArch64 compat long enough to pass `-h`, then die
# at runtime on a missing time64 syscall, so `-h` alone cannot tell the archs apart.
PKG_ARCH=$(cat "$HERE/remodroid/arch" 2>/dev/null || echo unknown)
case "${REMODROID_DEV_ARCH:-$(uname -m)}" in
    aarch64|arm64)            DEV_ARCH=aarch64 ;;
    armv7*|armv6*|armv5*|arm) DEV_ARCH=armv7 ;;
    *)                        DEV_ARCH=${REMODROID_DEV_ARCH:-$(uname -m)} ;;
esac
if [ "$PKG_ARCH" != "$DEV_ARCH" ]; then
    echo "Arch mismatch: this router is $DEV_ARCH but the package is $PKG_ARCH."
    echo "64-bit models (RT-AC86U, RT-AX...) need the aarch64 package; 32-bit models (RT-AC68U...) need the armv7 package."
    exit 1
fi
# Secondary sanity check: the binary must at least exec on this firmware.
"$HERE/remodroid/bin/remodroid-agent" -h >/dev/null 2>&1 || {
    echo 'The agent cannot run on this firmware; check that the architecture matches.'; exit 1;
}
FREE=$(df -k /jffs | awk 'END {print $4}')
NEED=$(du -sk "$HERE" | awk '{print $1}')
[ "$FREE" -gt "$((NEED + 2048))" ] || { echo 'Not enough JFFS space; 2MB must stay free for the system.'; exit 1; }
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
dbus set softcenter_module_remodroid_description='Android remote-control agent: configure, start/stop, status, and logs'
dbus set "softcenter_module_remodroid_version=$(cat /koolshare/remodroid/version)"
dbus set softcenter_module_remodroid_install=1
sync
echo 'RemoDroid installed. Open the plugin in the software center, fill in the config, then start it.'
