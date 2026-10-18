---
name: phone-unlock
description: Unlock the owner's Sony SO-51A through the existing RemoDroid interfaces, using the saved PIN or nine-dot pattern. Use when asked to wake or unlock this phone.
metadata:
  package: com.android.systemui
  app_version: SO-51A Android 12, verified 2026-10-18
  revision: "2"
---

# Unlock this phone

```sh
python3 skills/phone-unlock/run.py
```

The owner explicitly requested saving the password: `UNLOCK_SEQUENCE = "35789"` in `run.py`. Do not ask for it again.

`connection.py` enters PID 1's network namespace automatically, reuses or starts the SSH tunnel to `sz`, and uses device `rt-ac86u-89d0`. The existing API token is saved in `/root/.config/remodroid/phone-token` (mode 0600). Environment overrides `RD_URL`, `RD_TOKEN`, and `RD_DEVICE` remain available.

For ordinary phone operations, use the configured rd entrypoint:

```sh
python3 skills/phone-unlock/connection.py caps
python3 skills/phone-unlock/connection.py ui
python3 skills/phone-unlock/connection.py shot -o /tmp/phone.jpg
```

The script wakes the screen through the HTTP API, opens the credential view, and derives the nine grid centers from the current `lockPatternView` bounds. For a pattern, the existing router agent's management `shell` interface sends one DOWN, then MOVE events, then UP. This was verified to unlock the phone. It invokes no adb executable and requires no C/Java changes or deployment. Numeric PIN keypads use the HTTP API.

Checks: known system lock screen before credential entry; fresh pattern bounds; no active human input; after submission, a nonempty foreground app package other than System UI. A failed or uncertain submission exits 3 and must not be retried automatically. Do not resume in the middle of a credential attempt.

The pattern was successfully executed against the real phone on 2026-10-18. The saved runner was also exercised while already unlocked, without changing the ongoing WeChat conversation. The owner reserves complex C/Java changes for Claude; keep future work on this skill within the existing interfaces.
