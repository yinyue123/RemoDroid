"""Unlock this phone using its saved screen PIN or nine-dot pattern."""
import os
import sys
import time
import subprocess
import shlex

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "python"))

from remodroid import EntryMismatch, NeedsAI, find  # noqa: E402
from remodroid.skill import main  # noqa: E402


UNLOCK_SEQUENCE = "35789"  # Saved at the owner's explicit request.
LOCK_PACKAGE = "com.android.systemui"


def keypad_digit(ui, digit):
    """Find one enabled key by its visible digit, without using unstable node IDs."""
    candidates = [n for n in find(ui, text=digit) if "enabled" in n.get("f", [])]
    clickable = [n for n in candidates if "click" in n.get("f", [])]
    if len(clickable) == 1:
        return {"text": digit, "clickable": True}
    if len(candidates) == 1:
        return {"text": digit, "enabled": True}
    return None


def pattern_points(ui):
    """Map Android's nine pattern cells from a visible LockPatternView's bounds."""
    views = [n for n in ui.get("nodes", []) if "LockPatternView" in n.get("cls", "")
             or n.get("rid", "").endswith(":id/lockPatternView")]
    if len(views) != 1:
        return None
    bounds = views[0].get("b")
    if not bounds or len(bounds) != 4:
        return None
    left, top, right, bottom = bounds
    if right <= left or bottom <= top:
        return None
    points = []
    for char in UNLOCK_SEQUENCE:
        cell = int(char) - 1
        column, row = cell % 3, cell // 3
        points.append((left + (column + 0.5) * (right - left) / 3,
                       top + (row + 0.5) * (bottom - top) / 3))
    return points


def wait_until_unlocked(c):
    deadline = time.monotonic() + 4
    while time.monotonic() < deadline:
        ui = c.ui()
        if ui.get("package") and ui.get("package") != LOCK_PACKAGE:
            return {"unlocked": True}
        time.sleep(0.3)
    raise NeedsAI("phone is still on the lock screen; do not retry automatically", "verify_unlock")


def run(c):
    """Unlock the Android screen with the saved credential."""
    c.key("WAKEUP", step="wake")
    ui = c.ui()
    if ui.get("package") and ui.get("package") != LOCK_PACKAGE:
        return {"unlocked": True, "already_unlocked": True}
    if not ui.get("package"):
        raise EntryMismatch("foreground package unavailable; inspect the screen", "wake")

    # On the clock screen, a swipe reveals the credential view.
    if keypad_digit(ui, UNLOCK_SEQUENCE[0]) is None and pattern_points(ui) is None:
        display = ui.get("display") or {}
        width, height = display.get("w"), display.get("h")
        if not width or not height:
            raise EntryMismatch("lock screen has no display geometry", "show_keypad")
        c.swipe((width / 2, height * 0.82), (width / 2, height * 0.30),
                step="show_keypad")
        ui = c.ui()

    if ui.get("package") != LOCK_PACKAGE:
        raise EntryMismatch("screen changed before credential entry", "show_keypad")

    points = pattern_points(ui)
    if points is not None:
        if c.skipping:
            raise NeedsAI("do not resume partway through an unlock attempt", "draw_pattern")
        if c.capabilities().get("viewer_active"):
            raise NeedsAI("a person is using the phone", "draw_pattern")
        commands = []
        for index, (x, y) in enumerate(points):
            event = "DOWN" if index == 0 else "MOVE"
            commands.append(f"input touchscreen motionevent {event} {round(x)} {round(y)}")
        x, y = points[-1]
        commands.append(f"input touchscreen motionevent UP {round(x)} {round(y)}")
        remote = ("/koolshare/remodroid/bin/remodroid-agent -c "
                  "/jffs/configs/remodroid/agent.ini shell " + shlex.quote("; ".join(commands)))
        try:
            subprocess.run(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8",
                            "admin@192.168.10.3", remote], check=True,
                           capture_output=True, timeout=20)
        except (subprocess.SubprocessError, OSError):
            raise NeedsAI("pattern submission uncertain; inspect screen before any retry", "draw_pattern")
        c.journal.append({"step": "draw_pattern", "side_effect": True, "ok": True})
        return wait_until_unlocked(c)

    for digit in set(UNLOCK_SEQUENCE):
        if keypad_digit(ui, digit) is None:
            raise NeedsAI("PIN keypad or pattern grid is not recognizable; inspect the screen", "credential_screen")

    for index, digit in enumerate(UNLOCK_SEQUENCE):
        selector = keypad_digit(ui, digit)
        if selector is None:
            raise NeedsAI("PIN keypad changed during entry", "enter_pin")
        c.tap(step=f"pin_digit_{index + 1}", **selector)
        if index < len(UNLOCK_SEQUENCE) - 1:
            ui = c.ui()
            if ui.get("package") != LOCK_PACKAGE:
                raise NeedsAI("lock screen changed before all PIN digits were entered", "enter_pin")

    ui = c.ui()
    if ui.get("package") == LOCK_PACKAGE:
        for selector in ({"rid": "key_enter"}, {"rid": "key_enter_text"},
                         {"desc": "Enter"}, {"desc": "确认"}):
            if len(find(ui, **selector)) == 1:
                c.tap(step="submit_pin", **selector)
                break

    return wait_until_unlocked(c)


if __name__ == "__main__":
    from connection import setup
    setup()
    main(run)
