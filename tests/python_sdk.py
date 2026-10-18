#!/usr/bin/env python3
"""Tests the Python SDK, the rd CLI and the skill runner (exit codes, resume) against a
fake phone API that behaves like a tiny Settings app.   python3 tests/python_sdk.py"""
import io
import json
import os
import subprocess
import sys
import threading
from contextlib import redirect_stdout
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "python"))
from remodroid import ApiError, Client, NeedsAI, find, outline  # noqa: E402
from remodroid import cli  # noqa: E402

TOKEN = "test.secret"
JPEG = b"\xff\xd8\xff" + bytes(range(256)) * 100


class Phone:
    """screen: launcher → settings → search → results; dialog: a popup covers settings."""

    def __init__(self):
        self.reset()

    def reset(self, dialog=False, launch_works=True):
        self.screen, self.query, self.dialog, self.launch_works = "launcher", "", dialog, launch_works
        self.actions, self.ui_id = [], 0

    def ui(self):
        self.ui_id += 1
        n = lambda i, p, **kw: dict(id=i, p=p, b=[0, i * 100, 1080, i * 100 + 90], f=kw.pop("f", ["enabled"]), **kw)
        if self.screen == "launcher":
            pkg, nodes = "com.android.launcher3", [n(0, None, cls="FrameLayout"), n(1, 0, text="Settings", f=["click", "enabled"])]
        elif self.screen == "settings" and self.dialog:
            pkg, nodes = "com.android.settings", [n(0, None, cls="FrameLayout"), n(1, 0, text="Rate this app?"),
                                                  n(2, 0, text="Later", cls="Button", f=["click", "enabled"])]
        elif self.screen == "settings":
            pkg, nodes = "com.android.settings", [n(0, None, cls="FrameLayout"),
                                                  n(1, 0, rid="com.android.settings:id/search_action_bar",
                                                    desc="Search settings", f=["click", "enabled"])]
        else:
            pkg = "com.android.settings"
            nodes = [n(0, None, cls="FrameLayout"),
                     n(1, 0, rid="com.android.settings:id/search_src_text", cls="EditText", text=self.query or None,
                       f=["click", "edit", "enabled", "focused"])]
            if self.screen == "results":
                nodes += [n(2, 0, rid="com.android.settings.intelligence:id/list_results", f=["scroll", "enabled"]),
                          n(3, 2, rid="android:id/title", text="Wi‑Fi"), n(4, 2, rid="android:id/title", text="Wi‑Fi calling")]
        return {"schema": 1, "ui_id": self.ui_id, "package": pkg, "backend": "uiautomation", "collect_ms": 12,
                "display": {"w": 1080, "h": 2400, "rotation": 0, "epoch": 0}, "count": len(nodes), "nodes": nodes}

    def act(self, a):
        self.actions.append(a)
        kind, t = a.get("action"), a.get("target") or {}
        ui = self.ui()
        if kind == "launch":
            if self.launch_works:
                self.screen = "settings"
            return 200, {"ok": True, "injected": True}
        if kind == "wait":
            sel = a["wait"].get("selector") or {}
            hit = find(ui, **sel)
            return 200, {"ok": True, "injected": True, "verified": bool(hit) != (a["wait"]["for"] == "gone"),
                         "match": hit[0] if hit else None}
        if kind in ("tap", "set_text"):
            hit = find(ui, **t) if "x" not in t else [{"id": -1}]
            if not hit:
                return 404, {"error": "no_match", "msg": "no element matches the selector", "retryable": True}
            if kind == "tap" and t.get("text") == "Later":
                self.dialog = False
            elif kind == "tap" and "search_action_bar" in json.dumps(t):
                self.screen = "search"
            elif kind == "set_text":
                self.query, self.screen = a["text"], "results"
            res = {"ok": True, "injected": True}
            if a.get("wait"):
                res["verified"] = True
            return 200, res
        if kind in ("back", "home", "key"):
            return 200, {"ok": True, "injected": True}
        return 400, {"error": "unsupported", "msg": kind}


PHONE = Phone()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def reply(self, status, body, ctype="application/json", headers=None):
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        for k, v in (headers or {}).items():
            self.send_header(k, str(v))
        self.end_headers()
        self.wfile.write(data)

    def route(self):
        if self.headers.get("Authorization") != "Bearer " + TOKEN:
            return self.reply(401, {"error": "unauthorized", "msg": "token"})
        u = urlparse(self.path)
        if u.path == "/api/v1/devices":
            return self.reply(200, {"devices": [{"id": "router1", "online": True}]})
        pre = "/api/v1/devices/local"
        if not u.path.startswith(pre):
            return self.reply(404, {"error": "not_found", "msg": u.path})
        suffix, q = u.path[len(pre):], {k: v[0] for k, v in parse_qs(u.query).items()}
        if suffix == "/ui" and self.command == "GET":
            return self.reply(200, PHONE.ui())
        if suffix == "/screenshot" and self.command == "GET":
            Handler.last_query = q
            return self.reply(200, JPEG, "image/jpeg", {"X-RD-Width": 576, "X-RD-Height": 1280, "X-RD-Display-W": 1080,
                                                         "X-RD-Display-H": 2400, "X-RD-Epoch": 3, "X-RD-Backend": "mirror+c"})
        if suffix == "/actions" and self.command == "POST":
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            return self.reply(*PHONE.act(body))
        return self.reply(404, {"error": "not_found", "msg": suffix})

    do_GET = do_POST = route


passes = failures = 0


def ok(cond, name):
    global passes, failures
    if cond:
        passes += 1
        print("  ✓ " + name)
    else:
        failures += 1
        print("  ✗ " + name)


def skill(*args, env):
    p = subprocess.run([sys.executable, os.path.join(ROOT, "skills", "settings-search", "run.py"), *args],
                       capture_output=True, text=True, env=env, timeout=30)
    try:
        return p.returncode, json.loads(p.stdout)
    except ValueError:
        return p.returncode, {"stdout": p.stdout, "stderr": p.stderr}


def main():
    srv = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    url = f"http://127.0.0.1:{srv.server_address[1]}"
    env = dict(os.environ, RD_URL=url, RD_TOKEN=TOKEN, RD_DEVICE="local", PYTHONPATH=os.path.join(ROOT, "python"))

    print("client")
    c = Client(url, token=TOKEN)
    try:
        Client(url, token="wrong").ui()
        ok(False, "a wrong token raises ApiError")
    except ApiError as e:
        ok(e.status == 401 and e.code == "unauthorized", "a wrong token raises ApiError 401")
    PHONE.reset()
    PHONE.screen = "settings"
    ui = c.ui()
    ok(ui["package"] == "com.android.settings" and find(ui, rid="search_action_bar"), "ui() and find() by short rid")
    ok("#search_action_bar" in outline(ui) and 'desc="Search settings"' in outline(ui), "outline() shows ids and labels")
    shot = c.screenshot(max_edge=1280, quality=70)
    ok(shot.data == JPEG and shot.width == 576 and shot.epoch == 3 and shot.backend == "mirror+c",
       "screenshot() keeps the bytes in memory with X-RD geometry")
    ok(Handler.last_query == {"format": "jpeg", "max_edge": "1280", "quality": "70"}, "screenshot parameters in the query")
    ok(shot.point(288, 640) == {"x": 288, "y": 640, "space": "image", "w": 576, "h": 1280}, "Screenshot.point() targets image pixels")
    c.tap(rid="search_action_bar", step="open")
    ok(PHONE.actions[-1]["target"] == {"rid": "search_action_bar"} and c.journal[-1]["step"] == "open",
       "tap(rid=…) sends a selector and journals the step")
    try:
        c.tap(text="Nope", step="missing")
        ok(False, "a missing element raises NeedsAI")
    except NeedsAI as e:
        ok(e.step == "missing" and "no_match" in e.reason, "a missing element raises NeedsAI with the step")

    print("rd CLI")
    PHONE.reset()
    out = io.StringIO()
    with redirect_stdout(out):
        rc = cli.main(["--url", url, "--token", TOKEN, "ui"])
    ok(rc == 0 and out.getvalue().startswith("# com.android.launcher3") and '"Settings"' in out.getvalue(), "rd ui prints the outline")
    out = io.StringIO()
    with redirect_stdout(out):
        rc = cli.main(["--url", url, "--token", TOKEN, "tap", "--text", "Settings", "--wait-change"])
    ok(rc == 0 and PHONE.actions[-1]["wait"]["for"] == "ui_changed", "rd tap --text --wait-change")
    p = subprocess.run([sys.executable, "-m", "remodroid", "shot"], capture_output=True, env=env, timeout=30)
    ok(p.returncode == 0 and p.stdout == JPEG, "rd shot writes the image to stdout (no file)")

    print("skill runner")
    PHONE.reset()
    rc, out = skill("--query", "wifi", env=env)
    ok(rc == 0 and out["result"]["results"] == ["Wi‑Fi", "Wi‑Fi calling"], "a known run finishes (exit 0) with the result")
    ok([j["step"] for j in out["journal"]] == ["launch", "open_search", "enter_query"], "the journal lists the steps")

    PHONE.reset(dialog=True)
    rc, out = skill("--query", "wifi", env=env)
    ok(rc == 3 and out["needs_ai"] and out["step"] == "open_search", "an unknown dialog hands over to the AI (exit 3)")
    ok(any(n.get("text") == "Later" for n in out["ui"]["nodes"]) and out["completed"][0]["step"] == "launch",
       "the hand-over carries a fresh UI dump and the completed steps")
    PHONE.act({"action": "tap", "target": {"text": "Later"}})  # the AI deals with the dialog
    n_before = len(PHONE.actions)
    rc, out = skill("--query", "wifi", "--from-step", "open_search", env=env)
    sent = [a["action"] for a in PHONE.actions[n_before:]]
    ok(rc == 0 and "launch" not in sent and out["journal"][0] == {"step": "launch", "action": "launch", "skipped": True},
       "resuming with --from-step skips the steps already done")

    PHONE.reset(launch_works=False)
    rc, out = skill("--query", "wifi", env=env)
    ok(rc == 2 and out["entry_mismatch"], "a phone that is not in the entry state exits 2")

    srv.shutdown()
    print(f"\n{passes} passed, {failures} failed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
