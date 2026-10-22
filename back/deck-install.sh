#!/bin/sh
# RemoDroid on a Steam Deck (Desktop Mode), as the deck user; no root, SteamOS's read-only system untouched.
#   ssh deck@192.168.10.52 'sh -s' < back/deck-install.sh   (one release: 'REMODROID_VERSION=0.4.6 sh -s')
# Puts the latest release's AppImage in ~/Applications (RemoDroid.AppImage points at it), adds the app
# menu entry and its icon, and starts it in the tray at every Desktop Mode login.
set -eu
REPO=yinyue123/RemoDroid
VERSION=${REMODROID_VERSION:-latest}
case "$(uname -m)" in x86_64) ARCH=x86_64 ;; aarch64) ARCH=aarch64 ;; *) echo "unsupported CPU $(uname -m)" >&2; exit 1 ;; esac

if [ "$VERSION" = latest ]; then API=https://api.github.com/repos/$REPO/releases/latest
else API=https://api.github.com/repos/$REPO/releases/tags/v$VERSION; fi
URL=$(curl -fsSL "$API" | sed -n "s/.*\"browser_download_url\": *\"\([^\"]*-linux-$ARCH\.AppImage\)\".*/\1/p" | head -n1)
[ -n "$URL" ] || { echo "no AppImage for $ARCH in $API" >&2; exit 1; }
SUMS=$(dirname "$URL")/SHA256SUMS
NAME=$(basename "$URL")

APPS=$HOME/Applications
mkdir -p "$APPS"
echo "▸ downloading $NAME"
curl -fsSL -o "$APPS/$NAME.part" "$URL"
WANT=$(curl -fsSL "$SUMS" | awk -v n="$NAME" '$2 == n { print $1 }')
GOT=$(sha256sum "$APPS/$NAME.part" | cut -d' ' -f1)
[ -n "$WANT" ] && [ "$WANT" = "$GOT" ] || { rm -f "$APPS/$NAME.part"; echo "checksum mismatch for $NAME" >&2; exit 1; }
mv "$APPS/$NAME.part" "$APPS/$NAME"
chmod +x "$APPS/$NAME"
ln -sfn "$NAME" "$APPS/RemoDroid.AppImage"

# icons and the menu entry, from the AppImage itself
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
(cd "$TMP" && "$APPS/RemoDroid.AppImage" --appimage-extract >/dev/null)
ICONS=$HOME/.local/share/icons/hicolor
mkdir -p "$ICONS"
if [ -d "$TMP/squashfs-root/usr/share/icons/hicolor" ]; then cp -r "$TMP/squashfs-root/usr/share/icons/hicolor/." "$ICONS/"
else cp -r "$TMP/squashfs-root/usr/share/icons/." "$ICONS/"; fi   # releases before the icon fix
mkdir -p "$HOME/.local/share/applications" "$HOME/.config/autostart"
cat > "$HOME/.local/share/applications/remodroid-desktop.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=RemoDroid
GenericName=Remote desktop
Comment=Control this computer from your RemoDroid panel
Exec="$APPS/RemoDroid.AppImage"
Icon=remodroid
Terminal=false
Categories=Network;RemoteAccess;
StartupNotify=false
EOF
cat > "$HOME/.config/autostart/remodroid-desktop.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=RemoDroid
Comment=Remote control of this computer
Exec="$APPS/RemoDroid.AppImage" --background
Icon=remodroid
X-GNOME-Autostart-enabled=true
EOF
command -v update-desktop-database >/dev/null && update-desktop-database "$HOME/.local/share/applications" 2>/dev/null || true

echo "✓ installed $NAME in $APPS"
[ -w /dev/uinput ] && echo "✓ gamepads: /dev/uinput is writable" || echo "! gamepads: /dev/uinput isn't writable for $(id -un) (log in at the Deck's screen in Desktop Mode)"
echo "  Pair it: Desktop Mode → RemoDroid in the app menu, paste the line from the panel (Agents → Add → Computer),"
echo "  or: ~/Applications/RemoDroid.AppImage desktopd pair --panel URL --code CODE"
