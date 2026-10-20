#!/usr/bin/env python3
"""Desktop end-to-end test (multi-platform.md §14): the real web client in
headless Chrome controlling a Linux desktop through remodroid-server and
remodroid-desktopd (libremodroid.so: the shared WebRTC stack in "embed" mode,
X11 capture, software H.264, XTest input, X11 clipboard).

  Xvfb :N ── xprobe (four colour quadrants; prints input and clipboard events)
     ▲
  remodroid-desktopd ──ws──▶ remodroid-server ◀──ws── Chrome (web/app.js) ◀── WebRTC ──┘

Checks: pairing with a one-time code from the panel (remodroid-desktopd pair);
the device shows up as a Linux computer; video decodes and shows the
quadrant colours in the right places; a click lands at the right X pixel; the
wheel scrolls; physical keys arrive as the expected keycodes; text from the
Keyboard sheet is typed; the clipboard syncs both ways; an upload lands in
~/Downloads/RemoDroid.

  python tests/e2e_desktop.py --bin build/host [--chrome PATH] [--verbose]
"""
import argparse
import asyncio
import json
import os
import queue
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

from playwright.async_api import async_playwright

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def free_display():
    for n in range(90, 200):
        if not os.path.exists(f"/tmp/.X11-unix/X{n}") and not os.path.exists(f"/tmp/.X{n}-lock"):
            return n
    raise RuntimeError("no free X display number")


class Probe:
    """xprobe's JSON events, read on a thread."""

    def __init__(self, path, env):
        self.p = subprocess.Popen([path], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, env=env)
        self.q = queue.Queue()
        self.events = []
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.p.stdout:
            try:
                e = json.loads(line)
            except ValueError:
                continue
            self.events.append(e)
            self.q.put(e)

    def send(self, line):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()

    async def wait(self, pred, timeout=10):
        end = time.time() + timeout
        while time.time() < end:
            for e in list(self.events):
                if pred(e):
                    self.events.remove(e)
                    return e
            await asyncio.sleep(0.05)
        return None

    def drain(self):
        self.events.clear()


async def run(args):
    b = os.path.abspath(args.bin)
    d = tempfile.mkdtemp(prefix="remodroid-desktop-")
    home = f"{d}/home"
    os.makedirs(home)
    http_port, disp = free_port(), free_display()
    probe_bin = f"{d}/xprobe"
    subprocess.check_call(["cc", "-O1", "-o", probe_bin, f"{ROOT}/desktop/test/xprobe.c", "-lX11"])
    hp = lambda pw: subprocess.check_output([f"{b}/remodroid-server", "hashpw", pw]).decode().strip()
    open(f"{d}/server.ini", "w").write(f"""[http]
listen = 127.0.0.1:{http_port}
web_root = {ROOT}/web
[users]
me = {hp('pw')}
""")
    open(f"{d}/desktop.ini", "w").write("""[desktop]
notify = false
""")
    quiet = None if args.verbose else subprocess.DEVNULL
    res, ok = {}, False
    env = dict(os.environ, DISPLAY=f":{disp}", HOME=home, XDG_CONFIG_HOME=f"{home}/.config")
    env.pop("WAYLAND_DISPLAY", None)
    env.pop("XDG_SESSION_TYPE", None)
    procs = [subprocess.Popen(["Xvfb", f":{disp}", "-screen", "0", "1280x720x24", "-nolisten", "tcp"], stderr=quiet)]
    for _ in range(50):
        if os.path.exists(f"/tmp/.X11-unix/X{disp}"):
            break
        time.sleep(0.1)
    probe = Probe(probe_bin, env)
    procs.append(probe.p)
    procs.append(subprocess.Popen([f"{b}/remodroid-server", "-c", f"{d}/server.ini"], stderr=quiet))
    time.sleep(0.3)
    # pair the way a person does: a one-time code from the panel, redeemed by the agent
    base = f"http://127.0.0.1:{http_port}"
    req = urllib.request.Request(base + "/login", data=json.dumps({"user": "me", "password": "pw"}).encode(),
                                 headers={"Content-Type": "application/json"}, method="POST")
    cookie = urllib.request.urlopen(req).headers["Set-Cookie"].split(";")[0]
    req = urllib.request.Request(base + "/api/v1/enroll/codes", data=b'{"name":"Test computer"}',
                                 headers={"Cookie": cookie}, method="POST")
    code = json.loads(urllib.request.urlopen(req).read())["code"]
    try:
        pair = subprocess.run([f"{b}/desktop/remodroid-desktopd", "-c", f"{d}/desktop.ini", "-v", "pair", "--panel", base,
                               "--code", code.lower().replace("-", "")], env=env, capture_output=True, text=True, timeout=30)
        res["pair"] = pair.returncode == 0 and "paired as" in pair.stderr
        out = pair.stderr
    except subprocess.TimeoutExpired as e:
        res["pair"] = False
        out = (e.stderr or b"").decode(errors="replace") if isinstance(e.stderr, bytes) else (e.stderr or "")
    if not res["pair"]:
        print("---- pair ----\n" + out)
    agent_log = open(f"{d}/agent.log", "w")
    procs.append(subprocess.Popen([f"{b}/desktop/remodroid-desktopd", "-c", f"{d}/desktop.ini", "-v"], env=env,
                                  stderr=agent_log, stdout=agent_log))
    try:
        if not await probe.wait(lambda e: e["ev"] == "ready", 10):
            raise RuntimeError("xprobe did not start")
        async with async_playwright() as p:
            launch = {"args": ["--autoplay-policy=no-user-gesture-required"]}
            if args.chrome:
                launch["executable_path"] = args.chrome
            browser = await p.chromium.launch(**launch)
            ctx = await browser.new_context(viewport={"width": 1280, "height": 720})
            page = await ctx.new_page()
            logs = []
            page.on("console", lambda m: logs.append(m.text))
            page.on("pageerror", lambda e: logs.append("PAGEERROR " + str(e)))
            await page.goto(f"http://127.0.0.1:{http_port}/")
            await page.fill("#user", "me")
            await page.fill("#password", "pw")
            await page.click("#go")
            await page.wait_for_url(f"http://127.0.0.1:{http_port}/")
            t0 = time.time()
            try:
                await page.wait_for_function(
                    "() => { const v = document.querySelector('#video'); return v && v.videoWidth > 0; }", timeout=30000)
            except Exception:
                print("overlay:", await page.inner_text("#disc"))
                print("console:", "\n".join(logs[-30:]))
                raise
            await page.wait_for_function("() => document.querySelector('#disc').hidden", timeout=15000)
            res["first_frame_s"] = round(time.time() - t0, 2)
            await page.wait_for_timeout(1500)
            # the device as the viewer sees it
            res["platform"] = await page.evaluate("async () => { const r = await fetch('/api/v1/devices'); const j = await r.json();"
                                                  " const d = (j.devices || j)[0]; return d.platform; }")
            # colours at the quadrant centres, read from the decoded video
            res["video"] = await page.evaluate("""() => {
                const v = document.querySelector('#video'), c = document.createElement('canvas');
                c.width = v.videoWidth; c.height = v.videoHeight;
                const g = c.getContext('2d'); g.drawImage(v, 0, 0);
                const at = (fx, fy) => Array.from(g.getImageData(Math.floor(fx * c.width), Math.floor(fy * c.height), 1, 1).data.slice(0, 3));
                return { w: v.videoWidth, h: v.videoHeight, tl: at(.25, .2), tr: at(.75, .2), bl: at(.25, .8), br: at(.75, .8) };
            }""")
            near = lambda px, want: all(abs(a - b) < 60 for a, b in zip(px, want))
            v = res["video"]
            res["colours_ok"] = (near(v["tl"], (255, 0, 0)) and near(v["tr"], (0, 255, 0)) and near(v["bl"], (0, 0, 255))
                                 and near(v["br"], (255, 255, 0)))
            box = await page.locator("#video").bounding_box()
            # a click at 75% / 25% of the picture → X pixel (960, 180) on the 1280x720 screen
            probe.drain()
            await page.mouse.move(box["x"] + box["width"] * 0.75, box["y"] + box["height"] * 0.25)
            await page.mouse.down()
            await page.mouse.up()
            ev = await probe.wait(lambda e: e["ev"] == "button" and e["b"] == 1 and e["down"] == 1)
            res["click"] = ev and [ev["x"], ev["y"]]
            res["click_ok"] = bool(ev and abs(ev["x"] - 960) <= 4 and abs(ev["y"] - 180) <= 4)
            # right button
            await page.mouse.click(box["x"] + box["width"] * 0.25, box["y"] + box["height"] * 0.75, button="right")
            ev = await probe.wait(lambda e: e["ev"] == "button" and e["b"] == 3 and e["down"] == 1)
            res["right_click_ok"] = bool(ev and abs(ev["x"] - 320) <= 4 and abs(ev["y"] - 540) <= 4)
            # wheel down → X button 5
            await page.mouse.wheel(0, 300)
            ev = await probe.wait(lambda e: e["ev"] == "button" and e["b"] == 5)
            res["wheel_ok"] = bool(ev)
            # physical keys: KeyA → keycode 38 (evdev 30 + 8), Shift+Digit1 → exclam
            probe.drain()
            await page.keyboard.press("KeyA")
            ev = await probe.wait(lambda e: e["ev"] == "key" and e["down"] == 1 and e["keycode"] == 38)
            res["key_a"] = ev and ev["keysym"]
            await page.keyboard.press("Shift+Digit1")
            ev = await probe.wait(lambda e: e["ev"] == "key" and e["down"] == 1 and e["keycode"] == 10)
            res["shift_1"] = ev and ev["keysym"]
            await page.keyboard.press("Enter")
            ev = await probe.wait(lambda e: e["ev"] == "key" and e["down"] == 1 and e["keysym"] == "Return")
            res["enter_ok"] = bool(ev)
            # nothing left held down
            await page.wait_for_timeout(300)
            # text from the Keyboard sheet, including a character no key types
            probe.drain()
            await page.locator("#ball").click()
            await page.locator('#menu button[data-act="keys"]').click()
            await page.fill("#kbText", "Hi é")
            await page.click("#kbSend")
            want = ["H", "i", "space", "eacute"]
            got = []
            end = time.time() + 8
            while len(got) < len(want) and time.time() < end:
                ev = await probe.wait(lambda e: e["ev"] == "key" and e["down"] == 1 and e["keysym"] not in ("Shift_L",), 1)
                if ev:
                    got.append(ev["keysym"])
            res["text"] = got
            await page.locator("#sheet-keys button[data-close]").click()
            # clipboard: the computer copies → the viewer's clip bar shows it
            probe.send("setclip copied on linux")
            await probe.wait(lambda e: e["ev"] == "setclip")
            try:
                await page.wait_for_function("() => (document.querySelector('#clipbarText') || {}).textContent === 'copied on linux'",
                                             timeout=8000)
                res["clip_to_viewer"] = True
            except Exception:
                res["clip_to_viewer"] = False
            # the viewer sends → the computer's clipboard
            await page.locator("#ball").click()
            await page.locator('#menu button[data-act="clip"]').click()
            await page.fill("#clipAndroid", "sent from the browser")
            await page.click("#clipSend")
            await page.wait_for_timeout(700)
            probe.send("getclip")
            ev = await probe.wait(lambda e: e["ev"] == "clip", 5)
            res["clip_to_computer"] = ev and ev["text"]
            await page.locator("#sheet-clip button[data-close]").click()
            # upload a file
            src = f"{d}/notes.txt"
            open(src, "w").write("hello computer\n")
            await page.locator("#ball").click()
            await page.locator('#menu button[data-act="photo"]').click()
            await page.set_input_files("#photoInput", src)
            dest = f"{home}/Downloads/RemoDroid/notes.txt"
            for _ in range(80):
                if os.path.exists(dest):
                    break
                await page.wait_for_timeout(100)
            res["upload_ok"] = os.path.exists(dest) and open(dest).read() == "hello computer\n"
            await page.locator("#sheet-photo button[data-close]").click()
            res["hud"] = await page.inner_text("#hud")
            res["errors"] = [l for l in logs if "PAGEERROR" in l][:5]
            await browser.close()
        ok = (res.get("pair") and res.get("platform") == "linux" and res["video"]["w"] == 1280 and res.get("colours_ok") and res.get("click_ok")
              and res.get("right_click_ok") and res.get("wheel_ok") and res.get("key_a") == "a" and res.get("shift_1") == "exclam"
              and res.get("enter_ok") and res.get("text") == ["H", "i", "space", "eacute"] and res.get("clip_to_viewer")
              and res.get("clip_to_computer") == "sent from the browser" and res.get("upload_ok") and not res.get("errors"))
    finally:
        for pr in procs[::-1]:
            if pr.poll() is None:
                pr.terminate()
        for pr in procs:
            try:
                pr.wait(5)
            except Exception:
                pr.kill()
        agent_log.close()
        if not ok or args.verbose:
            print("---- agent log (tail) ----")
            print("".join(open(f"{d}/agent.log").readlines()[-40:]))
        shutil.rmtree(d, ignore_errors=True)
    print(json.dumps(res, indent=2))
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default="build/host")
    ap.add_argument("--chrome", help="Chrome / headless shell to use instead of Playwright's")
    ap.add_argument("--verbose", action="store_true")
    sys.exit(asyncio.run(run(ap.parse_args())))
