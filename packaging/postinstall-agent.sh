#!/bin/sh
# After installing remodroid-agent (any distro).
if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
    systemctl daemon-reload >/dev/null 2>&1 || true
fi
# OpenWrt with LuCI: show Services → RemoDroid without a reboot
if [ -z "${IPKG_INSTROOT:-}" ] && [ -f /usr/share/luci/menu.d/luci-app-remodroid.json ]; then
    rm -rf /tmp/luci-indexcache* /tmp/luci-modulecache
    [ -x /etc/init.d/rpcd ] && /etc/init.d/rpcd reload >/dev/null 2>&1
fi
cat <<'MSG'
RemoDroid agent installed.
  OpenWrt:    LuCI → Services → RemoDroid pairs, starts and shows it.
  Start it:   systemctl enable --now remodroid-agent
              (OpenWrt/Alpine: /etc/init.d/remodroid-agent enable && /etc/init.d/remodroid-agent start)
  Connect it: remodroid enroll --panel https://your.panel --code XXXX-XXXX
              (your panel → Agents → Add shows this command with a fresh code)
MSG
exit 0
