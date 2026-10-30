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
~/Downloads/RemoDroid; the computer's sound (a tone into a PulseAudio null sink)
arrives as the Opus track, when pulseaudio is installed; and from a touch screen, tap / long press / two-finger
drag become click / right click / scroll, the floating keyboard types, and the
on-screen gamepad drives a virtual Xbox 360 pad (when /dev/uinput is writable).

  python tests/e2e_desktop.py --bin build/host [--chrome PATH] [--verbose]
"""
import argparse
import asyncio
import json
import os
import queue
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
import wave
import math
import struct

from playwright.async_api import async_playwright

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def tone_wav(path, seconds=90, hz=440):
    """A 48 kHz stereo sine, for the computer to play while the viewer listens."""
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(48000)
        one = b"".join(struct.pack("<hh", v, v) for v in
                       (int(12000 * math.sin(2 * math.pi * hz * i / 48000)) for i in range(48000)))
        w.writeframes(one * seconds)


def pad_node():
    """The event node of the agent's virtual Xbox 360 pad, from /proc/bus/input/devices."""
    for block in open("/proc/bus/input/devices").read().split("\n\n"):
        if 'Name="Microsoft X-Box 360 pad"' in block and "Vendor=045e Product=028e" in block:
            for word in block.replace("Handlers=", "").split():
                if word.startswith("event"):
                    return "/dev/input/" + word
    return None


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


async def trackpad(browser, http_port, probe, logs):
    """An iPhone viewer with the trackpad (trackpad.md §11): the pointer stays in the middle of the
    picture above the panel and moves by how far a finger moves; taps click at the pointer; two
    fingers right-click, scroll and zoom; ⚙ speeds it up; ⌨ / ▭ / ⌄ switch the panel; the grip
    resizes it within limits; a reload brings it all back. The screen is 1280×720."""
    out, ok = {}, {}
    ctx = await browser.new_context(viewport={"width": 390, "height": 844}, has_touch=True, is_mobile=True)
    page = await ctx.new_page()
    page.on("pageerror", lambda e: logs.append("PAGEERROR " + str(e)))
    await page.goto(f"http://127.0.0.1:{http_port}/")
    await page.fill("#user", "me")
    await page.fill("#password", "pw")
    await page.click("#go")
    await page.wait_for_url(f"http://127.0.0.1:{http_port}/")
    ready = "() => document.querySelector('#disc').hidden && document.querySelector('#video').videoWidth > 0"
    await page.wait_for_function(ready, timeout=30000)
    await page.wait_for_selector("#tpad:not([hidden])")
    await page.wait_for_timeout(800)
    cdp = await ctx.new_cdp_session(page)
    touch = lambda kind, pts: cdp.send("Input.dispatchTouchEvent", {"type": kind, "touchPoints": pts})
    box = lambda sel: page.locator(sel).bounding_box()
    scale = lambda: page.evaluate("() => new DOMMatrix(getComputedStyle(document.querySelector('#video')).transform).a")

    async def pad():
        b = await box("#tpadSurface")
        return b["x"] + b["width"] / 2, b["y"] + b["height"] / 2 + 15

    async def drag(x, y, dx, dy, steps=30, ms=50, fingers=1):
        """slowly (no acceleration): 2 px every 50 ms"""
        pts = lambda k: [{"x": x + i * 40 + dx * k / steps, "y": y + dy * k / steps, "id": i} for i in range(fingers)]
        await touch("touchStart", pts(0))
        for k in range(1, steps + 1):
            await touch("touchMove", pts(k))
            await asyncio.sleep(ms / 1000)
        await touch("touchEnd", [])

    async def tap(x, y, fingers=1):
        await touch("touchStart", [{"x": x + i * 40, "y": y, "id": i} for i in range(fingers)])
        await touch("touchEnd", [])

    async def click_at(x, y, b=1, fingers=1):
        probe.drain()
        await tap(x, y, fingers)
        ev = await probe.wait(lambda e: e["ev"] == "button" and e["b"] == b and e["down"] == 1)
        return ev and (ev["x"], ev["y"])

    # the pointer: drawn at the middle of the area above the panel
    cur, tpb = await box("#tpCursor"), await box("#tpad")
    out["cursor"] = [round(cur["x"] + 1.5), round(cur["y"] + 1.5)]
    ok["cursor_centred"] = abs(cur["x"] + 1.5 - 195) <= 2 and abs(cur["y"] + 1.5 - tpb["y"] / 2) <= 2
    ok["scale_1"] = abs(await scale() - 1) < 0.01  # a screen pixel to a CSS pixel
    x, y = await pad()
    p0 = await click_at(x, y)  # the computer's pointer starts in the middle
    out["start"] = p0
    ok["tap_at_pointer"] = bool(p0 and abs(p0[0] - 640) <= 2 and abs(p0[1] - 360) <= 2)
    # 60 px to the right moves the pointer 60 px, and the arrow, not the picture; the tap after it lands
    # there, wherever it is on the pad
    v0 = await box("#video")
    await drag(x - 30, y, 60, 0)
    p1 = await click_at(x - 80, y + 20)
    out["moved"] = p1
    ok["move_60"] = bool(p0 and p1 and abs(p1[0] - p0[0] - 60) <= 3 and abs(p1[1] - p0[1]) <= 3)
    ok["arrow_moves_picture_stays"] = abs((await box("#tpCursor"))["x"] - cur["x"] - 60) <= 3 and await box("#video") == v0
    # two taps: two clicks on one pixel, though the second lands elsewhere and wobbles
    probe.drain()
    await tap(x, y)
    await touch("touchStart", [{"x": x + 50, "y": y - 30}])
    await touch("touchMove", [{"x": x + 54, "y": y - 27}])
    await touch("touchEnd", [])
    downs = []
    for _ in range(2):
        ev = await probe.wait(lambda e: e["ev"] == "button" and e["b"] == 1 and e["down"] == 1)
        if ev:
            downs.append((ev["x"], ev["y"]))
    out["double"] = downs
    ok["double_click"] = len(downs) == 2 and downs[0] == downs[1] == p1
    # two fingers: tap = right click at the pointer; drag up = scroll down (natural)
    await asyncio.sleep(0.4)
    r = await click_at(x - 20, y, b=3, fingers=2)
    ok["right_click"] = r == p1
    probe.drain()
    await drag(x - 20, y + 40, 0, -80, steps=10, ms=30, fingers=2)
    ev = await probe.wait(lambda e: e["ev"] == "button" and e["b"] in (4, 5))
    ok["scroll_down"] = bool(ev and ev["b"] == 5)
    # taps as forgiving as iOS's: up to 450 ms and 12 px; a longer hold isn't a click; and a finger whose
    # lift the page never got (iOS can drop one) doesn't turn every later tap into a second finger
    await page.wait_for_timeout(1500)  # the scroll's momentum

    async def tap_v(ms=0, wob=0, stale=False):
        if stale:
            await page.evaluate("() => document.querySelector('#tpadSurface').dispatchEvent(new PointerEvent('pointerdown',"
                                " { pointerId: 77, pointerType: 'touch', isPrimary: true, bubbles: true, clientX: 100, clientY: 700 }))")
        probe.drain()
        await touch("touchStart", [{"x": x, "y": y, "id": 0}])
        if wob:
            await touch("touchMove", [{"x": x + wob, "y": y, "id": 0}])
        await asyncio.sleep(ms / 1000)
        await touch("touchEnd", [])
        ev = await probe.wait(lambda e: e["ev"] == "button" and e["down"] == 1, 1.5)
        await asyncio.sleep(0.5)  # not the second tap of a double tap
        return ev and ev["b"]
    out["taps"] = {"held_400": await tap_v(ms=400), "moved_10": await tap_v(ms=60, wob=10),
                   "held_600": await tap_v(ms=600), "after_lost_lift": await tap_v(ms=60, stale=True)}
    ok["taps_forgiving"] = out["taps"] == {"held_400": 1, "moved_10": 1, "held_600": None, "after_lost_lift": 1}
    # the stats overlay says what the last touch was taken as, and counts downs, lifts and cancels
    await page.wait_for_timeout(1100)
    hud = await page.evaluate("() => document.querySelector('#hud').innerText")
    out["hud_pad"] = next((l for l in hud.split("\n") if l.startswith("pad ")), None)
    ok["hud_pad"] = bool(out["hud_pad"] and re.match(r"^pad tap \d+ ms \d+ px → click · \d+↓ \d+↑ \d+✕$", out["hud_pad"]))
    # ⚙: Move speed 2× doubles the same move
    await page.wait_for_timeout(1200)  # the scroll's momentum
    await page.locator("#tpSet").tap()
    ok["settings_clear"] = (await box(".tpset"))["y"] + (await box(".tpset"))["height"] <= (await box("#tpad"))["y"]
    await page.locator('.tpset input[aria-label="Move speed"]').evaluate("r => { r.value = 2; r.dispatchEvent(new Event('input')); }")
    await drag(x - 30, y, 30, 0)
    p2 = await click_at(x, y)
    out["moved_2x"] = p2
    ok["move_2x"] = bool(p2 and abs(p2[0] - p1[0] - 60) <= 3)
    await page.locator('.tpset input[aria-label="Move speed"]').evaluate("r => { r.value = 1; r.dispatchEvent(new Event('input')); }")
    await page.locator(".tpset .hd button").tap()
    # pinch out: the picture grows about the pointer, which stays put; it stops at 4×
    await touch("touchStart", [{"x": x - 5, "y": y, "id": 0}, {"x": x + 5, "y": y, "id": 1}])
    for k in range(1, 25):
        await touch("touchMove", [{"x": x - 5 - k * 4, "y": y, "id": 0}, {"x": x + 5 + k * 4, "y": y, "id": 1}])
        await asyncio.sleep(0.02)
    await touch("touchEnd", [])
    await page.wait_for_timeout(500)
    out["zoom"] = round(await scale(), 2)
    ok["zoom_max_4"] = abs(out["zoom"] - 4) < 0.01
    p3 = await click_at(x, y)
    ok["zoom_keeps_pointer"] = p3 == p2
    # ⤢: the whole screen, and back
    await page.locator("#tpFit").tap()
    await page.wait_for_timeout(500)
    vb, tpb = await box("#video"), await box("#tpad")
    ok["whole_screen"] = abs(vb["width"] - 390) <= 2 or abs(vb["height"] - tpb["y"]) <= 2
    await page.locator("#tpFit").tap()
    await page.wait_for_timeout(500)
    ok["whole_and_back"] = abs(await scale() - 4) < 0.01
    # ⌨ types; the same key, in the same spot (now ▭), brings the trackpad back; the panel keeps its size
    sw, ph = await box("#tpKb"), (await box("#tpad"))["height"]
    await page.locator("#tpKb").tap()
    await page.wait_for_selector("#tpadKeys:not([hidden])")
    await page.wait_for_timeout(300)
    sw2, ph2 = await box("#tpKb"), (await box("#tpad"))["height"]
    ok["switch_same_spot"] = sw == sw2 and sw["x"] > 390 - 3 * 40  # in the right corner
    ok["same_size"] = abs(ph - ph2) <= 1
    probe.drain()
    for label in ("a", "b"):
        await page.locator("#tpadKeys .k", has_text=re.compile("^" + label + "$")).first.tap()
        await page.wait_for_timeout(120)
    got = []
    for _ in range(2):
        ev = await probe.wait(lambda e: e["ev"] == "key" and e["down"] == 1)
        if ev:
            got.append(ev["keysym"])
    ok["keyboard_types"] = got == ["a", "b"]
    # 🌐: the next input method, with the computer's shortcut (Linux: Super+Space)
    probe.drain()
    await page.locator("#tpadKeys .k", has_text=re.compile("^🌐$")).first.tap()
    ev = await probe.wait(lambda e: e["ev"] == "key" and e["down"] == 1 and e["keysym"] == "space", 3)
    out["globe"] = ev and {"state": ev["state"]}
    ok["globe_super_space"] = bool(ev and ev["state"] & 0x40)
    gb = await page.locator("#tpadKeys .k", has_text=re.compile("^🌐$")).first.bounding_box()
    eb = await page.locator("#tpadKeys .k", has_text=re.compile("^esc$")).first.bounding_box()
    ok["globe_top_row"] = abs(gb["y"] - eb["y"]) < 1
    await page.locator("#tpKb").tap()
    await page.wait_for_selector("#tpadSurface:not([hidden])")
    # ⌄: the panel folds to a button; the pointer and the picture stay where they are
    cur = await box("#tpCursor")
    await page.locator("#tpHide").tap()
    await page.wait_for_selector("#tpBall:not([hidden])")
    await page.wait_for_timeout(400)
    ok["collapsed_stays"] = await page.locator("#tpad").is_hidden() and await box("#tpCursor") == cur
    p4 = await click_at(195, 200)  # the picture is a trackpad too
    ok["collapsed_same_point"] = p4 == p2
    await page.locator("#tpBall").tap()
    await page.wait_for_selector("#tpad:not([hidden])")
    await page.wait_for_timeout(400)  # it grows back from nothing
    ok["ball_restores"] = await page.locator("#tpadSurface").is_visible()
    # the header resizes both (the keys' height): up as far as it goes stops at 58 px keys, down at 34 px
    # (the header, 5 rows, the gaps and margins: 36 + 5 × key + 32)
    await page.wait_for_timeout(400)
    g = await box("#tpGrip")
    await drag(g["x"] + g["width"] / 2, g["y"] + 18, 0, -450, steps=20, ms=20)
    await page.wait_for_timeout(400)
    hi = (await box("#tpad"))["height"]
    await drag(g["x"] + g["width"] / 2, 844 - hi + 18, 0, 400, steps=20, ms=20)
    await page.wait_for_timeout(400)
    lo = (await box("#tpad"))["height"]
    out["grip"] = [round(hi), round(lo)]
    ok["grip_limits"] = abs(hi - (68 + 5 * 58)) <= 2 and abs(lo - (68 + 5 * 34)) <= 2
    # a reload brings back the panel's size and the zoom
    await page.wait_for_timeout(800)
    await page.reload()
    await page.wait_for_function(ready, timeout=30000)
    await page.wait_for_selector("#tpad:not([hidden])")
    await page.wait_for_timeout(800)
    ok["reload_kept"] = abs((await box("#tpad"))["height"] - (68 + 5 * 34)) <= 2 and abs(await scale() - 4) < 0.01
    # the computer's own pointer is left out of the picture (cursor.show), and comes back in Direct mode:
    # count the pixels around it (in the yellow quadrant) that aren't yellow
    off_yellow = """() => { const v = document.querySelector('#video'), c = document.createElement('canvas');
        c.width = v.videoWidth; c.height = v.videoHeight; const g = c.getContext('2d'); g.drawImage(v, 0, 0);
        const px = g.getImageData(%d, %d, 16, 12).data; let n = 0;
        for (let i = 0; i < px.length; i += 4) if (Math.abs(px[i] - 255) + Math.abs(px[i + 1] - 242) + px[i + 2] > 90) n++;
        return n; }""" % (p2[0] - 7, p2[1] + 2)
    out["pointer_px"] = [await page.evaluate(off_yellow)]
    await page.locator("#ball").tap()
    await page.locator('#menu button[data-act="settings"]').tap()
    await page.get_by_role("button", name="Direct", exact=True).tap()
    await page.locator("#sheet-settings button[data-close]").tap()
    await page.wait_for_timeout(1200)
    out["pointer_px"].append(await page.evaluate(off_yellow))
    ok["pointer_hidden"] = out["pointer_px"][0] == 0 and out["pointer_px"][1] > 5
    ok["direct_off"] = await page.locator("#tpad").is_hidden() and await page.locator("#tpCursor").is_hidden()
    # the menu's Trackpad switch: on (lit), and off again
    await page.locator("#ball").tap()
    await page.locator("#menuTrackpad").tap()
    await page.wait_for_timeout(500)
    lit = await page.evaluate("() => document.querySelector('#menuTrackpad').classList.contains('on')")
    on = await page.locator("#tpad").is_visible() and lit
    await page.locator("#ball").tap()
    await page.locator("#menuTrackpad").tap()
    await page.wait_for_timeout(500)
    ok["menu_switch"] = on and await page.locator("#tpad").is_hidden()
    await ctx.close()

    # a desktop browser with a mouse: Auto leaves it off, the switch turns it on, and the mouse is a finger
    await asyncio.sleep(1.5)
    ctx = await browser.new_context(viewport={"width": 1000, "height": 700})
    await ctx.add_init_script("""(() => { const O = window.RTCPeerConnection; window.__pcs = [];
        window.RTCPeerConnection = class extends O { constructor(...a) { super(...a); window.__pcs.push(this); } }; })()""")
    page = await ctx.new_page()
    page.on("pageerror", lambda e: logs.append("PAGEERROR " + str(e)))
    await page.goto(f"http://127.0.0.1:{http_port}/")
    await page.fill("#user", "me")
    await page.fill("#password", "pw")
    await page.click("#go")
    await page.wait_for_url(f"http://127.0.0.1:{http_port}/")
    await page.wait_for_function(ready, timeout=30000)
    await page.wait_for_timeout(800)
    off = await page.locator("#tpad").is_hidden()
    await page.locator("#ball").click()
    await page.locator("#menuTrackpad").click()
    await page.wait_for_selector("#tpad:not([hidden])")
    await page.wait_for_timeout(800)
    ok["mouse_switch_on"] = off and await page.locator("#tpCursor").is_visible()
    # a desktop: a 560 px card floating at the bottom centre, keyboard-sized, the switch in its right corner
    tb, sw = await page.locator("#tpad").bounding_box(), await page.locator("#tpKb").bounding_box()
    out["card"] = [round(tb["x"]), round(tb["y"]), round(tb["width"]), round(tb["height"])]
    ok["mouse_card"] = (abs(tb["width"] - 560) <= 2 and abs(tb["x"] + tb["width"] / 2 - 500) <= 2
                        and abs(tb["height"] - (68 + 5 * 38)) <= 2 and sw["x"] > tb["x"] + tb["width"] - 3 * 40)
    await page.locator("#tpKb").click()
    await page.wait_for_selector("#tpadKeys:not([hidden])")
    tb2 = await page.locator("#tpad").bounding_box()
    ok["mouse_same_size"] = tb2 == tb and await page.locator("#tpKb").bounding_box() == sw
    await page.locator("#tpKb").click()
    await page.wait_for_selector("#tpadSurface:not([hidden])")
    b = await page.locator("#tpadSurface").bounding_box()
    x, y = b["x"] + b["width"] / 2, b["y"] + b["height"] / 2 + 15

    async def mclick(x, y, button="left", b=1):
        probe.drain()
        await page.mouse.click(x, y, button=button)
        ev = await probe.wait(lambda e: e["ev"] == "button" and e["b"] == b and e["down"] == 1)
        return ev and (ev["x"], ev["y"])
    m0 = await mclick(x, y)
    ok["mouse_click_at_pointer"] = bool(m0 and abs(m0[0] - 640) <= 2 and abs(m0[1] - 360) <= 2)
    await asyncio.sleep(0.4)  # not a tap-then-drag
    await page.mouse.move(x - 30, y)
    await page.mouse.down()
    for k in range(1, 31):
        await page.mouse.move(x - 30 + 2 * k, y)
        await asyncio.sleep(0.08)  # 25 px/s, under the acceleration's 50; mouse events come in bursts
    await page.mouse.up()
    m1 = await mclick(x + 100, y - 20)
    out["mouse"] = [m0, m1]
    ok["mouse_move_60"] = bool(m0 and m1 and abs(m1[0] - m0[0] - 60) <= 3 and abs(m1[1] - m0[1]) <= 3)
    await asyncio.sleep(0.4)
    ok["mouse_right_click"] = await mclick(x - 100, y, button="right", b=3) == m1
    probe.drain()
    await page.mouse.wheel(0, 300)
    ev = await probe.wait(lambda e: e["ev"] == "button" and e["b"] == 5)
    ok["mouse_wheel"] = bool(ev and abs(ev["x"] - m1[0]) <= 2 and abs(ev["y"] - m1[1]) <= 2)

    # the 10% margin: the 1280×720 screen at 1× in the 1000×700 window. The arrow moves freely up to
    # x 900; past it the picture scrolls until its right edge meets the window's; then the arrow goes on
    # to the screen's right edge
    async def mdrag(dx):
        await asyncio.sleep(0.4)  # not a tap-then-drag
        await page.mouse.move(x - dx / 2, y)
        await page.mouse.down()
        for k in range(1, int(dx / 2) + 1):
            await page.mouse.move(x - dx / 2 + 2 * k, y)
            await asyncio.sleep(0.08)
        await page.mouse.up()
        await asyncio.sleep(0.2)
        c, v = await page.locator("#tpCursor").bounding_box(), await page.locator("#video").bounding_box()
        return c["x"] + 1.5, v["x"]
    c0, v0 = await mdrag(2)
    c1, v1 = await mdrag(round((880 - c0) / 2) * 2)
    c2, v2 = await mdrag(200)
    c3, v3 = await mdrag(200)
    out["margin"] = [[round(c0), round(v0)], [round(c1), round(v1)], [round(c2), round(v2)], [round(c3), round(v3)]]
    ok["margin_free"] = abs(c1 - 880) <= 4 and v1 == v0
    ok["margin_scrolls"] = v2 < v0 and abs(v2 + 1280 - 1000) <= 2 and 900 < c2 < 1000
    ok["margin_corner"] = abs(c3 - 1000) <= 2 and v3 == v2
    # the menu's Full screen goes full screen, and the same button leaves it (it used to take the page
    # for a Home Screen app, since Chrome reports display-mode fullscreen, and stay)
    fs = "() => !!document.fullscreenElement"
    await page.locator("#ball").click()
    await page.locator("#menuFull").click()
    await page.wait_for_timeout(600)
    went = await page.evaluate(fs)
    await page.locator("#ball").click()
    await page.locator("#menuFull").click()
    await page.wait_for_timeout(600)
    ok["fullscreen_toggles"] = went and not await page.evaluate(fs)
    # the stats overlay names the path and shows its addresses: from this browser's ip:port to the agent's
    await page.wait_for_timeout(1200)
    hud = await page.evaluate("() => document.querySelector('#hud').innerText")
    out["hud_path"] = [l for l in hud.split("\n") if l.split(" ")[0] in ("path", "from", "relay", "to")]
    ok["hud_path"] = (bool(re.search(r"^path direct, same network · udp$", hud, re.M))
                      and bool(re.search(r"^from (\S+:\d+|this device, port \d+)$", hud, re.M)) and bool(re.search(r"^to \S+:\d+$", hud, re.M)))
    # a new codec applies at once: a fresh stream, with video again
    pcs = await page.evaluate("() => window.__pcs.length")
    await page.locator("#ball").click()
    await page.locator('#menu button[data-act="settings"]').click()
    await page.get_by_role("button", name="H.264", exact=True).click()
    await page.locator("#sheet-settings button[data-close]").click()
    try:
        await page.wait_for_function("n => window.__pcs.length > n && document.querySelector('#disc').hidden"
                                     " && window.__pcs[window.__pcs.length - 1].connectionState === 'connected'", arg=pcs, timeout=20000)
        await page.wait_for_function("() => document.querySelector('#video').videoWidth > 0", timeout=10000)
        ok["codec_applies_now"] = True
    except Exception:
        ok["codec_applies_now"] = False
    out["ok"] = ok
    await ctx.close()
    return out


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
    # sound: a PulseAudio with a null sink as the default output, playing a tone the agent records
    # from its monitor (@DEFAULT_MONITOR@)
    sound = bool(shutil.which("pulseaudio") and shutil.which("paplay"))
    pads = os.access("/dev/uinput", os.W_OK)
    if sound:
        env["XDG_RUNTIME_DIR"] = f"{d}/run"
        os.makedirs(env["XDG_RUNTIME_DIR"], mode=0o700)
        procs.append(subprocess.Popen(["pulseaudio", "-n", "--daemonize=no", "--exit-idle-time=-1", "--use-pid-file=no",
                                       "--load=module-null-sink sink_name=out", "--load=module-native-protocol-unix"],
                                      env=env, stderr=quiet, stdout=quiet))
        tone_wav(f"{d}/tone.wav")
        for _ in range(50):
            if subprocess.run(["pactl", "info"], env=env, capture_output=True).returncode == 0:
                break
            time.sleep(0.1)
        procs.append(subprocess.Popen(["paplay", f"{d}/tone.wav"], env=env, stderr=quiet))
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
            await ctx.grant_permissions(["clipboard-read", "clipboard-write"], origin=f"http://127.0.0.1:{http_port}")
            await ctx.add_init_script("""(() => { const O = window.RTCPeerConnection; window.__pcs = [];
                window.RTCPeerConnection = class extends O { constructor(...a) { super(...a); window.__pcs.push(this); } }; })()""")
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
            # text from the Keyboard sheet (⋯ on the floating keyboard), including a character no key types
            probe.drain()
            await page.locator("#ball").click()
            await page.locator('#menu button[data-act="keys"]').click()
            await page.click("#fkbMore")
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
            # a shortcut button: C arrives with Control still held (state bit 4)
            probe.drain()
            await page.locator("#kbKeys button", has_text="Ctrl+C").click()
            ev = await probe.wait(lambda e: e["ev"] == "key" and e["down"] == 1 and e["keycode"] == 54)
            res["ctrl_c_ok"] = bool(ev and ev["state"] & 4)
            await page.locator("#sheet-keys button[data-close]").click()
            # clipboard: the computer copies → this clipboard by itself, or the clip bar where the browser
            # doesn't let the page write it (tests/e2e_files.py checks the automatic sync both ways)
            probe.send("setclip copied on linux")
            await probe.wait(lambda e: e["ev"] == "setclip")
            try:
                await page.wait_for_function(
                    "async () => (document.querySelector('#clipbarText') || {}).textContent === 'copied on linux'"
                    " || await navigator.clipboard.readText().catch(() => '') === 'copied on linux'", timeout=8000)
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
            # the computer's sound: Opus packets keep arriving, and they decode to a tone
            stats = """async () => { let a = null; (await window.__pcs[window.__pcs.length - 1].getStats()).forEach(r => {
                if (r.type === 'inbound-rtp' && r.kind === 'audio') a = r; });
                return a && { packets: a.packetsReceived, energy: a.totalAudioEnergy || 0, samples: a.totalSamplesReceived || 0 }; }"""
            a0 = await page.evaluate(stats)
            await page.wait_for_timeout(2000)
            a1 = await page.evaluate(stats)
            res["sound"] = a0 and a1 and {"packets_2s": a1["packets"] - a0["packets"],
                                          "energy_2s": round(a1["energy"] - a0["energy"], 4)}
            if not sound:
                res["sound"] = "skipped (no pulseaudio)"
            res["hud"] = await page.inner_text("#hud")
            res["errors"] = [l for l in logs if "PAGEERROR" in l][:5]
            await ctx.close()
            # a phone-sized touch screen (an iPhone viewer) set to Direct: tap = click where it touches,
            # long press = right click, two fingers = scroll (the trackpad, a phone's default, comes after)
            await asyncio.sleep(1.5)
            tctx = await browser.new_context(viewport={"width": 390, "height": 844}, has_touch=True, is_mobile=True)
            await tctx.add_init_script("""try { const k = 'remodroid-v1', s = JSON.parse(localStorage.getItem(k) || '{}');
                s.touchCtl = 'direct'; localStorage.setItem(k, JSON.stringify(s)); } catch (e) {}""")
            # the viewer runs on the computer itself here: it would take the agent's virtual pad for a
            # controller paired with it and feed its state back into pad 1
            await tctx.add_init_script("Navigator.prototype.getGamepads = () => [];")
            # the gamepad messages the page sends (shown when the gamepad check fails)
            await tctx.add_init_script("""window.__padLog = []; const send = RTCDataChannel.prototype.send;
                RTCDataChannel.prototype.send = function (d) {
                  if (d instanceof Uint8Array && d[0] === 6) { const v = new DataView(d.buffer, d.byteOffset);
                    window.__padLog.push([performance.now() | 0, v.getUint32(2), v.getInt16(6), v.getInt16(8), this.readyState]); }
                  return send.call(this, d); };
                window.__ptrLog = [];
                for (const t of ['pointerdown', 'pointerup', 'pointercancel', 'lostpointercapture', 'touchend', 'touchcancel'])
                  window.addEventListener(t, e => window.__ptrLog.push([performance.now() | 0, t, e.pointerId, e.pointerType,
                    e.clientX | 0, e.target && e.target.className]), true);""")
            tp = await tctx.new_page()
            tp.on("pageerror", lambda e: logs.append("PAGEERROR " + str(e)))
            await tp.goto(f"http://127.0.0.1:{http_port}/")
            await tp.fill("#user", "me")
            await tp.fill("#password", "pw")
            await tp.click("#go")
            await tp.wait_for_url(f"http://127.0.0.1:{http_port}/")
            await tp.wait_for_function("() => document.querySelector('#disc').hidden && document.querySelector('#video').videoWidth > 0",
                                       timeout=30000)
            await tp.wait_for_timeout(800)
            tb = await tp.locator("#video").bounding_box()
            at = lambda fx, fy: {"x": tb["x"] + tb["width"] * fx, "y": tb["y"] + tb["height"] * fy}
            cdp = await tctx.new_cdp_session(tp)
            touch = lambda kind, pts: cdp.send("Input.dispatchTouchEvent", {"type": kind, "touchPoints": pts})
            probe.drain()
            await touch("touchStart", [at(0.75, 0.75)])
            await touch("touchEnd", [])
            ev = await probe.wait(lambda e: e["ev"] == "button" and e["b"] == 1 and e["down"] == 1)
            res["touch_tap"] = ev and [ev["x"], ev["y"]]
            res["touch_tap_ok"] = bool(ev and abs(ev["x"] - 960) <= 6 and abs(ev["y"] - 540) <= 6)
            await touch("touchStart", [at(0.25, 0.25)])
            await asyncio.sleep(0.8)
            await touch("touchEnd", [])
            ev = await probe.wait(lambda e: e["ev"] == "button" and e["b"] == 3 and e["down"] == 1)
            res["touch_long_press_ok"] = bool(ev and abs(ev["x"] - 320) <= 6 and abs(ev["y"] - 180) <= 6)
            await touch("touchStart", [at(0.5, 0.6), at(0.6, 0.6)])
            for k in range(1, 8):
                await touch("touchMove", [at(0.5, 0.6 - k * 0.04), at(0.6, 0.6 - k * 0.04)])
                await asyncio.sleep(0.03)
            await touch("touchEnd", [])
            ev = await probe.wait(lambda e: e["ev"] == "button" and e["b"] in (4, 5))
            res["touch_scroll"] = ev and ev["b"]
            # the floating keyboard (Keyboard in the menu on a computer), tapped: h ⇧ i ?123 1 abc ⌫ ctrl c
            await tp.locator("#ball").tap()
            await tp.locator('#menu button[data-act="keys"]').tap()
            await tp.wait_for_selector("#fkb:not([hidden])")
            box = await tp.locator("#fkb").bounding_box()
            res["fkb_fits"] = bool(box and box["x"] >= 0 and box["x"] + box["width"] <= 390 and box["y"] + box["height"] <= 844)
            probe.drain()
            got = []
            for label in ("h", "⇧", "I", "?123", "1", "abc", "⌫", "ctrl", "c"):
                await tp.locator("#fkb .k", has_text=re.compile("^" + re.escape(label) + "$")).first.tap()
                await tp.wait_for_timeout(120)
            end = time.time() + 8
            while len(got) < 5 and time.time() < end:
                ev = await probe.wait(lambda e: e["ev"] == "key" and e["down"] == 1 and e["keysym"] not in ("Shift_L", "Control_L"), 1)
                if ev:
                    got.append(ev["keysym"] + ("+ctrl" if ev["state"] & 4 else ""))
            res["fkb"] = got
            # the on-screen gamepad, sideways: A plugs in a virtual Xbox 360 pad; B and the left stick
            # arrive as its evdev events (BTN_B, ABS_X)
            if pads:
                await tp.locator("#fkbHide").tap()
                await tp.set_viewport_size({"width": 844, "height": 390})
                await tp.locator("#ball").tap()
                await tp.locator('#menu button[data-act="gamepad"]').tap()
                await tp.wait_for_selector("#gpad:not([hidden])")
                face = lambda t: tp.locator("#gpad .gc", has_text=re.compile("^" + t + "$"))
                await face("A").tap()
                node = None
                for _ in range(50):
                    node = pad_node()
                    if node:
                        break
                    await tp.wait_for_timeout(100)
                evs, fd = [], None
                if node:
                    await tp.wait_for_timeout(300)
                    try:
                        fd = os.open(node, os.O_RDONLY | os.O_NONBLOCK)
                    except OSError:  # a sandbox without input device nodes
                        pass
                if fd is not None:
                    await face("B").tap()
                    sb = await tp.locator("#gpad .stick:not(.r)").bounding_box()
                    cx, cy = sb["x"] + sb["width"] / 2, sb["y"] + sb["height"] / 2
                    await touch("touchStart", [{"x": cx, "y": cy}])
                    for k in range(1, 6):
                        await touch("touchMove", [{"x": cx + k * sb["width"] / 8, "y": cy}])
                        await asyncio.sleep(0.03)
                    await tp.wait_for_timeout(300)
                    await touch("touchEnd", [])
                    # read until the stick is back in the middle (a busy CI runner can take a while)
                    data, end = b"", time.time() + 4
                    while True:
                        try:
                            data += os.read(fd, 24 * 256)
                        except BlockingIOError:
                            pass
                        evs = [struct.unpack("llHHi", data[o:o + 24])[2:] for o in range(0, len(data) - 23, 24)]
                        xs = [v for t, c, v in evs if t == 3 and c == 0]
                        res["pad"] = {"device": True, "b": any(e == (1, 0x131, 1) for e in evs),
                                      "stick_right": max(xs or [0]) > 20000, "stick_back": xs[-1:] == [0]}
                        if all(res["pad"].values()) or time.time() > end:
                            break
                        await tp.wait_for_timeout(100)
                    os.close(fd)
                    if not all(res["pad"].values()):  # what arrived, for the log
                        res["pad_events"] = [e for e in evs if e[0] != 0][-16:]
                        res["pad_sent"] = await tp.evaluate("window.__padLog || null")
                        res["pad_pointers"] = await tp.evaluate("(window.__ptrLog || []).slice(-8)")
                else:
                    res["pad"] = {"device": bool(node), "events": "not readable here"}
                # ⚙ → Fill screen: the picture covers the sideways phone, and a tap still lands where it shows
                await tp.locator("#gpad .cfg").tap()
                await tp.get_by_role("button", name="Fill screen").tap()
                await tp.get_by_role("button", name="Done").tap()
                vb = await tp.locator("#video").bounding_box()
                res["zoom_fills"] = bool(vb and vb["x"] <= 0 and vb["y"] <= 0 and vb["x"] + vb["width"] >= 843 and vb["y"] + vb["height"] >= 389)
                # swipe to look: a finger dragged right and down across the picture tilts the right stick
                # (ABS_RX, ABS_RY) while it moves, and the stick springs back when it stops
                try:
                    fd = os.open(pad_node() or "/nonexistent", os.O_RDONLY | os.O_NONBLOCK)
                except OSError:
                    fd = None
                await touch("touchStart", [{"x": 380, "y": 150}])
                for k in range(1, 10):
                    await touch("touchMove", [{"x": 380 + k * 12, "y": 150 + k * 8}])
                    await asyncio.sleep(0.016)
                await tp.wait_for_timeout(300)
                await touch("touchEnd", [])
                if fd is not None:
                    data, end = b"", time.time() + 4
                    while True:
                        try:
                            data += os.read(fd, 24 * 256)
                        except BlockingIOError:
                            pass
                        evs = [struct.unpack("llHHi", data[o:o + 24])[2:] for o in range(0, len(data) - 23, 24)]
                        rxs = [v for t, c, v in evs if t == 3 and c == 3]
                        rys = [v for t, c, v in evs if t == 3 and c == 4]
                        res["look"] = {"turned": max(rxs or [0]) > 8000, "tilted": max(rys or [0]) > 4000,
                                       "back": rxs[-1:] == [0] and rys[-1:] == [0]}
                        if all(res["look"].values()) or time.time() > end:
                            break
                        await tp.wait_for_timeout(100)
                    os.close(fd)
                else:
                    res["look"] = "not readable here"
                # ⚙ → Edit layout: drag the left stick 100 px to the right; Done keeps it there next time
                await tp.locator("#gpad .cfg").tap()
                await tp.get_by_role("button", name=re.compile("Edit layout")).tap()
                st0 = await tp.locator("#gpad .stick:not(.r)").bounding_box()
                sx, sy = st0["x"] + st0["width"] / 2, st0["y"] + st0["height"] / 2
                await touch("touchStart", [{"x": sx, "y": sy}])
                for k in range(1, 6):
                    await touch("touchMove", [{"x": sx + k * 20, "y": sy}])
                    await asyncio.sleep(0.016)
                await touch("touchEnd", [])
                await tp.wait_for_timeout(300)
                await tp.get_by_role("button", name="Done").tap()
                st1 = await tp.locator("#gpad .stick:not(.r)").bounding_box()
                res["edit_moved"] = abs(st1["x"] - st0["x"] - 100) < 3
                await tp.locator("#gpad .x").tap()
                await tp.locator("#ball").tap()
                await tp.locator('#menu button[data-act="gamepad"]').tap()
                st2 = await tp.locator("#gpad .stick:not(.r)").bounding_box()
                res["edit_kept"] = abs(st2["x"] - st1["x"]) < 2
                # the cookie brings the gamepad settings back when the page's storage is cleared
                await tp.evaluate("localStorage.clear()")
                await tp.reload()
                await tp.wait_for_function("() => document.querySelector('#disc').hidden && document.querySelector('#video').videoWidth > 0",
                                           timeout=30000)
                await tp.locator("#ball").tap()
                await tp.locator('#menu button[data-act="gamepad"]').tap()
                st3 = await tp.locator("#gpad .stick:not(.r)").bounding_box()
                res["cookie_kept"] = abs(st3["x"] - st1["x"]) < 2
                # closed, the gamepad leaves the zoom on, and a tap clicks where the zoomed picture shows
                await tp.locator("#gpad .x").tap()
                probe.drain()
                await touch("touchStart", [{"x": 422, "y": 195}])
                await touch("touchEnd", [])
                ev = await probe.wait(lambda e: e["ev"] == "button" and e["b"] == 1 and e["down"] == 1)
                want = (round((422 - vb["x"]) / vb["width"] * 1280), round((195 - vb["y"]) / vb["height"] * 720))
                res["zoom_tap_ok"] = bool(ev and abs(ev["x"] - want[0]) <= 6 and abs(ev["y"] - want[1]) <= 6)
            else:
                res["pad"] = "skipped (no /dev/uinput)"
            await tctx.close()
            await asyncio.sleep(1.5)
            res["trackpad"] = await trackpad(browser, http_port, probe, logs)
            # a Mac viewer: Ctrl+C stays Ctrl+C (a terminal's interrupt), ⌘C becomes Ctrl+C (copy)
            await asyncio.sleep(1.5)
            mctx = await browser.new_context(viewport={"width": 1280, "height": 720})
            await mctx.add_init_script("Object.defineProperty(Navigator.prototype, 'platform', { get: () => 'MacIntel' })")
            mp = await mctx.new_page()
            mp.on("pageerror", lambda e: logs.append("PAGEERROR " + str(e)))
            await mp.goto(f"http://127.0.0.1:{http_port}/")
            await mp.fill("#user", "me")
            await mp.fill("#password", "pw")
            await mp.click("#go")
            await mp.wait_for_url(f"http://127.0.0.1:{http_port}/")
            await mp.wait_for_function("() => document.querySelector('#disc').hidden && document.querySelector('#video').videoWidth > 0",
                                       timeout=30000)
            await mp.wait_for_timeout(800)
            for name, combo in (("mac_ctrl_c", "Control+KeyC"), ("mac_cmd_c", "Meta+KeyC")):
                probe.drain()
                await mp.keyboard.press(combo)
                ev = await probe.wait(lambda e: e["ev"] == "key" and e["down"] == 1 and e["keycode"] == 54)
                res[name] = ev and {"state": ev["state"], "ctrl": bool(ev["state"] & 4), "super": bool(ev["state"] & 0x40)}
                await mp.wait_for_timeout(200)
            res["errors"] = [l for l in logs if "PAGEERROR" in l][:5]
            await browser.close()
        ok = (res.get("pair") and res.get("platform") == "linux" and res["video"]["w"] == 1280 and res.get("colours_ok") and res.get("click_ok")
              and res.get("right_click_ok") and res.get("wheel_ok") and res.get("key_a") == "a" and res.get("shift_1") == "exclam"
              and res.get("enter_ok") and res.get("text") == ["H", "i", "space", "eacute"] and res.get("ctrl_c_ok") and res.get("clip_to_viewer")
              and res.get("clip_to_computer") == "sent from the browser" and res.get("upload_ok")
              and res.get("touch_tap_ok") and res.get("touch_long_press_ok") and res.get("touch_scroll") == 5
              and res.get("fkb_fits") and res.get("fkb") == ["h", "I", "1", "BackSpace", "c+ctrl"]
              and (not pads or res.get("pad") == {"device": True, "b": True, "stick_right": True, "stick_back": True}
                   or (not os.environ.get("CI") and res.get("pad") == {"device": True, "events": "not readable here"}))
              and (not pads or (res.get("zoom_fills") and res.get("zoom_tap_ok")
                                and res.get("edit_moved") and res.get("edit_kept") and res.get("cookie_kept")
                                and (res.get("look") == {"turned": True, "tilted": True, "back": True}
                                     or (not os.environ.get("CI") and res.get("look") == "not readable here"))))
              and isinstance(res.get("trackpad"), dict) and all(res["trackpad"]["ok"].values())
              and res.get("mac_ctrl_c") == {"state": 4, "ctrl": True, "super": False}
              and res.get("mac_cmd_c") == {"state": 4, "ctrl": True, "super": False}
              and (not sound or (isinstance(res.get("sound"), dict) and res["sound"]["packets_2s"] >= 80 and res["sound"]["energy_2s"] > 0.01))
              and not res.get("errors"))
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
