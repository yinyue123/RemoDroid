#!/bin/sh
# Restricted SSH entry for the browser-sx Actions deployment, reached through nl.
set -eu
[ "${SSH_ORIGINAL_COMMAND:-}" = browser-sx ] || exit 2
umask 077
mkdir -p /root/remodroid-browser-deploy
cd /root/remodroid-browser-deploy
tar -xf -
sha256sum -c SHA256SUMS
python3 browser-install.py remote-browser-amd64.tar
rm -f remote-browser-amd64.tar
