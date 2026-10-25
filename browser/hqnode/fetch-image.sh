#!/bin/bash
# Fetch the arm64 image that .github/workflows/browser-image.yml built, decrypt it and sideload it
# into the machine's hqnode, ready for create.sh (IMAGE_REF below) or a reinstall.
#
#     ./browser/hqnode/fetch-image.sh                 the newest successful run
#     RUN_ID=123456789 ./browser/hqnode/fetch-image.sh
#     NODE_SSH=root@192.168.10.15 KEY_FILE=… ./browser/hqnode/fetch-image.sh
#
# The artifact is encrypted because the image holds CloakBrowser, whose binary licence allows it in
# internal Docker images but not redistribution, and this repository's artifacts are public.
# KEY_FILE holds the same key as the BROWSER_IMAGE_KEY secret.
#
# Needs gh (logged in to the repository) and podman here, ssh to the machine.

set -euo pipefail

KEY_FILE="${KEY_FILE:-/root/.config/remodroid/browser-image.key}"
NODE_SSH="${NODE_SSH:-root@192.168.10.15}"
IMAGE_REF="${IMAGE_REF:-localhost/remote-browser-hqnode:arm64}"
ARCHIVE="${ARCHIVE:-remote-browser-arm64.tar}"
RUN_ID="${RUN_ID:-}"

[ -s "$KEY_FILE" ] || { echo "no key in $KEY_FILE" >&2; exit 1; }
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

if [ -z "$RUN_ID" ]; then
	RUN_ID=$(gh run list --workflow browser-image.yml --status success --limit 1 --json databaseId -q '.[0].databaseId')
	[ -n "$RUN_ID" ] || { echo "no successful browser-image run" >&2; exit 1; }
fi
echo "==> run $RUN_ID: downloading"
gh run download "$RUN_ID" -n remote-browser-arm64 -D "$work"
(cd "$work" && sha256sum -c remote-browser-arm64.tar.enc.sha256)

echo "==> decrypting"
openssl enc -d -aes-256-cbc -pbkdf2 -iter 200000 -pass file:"$KEY_FILE" \
	-in "$work/remote-browser-arm64.tar.enc" -out "$work/oci.tar"
rm "$work/remote-browser-arm64.tar.enc"

# buildx's OCI tarball names the image by tag only; podman re-saves it as an oci-archive whose ref is
# the full name, which is what hqnode's sideload import takes the image's name from.
echo "==> naming it $IMAGE_REF"
podman load -q -i "$work/oci.tar" | tee "$work/loaded"
loaded=$(sed -n 's/^Loaded image: //p' "$work/loaded" | head -1)
[ -n "$loaded" ] || { echo "podman loaded nothing" >&2; exit 1; }
[ "$loaded" = "$IMAGE_REF" ] || podman tag "$loaded" "$IMAGE_REF"
rm "$work/oci.tar"
podman save --format oci-archive -o "$work/$ARCHIVE" "$IMAGE_REF"

echo "==> sideloading onto $NODE_SSH"
# hqnode's API on the machine; its sideload directory is whatever its config says (images.sideload_dir,
# else <root>/upload), so ask it rather than assume.
api="curl -sS --noproxy '*' --unix-socket /run/hqnode.sock -H \"Authorization: Bearer \$(cat /etc/hqnode/cli-token)\" -H 'Content-Type: application/json'"
dir=$(ssh "$NODE_SSH" "$api http://localhost/v1/images/sideload" | python3 -c 'import json,sys; print(json.load(sys.stdin)["dir"])')
[ -n "$dir" ] || { echo "the machine's hqnode named no sideload directory" >&2; exit 1; }
echo "    into $dir"
ssh "$NODE_SSH" "mkdir -p '$dir' && cat > '$dir/$ARCHIVE.part' && mv '$dir/$ARCHIVE.part' '$dir/$ARCHIVE'" <"$work/$ARCHIVE"
ssh "$NODE_SSH" "$api -d '{\"file\":\"$ARCHIVE\"}' -X POST http://localhost/v1/images/sideload; echo"
echo "==> done: create with IMAGE_REF=$IMAGE_REF ./browser/hqnode/create.sh, or reinstall with archive $ARCHIVE"
