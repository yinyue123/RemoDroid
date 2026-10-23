#!/bin/sh
# The inputtest touch backend against a real Xorg: the dummy screen of browser/hqnode/rd-xorg-light.conf
# with its touch socket moved to a temporary directory. Needs Xorg 21.1+ (its inputtest driver ships in
# xserver-xorg-core), xserver-xorg-video-dummy, libxi-dev, and root or Xorg.wrap allowing it.
#
#   browser/test/touch_test.sh [CC]
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd -P)
CC=${1:-${CC:-cc}}
T=$(mktemp -d)
trap 'kill $XPID 2>/dev/null || true; rm -rf "$T"' EXIT
XPID=

have=
for f in /usr/lib/xorg/modules/drivers/dummy_drv.so /usr/lib/*/xorg/modules/drivers/dummy_drv.so; do
	[ -e "$f" ] && have=1
done
[ -n "$have" ] || { echo "skip: no Xorg dummy driver (xserver-xorg-video-dummy)"; exit 77; }

$CC -std=gnu11 -O1 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE -I"$ROOT/browser/agent" \
	-I"$ROOT/desktop/core" -I"$ROOT/desktop/linux" -I"$ROOT/common" -I"$ROOT/third_party/cjson" \
	-I"$ROOT/third_party/libev" -I"$ROOT/third_party/inih" \
	-o "$T/touch_test" "$ROOT/browser/test/touch_test.c" "$ROOT/browser/agent/touch_inputtest.c" -lX11 -lXi

sed "s|/run/rd-touch.sock|$T/touch.sock|" "$ROOT/browser/hqnode/rd-xorg-light.conf" > "$T/xorg.conf"
mkdir "$T/conf.d"
N=50
while [ -e /tmp/.X$N-lock ]; do N=$((N + 1)); done
Xorg :$N -config "$T/xorg.conf" -configdir "$T/conf.d" -noreset -nolisten tcp -novtswitch -keeptty \
	-logfile "$T/xorg.log" >/dev/null 2>&1 &
XPID=$!
i=0
until [ -S "$T/touch.sock" ] && DISPLAY=:$N "$T/touch_test" --probe >/dev/null 2>&1 || [ $i -ge 50 ]; do
	i=$((i + 1)); sleep 0.1
done
[ -S "$T/touch.sock" ] || { echo "Xorg didn't start:"; grep -E '\(EE\)' "$T/xorg.log"; exit 1; }
DISPLAY=:$N "$T/touch_test" "$T/touch.sock"
