# Build container: every toolchain RemoDroid needs, pinned.
#   make dist      (runs ./build/dist.sh inside this image)
#
# Router and server binaries are cross-compiled with Zig's clang + bundled musl,
# so one toolchain covers mipsel, mips, arm, aarch64 and x86_64.
FROM docker.io/library/debian:trixie-slim

ARG ZIG_VERSION=0.17.0
ARG ZIG_SHA256=1cbe9df9f27e6b78d14ccbca43b6703a404ef79ef1c463de901d7f088d4e2026

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl xz-utils make file \
    && rm -rf /var/lib/apt/lists/*

RUN curl -fsSL -o /tmp/zig.tar.xz "https://ziglang.org/download/${ZIG_VERSION}/zig-x86_64-linux-${ZIG_VERSION}.tar.xz" \
    && echo "${ZIG_SHA256}  /tmp/zig.tar.xz" | sha256sum -c - \
    && mkdir -p /opt/zig && tar -C /opt/zig --strip-components=1 -xf /tmp/zig.tar.xz \
    && rm /tmp/zig.tar.xz && ln -s /opt/zig/zig /usr/local/bin/zig

WORKDIR /src
