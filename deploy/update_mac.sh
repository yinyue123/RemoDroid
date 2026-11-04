#!/bin/bash
# Install or update RemoDroid on this Mac, and keep its permissions across updates.
#
#   ./update_mac.sh            the latest release
#   ./update_mac.sh 0.6.2      that release
#   ./update_mac.sh some.zip   a RemoDroid-Computer-…-macos.zip you already have
#   ./update_mac.sh --force    reinstall even when this version is installed
#
# Release builds are signed ad hoc, so each one looks like a new app to macOS and loses the Screen
# Recording and Accessibility switches (multi-platform.md §13.4). This script signs the app with a
# certificate of this Mac's own instead, made the first time and kept in its own keychain
# (~/Library/Application Support/RemoDroid/signing). macOS then sees every update as the same app:
# switch the two permissions on once, and later updates keep them. Your pairing and settings in
# ~/Library/Application Support/RemoDroid are left alone.
set -euo pipefail

REPO=yinyue123/RemoDroid
BUNDLE_ID=com.remodroid.desktop
DEST=/Applications/RemoDroid.app
SIGN_DIR="$HOME/Library/Application Support/RemoDroid/signing"
KC="$SIGN_DIR/remodroid-signing.keychain-db"
PASSF="$SIGN_DIR/keychain.pass"
CN="RemoDroid Local Signing"

say() { printf '\033[1m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[33m!!\033[0m %s\n' "$*" >&2; }
die() { printf '\033[31mxx\033[0m %s\n' "$*" >&2; exit 1; }

[ "$(uname -s)" = Darwin ] || die "this is for macOS"

force=0 want=""
for a in "$@"; do
    case "$a" in
    -f | --force) force=1 ;;
    -h | --help) sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) want="$a" ;;
    esac
done

tmp=$(mktemp -d -t remodroid-update)
trap 'rm -rf "$tmp"' EXIT

installed=""
[ -d "$DEST" ] && installed=$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" "$DEST/Contents/Info.plist" 2>/dev/null || true)

# ---------------- which build ----------------
zip="" ver=""
if [ -n "$want" ] && [ -f "$want" ]; then
    zip="$want"
    ver=$(basename "$zip" | sed -n 's/^RemoDroid-Computer-\(.*\)-macos\.zip$/\1/p')
else
    if [ -n "$want" ]; then
        ver="${want#v}"
    else
        say "Looking up the latest release"
        # the release page redirects to …/releases/tag/vX.Y.Z (no API rate limit)
        url=$(curl -fsSL -o /dev/null -w '%{url_effective}' "https://github.com/$REPO/releases/latest") ||
            die "can't reach github.com (set https_proxy if you need a proxy)"
        ver="${url##*/tag/v}"
        [ "$ver" != "$url" ] && [ -n "$ver" ] || die "no release found at $url"
    fi
fi
say "Installed: ${installed:-none}   wanted: ${ver:-from $zip}"

# ---------------- the signing identity (made once) ----------------
make_identity() {
    say "Making this Mac's signing certificate ($CN), once"
    mkdir -p "$SIGN_DIR"
    chmod 700 "$SIGN_DIR"
    local pass
    pass=$(/usr/bin/openssl rand -hex 16)
    (umask 077 && printf '%s' "$pass" >"$PASSF")
    rm -f "$KC"
    security create-keychain -p "$pass" "$KC"
    security set-keychain-settings "$KC" # no auto-lock
    cat >"$tmp/cert.cnf" <<EOF
[req]
distinguished_name = dn
x509_extensions = ext
prompt = no
[dn]
CN = $CN
[ext]
basicConstraints = critical, CA:false
keyUsage = critical, digitalSignature
extendedKeyUsage = critical, codeSigning
EOF
    # macOS's own openssl (LibreSSL): its .p12 is one `security import` reads, unlike OpenSSL 3's
    /usr/bin/openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 7300 -config "$tmp/cert.cnf" \
        -keyout "$tmp/key.pem" -out "$tmp/cert.pem" 2>/dev/null
    /usr/bin/openssl pkcs12 -export -inkey "$tmp/key.pem" -in "$tmp/cert.pem" -name "$CN" \
        -out "$tmp/id.p12" -passout "pass:$pass"
    security import "$tmp/id.p12" -k "$KC" -P "$pass" -T /usr/bin/codesign >/dev/null
    security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "$pass" "$KC" >/dev/null
    rm -f "$tmp/key.pem" "$tmp/id.p12"
}

identity() { # the certificate's SHA-1, as codesign takes it; empty when there is none
    [ -f "$KC" ] && [ -f "$PASSF" ] || return 0
    security unlock-keychain -p "$(cat "$PASSF")" "$KC" 2>/dev/null || return 0
    # codesign looks for identities in the keychain search list: add ours, keeping the others
    if ! security list-keychains -d user | grep -qF "$KC"; then
        local list=() l
        while IFS= read -r l; do
            l="${l#"${l%%[![:space:]]*}"}"
            l="${l#\"}"
            l="${l%\"}"
            [ -n "$l" ] && list+=("$l")
        done < <(security list-keychains -d user)
        security list-keychains -d user -s ${list[@]+"${list[@]}"} "$KC"
    fi
    security find-certificate -c "$CN" -Z "$KC" 2>/dev/null | awk '/SHA-1 hash:/ { print $3; exit }'
}

id=$(identity)
if [ -z "$id" ]; then
    make_identity
    id=$(identity)
fi
[ -n "$id" ] || warn "couldn't make a signing certificate: this build keeps its ad hoc signature, and macOS will ask for permissions again after each update"

signed_by_us() { [ -d "$1" ] && codesign -dvv "$1" 2>&1 | grep -qF "Authority=$CN"; }

# ---------------- download, or reuse what is installed ----------------
need_install=1
if [ -z "$zip" ] && [ "$installed" = "$ver" ] && [ "$force" = 0 ]; then
    if [ -z "$id" ] || signed_by_us "$DEST"; then
        need_install=0
        say "RemoDroid $ver is already installed"
    fi
fi

if [ "$need_install" = 1 ]; then
    mkdir -p "$tmp/x"
    if [ -z "$zip" ] && [ "$installed" = "$ver" ] && [ "$force" = 0 ]; then
        say "Re-signing the installed RemoDroid $ver"
        ditto "$DEST" "$tmp/x/RemoDroid.app"
    else
        if [ -z "$zip" ]; then
            name="RemoDroid-Computer-$ver-macos.zip"
            say "Downloading $name"
            curl -fL --retry 3 --progress-bar -o "$tmp/$name" "https://github.com/$REPO/releases/download/v$ver/$name" ||
                die "download failed: https://github.com/$REPO/releases/download/v$ver/$name"
            zip="$tmp/$name"
        fi
        ditto -x -k "$zip" "$tmp/x"
    fi
    app=$(find "$tmp/x" -maxdepth 3 -name RemoDroid.app -type d | head -n 1)
    [ -n "$app" ] || die "no RemoDroid.app in $zip"
    xattr -dr com.apple.quarantine "$app" 2>/dev/null || true

    # sign the parts first, then the bundle (as desktop/app/app.mk does ad hoc)
    sign() { codesign --force --timestamp=none --keychain "$KC" --sign "$id" "$@" 2>"$tmp/codesign.err"; }
    if [ -n "$id" ]; then
        say "Signing with this Mac's certificate"
        ok=1
        for f in "$app"/Contents/Frameworks/* "$app"/Contents/MacOS/*; do
            [ "$f" = "$app/Contents/MacOS/RemoDroid" ] && continue
            [ -f "$f" ] && { sign "$f" || ok=0; }
        done
        [ "$ok" = 1 ] && sign --identifier "$BUNDLE_ID" "$app" || ok=0
        if [ "$ok" = 1 ] && codesign --verify --deep --strict "$app" 2>/dev/null; then
            :
        else
            warn "signing failed: $(cat "$tmp/codesign.err"); keeping the ad hoc signature"
            for f in "$app"/Contents/Frameworks/* "$app"/Contents/MacOS/*; do
                [ "$f" = "$app/Contents/MacOS/RemoDroid" ] || codesign --force --sign - "$f" 2>/dev/null || true
            done
            codesign --force --sign - --identifier "$BUNDLE_ID" "$app"
            id=""
        fi
    fi

    # switches from a build signed some other way don't count for this one: clear them, so macOS asks anew
    if [ -n "$id" ] && ! signed_by_us "$DEST"; then
        fresh_permissions=1
    else
        fresh_permissions=0
    fi

    say "Quitting RemoDroid"
    osascript -e "tell application id \"$BUNDLE_ID\" to quit" >/dev/null 2>&1 || true
    for _ in 1 2 3 4 5 6 7 8 9 10; do pgrep -x RemoDroid >/dev/null || break; sleep 1; done
    pkill -x RemoDroid 2>/dev/null || true

    say "Installing to $DEST"
    SUDO=""
    [ -w /Applications ] && { [ ! -e "$DEST" ] || [ -w "$DEST" ]; } || SUDO=sudo
    $SUDO rm -rf "$DEST"
    $SUDO ditto "$app" "$DEST"

    if [ "$fresh_permissions" = 1 ]; then
        say "Clearing the old builds' permission switches"
        tccutil reset ScreenCapture "$BUNDLE_ID" >/dev/null 2>&1 || true
        tccutil reset Accessibility "$BUNDLE_ID" >/dev/null 2>&1 || true
    fi
fi

# ---------------- permissions ----------------
quit_app() {
    osascript -e "tell application id \"$BUNDLE_ID\" to quit" >/dev/null 2>&1 || true
    for _ in 1 2 3 4 5 6 7 8 9 10; do pgrep -x RemoDroid >/dev/null || return 0; sleep 1; done
    pkill -x RemoDroid 2>/dev/null || true
}

# The app checks both permissions at launch and logs the ones it lacks ("… is off for this build").
missing() {
    quit_app
    local since
    since=$(date '+%Y-%m-%d %H:%M:%S')
    open -g "$DEST" --args --background
    sleep 4
    log show --start "$since" --style compact \
        --predicate 'process == "RemoDroid" AND eventMessage CONTAINS "is off for this build"' 2>/dev/null |
        sed -n 's/.*RemoDroid: \([A-Za-z]*\) is off for this build.*/\1/p' | sort -u | tr '\n' ' '
}

say "Checking permissions"
gaps=$(missing)
while [ -n "${gaps// /}" ]; do
    echo
    for g in $gaps; do
        case "$g" in
        ScreenCapture) echo "  ✗ Screen Recording (to show the screen)" ;;
        Accessibility) echo "  ✗ Accessibility (for the mouse and keyboard)" ;;
        esac
    done
    if [ ! -t 0 ]; then
        warn "run this script in Terminal to be walked through the permissions"
        break
    fi
    cat <<'EOF'

  In the RemoDroid window that opens now:
    1. Click "Open Settings" next to each ✗ line. macOS asks, or opens System Settings.
    2. Switch RemoDroid on in that list (if it isn't listed, click + and pick /Applications/RemoDroid.app).
       macOS may ask for your password, or offer "Quit & Reopen": either is fine.
  You only do this once: later updates through this script keep the switches.

EOF
    quit_app
    open "$DEST"
    printf 'Press Enter when both are on (or type s to skip): '
    read -r answer || answer=s
    [ "$answer" = s ] && break
    gaps=$(missing)
done
[ -z "${gaps// /}" ] && say "Screen Recording and Accessibility are on"

final=$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" "$DEST/Contents/Info.plist" 2>/dev/null || echo "?")
if signed_by_us "$DEST"; then
    say "RemoDroid $final is running, signed with this Mac's certificate: later updates keep its permissions"
else
    say "RemoDroid $final is running (ad hoc signature: macOS asks for permissions after each update)"
fi
