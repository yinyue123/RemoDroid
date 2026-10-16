#!/bin/sh
# After installing remodroid-server (any distro).
if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
    systemctl daemon-reload >/dev/null 2>&1 || true
fi
cat <<'MSG'
RemoDroid server installed.
  Start it:   systemctl enable --now remodroid-server
              (Alpine: rc-update add remodroid-server && rc-service remodroid-server start)
  Then open:  http://<this server>:5580/  — the setup wizard. Its code is in
              /var/lib/remodroid/setup-token and in the server's log.
MSG
exit 0
