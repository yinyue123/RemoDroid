#!/usr/bin/env python3
"""Page mode end to end (browser/dom-mirror.md): the real web client on an iPhone-sized touch screen in
headless Chrome, showing the remote browser's page as elements instead of video, and operating it.

  Xorg :N (dummy 880x1808, inputtest touch) ── rd-shell ── Chrome (kiosk, the real page) ◀─ DevTools pipe
     ▲ touches                                    ▲ socket
  remodroid-desktopd ◀── socket ── rd-mirror ─────┘
     ▲ WebRTC: "dom" channel (the page), input, ctrl
  Chrome, 428x926 with touch: web/app.js + pagemode.js + mirror.js

Checks: Page mode shows the page's own elements under the viewer's bar; a tap on a button in the mirror is
a real (trusted) tap on the real one; typing into the mirror's field arrives in the real field; a <select>
picked in the mirror is picked on the real page; scrolling the mirror to its end makes the real page load
more rows, which arrive; a finger dragged over something that doesn't scroll reaches the page as a touch;
an address typed in the viewer's bar opens there; back goes back; a link tapped in the mirror navigates the
real page and never the viewer; the viewer's browser asks the network for nothing of the page's; and Video
mode gives the picture back. Needs Xorg with the dummy driver, Chrome, and to run as root.

  python tests/e2e_mirror.py --bin build/host [--chrome PATH] [--site-chrome PATH] [--verbose]
"""
import argparse
import asyncio
import functools
import http.server
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

from playwright.async_api import async_playwright

sys.path.insert(0, os.path.dirname(__file__))
from e2e_desktop import free_display, free_port  # noqa: E402
from e2e_remote_browser import dummy_driver  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *a):
        pass


async def run(args):
    if not dummy_driver():
        print("skip: no Xorg dummy driver (xserver-xorg-video-dummy)")
        return 77
    site_chrome = args.site_chrome or shutil.which("google-chrome") or shutil.which("chromium")
    if not site_chrome:
        print("skip: no Chrome for the remote browser itself (--site-chrome)")
        return 77
    b = os.path.abspath(args.bin)
    d = tempfile.mkdtemp(prefix="remodroid-mirror-")
    home = f"{d}/home"
    os.makedirs(home)
    http_port, site_port, disp = free_port(), free_port(), free_display()
    site = http.server.ThreadingHTTPServer(("127.0.0.1", site_port),
                                           functools.partial(Quiet, directory=f"{ROOT}/browser/test/mirror/pages"))
    threading.Thread(target=site.serve_forever, daemon=True).start()
    origin = f"http://127.0.0.1:{site_port}"
    conf = open(f"{ROOT}/browser/hqnode/rd-xorg-light.conf").read().replace("/run/rd-touch.sock", f"{d}/touch.sock")
    open(f"{d}/xorg.conf", "w").write(conf)
    os.makedirs(f"{d}/conf.d")
    hp = lambda pw: subprocess.check_output([f"{b}/remodroid-server", "hashpw", pw]).decode().strip()
    open(f"{d}/server.ini", "w").write(f"[http]\nlisten = 127.0.0.1:{http_port}\nweb_root = {ROOT}/web\n[users]\nme = {hp('pw')}\n")
    open(f"{d}/desktop.ini", "w").write(f"""[input]
touch = inputtest:{d}/touch.sock
touch_points = 5
[desktop]
notify = false
clipboard_sync = false
audio = false
[mirror]
socket = {d}/mirror.sock
""")
    open(f"{d}/browser.conf", "w").write(f"""allow_direct = 1
users = Alice Bob
data = {d}/users
cache = {d}/cache
home = {origin}/ops.html
browser = {site_chrome}
args = --no-sandbox --disable-gpu
mirror_socket = {d}/shell.sock
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
    logs = {n: open(f"{d}/{n}.log", "w") for n in ("shell", "mirror", "agent")}
    procs.append(subprocess.Popen([f"{b}/browser/rd-shell", "-c", f"{d}/browser.conf"], env=env, stdout=logs["shell"],
                                  stderr=subprocess.STDOUT))
    procs.append(subprocess.Popen([f"{b}/browser/rd-mirror", "-s", f"{d}/shell.sock", "-l", f"{d}/mirror.sock"] + (["-v"] if args.verbose else []),
                                  stdout=logs["mirror"], stderr=subprocess.STDOUT))
    procs.append(subprocess.Popen([f"{b}/remodroid-server", "-c", f"{d}/server.ini"], stderr=quiet))
    time.sleep(0.5)
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
    procs.append(subprocess.Popen([f"{b}/desktop/remodroid-desktopd", "-c", f"{d}/desktop.ini", "-v"], env=env,
                                  stderr=logs["agent"], stdout=logs["agent"]))
    try:
        async with async_playwright() as p:
            launch = {"args": ["--autoplay-policy=no-user-gesture-required"]}
            if args.chrome:
                launch["executable_path"] = args.chrome
            browser = await p.chromium.launch(**launch)
            ctx = await browser.new_context(viewport={"width": 428, "height": 926}, device_scale_factor=3,
                                            has_touch=True, is_mobile=True)
            page = await ctx.new_page()
            clog, asked = [], []
            page.on("console", lambda m: clog.append(m.text))
            page.on("pageerror", lambda e: clog.append("PAGEERROR " + str(e)))
            # anything the viewer's browser asks the network for that isn't the panel: there must be none
            page.on("request", lambda r: asked.append(r.url) if not r.url.startswith((base, "blob:", "data:", "about:")) else None)
            await page.goto(base + "/")
            await page.fill("#user", "me")
            await page.fill("#password", "pw")
            await page.click("#go")
            await page.wait_for_url(re.compile(r".*/panel\.html#devices$"))
            await page.locator('a[href^="/?device="]').first.click()
            await page.wait_for_url(base + "/")
            await page.wait_for_function("() => document.querySelector('#disc').hidden && "
                                         "document.querySelector('#video').videoWidth > 0", timeout=30000)
            res["offered"] = await page.evaluate("() => PageMode.can")

            # Page mode on: the page's own elements, in the frame, under the viewer's bar
            await page.evaluate("() => PageMode.set(true)")
            fr = page.frame_locator("#pmFrame")
            txt = lambda sel: fr.locator(sel).inner_text(timeout=8000)
            async def until(sel, want, ms=8000):
                try:
                    await fr.locator(sel).filter(has_text=want).wait_for(timeout=ms)
                except Exception:  # what the mirror shows instead
                    res["mirror_now"] = await page.evaluate(
                        "() => { const d = document.querySelector('#pmFrame').contentDocument; return [d.title, d.body ? "
                        "d.body.innerText.slice(0, 100) : null, document.querySelector('#pmAddr').value, "
                        "document.querySelector('#pmNote').textContent, PageMode.log.slice(-8)]; }")
                    raise
            await until("#head", "ops test", 15000)
            res["shown"] = await txt("#btn")
            res["bar"] = await page.input_value("#pmAddr")
            res["frame"] = await page.evaluate("() => { const f = document.querySelector('#pmFrame');"
                                               " return [f.clientWidth, f.clientHeight, f.sandbox.value,"
                                               " f.contentDocument.querySelectorAll('script').length]; }")
            cdp = await ctx.new_cdp_session(page)
            touch = lambda kind, pts: cdp.send("Input.dispatchTouchEvent", {"type": kind, "touchPoints": pts})

            async def mid(sel, fx=0.5, fy=0.5):
                bb = await fr.locator(sel).bounding_box()
                return {"x": bb["x"] + bb["width"] * fx, "y": bb["y"] + bb["height"] * fy}

            async def tap(sel):
                await touch("touchStart", [await mid(sel)])
                await asyncio.sleep(0.06)
                await touch("touchEnd", [])

            # a tap on the mirror's button: a real, trusted tap on the real one, and the new text comes back
            await tap("#btn")
            await until("#btn", "taps: 1")
            await tap("#btn")
            await until("#btn", "taps: 2")
            res["tap"] = await txt("#btn")

            # typing into the mirror's field: the real field gets the keys, and the page's echo comes back
            await tap("#in")
            await asyncio.sleep(0.5)
            await fr.locator("#in").focus()
            await page.keyboard.type("Hello wrld", delay=30)
            await until("#echo", "echo:Hello wrld")
            for _ in range(3):
                await page.keyboard.press("Backspace")
            await page.keyboard.type("orld", delay=30)
            await until("#echo", "echo:Hello world")
            res["typed"] = await txt("#echo")

            # a <select>: picked with this device's control, set on the real page
            await fr.locator("#sel").select_option("blue")
            await until("#picked", "picked: blue")
            res["select"] = await txt("#picked")

            # a finger dragged across something that doesn't scroll: the page gets it as a touch
            a = await mid("#drag", 0.2)
            await touch("touchStart", [a])
            await asyncio.sleep(0.12)
            for k in range(1, 9):
                await touch("touchMove", [{"x": a["x"] + k * 12, "y": a["y"]}])
                await asyncio.sleep(0.03)
            await touch("touchEnd", [])
            await until("#drag", "end")
            res["drag"] = await txt("#drag")
            m = re.search(r"drag: (-?\d+) end", res["drag"])
            res["drag_ok"] = bool(m) and 60 <= int(m.group(1)) <= 130  # 96 px across the viewer, scaled to the real page's

            # where the viewer looks is where the real page is: scrolled to its end, the page loads more rows
            rows = lambda: fr.locator("#rows div").count()
            res["rows_before"] = await rows()
            for _ in range(12):
                await page.evaluate("() => { const s = document.querySelector('#pmFrame').contentDocument.scrollingElement;"
                                    " s.scrollTo(0, s.scrollHeight); }")
                await asyncio.sleep(0.5)
                if await rows() >= 80:
                    break
            res["rows_after"] = await rows()
            res["real_scroll"] = await page.evaluate("() => document.querySelector('#pmFrame').contentDocument.scrollingElement.scrollTop")

            # a link tapped in the mirror: the real page goes there; the viewer's frame goes nowhere
            await page.evaluate("() => document.querySelector('#pmFrame').contentDocument.scrollingElement.scrollTo(0, 0)")
            await asyncio.sleep(0.6)
            await tap("#lnk")
            await until("#head", "second page", 12000)
            res["link"] = await txt("#head")
            res["frame_url"] = await page.evaluate("() => document.querySelector('#pmFrame').contentWindow.location.href")

            # the viewer's bar: back, then an address typed into it
            await page.click("#pmBar button[title=Back]")
            await until("#head", "ops test", 12000)
            await page.click("#pmAddr")
            await page.fill("#pmAddr", f"127.0.0.1:{site_port}/page.html")
            await page.keyboard.press("Enter")
            await until("#h", "Mirror test", 12000)
            await asyncio.sleep(1.5)
            res["address"] = await page.input_value("#pmAddr")
            # a picture of the page's (a CSS background): its bytes came over the channel, as a blob
            res["picture"] = await page.evaluate("() => { const f = document.querySelector('#pmFrame'), e = f.contentDocument.getElementById('bg');"
                                                 " return e ? f.contentWindow.getComputedStyle(e).backgroundImage.slice(0, 10) : null; }")
            res["requests_for_the_site"] = asked[:5]

            # Video mode again: the picture comes back
            f0 = await page.evaluate("() => document.querySelector('#video').getVideoPlaybackQuality().totalVideoFrames")
            await page.evaluate("() => PageMode.set(false)")
            await page.wait_for_function("f0 => document.querySelector('#pm').hidden && "
                                         "document.querySelector('#video').getVideoPlaybackQuality().totalVideoFrames > f0 + 2",
                                         arg=f0, timeout=15000)
            res["video_again"] = True
            res["errors"] = [l for l in clog if "PAGEERROR" in l][:5]
            await browser.close()
        ok = (res.get("pair") and res.get("offered") and res.get("shown") == "taps: 0" and "ops test" in res.get("bar", "")
              and res["frame"][:2] == [440, 860] and res["frame"][2] == "allow-same-origin" and res["frame"][3] == 0
              and res.get("tap") == "taps: 2" and res.get("typed") == "echo:Hello world" and res.get("select") == "picked: blue"
              and res.get("drag_ok") and res["rows_before"] == 20 and res["rows_after"] >= 80
              and res.get("link") == "second page" and res.get("frame_url") == "about:srcdoc"
              and "mirror test" in res.get("address", "").lower() and res.get("picture") == 'url("blob:'
              and not res.get("requests_for_the_site") and res.get("video_again") and not res.get("errors"))
    except Exception as e:  # what was reached is still worth printing
        res["stopped_at"] = repr(e)[:400]
        if "clog" in locals():
            res["console"] = [l for l in clog if "Blocked script execution" not in l][-12:]  # (not Playwright's own probing of the frame)
    finally:
        for pr in procs[::-1]:
            if pr.poll() is None:
                pr.terminate()
        for pr in procs:
            try:
                pr.wait(5)
            except Exception:
                pr.kill()
        for f in logs.values():
            f.close()
        if not ok or args.verbose:
            for n in logs:
                print(f"---- {n} log (tail) ----")
                print("".join(l for l in open(f"{d}/{n}.log").readlines()[-(400 if args.verbose else 25):] if "dbus" not in l))
        shutil.rmtree(d, ignore_errors=True)
    print(json.dumps(res, indent=2))
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default="build/host")
    ap.add_argument("--chrome", help="Chrome / headless shell for the viewer instead of Playwright's")
    ap.add_argument("--site-chrome", help="the Chrome that plays the remote browser (default: google-chrome)")
    ap.add_argument("--verbose", action="store_true")
    sys.exit(asyncio.run(run(ap.parse_args())))
