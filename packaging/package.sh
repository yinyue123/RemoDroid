#!/bin/sh
# Builds the distro packages and a tarball for one CPU architecture.
#   packaging/package.sh VERSION ARCH
# Expects build/out/ARCH/ (static binaries from build/dist.sh) and build/android/
# (phone files from android/build.sh). Output: build/packages/.
#
#   ARCH     deb (Debian/Ubuntu)  rpm (CentOS/RHEL)  apk (Alpine)  ipk (OpenWrt)
#   x86      i386                 i686               x86           i386_pentium4
#   x86_64   amd64                x86_64             x86_64        x86_64
#   armv7    armhf                armv7hl            armv7         arm_cortex-a7_neon-vfpv4, arm_cortex-a9_vfpv3-d16, arm_cortex-a15_neon-vfpv4
#   aarch64  arm64                aarch64            aarch64       aarch64_generic, aarch64_cortex-a53, aarch64_cortex-a72
#   mipsel   -                    -                  -             mipsel_24kc
#   mips     -                    -                  -             mips_24kc
set -eu
cd "$(dirname "$0")/.."
VERSION=$1
ARCH=$2
OUT=${OUT:-build/packages}
BIN=build/out/$ARCH
PHONE=build/android
case "$ARCH" in
    x86)     DEB=i386  RPM=i686    APK=x86     IPK="i386_pentium4" ;;
    x86_64)  DEB=amd64 RPM=x86_64  APK=x86_64  IPK="x86_64" ;;
    armv7)   DEB=armhf RPM=armv7hl APK=armv7   IPK="arm_cortex-a7_neon-vfpv4 arm_cortex-a9_vfpv3-d16 arm_cortex-a15_neon-vfpv4" ;;
    aarch64) DEB=arm64 RPM=aarch64 APK=aarch64 IPK="aarch64_generic aarch64_cortex-a53 aarch64_cortex-a72" ;;
    mipsel)  DEB=      RPM=        APK=        IPK="mipsel_24kc" ;;
    mips)    DEB=      RPM=        APK=        IPK="mips_24kc" ;;
    *) echo "unknown arch: $ARCH" >&2; exit 2 ;;
esac
chmod 0755 "$BIN"/remodroid-*
mkdir -p "$OUT"

pkg() { # packager, distro arch
    for p in server agent; do
        PKG_ARCH=$2 VERSION=$VERSION BIN=$BIN PHONE=$PHONE nfpm package -f "packaging/nfpm-$p.yaml" -p "$1" -t "$OUT/"
    done
}
[ -n "$DEB" ] && pkg deb "$DEB"
[ -n "$RPM" ] && pkg rpm "$RPM"
[ -n "$APK" ] && pkg apk "$APK"
for a in $IPK; do pkg ipk "$a"; done

# plain tarball for anything else
T=remodroid-$VERSION-linux-$ARCH
rm -rf "build/$T" && mkdir -p "build/$T/bin" "build/$T/etc" "build/$T/phone"
cp "$BIN"/remodroid-* "build/$T/bin/"
cp -r web "build/$T/web"
cp deploy/*.example deploy/*.service deploy/*.init "build/$T/etc/"
cp -r deploy/openrc "build/$T/etc/openrc"
cp -r "$PHONE/remodroid-server.jar" "$PHONE/arm64-v8a" "$PHONE/armeabi-v7a" "build/$T/phone/"
tar -C build -czf "$OUT/$T.tar.gz" "$T"
ls -l "$OUT"
