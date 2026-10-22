#!/bin/sh
# Packages the desktop agents for download.
#   packaging/desktop/package.sh VERSION linux ARCH DIR    .deb, .rpm, .tar.gz, .AppImage from a Linux build dir
#   packaging/desktop/package.sh VERSION windows ARCH DIR  RemoDroid-VERSION-windows-ARCH.zip
#   packaging/desktop/package.sh VERSION macos - DIR        RemoDroid-VERSION-macos.zip (RemoDroid.app)
# Output: build/packages/.
set -eu
cd "$(dirname "$0")/../.."
VERSION=$1 OS=$2 ARCH=$3 DIR=$4
OUT=${OUT:-build/packages}
mkdir -p "$OUT"
case "$OS" in
linux)
    case "$ARCH" in
        x86_64)  DEB=amd64 RPM=x86_64 ;;
        aarch64) DEB=arm64 RPM=aarch64 ;;
        *) echo "unknown arch: $ARCH" >&2; exit 2 ;;
    esac
    chmod 0755 "$DIR"/remodroid-desktop "$DIR"/remodroid-desktopd
    for p in "deb $DEB" "rpm $RPM"; do
        set -- $p
        sed -e "s|\${PKG_ARCH}|$2|g" -e "s|\${VERSION}|$VERSION|g" -e "s|\${DESK}|$DIR|g" \
            packaging/nfpm-desktop.yaml > build/nfpm-desktop.yaml
        nfpm package -f build/nfpm-desktop.yaml -p "$1" -t "$OUT/"
    done
    T=remodroid-desktop-$VERSION-linux-$ARCH
    rm -rf "build/$T" && mkdir -p "build/$T/lib/remodroid" "build/$T/bin" "build/$T/share/applications"
    cp "$DIR/libremodroid.so" "build/$T/lib/remodroid/"
    cp "$DIR/remodroid-desktop" "$DIR/remodroid-desktopd" "build/$T/bin/"
    cp packaging/desktop/remodroid-desktop.desktop "build/$T/share/applications/"
    cp -r "$DIR/icons" "build/$T/share/icons"
    cp packaging/desktop/remodroid-desktopd.service "build/$T/"
    tar -C build -czf "$OUT/$T.tar.gz" "$T"
    # AppImage: the same files in one runnable file for any distribution with GTK 3 and X11.
    # Needs appimagetool on PATH and APPIMAGE_RUNTIME, a directory with runtime-x86_64 and
    # runtime-aarch64 from AppImage/type2-runtime.
    A=build/RemoDroid-$VERSION-$ARCH.AppDir
    rm -rf "$A" && mkdir -p "$A/usr/lib/remodroid" "$A/usr/bin" "$A/usr/share/applications"
    cp "$DIR/libremodroid.so" "$A/usr/lib/remodroid/"
    cp "$DIR/remodroid-desktop" "$DIR/remodroid-desktopd" "$A/usr/bin/"
    cp -r "$DIR/icons" "$A/usr/share/icons"
    cp packaging/desktop/remodroid-desktop.desktop "$A/usr/share/applications/"
    cp packaging/desktop/remodroid-desktop.desktop "$DIR/icons/256x256/apps/remodroid.png" "$A/"
    install -m 0755 packaging/desktop/AppRun "$A/AppRun"
    ARCH=$ARCH appimagetool --no-appstream --runtime-file "$APPIMAGE_RUNTIME/runtime-$ARCH" \
        "$A" "$OUT/RemoDroid-$VERSION-linux-$ARCH.AppImage"
    ;;
windows)
    T=RemoDroid-$VERSION-windows-$ARCH
    rm -rf "build/$T" && mkdir -p "build/$T"
    cp "$DIR/RemoDroid.exe" "$DIR/remodroid-desktopd.exe" "$DIR/remodroid.dll" "build/$T/"
    (cd build && rm -f "$T.zip" && zip -qr "$T.zip" "$T") && mv "build/$T.zip" "$OUT/"
    ;;
macos)
    T=RemoDroid-$VERSION-macos
    (cd "$DIR" && rm -f "$T.zip" && ditto -c -k --sequesterRsrc --keepParent RemoDroid.app "$T.zip")
    mv "$DIR/$T.zip" "$OUT/"
    ;;
*) echo "unknown OS: $OS" >&2; exit 2 ;;
esac
ls -l "$OUT"
