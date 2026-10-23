#!/bin/bash
# Build the remote-browser image for an hqnode container.
#
#     ./browser/hqnode/build.sh                  build for the OECT (linux/arm64)
#     ./browser/hqnode/build.sh --push           and push
#     PLATFORM=linux/amd64 ./browser/hqnode/build.sh
#
#     CloakBrowser: the free build from GitHub Releases, pinned in the Dockerfile, by default
#     CLOAKBROWSER_URL=… CLOAKBROWSER_SHA256=…   another build of it (a .tar.gz for that arch)
#     CLOAKBROWSER=chromium                      Debian's chromium instead
#
# The CloakBrowser binary's licence allows no redistribution: keep the image private (no --push to a
# public repository).
#
# The context is the repository root, because the image compiles the agent and rd-shell from it.
# --pull=always so it starts from the current ghcr.io/yinyue123/hqnode:debian-13. Building arm64 on
# an x86 machine needs qemu-user-static (binfmt); on the OECT itself it is native but slow.

set -euo pipefail

IMAGE=${IMAGE:-docker.io/yinyue123/hub:remote-browser-hqnode}
PLATFORM=${PLATFORM:-linux/arm64}
here=$(cd "$(dirname "$0")" && pwd)
context=$(dirname "$(dirname "$here")")

if command -v podman >/dev/null 2>&1; then engine=podman
elif command -v docker >/dev/null 2>&1; then engine=docker
else echo "neither podman nor docker is on PATH" >&2; exit 1
fi

net=()
case "${HTTPS_PROXY:-}${HTTP_PROXY:-}${https_proxy:-}${http_proxy:-}" in
	*127.0.0.1*|*localhost*) net=(--network=host) ;;
esac

echo "==> building $IMAGE for $PLATFORM ($engine, context $context)"
"$engine" build --pull=always --platform "$PLATFORM" "${net[@]}" \
	--build-arg CLOAKBROWSER="${CLOAKBROWSER:-free}" \
	--build-arg CLOAKBROWSER_URL="${CLOAKBROWSER_URL:-}" --build-arg CLOAKBROWSER_SHA256="${CLOAKBROWSER_SHA256:-}" \
	-f "$here/Dockerfile" -t "$IMAGE" "$context"

if [ "${1:-}" = "--push" ]; then
	echo "==> pushing $IMAGE"
	"$engine" push "$IMAGE"
fi

cat <<EOF

Next:
    ssh -p 36000 root@<oect> 'hqnode pull $IMAGE'
    MACHINE=oect ./browser/hqnode/create.sh
    MACHINE=oect PANEL=https://your.panel PAIR_CODE=XXXX-XXXX ./browser/hqnode/deploy.sh
EOF
