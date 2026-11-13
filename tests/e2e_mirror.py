#!/usr/bin/env python3
"""Page mode end to end (browser/dom-mirror.md): the real web client on an iPhone-sized touch screen in
headless Chrome, showing the remote browser's page as elements instead of video, and operating it.

  Xorg :N (dummy 880x1808, inputtest touch) ── rd-shell ── Chrome (kiosk, the real page) ◀─ DevTools pipe
     ▲ touches                                    ▲ socket
  remodroid-desktopd ◀── socket ── rd-mirror ─────┘
     ▲ WebRTC: "dom" channel (the page), input, ctrl
  Chrome, 428x926 with touch: web/app.js + pagemode.js + mirror.js

Checks: Page mode shows the page's own elements under the viewer's bar; photos arrive, a big one made
smaller as WebP; a tap on a button in the mirror is
a real (trusted) tap on the real one; typing into the mirror's field arrives in the real field; a <select>
picked in the mirror is picked on the real page; a confirm() is asked on the viewer; a playing video brings the picture
for as long as it plays in Auto, and never in Page; scrolling the mirror to its end makes the real page load
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


class SiteDone(Exception):
    """--url: the real site has been looked at; the rest of the checks are for the test page."""


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
home = {origin + "/ops.html" if args.url in (None, "local") else args.url}
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
            if args.webkit:  # Safari's engine: what the iPhone and the Mac app run
                browser = await p.webkit.launch()
            else:
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
            await page.evaluate("() => PageMode.choose('auto')")
            fr = page.frame_locator("#pmFrame")
            if args.url:  # a real site: does it show, pictures included, and does the session hold
                await asyncio.sleep(args.watch)
                res["site"] = await page.evaluate("""() => { const d = document.querySelector('#pmFrame').contentDocument;
                    const imgs = [...d.querySelectorAll('img')], vis = imgs.filter(i => { const r = i.getBoundingClientRect(); return r.width > 20 && r.bottom > 0 && r.top < 860; });
                    return { title: d.title, nodes: d.getElementsByTagName('*').length, imgs: imgs.length, with_src: imgs.filter(i => i.getAttribute('src')).length,
                             loaded: imgs.filter(i => i.naturalWidth > 0).length, visible: vis.length, visible_loaded: vis.filter(i => i.naturalWidth > 0).length,
                             visible_src: vis.slice(0, 6).map(i => (i.getAttribute('src') || '-').slice(0, 40)), log: PageMode.log.slice(-6),
                             on: PageMode.on, pm_hidden: document.querySelector('#pm').hidden }; }""")
                res["pictures"] = await page.evaluate("""() => { const f = document.querySelector('#pmFrame'), d = f.contentDocument, m = window.__pmMirror;
                    const states = {}; if (m) for (const [u, v] of m.res) { const k = v === '' ? 'asked' : v === null ? 'none' : 'have'; states[k] = (states[k] || 0) + 1; }
                    const big = [...d.querySelectorAll('img')].filter(i => i.getBoundingClientRect().width > 40).slice(0, 5)
                      .map(i => ({ src: (i.getAttribute('src') || '-').slice(0, 30), nat: i.naturalWidth, w: Math.round(i.getBoundingClientRect().width),
                                   cls: i.className.slice(0, 30), parent: i.parentNode && i.parentNode.localName }));
                    return { states, waits: m ? m.waits.size : -1, big }; }""")
                res["sessions"] = sum(1 for l in open(f"{d}/agent.log") if "connected: h26" in l)
                res["ended"] = [l.strip()[-60:] for l in open(f"{d}/agent.log") if " ended: " in l]
                if args.shot:
                    await page.screenshot(path=args.shot)
                ok = bool(res["site"]["visible_loaded"]) and res["sessions"] == 1
                await browser.close()
                raise SiteDone()
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

            # photos: captured from the real browser's cache, made smaller (WebP, no wider than this screen), shown
            await page.wait_for_function("""() => { const d = document.querySelector('#pmFrame').contentDocument;
                return ['photo', 'photo2', 'logo'].every(id => { const i = d.getElementById(id); return i && i.naturalWidth > 0; }); }""",
                                         timeout=15000)
            res["photos"] = await page.evaluate("""async () => { const d = document.querySelector('#pmFrame').contentDocument, out = {};
                for (const id of ['photo', 'photo2', 'logo']) { const i = d.getElementById(id), b = await (await fetch(i.src)).blob();
                    out[id] = [i.naturalWidth, i.naturalHeight, b.type, b.size]; }
                return out; }""")
            p1, p2 = res["photos"]["photo"], res["photos"]["photo2"]
            res["photos_ok"] = (p1[2] == "image/webp" and p1[0] <= 1284 and p1[3] < os.path.getsize(f"{ROOT}/browser/test/mirror/pages/photo.jpg")
                                and p2[0] == 900 and p2[3] > 0 and res["photos"]["logo"][2] == "image/svg+xml")

            # an icon from a file of icons (<use href="icons.svg#dot">): the file came as bytes and is in the mirror
            icon = lambda: page.evaluate("""() => { const d = document.querySelector('#pmFrame').contentDocument, u = d.querySelector('#icon use'),
                h = u && u.getAttribute('href'), t = h && h[0] === '#' && d.getElementById(h.slice(1));
                return [h, !!t, u ? Math.round(u.getBoundingClientRect().width) : 0]; }""")
            await page.wait_for_function("() => { const u = document.querySelector('#pmFrame').contentDocument.querySelector('#icon use');"
                                         " return u && /^#rd-i\\d+-dot$/.test(u.getAttribute('href') || ''); }", timeout=10000)
            res["icon"] = await icon()
            # a file the page's tools have no copy of (it never loaded this one): refused as a picture would be,
            # and loaded by the browser when it is asked for as a file of icons is
            res["loaded"] = await page.evaluate("""async ([a, b]) => { const m = window.__pmMirror, size = async (u, load) => {
                await new Promise(r => m.want(u, r, load)); const x = m.blobs && m.blobs.get(u); return x ? (await x.text()).length : 0; };
                return [await size(a, false), await size(b, true)]; }""", [f"{origin}/imported.css", f"{origin}/late.css"])
            res["loaded_ok"] = res["loaded"] == [0, os.path.getsize(f"{ROOT}/browser/test/mirror/pages/late.css")]

            # a tap on the mirror's button: a real, trusted tap on the real one, and the new text comes back
            await tap("#btn")
            await until("#btn", "taps: 1")
            await page.wait_for_function("() => /-bar$/.test(document.querySelector('#pmFrame').contentDocument.querySelector('#icon use').getAttribute('href') || '')",
                                         timeout=10000)  # the page changed the icon: the mirror's points at the other one
            res["icon2"] = await icon()
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
            await page.keyboard.insert_text("，你好")  # no key types these: the browser puts them in
            await until("#echo", "echo:Hello world，你好")
            res["typed_zh"] = await txt("#echo")

            # a box the page made editable itself (a chat's message box): what is typed in the mirror's is typed
            # in the real one; its button, dead while the box is empty, sends it; then Enter does
            await tap("#ed")
            await asyncio.sleep(0.5)
            await fr.locator("#ed").focus()
            await page.keyboard.type("Hi thare", delay=30)
            await until("#ed", "Hi thare")
            for _ in range(3):
                await page.keyboard.press("Backspace")
            await page.keyboard.type("ere", delay=30)
            await until("#ed", "Hi there")
            await tap("#send")
            await until("#sent", "sent: Hi there (button)")
            await tap("#ed")
            await asyncio.sleep(0.5)
            await fr.locator("#ed").focus()
            await page.keyboard.type("again", delay=30)
            await until("#ed", "again")
            await page.keyboard.insert_text("乔布斯")
            await until("#ed", "again乔布斯")
            # an input method at work: the letters it composes from stay here, its finished word goes over
            await cdp.send("Input.imeSetComposition", {"text": "hao", "selectionStart": 3, "selectionEnd": 3})
            await asyncio.sleep(0.3)
            await cdp.send("Input.insertText", {"text": "好"})
            await until("#ed", "again乔布斯好")
            await asyncio.sleep(0.8)
            res["composed"] = await txt("#ed")
            await page.keyboard.press("Enter")
            await until("#sent", "sent: again乔布斯好 (enter)")
            res["editable"] = await txt("#sent")

            # a <select>: picked with this device's control, set on the real page
            await fr.locator("#sel").select_option("blue")
            await until("#picked", "picked: blue")
            res["select"] = await txt("#picked")

            # confirm(): the real page waits, the viewer's own dialog asks, the answer goes back
            page.on("dialog", lambda dlg: asyncio.ensure_future(dlg.accept()))
            await tap("#ask")
            await until("#ask", "said yes")
            res["dialog"] = await txt("#ask")

            # a video that plays can't be sent as elements: the picture while it plays, the page again after
            f0 = await page.evaluate("() => document.querySelector('#video').getVideoPlaybackQuality().totalVideoFrames")
            await tap("#play")
            await page.wait_for_function("f0 => document.querySelector('#pm').hidden && PageMode.on && "
                                         "document.querySelector('#video').getVideoPlaybackQuality().totalVideoFrames > f0 + 2",
                                         arg=f0, timeout=8000)
            await page.wait_for_function("() => !document.querySelector('#pm').hidden", timeout=10000)
            await until("#play", "played")
            res["video_while_playing"] = True

            # Page: the video plays on the browser and the page stays; the picture never comes by itself
            await page.evaluate("() => PageMode.choose('page')")
            await page.wait_for_function("() => PageMode.on && !document.querySelector('#pm').hidden", timeout=10000)
            await asyncio.sleep(1)
            f1 = await page.evaluate("() => document.querySelector('#video').getVideoPlaybackQuality().totalVideoFrames")
            await tap("#play")
            stayed = True
            for _ in range(8):
                await asyncio.sleep(0.5)
                stayed = stayed and await page.evaluate("() => !document.querySelector('#pm').hidden")
            f2 = await page.evaluate("() => document.querySelector('#video').getVideoPlaybackQuality().totalVideoFrames")
            res["page_only"] = {"page_stayed": stayed, "video_frames": f2 - f1,
                                "remembered": await page.evaluate("() => Object.keys(localStorage).filter(k => k.startsWith('remodroid-page:')).map(k => localStorage.getItem(k))")}
            res["page_only_ok"] = stayed and f2 - f1 <= 2 and res["page_only"]["remembered"] == ["page"]
            await page.evaluate("() => PageMode.choose('auto')")
            await page.wait_for_function("() => PageMode.on && !document.querySelector('#pm').hidden", timeout=10000)

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

            # the user's own proxy, typed in the viewer's menu: kept in their config beside the profile, the browser
            # started again through it; something that isn't a proxy is refused; nothing typed takes it away
            conf_file = f"{d}/users/Alice/config"
            open(conf_file, "w").write("# mine\ntimezone = Europe/Berlin\n")
            def chrome_proxy():
                out = subprocess.run(["pgrep", "-af", "--", f"--user-data-dir={d}/users/Alice"], capture_output=True, text=True).stdout
                m = re.search(r"--proxy-server=(\S+)", out)
                return m.group(1) if m else ("" if out.strip() else None)
            async def set_proxy(text):
                await page.evaluate("t => { window.prompt = () => t; }", text)
                await page.evaluate("""() => { document.querySelector('#pmBar button[title=Menu]').click();
                    [...document.querySelectorAll('#pmMenu button')].find(b => /^Proxy:/.test(b.textContent)).click(); }""")
            async def wait_proxy(want):
                for _ in range(40):
                    if chrome_proxy() == want:
                        return True
                    await asyncio.sleep(0.25)
                return False
            await set_proxy("not a proxy")
            await asyncio.sleep(1)
            px = {"refused": "proxy" not in open(conf_file).read() and chrome_proxy() == ""}
            await set_proxy("127.0.0.1:9")
            px["set"] = await wait_proxy("socks5://127.0.0.1:9")
            px["file"] = open(conf_file).read()
            await asyncio.sleep(1)
            await set_proxy("")
            px["unset"] = await wait_proxy("")
            px["file_after"] = open(conf_file).read()
            res["proxy"] = px
            res["proxy_ok"] = (px["refused"] and px["set"] and px["unset"] and px["file"] == "# mine\ntimezone = Europe/Berlin\nproxy = socks5://127.0.0.1:9\n"
                               and px["file_after"] == "# mine\ntimezone = Europe/Berlin\n")
            await until("#h", "Mirror test", 20000)  # the browser is back, with the page it had

            # Video mode again: the picture comes back
            f0 = await page.evaluate("() => document.querySelector('#video').getVideoPlaybackQuality().totalVideoFrames")
            await page.evaluate("() => PageMode.choose('video')")
            await page.wait_for_function("f0 => document.querySelector('#pm').hidden && "
                                         "document.querySelector('#video').getVideoPlaybackQuality().totalVideoFrames > f0 + 2",
                                         arg=f0, timeout=15000)
            res["video_again"] = True
            res["errors"] = [l for l in clog if "PAGEERROR" in l][:5]
            await browser.close()
        ok = (res.get("pair") and res.get("offered") and res.get("shown") == "taps: 0" and "ops test" in res.get("bar", "")
              and res["frame"][:2] == [440, 860] and res["frame"][2] == "allow-same-origin" and res["frame"][3] == 0
              and res.get("tap") == "taps: 2" and res.get("typed") == "echo:Hello world" and res.get("editable") == "sent: again乔布斯好 (enter)" and res.get("typed_zh") and res.get("composed", "").strip() == "again乔布斯好" and res.get("proxy_ok") and res.get("loaded_ok") and res.get("icon", [0, 0, 0])[1:] == [True, 19] and res.get("icon2", [0, 0])[1] and res.get("select") == "picked: blue"
              and res.get("photos_ok") and res.get("dialog") == "said yes" and res.get("video_while_playing") and res.get("page_only_ok") and res.get("drag_ok") and res["rows_before"] == 20 and res["rows_after"] >= 80
              and res.get("link") == "second page" and res.get("frame_url") == "about:srcdoc"
              and "mirror test" in res.get("address", "").lower() and res.get("picture") == 'url("blob:'
              and not res.get("requests_for_the_site") and res.get("video_again") and not res.get("errors"))
    except SiteDone:
        pass
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
    ap.add_argument("--webkit", action="store_true", help="the viewer in WebKit (Safari's engine) instead of Chrome")
    ap.add_argument("--url", help="a real site as the browser's home page: only check that it shows, pictures and all")
    ap.add_argument("--watch", type=float, default=30, help="--url: seconds to watch it")
    ap.add_argument("--shot", help="--url: a screenshot of the viewer here")
    sys.exit(asyncio.run(run(ap.parse_args())))
