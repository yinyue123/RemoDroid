#!/bin/sh
# Restricted SSH entry on nl. sx maintains the loopback-only reverse tunnel.
set -eu
[ "${SSH_ORIGINAL_COMMAND:-}" = browser-sx ] || exit 2
exec ssh -i /root/.ssh/browser-sx-install -p 22082 \
    -o BatchMode=yes -o StrictHostKeyChecking=yes \
    -o UserKnownHostsFile=/root/.ssh/browser-sx-knownhosts \
    -o ServerAliveInterval=15 root@127.0.0.1 browser-sx
