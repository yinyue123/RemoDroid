#!/usr/bin/env python3
"""MCP on a computer, end to end (mcp.md): an AI assistant's tools reach a real desktop agent.

  Xvfb :N ── xprobe (four colour quadrants; prints button and key events)
     │
  remodroid-desktopd ──ws── remodroid-server ── POST /mcp/me/<computer> (JSON-RPC, an API token)

Checks the computer tool set, that a screenshot shows the screen (decoded with ffmpeg when it is
installed), that clicks, drags, wheel, keys and text arrive in X11 where the screenshot said, the
computer's own rules (view only, ask before each session), and that the server remembers a computer
while it is offline. Standard library only.

  python3 tests/e2e_desktop_mcp.py [build/host]
"""
import base64
import hashlib
import json
import os
import queue
import secrets
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
B = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build/host"))
passes = failures = 0


def ok(cond, name):
    global passes, failures
    if cond:
        passes += 1
        print("  ✓ " + name)
    else:
        failures += 1
        print("  ✗ " + name)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def free_display():
    for n in range(80, 200):
        if not os.path.exists(f"/tmp/.X11-unix/X{n}") and not os.path.exists(f"/tmp/.X{n}-lock"):
            return n
    raise RuntimeError("no free X display")


class Probe:
    """xprobe's JSON events, read on a thread."""

    def __init__(self, path, env):
        self.p = subprocess.Popen([path], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, env=env)
        self.events = []
        self.lock = threading.Lock()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.p.stdout:
            try:
                e = json.loads(line)
            except ValueError:
                continue
            with self.lock:
                self.events.append(e)

    def wait(self, pred, timeout=5):
        end = time.time() + timeout
        while time.time() < end:
            with self.lock:
                for e in self.events:
                    if pred(e):
                        self.events.remove(e)
                        return e
            time.sleep(0.05)
        return None

    def take(self, pred, settle=0.6):
        time.sleep(settle)
        with self.lock:
            got = [e for e in self.events if pred(e)]
            self.events = [e for e in self.events if not pred(e)]
        return got

    def drain(self):
        with self.lock:
            self.events.clear()


def http(method, url, body=None, headers=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method, headers=dict({"Content-Type": "application/json"}, **(headers or {})))
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.status, dict(r.headers), r.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read()


def main():
    d = tempfile.mkdtemp(prefix="remodroid-desktop-mcp-")
    home = f"{d}/home"
    os.makedirs(home)
    port, disp = free_port(), free_display()
    base = f"http://127.0.0.1:{port}"
    subprocess.check_call(["cc", "-O1", "-o", f"{d}/xprobe", f"{ROOT}/desktop/test/xprobe.c", "-lX11"])
    hp = lambda pw: subprocess.check_output([f"{B}/remodroid-server", "hashpw", pw]).decode().strip()
    token = "claude." + secrets.token_hex(24)
    open(f"{d}/server.ini", "w").write(f"""[http]
listen = 127.0.0.1:{port}
web_root = {ROOT}/web
[server]
state_dir = {d}/state
[users]
me = {hp('pw')}
[api_tokens]
claude = {hashlib.sha256(token.encode()).hexdigest()} user=me
""")
    open(f"{d}/desktop.ini", "w").write("[desktop]\nnotify = false\n")
    env = dict(os.environ, DISPLAY=f":{disp}", HOME=home, XDG_CONFIG_HOME=f"{home}/.config")
    for k in ("WAYLAND_DISPLAY", "XDG_SESSION_TYPE"):
        env.pop(k, None)
    procs = [subprocess.Popen(["Xvfb", f":{disp}", "-screen", "0", "1280x720x24", "-nolisten", "tcp"], stderr=subprocess.DEVNULL)]
    agent = None
    try:
        for _ in range(50):
            if os.path.exists(f"/tmp/.X11-unix/X{disp}"):
                break
            time.sleep(0.1)
        probe = Probe(f"{d}/xprobe", env)
        procs.append(probe.p)
        procs.append(subprocess.Popen([f"{B}/remodroid-server", "-c", f"{d}/server.ini"], stderr=subprocess.DEVNULL))
        time.sleep(0.3)
        st, h, body = http("POST", base + "/login", {"user": "me", "password": "pw"})
        cookie = h["Set-Cookie"].split(";")[0]
        code = json.loads(http("POST", base + "/api/v1/enroll/codes", {"name": "Test computer"}, {"Cookie": cookie})[2])["code"]
        pair = subprocess.run([f"{B}/desktop/remodroid-desktopd", "-c", f"{d}/desktop.ini", "pair", "--panel", base, "--code", code],
                              env=env, capture_output=True, text=True, timeout=30)
        ok(pair.returncode == 0, "the computer pairs with the panel")
        log = open(f"{d}/agent.log", "w")
        agent = subprocess.Popen([f"{B}/desktop/remodroid-desktopd", "-c", f"{d}/desktop.ini", "-v"], env=env, stderr=log, stdout=log)
        procs.append(agent)
        if not probe.wait(lambda e: e["ev"] == "ready", 10):
            raise RuntimeError("xprobe did not start")
        auth = {"Authorization": "Bearer " + token}
        dev = None
        for _ in range(100):
            devs = json.loads(http("GET", base + "/mcp/me/devices", headers=auth)[2])["devices"]
            dev = next((x for x in devs if x["online"]), None)
            if dev:
                break
            time.sleep(0.1)
        ok(dev and dev["platform"] == "linux" and dev["mcp"] is True, "MCP: the computer is listed, online, for assistants")
        url = dev["url"]
        rid = [0]

        def rpc(method, params=None):
            rid[0] += 1
            st, _, b = http("POST", url, {"jsonrpc": "2.0", "id": rid[0], "method": method, "params": params or {}}, auth)
            return json.loads(b)

        def call(name, args=None):
            return rpc("tools/call", {"name": name, "arguments": args or {}})["result"]

        m = rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "test"}})
        ok("computer" in m["result"]["instructions"] and "screenshot" in m["result"]["instructions"],
           "MCP: initialize tells the model it drives a computer through screenshots")
        names = sorted(t["name"] for t in rpc("tools/list")["result"]["tools"])
        ok(names == ["click", "drag", "move_mouse", "press_key", "screenshot", "scroll", "type_text"], "MCP: the computer's tools")

        r = call("screenshot")
        img = base64.b64decode(r["content"][0]["data"]) if r["content"][0]["type"] == "image" else b""
        ok(img[:3] == b"\xff\xd8\xff" and "1280x720 screenshot of a 1280x720 screen" in r["content"][1]["text"],
           "MCP: screenshot is a JPEG of the screen, 1280 px by default")
        if shutil.which("ffmpeg"):
            open(f"{d}/shot.jpg", "wb").write(img)
            raw = subprocess.run(["ffmpeg", "-v", "error", "-i", f"{d}/shot.jpg", "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
                                 capture_output=True).stdout
            px = lambda x, y: tuple(raw[(y * 1280 + x) * 3:(y * 1280 + x) * 3 + 3])
            near = lambda p, want: all(abs(a - b) < 40 for a, b in zip(p, want))
            ok(len(raw) == 1280 * 720 * 3 and near(px(320, 180), (255, 0, 0)) and near(px(960, 180), (0, 255, 0))
               and near(px(320, 540), (0, 0, 255)) and near(px(960, 540), (255, 255, 0)),
               "MCP: the screenshot shows the screen (four colour quadrants)")
        r = call("screenshot", {"max_edge": 640})
        ok("640x360 screenshot" in r["content"][1]["text"], "MCP: max_edge scales the screenshot")

        probe.drain()
        r = call("click", {"x": 480, "y": 90})  # screenshot pixels of the 640x360 shot: (960, 180) on screen
        e = probe.wait(lambda e: e["ev"] == "button" and e["down"] == 1)
        ok(not r["isError"] and e and e["b"] == 1 and abs(e["x"] - 960) <= 2 and abs(e["y"] - 180) <= 2,
           "MCP: click lands where the screenshot showed it")
        probe.drain()
        call("click", {"x": 300, "y": 300, "space": "display", "button": "right"})
        e = probe.wait(lambda e: e["ev"] == "button" and e["down"] == 1)
        ok(e and e["b"] == 3 and e["x"] == 300 and e["y"] == 300, "MCP: right click in screen pixels")
        probe.drain()
        call("click", {"x": 100, "y": 100, "space": "display", "count": 2})
        presses = probe.take(lambda e: e["ev"] == "button" and e["down"] == 1 and e["b"] == 1)
        ok(len(presses) == 2, "MCP: double click")
        probe.drain()
        call("scroll", {"direction": "down", "amount": 2, "x": 640, "y": 360, "space": "display"})
        wheel = probe.take(lambda e: e["ev"] == "button" and e["down"] == 1 and e["b"] == 5)
        ok(len(wheel) == 2 and wheel[0]["x"] == 640, "MCP: scroll down two notches over a point")
        probe.drain()
        r = call("drag", {"path": [{"x": 100, "y": 100}, {"x": 200, "y": 150}, {"x": 300, "y": 300}], "space": "display"})
        down = probe.wait(lambda e: e["ev"] == "button" and e["b"] == 1 and e["down"] == 1)
        up = probe.wait(lambda e: e["ev"] == "button" and e["b"] == 1 and e["down"] == 0)
        ok(not r["isError"] and down and up and (down["x"], down["y"]) == (100, 100) and (up["x"], up["y"]) == (300, 300),
           "MCP: drag presses at the first point and releases at the last")
        probe.drain()
        call("press_key", {"keys": "shift+1"})
        e = probe.wait(lambda e: e["ev"] == "key" and e["down"] == 1 and e["keysym"] == "exclam")
        ok(e is not None, "MCP: press_key shift+1 types !")
        probe.drain()
        call("press_key", {"keys": "enter", "count": 2})
        enters = probe.take(lambda e: e["ev"] == "key" and e["down"] == 1 and e["keysym"] == "Return")
        ok(len(enters) == 2, "MCP: press_key enter twice")
        probe.drain()
        call("type_text", {"text": "Hi"})
        typed = [e["keysym"] for e in probe.take(lambda e: e["ev"] == "key" and e["down"] == 1 and e["keysym"] in ("H", "i"))]
        ok(typed == ["H", "i"], "MCP: type_text")
        r = call("press_key", {"keys": "ctrl+nosuchkey"})
        ok(r["isError"] and "unknown key" in r["content"][0]["text"], "MCP: an unknown key is a tool error")
        r = call("click", {"x": 5000, "y": 5, "space": "display"})
        ok(r["isError"], "MCP: a click off the screen is a tool error")
        r = rpc("tools/call", {"name": "tap", "arguments": {"text": "OK"}})
        ok(r.get("error", {}).get("code") == -32602, "MCP: phone tools don't exist on a computer")

        cmd = lambda args: http("POST", f"{base}/api/v1/devices/{dev['id']}/cmd", {"cmd": "agent.config", "args": args}, {"Cookie": cookie})
        cmd({"desktop.view_only": True})
        r = call("click", {"x": 10, "y": 10, "space": "display"})
        shot = call("screenshot")
        ok(r["isError"] and "view only" in r["content"][0]["text"] and shot["content"][0]["type"] == "image",
           "MCP: a view-only computer can be looked at, not used")
        cmd({"desktop.view_only": False, "desktop.confirm": True})
        r = call("screenshot")
        ok(r["isError"] and "asks its user before every viewer" in r["content"][0]["text"],
           "MCP: a computer that asks before every viewer is off-limits to assistants")
        cmd({"desktop.confirm": False})

        agent.terminate()
        agent.wait(10)
        for _ in range(100):
            devs = json.loads(http("GET", base + "/mcp/me/devices", headers=auth)[2])["devices"]
            if not devs[0]["online"]:
                break
            time.sleep(0.1)
        names = sorted(t["name"] for t in rpc("tools/list")["result"]["tools"])
        ok(devs[0]["platform"] == "linux" and "click" in names, "the server remembers a computer while it is offline")
        r = call("screenshot")
        ok(r["isError"] and r["content"][0]["text"].startswith("offline"), "MCP: an offline computer is a tool error")
    finally:
        for p in reversed(procs):
            p.terminate()
        for p in procs:
            try:
                p.wait(5)
            except subprocess.TimeoutExpired:
                p.kill()
        if failures:
            print("---- agent ----\n" + open(f"{d}/agent.log").read()[-3000:] if os.path.exists(f"{d}/agent.log") else "")
        else:
            shutil.rmtree(d, ignore_errors=True)
    print(f"\n{passes} passed, {failures} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
