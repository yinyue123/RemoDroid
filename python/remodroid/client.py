"""HTTP client for the RemoDroid AI control API (skill-plan.md §8).

Works against the router agent (``http://router:8790``, ``[api] token``) or the
central server (``https://panel.example``, a ``remodroid-server gentoken`` token):
both serve ``/api/v1/devices/{id}/...``. Standard library only.

Screenshots stay in memory: ``Client.screenshot()`` returns the bytes.
"""
import json
import os
import time
import urllib.error
import urllib.parse
import urllib.request

SELECTOR_KEYS = ("rid", "text", "text_contains", "desc", "desc_contains", "label", "cls", "pkg",
                 "clickable", "enabled", "checked", "editable", "scrollable", "within", "index")


class ApiError(Exception):
    """A non-2xx answer: ``status``, ``code`` (e.g. ``no_match``, ``viewer_active``), ``retryable``."""

    def __init__(self, status, code, msg, retryable=False, body=None):
        super().__init__(f"{status} {code}: {msg}")
        self.status, self.code, self.msg, self.retryable, self.body = status, code, msg, retryable, body or {}


class NeedsAI(Exception):
    """The deterministic path cannot continue: hand the situation to the AI.

    ``reason`` says why, ``step`` names the step that failed, and the skill runner adds
    the completed-step journal and a fresh UI dump before printing it (exit code 3)."""

    def __init__(self, reason, step=None, detail=None):
        super().__init__(reason)
        self.reason, self.step, self.detail = reason, step, detail or {}


class EntryMismatch(NeedsAI):
    """The phone is not in the state the skill starts from (exit code 2: not this skill)."""


class Screenshot:
    """Image bytes plus the geometry from the X-RD-* headers."""

    def __init__(self, data, mime, headers):
        self.data, self.mime = data, mime
        h = {k.lower(): v for k, v in headers.items()}
        num = lambda k: int(h[k]) if h.get(k, "").lstrip("-").isdigit() else None
        self.width, self.height = num("x-rd-width"), num("x-rd-height")
        self.display_w, self.display_h = num("x-rd-display-w"), num("x-rd-display-h")
        self.rotation, self.epoch = num("x-rd-rotation"), num("x-rd-epoch")
        self.backend = h.get("x-rd-backend")
        self.headers = h

    def point(self, x, y):
        """A tap target in image pixels; the phone scales it to the display."""
        return {"x": x, "y": y, "space": "image", "w": self.width, "h": self.height}


def selector_of(kw):
    return {k: kw.pop(k) for k in list(kw) if k in SELECTOR_KEYS and kw[k] is not None}


def _short(cls):
    return cls.rsplit(".", 1)[-1] if cls and (cls.startswith("android.widget.") or cls.startswith("android.view.")) else cls


def matches(node, sel, nodes_by_id):
    """Client-side version of the phone's selector rules, over UI JSON nodes."""
    rid = node.get("rid")
    if "rid" in sel and not (rid and (rid == sel["rid"] or rid.endswith(":id/" + sel["rid"]))):
        return False
    text, desc = node.get("text"), node.get("desc")
    if "text" in sel and text != sel["text"]:
        return False
    if "text_contains" in sel and (not text or sel["text_contains"] not in text):
        return False
    if "desc" in sel and desc != sel["desc"]:
        return False
    if "desc_contains" in sel and (not desc or sel["desc_contains"] not in desc):
        return False
    if "label" in sel and sel["label"] not in (text, desc):
        return False
    if "cls" in sel and sel["cls"] not in (node.get("cls"), _short(node.get("cls"))):
        return False
    flags = set(node.get("f", []))
    for key, flag in (("clickable", "click"), ("enabled", "enabled"), ("checked", "checked"),
                      ("editable", "edit"), ("scrollable", "scroll")):
        if key in sel and bool(sel[key]) != (flag in flags):
            return False
    if "within" in sel:
        p = node.get("p")
        while p is not None:
            parent = nodes_by_id.get(p)
            if parent is None:
                break
            if matches(parent, sel["within"], nodes_by_id):
                return True
            p = parent.get("p")
        return False
    return True


def find(ui, **sel):
    """Nodes of a UI dump that match the selector (rid=, text=, desc=, label=, cls=, within=…)."""
    nodes = ui.get("nodes", [])
    by_id = {n["id"]: n for n in nodes}
    found = [n for n in nodes if matches(n, sel, by_id)]
    if "index" in sel:
        i = sel["index"]
        return found[i:i + 1]
    return found


def require(ui, package=None, has=None, lacks=None, step=None, entry=False):
    """Checks a UI dump; raises NeedsAI (or EntryMismatch with entry=True) when it does not fit."""
    exc = EntryMismatch if entry else NeedsAI
    if package and ui.get("package") != package:
        raise exc(f"expected package {package}, found {ui.get('package')}", step)
    for sel in [has] if isinstance(has, dict) else (has or []):
        if not find(ui, **sel):
            raise exc(f"expected an element matching {sel}", step)
    for sel in [lacks] if isinstance(lacks, dict) else (lacks or []):
        if find(ui, **sel):
            raise exc(f"did not expect an element matching {sel}", step)
    return ui


class Client:
    """``Client(url, device="local", token=...)``; defaults come from RD_URL, RD_DEVICE, RD_TOKEN.

    ``trace``: a JSONL file that records every call (exploration runs; see skills/README.md).
    ``from_step``: resume a skill; calls before that step are skipped (already done)."""

    def __init__(self, url=None, device=None, token=None, trace=None, timeout=45, from_step=None):
        self.url = (url or os.environ.get("RD_URL") or "http://127.0.0.1:8790").rstrip("/")
        self.device = device or os.environ.get("RD_DEVICE") or "local"
        self.token = token or os.environ.get("RD_TOKEN")
        self.timeout = timeout
        trace = trace or os.environ.get("RD_TRACE")
        self._trace = open(trace, "a", encoding="utf-8") if trace else None
        self.journal = []            # completed steps of this run
        self.skipping = from_step    # resume: skip until this step name
        self.last_ui = None

    # ---------------- transport ----------------

    def _request(self, method, suffix, params=None, body=None):
        path = f"/api/v1/devices/{urllib.parse.quote(self.device)}{suffix}" if suffix is not None else "/api/v1/devices"
        url = self.url + path
        if params:
            url += "?" + urllib.parse.urlencode({k: v for k, v in params.items() if v is not None})
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(url, data=data, method=method)
        if data is not None:
            req.add_header("Content-Type", "application/json")
        if self.token:
            req.add_header("Authorization", "Bearer " + self.token)
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as r:
                return r.status, dict(r.headers), r.read()
        except urllib.error.HTTPError as e:
            raw = e.read()
            try:
                j = json.loads(raw)
            except ValueError:
                j = {"error": "http_error", "msg": raw[:200].decode("utf-8", "replace")}
            raise ApiError(e.code, j.get("error", "http_error"), j.get("msg", ""), j.get("retryable", False), j) from None

    def _json(self, method, suffix, params=None, body=None):
        return json.loads(self._request(method, suffix, params, body)[2])

    def _record(self, call, args, result):
        if self._trace:
            self._trace.write(json.dumps({"ts": round(time.time(), 3), "call": call, "args": args, "result": result},
                                         ensure_ascii=False) + "\n")
            self._trace.flush()

    # ---------------- reading ----------------

    def devices(self):
        return self._json("GET", None)["devices"]

    def capabilities(self):
        return self._json("GET", "/capabilities")

    def ui(self, view="compact", windows=None, timeout_ms=None):
        """The current UI as JSON (never a screenshot)."""
        u = self._json("GET", "/ui", {"view": view, "windows": windows, "timeout_ms": timeout_ms})
        self.last_ui = u
        self._record("ui", {"view": view}, summarize(u))
        return u

    def screenshot(self, format="jpeg", max_edge=1280, quality=None, timeout_ms=None):
        """The current screen as image bytes, in memory (never a file, never the UI tree)."""
        _, headers, data = self._request("GET", "/screenshot", {"format": format, "max_edge": max_edge,
                                                                 "quality": quality, "timeout_ms": timeout_ms})
        shot = Screenshot(data, headers.get("Content-Type", "image/" + format), headers)
        self._record("screenshot", {"format": format, "max_edge": max_edge},
                     {"bytes": len(data), "w": shot.width, "h": shot.height})
        return shot

    # ---------------- acting ----------------

    def act(self, action, step=None, side_effect=False, **kw):
        """POST /actions. ``step`` names the step for the journal and for resuming."""
        if self.skipping:
            if step != self.skipping:
                self.journal.append({"step": step, "action": action, "skipped": True})
                return {"ok": True, "skipped": True}
            self.skipping = None
        body = dict(kw, action=action)
        try:
            res = self._json("POST", "/actions", body=body)
        except ApiError as e:
            self._record("act", body, {"error": e.code, "msg": e.msg})
            if e.code in ("no_match", "ambiguous", "stale_state", "disabled", "viewer_active"):
                raise NeedsAI(f"{action} failed: {e.code}: {e.msg}", step, e.body) from None
            raise
        self._record("act", body, res)
        self.journal.append({"step": step, "action": action, "side_effect": side_effect, "ok": res.get("ok"),
                             "verified": res.get("verified")})
        if not res.get("ok"):
            raise NeedsAI(f"{action} was not injected", step, res)
        return res

    def _target(self, kw, x=None, y=None, node=None, ui=None, image=None):
        if x is not None and y is not None:
            t = image.point(x, y) if image is not None else {"x": x, "y": y}
            if image is not None and image.epoch is not None:
                kw["epoch"] = image.epoch
            return t
        if node is not None:
            u = ui or self.last_ui
            kw["ui_id"] = u["ui_id"] if u else None
            return {"node": node}
        sel = selector_of(kw)
        return sel or None

    def tap(self, x=None, y=None, node=None, ui=None, image=None, step=None, side_effect=False, wait=None, **kw):
        t = self._target(kw, x, y, node, ui, image)
        return self.act("tap", step, side_effect, target=t, wait=wait, **kw)

    def long_press(self, x=None, y=None, node=None, ui=None, image=None, duration_ms=700, step=None,
                   side_effect=False, wait=None, **kw):
        t = self._target(kw, x, y, node, ui, image)
        return self.act("long_press", step, side_effect, target=t, duration_ms=duration_ms, wait=wait, **kw)

    def swipe(self, start, end, duration_ms=300, step=None, wait=None):
        return self.act("swipe", step, False, **{"from": {"x": start[0], "y": start[1]},
                                                  "to": {"x": end[0], "y": end[1]},
                                                  "duration_ms": duration_ms, "wait": wait})

    def scroll(self, direction="down", step=None, wait=None, **kw):
        t = self._target(kw)
        return self.act("scroll", step, False, direction=direction, target=t, wait=wait, **kw)

    def key(self, key, step=None, side_effect=False, wait=None):
        return self.act("key", step, side_effect, key=key, wait=wait)

    def text(self, text, step=None, side_effect=False, wait=None, **kw):
        """Types into the focused field (or taps the selected one first)."""
        t = self._target(kw)
        return self.act("text", step, side_effect, text=text, target=t, wait=wait, **kw)

    def set_text(self, text, step=None, side_effect=False, wait=None, **kw):
        """Replaces a field's text (accessibility ACTION_SET_TEXT; any Unicode)."""
        t = self._target(kw)
        return self.act("set_text", step, side_effect, text=text, target=t, wait=wait, **kw)

    def launch(self, package, step=None, wait=None):
        """Opens an app at its launcher activity."""
        return self.act("launch", step, False, package=package, wait=wait)

    def back(self, step=None, wait=None):
        return self.act("back", step, False, wait=wait)

    def home(self, step=None, wait=None):
        return self.act("home", step, False, wait=wait)

    def wait_for(self, timeout=5, gone=False, step=None, **sel):
        """Waits until an element matching the selector appears (or is gone); NeedsAI on timeout."""
        if self.skipping:
            return None
        res = self.act("wait", step, False,
                       wait={"for": "gone" if gone else "selector", "selector": sel, "timeout_ms": int(timeout * 1000)})
        if not res.get("verified"):
            raise NeedsAI(f"timed out waiting for {'no ' if gone else ''}element {sel}", step)
        if self.journal and self.journal[-1]["action"] == "wait":
            self.journal.pop()  # waits are checks, not steps
        return res.get("match")

    def require(self, ui=None, step=None, **checks):
        """``require(package=…, has={…}, lacks={…})`` on a fresh (or given) UI dump."""
        ui = ui or self.ui()
        return ui if self.skipping else require(ui, step=step, **checks)


def summarize(ui, limit=60):
    """What a trace keeps of a UI dump: the package and the labeled/actionable nodes."""
    keep = []
    for n in ui.get("nodes", []):
        if len(keep) >= limit:
            break
        entry = {k: n[k] for k in ("id", "rid", "text", "desc", "cls") if n.get(k)}
        if n.get("f"):
            entry["f"] = [f for f in n["f"] if f in ("click", "edit", "scroll", "checked", "long")]
        keep.append(entry)
    return {"ui_id": ui.get("ui_id"), "package": ui.get("package"), "count": ui.get("count"), "nodes": keep}


def outline(ui):
    """A compact text view of a UI dump, one node per line, indented by parent."""
    nodes = ui.get("nodes", [])
    depth = {}
    lines = [f"# {ui.get('package')}  ui_id={ui.get('ui_id')}  display={ui.get('display', {}).get('w')}x"
             f"{ui.get('display', {}).get('h')} rot={ui.get('display', {}).get('rotation')} "
             f"epoch={ui.get('display', {}).get('epoch')}  ({ui.get('backend')}, {ui.get('collect_ms')} ms)"]
    for n in nodes:
        d = depth.get(n.get("p"), -1) + 1
        depth[n["id"]] = d
        parts = [f"[{n['id']}]", n.get("cls") or "?"]
        if n.get("rid"):
            parts.append("#" + n["rid"].split(":id/")[-1])
        if n.get("text"):
            parts.append(json.dumps(n["text"], ensure_ascii=False))
        if n.get("desc"):
            parts.append("desc=" + json.dumps(n["desc"], ensure_ascii=False))
        flags = [f for f in n.get("f", []) if f not in ("enabled", "focusable")]
        if "enabled" not in n.get("f", []):
            flags.append("disabled")
        if flags:
            parts.append("{" + ",".join(flags) + "}")
        parts.append("@" + ",".join(map(str, n.get("b", []))))
        lines.append("  " * min(d, 12) + " ".join(parts))
    if ui.get("truncated"):
        lines.append("# (truncated)")
    return "\n".join(lines)
