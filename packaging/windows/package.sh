#!/bin/sh
# Packages the server for Windows (server/win/win.mk) for download.
#   packaging/windows/package.sh VERSION ARCH DIR   RemoDroid-Server-VERSION-windows-ARCH.zip
# DIR holds remodroid-server.exe and RemoDroidServer.exe. Output: build/packages/.
set -eu
cd "$(dirname "$0")/../.."
VERSION=$1 ARCH=$2 DIR=$3
OUT=${OUT:-build/packages}
T=RemoDroid-Server-$VERSION-windows-$ARCH
rm -rf "build/$T" && mkdir -p "build/$T" "$OUT"
cp "$DIR/RemoDroidServer.exe" "$DIR/remodroid-server.exe" "build/$T/"
cp -r web "build/$T/web"
cp deploy/server.ini.example "build/$T/"
sed "s/@VERSION@/$VERSION/" packaging/windows/README.txt | sed 's/$/\r/' > "build/$T/README.txt"
(cd build && rm -f "$T.zip" && zip -qr "$T.zip" "$T") && mv "build/$T.zip" "$OUT/"
ls -l "$OUT/$T.zip"
