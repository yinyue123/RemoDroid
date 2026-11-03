#!/bin/bash
# Fetch the arm64 image that .github/workflows/hermes-image.yml built and sideload it into the machine's
# hqnode, ready for create.sh (ARCHIVE=hermes-arm64.tar) or reinstall.sh. browser/hqnode/fetch-image.sh
# without the encryption (Hermes is MIT and the image holds no secrets) and without podman here: the
# workflow already saved it as an oci-archive named localhost/hermes-hqnode:arm64.
#
#     ./browser/hermes/hqnode/fetch-image.sh                 the newest successful run
#     RUN_ID=123456789 ./browser/hermes/hqnode/fetch-image.sh
#     NODE_SSH=root@192.168.10.15 SSH='nsenter -t 1 -n ssh' ./browser/hermes/hqnode/fetch-image.sh
#
# Needs gh (logged in to the repository) here, ssh to the machine.

set -euo pipefail

NODE_SSH="${NODE_SSH:-root@192.168.10.15}"
ARCHIVE="${ARCHIVE:-hermes-arm64.tar}"
RUN_ID="${RUN_ID:-}"
read -r -a SSH_CMD <<< "${SSH:-ssh}"
node_ssh() { "${SSH_CMD[@]}" -o BatchMode=yes -o ConnectTimeout=20 "$NODE_SSH" "$@"; }

# On disk, not /tmp: the artifact is 1.2 GB zipped and as much again unpacked, and /tmp is often a
# small tmpfs. gh stages its download in TMPDIR, so that goes there too.
WORK_ROOT="${WORK_ROOT:-/var/tmp}"
work=$(mktemp -d -p "$WORK_ROOT" hermes-fetch.XXXXXX)
trap 'rm -rf "$work"' EXIT
export TMPDIR="$work"

if [ -z "$RUN_ID" ]; then
	RUN_ID=$(gh run list --workflow hermes-image.yml --status success --limit 1 --json databaseId -q '.[0].databaseId')
	[ -n "$RUN_ID" ] || { echo "no successful hermes-image run" >&2; exit 1; }
fi
echo "==> run $RUN_ID: downloading"
gh run download "$RUN_ID" -n hermes-arm64 -D "$work"
(cd "$work" && sha256sum -c hermes-arm64.tar.sha256 && cat hermes-commit)
[ "$ARCHIVE" = hermes-arm64.tar ] || mv "$work/hermes-arm64.tar" "$work/$ARCHIVE"

echo "==> sideloading onto $NODE_SSH"
# hqnode's API on the machine; its sideload directory is whatever its config says (images.sideload_dir,
# else <root>/upload), so ask it rather than assume.
api="curl -sS --noproxy '*' --unix-socket /run/hqnode.sock -H \"Authorization: Bearer \$(cat /etc/hqnode/cli-token)\" -H 'Content-Type: application/json'"
dir=$(node_ssh "$api http://localhost/v1/images/sideload" | python3 -c 'import json,sys; print(json.load(sys.stdin)["dir"])')
[ -n "$dir" ] || { echo "the machine's hqnode named no sideload directory" >&2; exit 1; }
echo "    into $dir"
node_ssh "mkdir -p '$dir' && cat > '$dir/$ARCHIVE.part' && mv '$dir/$ARCHIVE.part' '$dir/$ARCHIVE'" <"$work/$ARCHIVE"
node_ssh "$api -d '{\"file\":\"$ARCHIVE\"}' -X POST http://localhost/v1/images/sideload; echo"
echo "==> done: create with ARCHIVE=$ARCHIVE ./browser/hermes/hqnode/create.sh, or ./browser/hermes/hqnode/reinstall.sh"
