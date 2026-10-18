"""rd: drive the phone from a shell (an AI agent's tool, or by hand).

    rd ui [--json] [--full] [--all-windows]      the UI tree (text outline by default)
    rd shot [-o FILE|-] [--format jpeg] [--max-edge 1280] [--quality 75]
    rd tap --rid search | --text OK | --desc Back | --label L | --node 17 | --xy 540 1200
    rd long-press …   rd text "hello" [--rid …]   rd set-text "你好" --rid query
    rd swipe X1 Y1 X2 Y2 [--ms 300]   rd scroll down|up|left|right [--rid list]
    rd key ENTER   rd back   rd home   rd launch com.android.settings
    rd wait --rid results [--gone] [--timeout 5]
    rd caps   rd devices

Connection: --url/$RD_URL (agent http://router:8790 or the panel https://…), --device/$RD_DEVICE
(default "local"), --token/$RD_TOKEN. --trace FILE (or $RD_TRACE) records every call for
turning an exploration into a skill (skills/README.md).
"""
import argparse
import json
import sys

from .client import ApiError, Client, NeedsAI, outline


def _selector_args(p):
    g = p.add_argument_group("target")
    g.add_argument("--rid", help="resource id (with or without the package:id/ prefix)")
    g.add_argument("--text", dest="sel_text", help="exact text")
    g.add_argument("--text-contains")
    g.add_argument("--desc", help="exact content description")
    g.add_argument("--desc-contains")
    g.add_argument("--label", help="text or description")
    g.add_argument("--cls", help="class (EditText or android.widget.EditText)")
    g.add_argument("--index", type=int, help="pick the n-th match when several match")
    g.add_argument("--node", type=int, help="node id from the last `rd ui` (needs --ui-id)")
    g.add_argument("--ui-id", type=int)
    g.add_argument("--xy", nargs=2, type=float, metavar=("X", "Y"), help="display pixels")
    g.add_argument("--image", nargs=2, type=int, metavar=("W", "H"),
                   help="--xy is in pixels of a W×H screenshot, not the display")


def _target(a):
    if a.xy:
        t = {"x": a.xy[0], "y": a.xy[1]}
        if a.image:
            t.update(space="image", w=a.image[0], h=a.image[1])
        return t, {}
    if a.node is not None:
        return {"node": a.node}, {"ui_id": a.ui_id}
    sel = {k: v for k, v in (("rid", a.rid), ("text", a.sel_text), ("text_contains", a.text_contains),
                             ("desc", a.desc), ("desc_contains", a.desc_contains), ("label", a.label),
                             ("cls", a.cls), ("index", a.index)) if v is not None}
    return (sel or None), {}


def _wait(a):
    if getattr(a, "wait_change", False):
        return {"for": "ui_changed", "timeout_ms": 3000}
    return None


def main(argv=None):
    p = argparse.ArgumentParser(prog="rd", description="RemoDroid AI control", formatter_class=argparse.RawDescriptionHelpFormatter,
                                epilog=__doc__.split("\n\n", 1)[1])
    p.add_argument("--url")
    p.add_argument("--device")
    p.add_argument("--token")
    p.add_argument("--trace")
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("devices")
    sub.add_parser("caps")
    s = sub.add_parser("ui")
    s.add_argument("--json", action="store_true", help="raw JSON instead of the outline")
    s.add_argument("--full", action="store_true", help="every node, not only labeled/actionable ones")
    s.add_argument("--all-windows", action="store_true", help="include system bars, keyboard, popups")
    s = sub.add_parser("shot")
    s.add_argument("-o", "--output", default="-", help="file, or - for stdout (default)")
    s.add_argument("--format", default="jpeg", choices=("jpeg", "png", "webp"))
    s.add_argument("--max-edge", type=int, default=1280)
    s.add_argument("--quality", type=int)
    for name in ("tap", "long-press"):
        s = sub.add_parser(name)
        _selector_args(s)
        s.add_argument("--wait-change", action="store_true", help="wait until the screen changes")
        if name == "long-press":
            s.add_argument("--ms", type=int, default=700)
    s = sub.add_parser("text")
    s.add_argument("value")
    _selector_args(s)
    s = sub.add_parser("set-text")
    s.add_argument("value")
    _selector_args(s)
    s = sub.add_parser("swipe")
    for c in ("x1", "y1", "x2", "y2"):
        s.add_argument(c, type=float)
    s.add_argument("--ms", type=int, default=300)
    s = sub.add_parser("scroll")
    s.add_argument("direction", choices=("down", "up", "left", "right"))
    _selector_args(s)
    s = sub.add_parser("key")
    s.add_argument("key", help="ENTER, BACK, DEL, TAB, 66, …")
    sub.add_parser("back")
    sub.add_parser("home")
    s = sub.add_parser("launch")
    s.add_argument("package")
    s = sub.add_parser("wait")
    _selector_args(s)
    s.add_argument("--gone", action="store_true")
    s.add_argument("--timeout", type=float, default=5)

    a = p.parse_args(argv)
    c = Client(a.url, a.device, a.token, trace=a.trace)
    try:
        return run(c, a)
    except ApiError as e:
        print(json.dumps({"error": e.code, "msg": e.msg, "status": e.status, **{k: v for k, v in e.body.items()
                                                                                if k not in ("error", "msg")}},
                         ensure_ascii=False), file=sys.stderr)
        return 1
    except NeedsAI as e:
        print(json.dumps({"error": "needs_ai", "msg": e.reason, **e.detail}, ensure_ascii=False), file=sys.stderr)
        return 3


def _print(obj):
    print(json.dumps(obj, ensure_ascii=False, indent=1))


def run(c, a):
    if a.cmd == "devices":
        _print(c.devices())
    elif a.cmd == "caps":
        _print(c.capabilities())
    elif a.cmd == "ui":
        u = c.ui(view="full" if a.full else "compact", windows="all" if a.all_windows else None)
        print(json.dumps(u, ensure_ascii=False) if a.json else outline(u))
    elif a.cmd == "shot":
        shot = c.screenshot(a.format, a.max_edge, a.quality)
        if a.output == "-":
            sys.stdout.buffer.write(shot.data)
        else:
            with open(a.output, "wb") as f:
                f.write(shot.data)
            print(json.dumps({"file": a.output, "bytes": len(shot.data), "w": shot.width, "h": shot.height,
                              "display": [shot.display_w, shot.display_h], "epoch": shot.epoch,
                              "backend": shot.backend}))
    elif a.cmd in ("tap", "long-press"):
        t, extra = _target(a)
        body = dict(target=t, wait=_wait(a), **extra)
        if a.cmd == "long-press":
            body["duration_ms"] = a.ms
        _print(c.act(a.cmd.replace("-", "_"), **body))
    elif a.cmd in ("text", "set-text"):
        t, extra = _target(a)
        _print(c.act(a.cmd.replace("-", "_"), text=a.value, target=t, **extra))
    elif a.cmd == "swipe":
        _print(c.swipe((a.x1, a.y1), (a.x2, a.y2), a.ms))
    elif a.cmd == "scroll":
        t, extra = _target(a)
        _print(c.act("scroll", direction=a.direction, target=t, **extra))
    elif a.cmd == "key":
        _print(c.key(a.key))
    elif a.cmd in ("back", "home"):
        _print(c.act(a.cmd))
    elif a.cmd == "launch":
        _print(c.launch(a.package))
    elif a.cmd == "wait":
        t, _ = _target(a)
        if not t or "x" in t or "node" in t:
            raise SystemExit("rd wait needs a selector (--rid, --text, --desc, --label, …)")
        _print(c.wait_for(timeout=a.timeout, gone=a.gone, **t))
    return 0


if __name__ == "__main__":
    sys.exit(main())
