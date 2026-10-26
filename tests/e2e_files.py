#!/usr/bin/env python3
"""Files and clipboard, like AirDrop (e2e, Linux): the real web client in Chrome and the real GTK app
(remodroid-desktop) as the agent, on Xvfb, through remodroid-server.

  - a file dropped on the viewer lands in the computer's ~/Downloads/RemoDroid
  - a file dragged onto the RemoDroid window (a real XDND drop from another GTK window, the way a file
    manager drags) is downloaded by the browser; the file is outside the home folder on purpose
  - the clipboard syncs both ways by itself: the computer copies → the browser's clipboard; the browser
    copies and the viewer gets the focus → the computer's clipboard

  python tests/e2e_files.py --bin build/host [--chrome PATH] [--verbose]

Needs Xvfb, xdotool, dbus-run-session and python3-gi (the drag source) besides Playwright.
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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from e2e_desktop import ROOT, Probe, free_display, free_port  # noqa: E402

# a small GTK window at the top left that drags one file (text/uri-list), as a file manager does
DRAG_SOURCE = r"""
import sys, gi
gi.require_version('Gtk', '3.0')
from gi.repository import Gtk, Gdk
uri = sys.argv[1]
w = Gtk.Window(title='drag-source')
w.set_default_size(160, 120)
w.move(0, 0)
w.add(Gtk.Label(label='drag me'))
w.drag_source_set(Gdk.ModifierType.BUTTON1_MASK, [], Gdk.DragAction.COPY)
w.drag_source_add_uri_targets()
w.connect('drag-data-get', lambda wid, ctx, data, info, t: data.set_uris([uri]))
w.connect('destroy', Gtk.main_quit)
w.show_all()
print('ready', flush=True)
Gtk.main()
"""


def xdo(env, *a):
    return subprocess.run(["xdotool", *a], env=env, capture_output=True, text=True).stdout.strip()


async def window_of(env, name, timeout=10):
    end = time.time() + timeout
    while time.time() < end:
        ids = xdo(env, "search", "--onlyvisible", "--name", name).split()
        if ids:
            g = dict(l.split("=") for l in xdo(env, "getwindowgeometry", "--shell", ids[-1]).splitlines() if "=" in l)
            return {k: int(v) for k, v in g.items()}
        await asyncio.sleep(0.2)
    return None


async def run(args):
    b = os.path.abspath(args.bin)
    d = tempfile.mkdtemp(prefix="remodroid-files-")
    home = f"{d}/home"
    os.makedirs(home)
    http_port, disp = free_port(), free_display()
    probe_bin = f"{d}/xprobe"
    subprocess.check_call(["cc", "-O1", "-o", probe_bin, f"{ROOT}/desktop/test/xprobe.c", "-lX11"])
    hp = lambda pw: subprocess.check_output([f"{b}/remodroid-server", "hashpw", pw]).decode().strip()
    open(f"{d}/server.ini", "w").write(f"[http]\nlisten = 127.0.0.1:{http_port}\nweb_root = {ROOT}/web\n[users]\nme = {hp('pw')}\n")
    conf = f"{d}/desktop.ini"
    open(conf, "w").write("[desktop]\nnotify = false\n")
    quiet = None if args.verbose else subprocess.DEVNULL
    res, ok = {}, False
    env = dict(os.environ, DISPLAY=f":{disp}", HOME=home, XDG_CONFIG_HOME=f"{home}/.config", NO_AT_BRIDGE="1")
    for k in ("WAYLAND_DISPLAY", "XDG_SESSION_TYPE", "DBUS_SESSION_BUS_ADDRESS"):
        env.pop(k, None)
    procs = [subprocess.Popen(["Xvfb", f":{disp}", "-screen", "0", "1280x720x24", "-nolisten", "tcp"], stderr=quiet)]
    for _ in range(50):
        if os.path.exists(f"/tmp/.X11-unix/X{disp}"):
            break
        time.sleep(0.1)
    probe = Probe(probe_bin, env) # the clipboard on the computer (and a full-screen window underneath)
    procs.append(probe.p)
    procs.append(subprocess.Popen([f"{b}/remodroid-server", "-c", f"{d}/server.ini"], stderr=quiet))
    time.sleep(0.3)
    base = f"http://127.0.0.1:{http_port}"
    req = urllib.request.Request(base + "/login", data=json.dumps({"user": "me", "password": "pw"}).encode(),
                                 headers={"Content-Type": "application/json"}, method="POST")
    cookie = urllib.request.urlopen(req).headers["Set-Cookie"].split(";")[0]
    req = urllib.request.Request(base + "/api/v1/enroll/codes", data=b'{"name":"Test computer"}',
                                 headers={"Cookie": cookie}, method="POST")
    code = json.loads(urllib.request.urlopen(req).read())["code"]
    pair = subprocess.run([f"{b}/desktop/remodroid-desktopd", "-c", conf, "pair", "--panel", base, "--code", code],
                          env=env, capture_output=True, text=True, timeout=30)
    res["pair"] = pair.returncode == 0
    # the GTK app is the agent: its window is the drop target
    app_log = open(f"{d}/app.log", "w")
    procs.append(subprocess.Popen(["dbus-run-session", "--", f"{b}/desktop/remodroid-desktop", "--conf", conf], env=env,
                                  stdout=app_log, stderr=app_log))
    try:
        if not await probe.wait(lambda e: e["ev"] == "ready", 10):
            raise RuntimeError("xprobe did not start")
        async with async_playwright() as p:
            launch = {"args": ["--autoplay-policy=no-user-gesture-required"]}
            if args.chrome:
                launch["executable_path"] = args.chrome
            browser = await p.chromium.launch(**launch)
            ctx = await browser.new_context(viewport={"width": 1280, "height": 720}, accept_downloads=True)
            await ctx.grant_permissions(["clipboard-read", "clipboard-write"], origin=base)
            page = await ctx.new_page()
            logs = []
            page.on("console", lambda m: logs.append(m.text))
            page.on("pageerror", lambda e: logs.append("PAGEERROR " + str(e)))
            await page.goto(base + "/")
            await page.fill("#user", "me")
            await page.fill("#password", "pw")
            await page.click("#go")
            await page.wait_for_url(base + "/")
            await page.wait_for_function("() => { const v = document.querySelector('#video'); return v && v.videoWidth > 0; }",
                                         timeout=30000)
            await page.wait_for_function("() => document.querySelector('#disc').hidden", timeout=15000)
            await page.bring_to_front()

            # 1. a file dropped on the viewer → the computer's Downloads/RemoDroid
            res["dropzone"] = await page.evaluate("""() => {
                const dt = new DataTransfer();
                dt.items.add(new File(['hello from the browser\\n'], 'dropped.txt', { type: 'text/plain' }));
                window.dispatchEvent(new DragEvent('dragenter', { dataTransfer: dt, bubbles: true, cancelable: true }));
                const shown = !document.querySelector('#dropzone').hidden && document.querySelector('#dropzoneText').textContent;
                window.dispatchEvent(new DragEvent('drop', { dataTransfer: dt, bubbles: true, cancelable: true }));
                return shown; }""")
            dest = f"{home}/Downloads/RemoDroid/dropped.txt"
            for _ in range(100):
                if os.path.exists(dest) and open(dest).read() == "hello from the browser\n":
                    break
                await page.wait_for_timeout(100)
            res["viewer_drop_ok"] = os.path.exists(dest) and open(dest).read() == "hello from the browser\n"

            # 2. a file dragged onto the RemoDroid window → downloaded by the browser
            src = f"{d}/outside home.txt"
            open(src, "w").write("hello from linux\n")
            target = await window_of(env, "^RemoDroid$")
            res["window"] = bool(target)
            dragger = subprocess.Popen(["/usr/bin/python3", "-c", DRAG_SOURCE, "file://" + urllib.request.pathname2url(src)],
                                       env=env, stdout=subprocess.PIPE, stderr=quiet, text=True)
            procs.append(dragger)
            dragger.stdout.readline()
            source = await window_of(env, "^drag-source$")
            if target and source:
                tx, ty = target["X"] + target["WIDTH"] // 2, target["Y"] + target["HEIGHT"] // 2
                try:
                    async with page.expect_download(timeout=20000) as dl:
                        xdo(env, "mousemove", str(source["X"] + 60), str(source["Y"] + 50))
                        xdo(env, "mousedown", "1")
                        for i in range(1, 21): # XDND needs real motion to the target
                            x = source["X"] + 60 + (tx - source["X"] - 60) * i // 20
                            y = source["Y"] + 50 + (ty - source["Y"] - 50) * i // 20
                            xdo(env, "mousemove", str(x), str(y))
                            await asyncio.sleep(0.03)
                        await asyncio.sleep(0.3)
                        xdo(env, "mouseup", "1")
                    got = await dl.value
                    path = await got.path()
                    res["download"] = {"name": got.suggested_filename, "ok": open(path).read() == "hello from linux\n"}
                except Exception as e:
                    res["download"] = {"error": str(e).splitlines()[0]}
            res["window_drop_ok"] = res.get("download", {}).get("ok") and res["download"]["name"] == "outside home.txt"

            # 3. the computer copies → this clipboard, without a tap (or the Copy bar where writing isn't allowed)
            probe.send("setclip copied on linux")
            await probe.wait(lambda e: e["ev"] == "setclip")
            res["clip_to_viewer"] = None
            for _ in range(60):
                if await page.evaluate("navigator.clipboard.readText().catch(() => '')") == "copied on linux":
                    res["clip_to_viewer"] = "auto"
                    break
                if await page.evaluate("(document.querySelector('#clipbarText') || {}).textContent === 'copied on linux'"
                                       " && !document.querySelector('#clipbar').hidden"):
                    res["clip_to_viewer"] = "bar"
                    break
                await page.wait_for_timeout(100)

            # 4. copied in the browser, then the viewer gets the focus → the computer's clipboard
            await page.evaluate("navigator.clipboard.writeText('copied in the browser')")
            await page.evaluate("window.dispatchEvent(new Event('focus'))")
            res["clip_to_computer"] = None
            for _ in range(30):
                probe.send("getclip")
                ev = await probe.wait(lambda e: e["ev"] == "clip", 2)
                if ev and ev["text"] == "copied in the browser":
                    res["clip_to_computer"] = ev["text"]
                    break
                await page.wait_for_timeout(200)
            res["errors"] = [l for l in logs if "PAGEERROR" in l][:5]
            await browser.close()
        ok = (res.get("pair") and res.get("dropzone") == "Drop to send to Test computer" and res.get("viewer_drop_ok")
              and res.get("window_drop_ok") and res.get("clip_to_viewer") == "auto"
              and res.get("clip_to_computer") == "copied in the browser" and not res.get("errors"))
    finally:
        for pr in procs[::-1]:
            if pr.poll() is None:
                pr.terminate()
        for pr in procs:
            try:
                pr.wait(5)
            except Exception:
                pr.kill()
        app_log.close()
        if not ok or args.verbose:
            print("---- app log (tail) ----")
            print("".join(open(f"{d}/app.log").readlines()[-30:]))
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
