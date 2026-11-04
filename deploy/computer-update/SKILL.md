---
name: computer-update
description: Install or update the RemoDroid desktop app on the owner's computers (Mac, Windows, Linux, Steam Deck) with the deploy/update_* scripts, which also set up permissions, the input service, the firewall and gamepads. Use when asked to install, update, reinstall or deploy RemoDroid on a computer, or when a computer lost permissions after an update.
metadata:
  scripts: deploy/update_mac.sh, deploy/update_windows.bat, deploy/update_linux.sh, deploy/update_deck.sh
  revision: "2"
---

# Install or update RemoDroid on a computer

There is one script per kind of computer, in `deploy/`. Each one does the same steps:

- It looks up the latest release. `https://github.com/yinyue123/RemoDroid/releases/latest`
  redirects to `…/tag/vX.Y.Z`, so no API rate limit applies.
- It installs the release, or the version or file you name.
- It restarts the app the way it ran before, then checks `desktop.log` for "connected to the server".
- Pairing and settings stay as they are.

Common arguments: none for the latest, `0.6.2` for that release, a file path for a build you already
have, `--force` to reinstall the same version.

| Computer | Who runs it, where | Script |
|---|---|---|
| Steam Deck (deck@192.168.10.52) | **you**, from this machine | `./deploy/update_deck.sh` |
| Music's MacBook Air | the owner, in Terminal on the Mac | `./deploy/update_mac.sh` |
| win11 | the owner, double-click on the PC | `deploy\update_windows.bat` |
| any other Linux desktop | the owner, in a terminal on it | `./deploy/update_linux.sh` |

Only the Deck can be reached from here. For the others, give the owner the command; don't try to
run it on their machine for them.

## Steam Deck: `update_deck.sh`

1. It downloads `RemoDroid-Computer-<ver>-linux-x86_64.AppImage` here and streams it over ssh into
   `~/Applications/RemoDroid-<ver>-linux-x86_64.AppImage` on the Deck.
2. It points `~/Applications/RemoDroid.AppImage` at that file. Desktop mode's autostart also starts
   that link.
3. It restarts the agent as the transient user unit `remodroid-desktop`, with the DISPLAY and
   XDG_CURRENT_DESKTOP the old agent had: gamescope in Game mode, KDE in Desktop mode.
4. If the new build doesn't stay up, it goes back to the previous one. It keeps the new build and the
   previous one, and deletes older `RemoDroid-*.AppImage` files.

For ssh, it uses key login. When plain ssh can't get through (a proxy's TUN device), it falls back to
`nsenter -t 1 -n` as root. `DECK=user@host` picks another Deck.

The install half runs on the Deck as `bash -s -- --install NAME`, with the script fed in on stdin.
That way no remote command line contains `remodroid-desktop`, and its `pkill -f` can't kill its own
shell.

## Mac: `update_mac.sh`

- **Signing.** Release builds are signed ad hoc (multi-platform.md §13.4), so macOS drops Screen
  Recording and Accessibility after every update. The script re-signs the app with a certificate
  made on the Mac the first time, `RemoDroid Local Signing`. It lives in its own keychain under
  `~/Library/Application Support/RemoDroid/signing/`, with a random password kept in a file there.
  macOS then sees each update as the same app.
- **Install.** It quits the app and installs it to `/Applications/RemoDroid.app`. It uses sudo only
  if that folder isn't writable.
- **Old toggles.** The first time the signature changes, it runs `tccutil reset` on both
  permissions, so the old, no longer valid toggles go away.
- **Permission check.** It restarts the app with `--background` and reads the system log. At launch
  the app logs `RemoDroid: <ScreenCapture|Accessibility> is off for this build` for each one it
  lacks. While one is missing, the owner clicks "Open Settings" in the RemoDroid window and switches
  RemoDroid on, then presses Enter. `s` skips.
- **If signing fails,** it warns and keeps the ad hoc signature. To check which signature is on the
  app: `codesign -dvv /Applications/RemoDroid.app 2>&1 | grep Authority` should say
  `RemoDroid Local Signing`.

## Windows: `update_windows.bat`

The file is both a batch file and a PowerShell script: the batch part runs the PowerShell part.
Keep its CRLF line ends; `deploy/.gitattributes` marks `*.bat` as `-text` so Git doesn't change them.

- **Where it installs.** It updates the copy that is running, or else the one the HKCU Run entry
  starts. Otherwise it installs to `%LOCALAPPDATA%\Programs\RemoDroid`, and records the version in
  `version.txt` there.
- **One UAC prompt** covers the steps that need an administrator:
  - `remodroid-input.exe install` for the RemoDroidInput service, so RemoDroid can control elevated
    windows and the lock screen. It asks the first time; once the service exists it is refreshed
    after each update.
  - An inbound Windows Firewall Allow rule for `RemoDroid.exe`, which helps peer-to-peer
    connections. It first removes any Block rule left by Cancel on Windows' first alert.
  - Copying the files, when the install folder needs an administrator.
- **Start at login.** It sets the HKCU Run entry for a new install, and repoints an existing one if
  the folder changed. It also adds a Start menu shortcut.

## Linux: `update_linux.sh`

- **Install.** It installs the .deb with `apt-get` or the .rpm with dnf, zypper or yum. Without
  those, or with `--appimage`, it installs the AppImage to `~/Applications`, as on the Deck. An
  install that is already an AppImage stays one.
- **Gamepads.** If `/dev/uinput` isn't writable, it asks for sudo and adds
  `/etc/udev/rules.d/60-remodroid-uinput.rules` (a `uaccess` tag, as Steam's steam-devices rules do),
  and loads the uinput module at boot.
- **Restart.** It restarts the `remodroid-desktopd` user service or the tray app, whichever was
  running.
- **Wayland.** It warns on Wayland: capture needs an X11 session.

## A build that isn't released yet

The scripts install releases. To install a commit from main without tagging, take the files from
that run's `packages` artifact and pass one to the script. The artifact holds every computer's
build: the Mac zip, the Windows zips, the Linux .deb, .rpm and AppImage. Their version is
`0.1.<run number>`.

```sh
gh run download <run id> -n packages -D build/ci   # run inside the repo
./deploy/update_deck.sh build/ci/RemoDroid-Computer-<ver>-linux-x86_64.AppImage
```

Tagging a release is the owner's call: ask first.

## Logs

- Mac: `~/Library/Application Support/RemoDroid/desktop.log`
- Linux and Deck: `~/.config/remodroid/desktop.log`
- Windows: `%APPDATA%\RemoDroid\desktop.log`
- Server, rm.11100.top: `ssh tw 'hqnode exec remodroid-tw sh -c "tail /var/log/remodroid-server.log"'`

Written 2026-11-04. The Mac, Windows and Linux scripts haven't run on real machines yet; their first
run there is the test.
