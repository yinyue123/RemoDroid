#!/bin/sh
# After installing remodroid-agent (any distro).
if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
    systemctl daemon-reload >/dev/null 2>&1 || true
fi
cat <<'MSG'
RemoDroid agent installed.
  Start it:   systemctl enable --now remodroid-agent
              (OpenWrt/Alpine: /etc/init.d/remodroid-agent enable && /etc/init.d/remodroid-agent start)
  Connect it: remodroid enroll --panel https://your.panel --code XXXX-XXXX
              (your panel → Agents → Add shows this command with a fresh code)
MSG
exit 0
