#!/bin/sh
# Build the Koolshare/KoolCenter offline installer from existing static release artifacts.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
VERSION=${1:?usage: packaging/merlin/build.sh VERSION [armv7|aarch64]}
ARCH=${2:-armv7}
case "$VERSION" in ''|*[!A-Za-z0-9.+_-]*) echo 'Invalid version' >&2; exit 2;; esac
case "$ARCH" in armv7|aarch64) ;; *) echo 'Merlin packages support armv7 or aarch64' >&2; exit 2;; esac
BIN=${BIN:-build/out/$ARCH}
PHONE=${PHONE:-build/android}
OUT=${OUT:-build/packages}
for f in "$BIN/remodroid-agent" "$PHONE/remodroid-server.jar" "$PHONE/arm64-v8a/libremodroid.so" "$PHONE/armeabi-v7a/libremodroid.so"; do
    [ -s "$f" ] || { echo "Missing build artifact: $f" >&2; exit 1; }
done
mkdir -p build "$OUT"
STAGE=$(mktemp -d "$ROOT/build/merlin.XXXXXX")
trap 'rm -rf "$STAGE"' EXIT HUP INT TERM
cp -R packaging/merlin/deploy "$STAGE/remodroid"
P="$STAGE/remodroid"
mkdir -p "$P/remodroid/bin" "$P/remodroid/phone/arm64-v8a" "$P/remodroid/phone/armeabi-v7a"
cp "$BIN/remodroid-agent" "$P/remodroid/bin/"
cp "$PHONE/remodroid-server.jar" "$P/remodroid/phone/"
cp "$PHONE/arm64-v8a/libremodroid.so" "$P/remodroid/phone/arm64-v8a/"
cp "$PHONE/armeabi-v7a/libremodroid.so" "$P/remodroid/phone/armeabi-v7a/"
sed -e 's|/usr/share/remodroid|/koolshare/remodroid/phone|g' \
    -e 's|/etc/remodroid/adbkey|/jffs/configs/remodroid/adbkey|g' \
    -e 's|/var/run/remodroid.sock|/tmp/remodroid/agent.sock|g' \
    deploy/agent.ini.example > "$P/remodroid/agent.ini.example"
printf '%s\n' "$VERSION" > "$P/remodroid/version"
printf '%s\n' "$ARCH" > "$P/remodroid/arch"
chmod 755 "$P/install.sh" "$P"/scripts/*.sh "$P"/init.d/*.sh "$P/remodroid/bin/remodroid-agent"
# Exclude macOS resource forks; the software center expects remodroid/install.sh.
COPYFILE_DISABLE=1 tar --format=ustar -C "$STAGE" -czf "$OUT/remodroid-${VERSION}-merlin-${ARCH}.tar.gz" remodroid
printf '%s\n' "$OUT/remodroid-${VERSION}-merlin-${ARCH}.tar.gz"
