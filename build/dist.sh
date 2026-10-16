#!/bin/sh
# Builds static, stripped binaries for every target into build/out/<arch>/.
# Runs inside the build container (make dist), or anywhere `zig` is on PATH.
#   ./build/dist.sh                 all targets
#   ./build/dist.sh mipsel aarch64  only these
set -eu
cd "$(dirname "$0")/.."

# arch name -> Zig target triple
triple() {
    case "$1" in
        mipsel)  echo mipsel-linux-musleabi ;;   # MT7621, MT7628, ... (soft float)
        mips)    echo mips-linux-musleabi ;;     # ath79 / QCA (big endian)
        arm|armv7) echo arm-linux-musleabihf ;;  # IPQ40xx, i.MX, Raspberry Pi 2+ (ARMv7, hard float)
        aarch64) echo aarch64-linux-musl ;;      # Filogic, IPQ807x, Raspberry Pi 3+, AWS Graviton
        x86_64)  echo x86_64-linux-musl ;;       # x86 routers, AWS x86 instances
        x86)     echo x86-linux-musl ;;          # 32-bit x86 (i686)
        *) echo "unknown arch: $1" >&2; exit 2 ;;
    esac
}

ARCHES=${*:-"x86 x86_64 armv7 aarch64 mipsel mips"}
JOBS=$(nproc 2>/dev/null || echo 4)

for arch in $ARCHES; do
    t=$(triple "$arch")
    echo "==> $arch ($t)"
    # Release objects live apart from debug builds (make doesn't track flag changes).
    make -j"$JOBS" TARGET="rel-$arch" CC="zig cc -target $t" AR="zig ar" STATIC=1 OPT=-Os LDFLAGS=-s >/dev/null
    mkdir -p "build/out/$arch"
    cp "build/rel-$arch/remodroid-agent" "build/rel-$arch/remodroid-server" "build/out/$arch/"
    ls -l "build/out/$arch"
done

mkdir -p build/out/web build/out/etc
cp -r web/. build/out/web/ 2>/dev/null || true
cp deploy/agent.ini.example deploy/server.ini.example build/out/etc/ 2>/dev/null || true
echo "done: build/out/"
