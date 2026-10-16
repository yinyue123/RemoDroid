#!/usr/bin/env python3
"""Browser end-to-end test: the real web client (web/) in headless Chrome,
logged in with Basic auth, streaming from the phone-side C WebRTC stack
(rtc_test) through remodroid-server and remodroid-agent (Path A tunnel).

  PLAYWRIGHT_BROWSERS_PATH=... python tests/e2e_browser.py --bin build/host
"""
import argparse
import asyncio
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "android", "native", "test"))
from rtc_interop import make_clip  # noqa: E402
from playwright.async_api import async_playwright  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


async def run(args):
    b = os.path.abspath(args.bin)
    d = tempfile.mkdtemp(prefix="remodroid-browser-")
    http_port, tun_port = free_port(), free_port()
    hp = lambda pw: subprocess.check_output([f"{b}/remodroid-server", "hashpw", pw]).decode().strip()
    open(f"{d}/server.ini", "w").write(f"""[http]
listen = 127.0.0.1:{http_port}
web_root = {ROOT}/web
[users]
me = {hp('pw')}
[devices]
router1 = {hp('dev')}
[device_names]
router1 = Test phone
""")
    open(f"{d}/agent.ini", "w").write(f"""[server]
url = ws://127.0.0.1:{http_port}/ws/agent
device_id = router1
device_secret = dev
[agent]
socket = {d}/agent.sock
adbkey = {d}/adbkey
[phone]
link = tcp:127.0.0.1:{tun_port}
""")
    clip = f"{d}/clip.frames"
    make_clip(clip, frames=120)
    quiet = None if args.verbose else subprocess.DEVNULL
    procs = [subprocess.Popen([f"{b}/remodroid-server", "-c", f"{d}/server.ini"], stderr=quiet),
             subprocess.Popen([f"{b}/rtc/rtc_test", "--listen", f"tcp:127.0.0.1:{tun_port}", "--frames", clip,
                               "--direct", "0", "--seconds", "90"], stdout=subprocess.PIPE, stderr=quiet, text=True)]
    time.sleep(0.3)
    procs.append(subprocess.Popen([f"{b}/remodroid-agent", "-c", f"{d}/agent.ini"], stderr=quiet))
    res = {}
    try:
        async with async_playwright() as p:
            browser = await p.chromium.launch(args=["--autoplay-policy=no-user-gesture-required"])
            ctx = await browser.new_context(http_credentials={"username": "me", "password": "pw"},
                                            viewport={"width": 390, "height": 844}, has_touch=True)
            page = await ctx.new_page()
            logs = []
            page.on("console", lambda m: logs.append(m.text))
            page.on("pageerror", lambda e: logs.append("PAGEERROR " + str(e)))
            await page.goto(f"http://127.0.0.1:{http_port}/")
            # wait for real decoded frames
            t0 = time.time()
            try:
                await page.wait_for_function(
                    "() => { const v = document.querySelector('#video'); return v && v.videoWidth > 0; }", timeout=30000)
            except Exception:
                print("overlay:", await page.inner_text("#disc"))
                print("console:", "\n".join(logs[-30:]))
                await page.locator("#ball").click(force=True)
                raise
            await page.wait_for_function(
                "() => { const v = document.querySelector('#video'); const q = v && v.getVideoPlaybackQuality && v.getVideoPlaybackQuality();"
                " return v && v.videoWidth > 0 && q && q.totalVideoFrames > 30; }", timeout=30000)
            res["first_frames_s"] = round(time.time() - t0, 2)
            res["video"] = await page.evaluate("() => { const v = document.querySelector('#video'); const q = v.getVideoPlaybackQuality();"
                                               " return { w: v.videoWidth, h: v.videoHeight, frames: q.totalVideoFrames, dropped: q.droppedVideoFrames }; }")
            res["overlay_hidden"] = await page.evaluate("() => document.querySelector('#disc').hidden")
            # HUD shows live stats
            await page.wait_for_timeout(2500)
            res["hud"] = await page.inner_text("#hud")
            # a tap on the stream -> input channel -> phone replies on ctrl ("got"), visible in the protocol log
            box = await page.locator("#video").bounding_box()
            await page.mouse.click(box["x"] + box["width"] / 2, box["y"] + box["height"] / 2)
            await page.wait_for_timeout(500)
            # open the floating ball menu and press Back -> ctrl nav -> echo from the phone
            await page.locator("#ball").click()
            res["menu_open"] = await page.evaluate("() => !document.querySelector('#menu').hidden")
            await page.locator('#menu button[data-act="back"]').click()
            await page.wait_for_timeout(500)
            # Settings -> Logs shows the protocol log, including the phone's replies
            await page.locator("#ball").click()
            await page.locator('#menu button[data-act="settings"]').click()
            await page.locator('#tabs button[data-tab="logs"]').click()
            await page.wait_for_timeout(300)
            plog = await page.locator('pre[aria-label="Protocol messages"]').inner_text()
            res["phone_got_touch"] = '"label":"input"' in plog and '"n":14' in plog
            res["phone_echoed_nav"] = '"t":"echo","d":{"t":"nav","d":{"button":"back"}}' in plog
            res["errors"] = [l for l in logs if "PAGEERROR" in l][:5]
            procs[1].send_signal(10)  # phone prints its stats while still connected
            await page.wait_for_timeout(500)
            await browser.close()
    finally:
        procs[1].send_signal(15)
        try:
            out, _ = procs[1].communicate(timeout=10)
            stats = [json.loads(l) for l in out.splitlines() if l.startswith("{")]
            res["phone"] = max(stats, key=lambda x: x["packets"]) if stats else None
        except Exception:
            pass
        for pr in procs:
            if pr.poll() is None:
                pr.terminate()
        for pr in procs:
            try:
                pr.wait(5)
            except Exception:
                pr.kill()
        shutil.rmtree(d, ignore_errors=True)
    print(json.dumps(res, indent=2))
    ok = (res.get("video", {}).get("w") == 640 and res["video"]["frames"] > 30 and res.get("overlay_hidden")
          and res.get("phone_got_touch") and res.get("phone_echoed_nav") and not res.get("errors"))
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default="build/host")
    ap.add_argument("--verbose", action="store_true")
    sys.exit(asyncio.run(run(ap.parse_args())))
