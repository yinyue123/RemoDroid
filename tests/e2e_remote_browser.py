#!/usr/bin/env python3
"""Remote browser end-to-end test (browser/remote-browser-plan.md): the real web client on an iPhone-sized
touch screen in headless Chrome, driving the remote browser's X screen through remodroid-server and
remodroid-desktopd in touch-screen mode ([input] touch = inputtest).

  Xorg :N (dummy 880x1808, the inputtest touch device) ── touchprobe (prints the touches X delivers)
     ▲ fingers as XI2 touches
  remodroid-desktopd ──ws──▶ remodroid-server ◀──ws── Chrome, 428x926 with touch (web/app.js) ◀── WebRTC ──┘

Checks: the video has the remote browser's screen's shape (880x1808, scaled to the viewer's quality preset); a one-finger swipe arrives as one touch
that begins, moves and ends where the finger was (Chromium scrolls a page from that, with its own
momentum), not as a mouse drag; a tap is a touch too; two fingers are two touches (a pinch); dragging the
connection chip moves the chip and sends nothing to the computer; and the picture has no mouse pointer in
it. Needs Xorg 21.1+ with xserver-xorg-video-dummy, run as root (or Xorg.wrap allowing it).

  python tests/e2e_remote_browser.py --bin build/host [--chrome PATH] [--verbose]
"""
import argparse
import asyncio
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request

from playwright.async_api import async_playwright

sys.path.insert(0, os.path.dirname(__file__))
from e2e_desktop import Probe, free_display, free_port  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
W, H = 880, 1808


def dummy_driver():
    import glob
    return glob.glob("/usr/lib/xorg/modules/drivers/dummy_drv.so") + glob.glob("/usr/lib/*/xorg/modules/drivers/dummy_drv.so")


async def run(args):
    if not dummy_driver():
        print("skip: no Xorg dummy driver (xserver-xorg-video-dummy)")
        return 77
    b = os.path.abspath(args.bin)
    d = tempfile.mkdtemp(prefix="remodroid-rbrowser-")
    home = f"{d}/home"
    os.makedirs(home)
    http_port, disp = free_port(), free_display()
    probe_bin = f"{d}/touchprobe"
    subprocess.check_call(["cc", "-O1", "-o", probe_bin, f"{ROOT}/browser/test/touchprobe.c", "-lX11", "-lXi"])
    conf = open(f"{ROOT}/browser/hqnode/rd-xorg-light.conf").read().replace("/run/rd-touch.sock", f"{d}/touch.sock")
    open(f"{d}/xorg.conf", "w").write(conf)
    os.makedirs(f"{d}/conf.d")
    hp = lambda pw: subprocess.check_output([f"{b}/remodroid-server", "hashpw", pw]).decode().strip()
    open(f"{d}/server.ini", "w").write(f"""[http]
listen = 127.0.0.1:{http_port}
web_root = {ROOT}/web
[users]
me = {hp('pw')}
""")
    open(f"{d}/desktop.ini", "w").write(f"""[input]
touch = inputtest:{d}/touch.sock
touch_points = 5
[desktop]
notify = false
clipboard_sync = false
audio = false
""")
    quiet = None if args.verbose else subprocess.DEVNULL
    res, ok = {}, False
    env = dict(os.environ, DISPLAY=f":{disp}", HOME=home, XDG_CONFIG_HOME=f"{home}/.config")
    for k in ("WAYLAND_DISPLAY", "XDG_SESSION_TYPE", "XAUTHORITY"):
        env.pop(k, None)
    procs = [subprocess.Popen(["Xorg", f":{disp}", "-config", f"{d}/xorg.conf", "-configdir", f"{d}/conf.d", "-noreset",
                               "-nolisten", "tcp", "-novtswitch", "-keeptty", "-logfile", f"{d}/xorg.log"],
                              stdout=quiet, stderr=quiet)]
    for _ in range(100):
        if os.path.exists(f"/tmp/.X11-unix/X{disp}") and os.path.exists(f"{d}/touch.sock"):
            break
        time.sleep(0.1)
    probe = Probe(probe_bin, env)
    procs.append(probe.p)
    procs.append(subprocess.Popen([f"{b}/remodroid-server", "-c", f"{d}/server.ini"], stderr=quiet))
    time.sleep(0.3)
    base = f"http://127.0.0.1:{http_port}"
    req = urllib.request.Request(base + "/login", data=json.dumps({"user": "me", "password": "pw"}).encode(),
                                 headers={"Content-Type": "application/json"}, method="POST")
    cookie = urllib.request.urlopen(req).headers["Set-Cookie"].split(";")[0]
    req = urllib.request.Request(base + "/api/v1/enroll/codes", data=b'{"name":"Remote browser"}',
                                 headers={"Cookie": cookie}, method="POST")
    code = json.loads(urllib.request.urlopen(req).read())["code"]
    pair = subprocess.run([f"{b}/desktop/remodroid-desktopd", "-c", f"{d}/desktop.ini", "pair", "--panel", base,
                           "--code", code], env=env, capture_output=True, text=True, timeout=30)
    res["pair"] = pair.returncode == 0
    agent_log = open(f"{d}/agent.log", "w")
    procs.append(subprocess.Popen([f"{b}/desktop/remodroid-desktopd", "-c", f"{d}/desktop.ini", "-v"], env=env,
                                  stderr=agent_log, stdout=agent_log))
    try:
        ready = await probe.wait(lambda e: e["ev"] == "ready", 10)
        if not ready:
            raise RuntimeError("touchprobe did not start (Xorg: %s)" % open(f"{d}/xorg.log").read()[-800:])
        res["screen"] = [ready["w"], ready["h"]]
        async with async_playwright() as p:
            launch = {"args": ["--autoplay-policy=no-user-gesture-required"]}
            if args.chrome:
                launch["executable_path"] = args.chrome
            browser = await p.chromium.launch(**launch)
            # an iPhone 14 Plus: 428x926, three device pixels to the CSS pixel, a touch screen
            ctx = await browser.new_context(viewport={"width": 428, "height": 926}, device_scale_factor=3,
                                            has_touch=True, is_mobile=True)
            page = await ctx.new_page()
            logs = []
            page.on("console", lambda m: logs.append(m.text))
            page.on("pageerror", lambda e: logs.append("PAGEERROR " + str(e)))
            await page.goto(base + "/")
            await page.fill("#user", "me")
            await page.fill("#password", "pw")
            await page.click("#go")
            await page.wait_for_url(base + "/")
            try:
                await page.wait_for_function("() => document.querySelector('#disc').hidden && "
                                             "document.querySelector('#video').videoWidth > 0", timeout=30000)
            except Exception:
                print("overlay:", await page.inner_text("#disc"))
                print("console:", "\n".join(logs[-30:]))
                raise
            await page.wait_for_timeout(1000)
            res["video"] = await page.evaluate("() => { const v = document.querySelector('#video');"
                                               " return [v.videoWidth, v.videoHeight]; }")
            vb = await page.locator("#video").bounding_box()
            at = lambda fx, fy: {"x": vb["x"] + vb["width"] * fx, "y": vb["y"] + vb["height"] * fy}
            px = lambda fx, fy: (round(fx * W), round(fy * H))
            near = lambda e, want, tol=8: bool(e) and abs(e["x"] - want[0]) <= tol and abs(e["y"] - want[1]) <= tol
            cdp = await ctx.new_cdp_session(page)
            touch = lambda kind, pts: cdp.send("Input.dispatchTouchEvent", {"type": kind, "touchPoints": pts})
            touches = lambda: [e for e in probe.events if e["ev"] == "touch"]
            buttons = lambda: [e for e in probe.events if e["ev"] == "button"]

            # one finger swiped up, as when scrolling a page
            probe.drain()
            await touch("touchStart", [at(0.5, 0.7)])
            for k in range(1, 11):
                await touch("touchMove", [at(0.5, 0.7 - k * 0.04)])
                await asyncio.sleep(0.016)
            await touch("touchEnd", [])
            await asyncio.sleep(0.5)
            t = touches()
            begins = [e for e in t if e["t"] == "begin"]
            ends = [e for e in t if e["t"] == "end"]
            res["swipe"] = {"begins": len(begins), "updates": sum(e["t"] == "update" for e in t), "ends": len(ends),
                            "from": begins and [begins[0]["x"], begins[0]["y"]], "to": ends and [ends[-1]["x"], ends[-1]["y"]],
                            "mouse_buttons": len(buttons())}
            res["swipe_ok"] = (len(begins) == 1 and len(ends) == 1 and res["swipe"]["updates"] >= 5
                               and near(begins[0], px(0.5, 0.7)) and near(ends[-1], px(0.5, 0.3)) and not buttons())

            # a tap: a touch that begins and ends in place
            probe.drain()
            await touch("touchStart", [at(0.25, 0.4)])
            await touch("touchEnd", [])
            await asyncio.sleep(0.4)
            t = touches()
            res["tap_ok"] = (len(t) >= 2 and t[0]["t"] == "begin" and t[-1]["t"] == "end" and near(t[0], px(0.25, 0.4))
                             and not buttons())

            # two fingers apart: two touches at once, a pinch
            probe.drain()
            await touch("touchStart", [at(0.45, 0.5), at(0.55, 0.5)])
            for k in range(1, 6):
                await touch("touchMove", [at(0.45 - k * 0.04, 0.5), at(0.55 + k * 0.04, 0.5)])
                await asyncio.sleep(0.016)
            await touch("touchEnd", [])
            await asyncio.sleep(0.4)
            t = touches()
            ids = {e["id"] for e in t if e["t"] == "begin"}
            spread = [e for e in t if e["t"] == "update"]
            res["pinch"] = {"fingers": len(ids), "updates": len(spread)}
            res["pinch_ok"] = len(ids) == 2 and len(spread) >= 4 and not buttons()

            # the connection chip dragged out of the way: the chip moves, the computer gets nothing
            cb = await page.locator("#chip").bounding_box()
            hud = lambda: page.evaluate("() => !document.querySelector('#hud').hidden")
            hud0 = await hud()
            probe.drain()
            c0 = {"x": cb["x"] + cb["width"] / 2, "y": cb["y"] + cb["height"] / 2}
            await touch("touchStart", [c0])
            for k in range(1, 9):
                await touch("touchMove", [{"x": c0["x"] - k * 15, "y": c0["y"] + k * 40}])
                await asyncio.sleep(0.016)
            await touch("touchEnd", [])
            await asyncio.sleep(0.4)
            cb2 = await page.locator("#chip").bounding_box()
            res["chip"] = {"moved": [round(cb2["x"] - cb["x"]), round(cb2["y"] - cb["y"])], "touches": len(touches()),
                           "stats_toggled": (await hud()) != hud0}  # a drag isn't the tap that toggles them
            res["chip_ok"] = cb2["y"] - cb["y"] > 200 and not touches() and not buttons() and not res["chip"]["stats_toggled"]
            await page.reload()
            await page.wait_for_function("() => document.querySelector('#video').videoWidth > 0", timeout=30000)
            cb3 = await page.locator("#chip").bounding_box()
            res["chip_kept"] = abs(cb3["y"] - cb2["y"]) < 4

            # no pointer drawn into the picture: the X pointer is put over a black part of the screen (the
            # root window, nothing on it) and the picture there must stay black
            subprocess.run(["xdotool", "mousemove", str(W // 2), str(H // 2)], env=env, check=False)
            await page.wait_for_timeout(1200)
            res["pointer_pixels"] = await page.evaluate("""() => {
                const v = document.querySelector('#video'), c = document.createElement('canvas');
                c.width = v.videoWidth; c.height = v.videoHeight; const g = c.getContext('2d'); g.drawImage(v, 0, 0);
                const d = g.getImageData(c.width / 2 - 2, c.height / 2 - 2, 24, 24).data; let n = 0;
                for (let i = 0; i < d.length; i += 4) if (d[i] + d[i + 1] + d[i + 2] > 120) n++;
                return n; }""")
            res["no_pointer"] = res["pointer_pixels"] == 0
            res["errors"] = [l for l in logs if "PAGEERROR" in l][:5]
            await browser.close()
        vw, vh = res.get("video") or (0, 0)
        res["video_shape_ok"] = 0 < vw <= W and abs(vw / vh - W / H) < 0.01
        ok = (res.get("pair") and res.get("screen") == [W, H] and res["video_shape_ok"] and res.get("swipe_ok")
              and res.get("tap_ok") and res.get("pinch_ok") and res.get("chip_ok") and res.get("chip_kept")
              and res.get("no_pointer") and not res.get("errors"))
    finally:
        for pr in procs[::-1]:
            if pr.poll() is None:
                pr.terminate()
        for pr in procs:
            try:
                pr.wait(5)
            except Exception:
                pr.kill()
        agent_log.close() if "agent_log" in locals() else None
        if not ok or args.verbose:
            print("---- agent log (tail) ----")
            print("".join(open(f"{d}/agent.log").readlines()[-30:]) if os.path.exists(f"{d}/agent.log") else "(none)")
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
