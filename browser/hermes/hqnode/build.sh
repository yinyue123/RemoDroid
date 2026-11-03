#!/bin/bash
# Build the Hermes Agent image for an hqnode container, where a container engine works. The usual
# build is CI's (.github/workflows/hermes-image.yml, native arm64, then fetch-image.sh): this
# workstation's podman is broken and the OECT has none.
#
#     ./browser/hermes/hqnode/build.sh                  for the OECT (linux/arm64)
#     ./browser/hermes/hqnode/build.sh --push           and push
#     PLATFORM=linux/amd64 ./browser/hermes/hqnode/build.sh
#     HERMES_COMMIT=<sha> ./browser/hermes/hqnode/build.sh       pin hermes-agent
#     HERMES_CHANNEL=main ./browser/hermes/hqnode/build.sh       its main branch instead of stable
#
# The context is this directory. --pull=always so it starts from the current hqnode:debian-13.

set -euo pipefail

IMAGE=${IMAGE:-localhost/hermes-hqnode:arm64}
PLATFORM=${PLATFORM:-linux/arm64}
here=$(cd "$(dirname "$0")" && pwd)

if command -v podman >/dev/null 2>&1; then engine=podman
elif command -v docker >/dev/null 2>&1; then engine=docker
else echo "neither podman nor docker is on PATH" >&2; exit 1
fi

net=()
case "${HTTPS_PROXY:-}${HTTP_PROXY:-}${https_proxy:-}${http_proxy:-}" in
	*127.0.0.1*|*localhost*) net=(--network=host) ;;
esac

echo "==> building $IMAGE for $PLATFORM ($engine)"
"$engine" build --pull=always --platform "$PLATFORM" "${net[@]}" \
	--build-arg HERMES_CHANNEL="${HERMES_CHANNEL:-stable}" --build-arg HERMES_COMMIT="${HERMES_COMMIT:-}" \
	-f "$here/Dockerfile" -t "$IMAGE" "$here"

if [ "${1:-}" = "--push" ]; then
	echo "==> pushing $IMAGE"
	"$engine" push "$IMAGE"
fi

cat <<NEXT

Next: $engine save --format oci-archive the image into the machine's sideload directory (as
fetch-image.sh does with CI's), then
    ARCHIVE=hermes-arm64.tar MACHINE=oect NODE_SSH=root@192.168.10.15 ./browser/hermes/hqnode/create.sh
    MACHINE=oect NODE_SSH=root@192.168.10.15 ./browser/hermes/hqnode/deploy.sh
NEXT
