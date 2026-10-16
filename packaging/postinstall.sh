#!/bin/sh
# Runs after installing a RemoDroid package on any distro. Services are not
# started automatically: the ini file needs your server address and secrets first.
if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
    systemctl daemon-reload >/dev/null 2>&1 || true
fi
echo "RemoDroid: edit the files in /etc/remodroid/, then enable and start the service."
exit 0
