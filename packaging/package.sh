#!/bin/sh
# Builds the distro packages and a tarball for one CPU architecture.
#   packaging/package.sh VERSION ARCH
# Expects build/out/ARCH/ (static binaries from build/dist.sh) and build/android/
# (phone files from android/build.sh). Output: build/packages/.
#
#   ARCH     deb (Debian/Ubuntu)  rpm (CentOS/RHEL)  apk (Alpine)  ipk (OpenWrt, agent only)
#   x86      i386                 i686               x86           i386_pentium4
#   x86_64   amd64                x86_64             x86_64        x86_64
#   armv7    armhf                armv7hl            armv7         arm_cortex-a7_neon-vfpv4, arm_cortex-a9_vfpv3-d16, arm_cortex-a15_neon-vfpv4
#   aarch64  arm64                aarch64            aarch64       aarch64_generic, aarch64_cortex-a53, aarch64_cortex-a72
#   mipsel   -                    -                  -             mipsel_24kc
#   mips     -                    -                  -             mips_24kc
# The server is for Linux servers (and Windows: packaging/windows/package.sh), not routers: there
# is no OpenWrt or MIPS server, and the MIPS tarballs hold only the agent.
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
case "$ARCH" in
    mipsel|mips) PARTS=agent ;;
    *) PARTS="server agent" ;;
esac
chmod 0755 "$BIN"/remodroid-*
mkdir -p "$OUT"

pkg() { # packager, distro arch, parts…
    kind=$1 parch=$2
    shift 2
    for p in "$@"; do
        # nFPM doesn't expand variables in every field (e.g. contents src), so fill them in here.
        sed -e "s|\${PKG_ARCH}|$parch|g" -e "s|\${VERSION}|$VERSION|g" -e "s|\${BIN}|$BIN|g" -e "s|\${PHONE}|$PHONE|g" \
            "packaging/nfpm-$p.yaml" > "build/nfpm-$p.yaml"
        nfpm package -f "build/nfpm-$p.yaml" -p "$kind" -t "$OUT/"
    done
}
[ -n "$DEB" ] && pkg deb "$DEB" $PARTS
[ -n "$RPM" ] && pkg rpm "$RPM" $PARTS
[ -n "$APK" ] && pkg apk "$APK" $PARTS
for a in $IPK; do pkg ipk "$a" agent; done

# plain tarball for anything else
T=remodroid-$VERSION-linux-$ARCH
rm -rf "build/$T" && mkdir -p "build/$T/bin" "build/$T/etc/openrc" "build/$T/phone"
for p in $PARTS; do
    cp "$BIN/remodroid-$p" "build/$T/bin/"
    cp deploy/$p.ini.example deploy/remodroid-$p.service "build/$T/etc/"
    cp deploy/openrc/remodroid-$p "build/$T/etc/openrc/"
done
cp deploy/remodroid-agent.init "build/$T/etc/"
case " $PARTS " in *" server "*) cp -r web "build/$T/web" ;; esac
cp -r "$PHONE/remodroid-server.jar" "$PHONE/arm64-v8a" "$PHONE/armeabi-v7a" "build/$T/phone/"
tar -C build -czf "$OUT/$T.tar.gz" "$T"
# CI builds Merlin agent installers in a separate step after these OpenWrt packages.
# For a local Merlin build, use packaging/merlin/build.sh VERSION ARCH.
ls -l "$OUT"
