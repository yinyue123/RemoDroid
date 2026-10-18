package io.remodroid;

import android.graphics.Rect;
import android.os.Bundle;
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.accessibility.AccessibilityNodeInfo;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.List;

/** POST /actions (skill-plan.md §8): one synchronous action, optionally followed by a wait.
 *  {"action": "tap", "target": {selector} | {"node": 17} | {"x":…, "y":…, "space": "display"|"image",
 *   "w":…, "h":…}, "ui_id": 42, "epoch": 7, "wait": {"for": "ui_changed"|"idle"|"selector"|"gone", …}} */
final class Actions {
    static final long VIEWER_GRACE_MS = 5000;

    private final Device device;
    private final UiTree tree;
    private final ApiServer api;

    Actions(Device device, UiTree tree, ApiServer api) {
        this.device = device;
        this.tree = tree;
        this.api = api;
    }

    JSONObject run(JSONObject req) throws Exception {
        String type = req.optString("action", req.optString("type"));
        if (type.isEmpty()) throw new ApiServer.Fault(400, "bad_request", "\"action\" is required", false);
        long sinceViewer = SystemClock.uptimeMillis() - Controller.lastInputAt;
        if (!"wait".equals(type) && Controller.lastInputAt > 0 && sinceViewer < VIEWER_GRACE_MS)
            throw new ApiServer.Fault(409, "viewer_active", "a person is using the phone in the viewer", true);
        JSONObject wait = req.optJSONObject("wait");
        synchronized (tree) {
            Device.Info di = device.displayInfo();
            int epoch = api.epoch(di);
            if (req.has("epoch") && req.optInt("epoch") != epoch)
                throw new ApiServer.Fault(409, "stale_state", "the display changed (rotation or size); look again", true);
            long before = wait != null && "ui_changed".equals(wait.optString("for")) ? UiTree.signature(collect(di, epoch)) : 0;
            JSONObject res = new JSONObject().put("ok", true).put("action", type);
            boolean injected = perform(type, req, di, epoch, res);
            res.put("injected", injected);
            if (!injected) res.put("ok", false);
            if (wait != null && injected) waitFor(wait, before, res);
            return res;
        }
    }

    private UiTree.Snapshot collect(Device.Info di, int epoch) throws ApiServer.Fault {
        return tree.collect(di, epoch, false, 3000);
    }

    // ---------------- actions ----------------

    private boolean perform(String type, JSONObject req, Device.Info di, int epoch, JSONObject res) throws Exception {
        JSONObject target = req.optJSONObject("target");
        switch (type) {
            case "tap":
            case "long_press": {
                float[] p = point(target, req, di, epoch, res);
                long hold = "tap".equals(type) ? 40 : req.optLong("duration_ms", 700);
                return gesture(p[0], p[1], p[0], p[1], hold, 1);
            }
            case "swipe": {
                float[] a, b;
                if (target != null && req.has("direction")) { /* swipe across an element */
                    UiTree.Node n = resolveNode(target, req, di, epoch);
                    a = new float[2];
                    b = new float[2];
                    swipeWithin(n.b, req.optString("direction"), req.optDouble("distance", 0.6), a, b);
                    res.put("node", n.id);
                } else {
                    a = point(req.optJSONObject("from"), req, di, epoch, res);
                    b = point(req.optJSONObject("to"), req, di, epoch, res);
                }
                long ms = Math.max(50, req.optLong("duration_ms", 300));
                return gesture(a[0], a[1], b[0], b[1], ms, (int) Math.max(2, ms / 16));
            }
            case "scroll":
                return scroll(target, req, di, epoch, res);
            case "key": {
                int code = keyCode(req.opt("key"));
                return device.press(code);
            }
            case "back": return device.press(KeyEvent.KEYCODE_BACK);
            case "home": return device.press(KeyEvent.KEYCODE_HOME);
            case "recents": return device.press(KeyEvent.KEYCODE_APP_SWITCH);
            case "text": {
                if (target != null) { /* focus the field first */
                    float[] p = point(target, req, di, epoch, res);
                    gesture(p[0], p[1], p[0], p[1], 40, 1);
                    SystemClock.sleep(150);
                }
                return device.text(req.optString("text"));
            }
            case "set_text":
                return setText(target, req, di, epoch, res);
            case "launch": { /* open an app at its launcher activity */
                String pkg = req.optString("package");
                if (!pkg.matches("[A-Za-z][A-Za-z0-9_]*(\\.[A-Za-z0-9_]+)+"))
                    throw new ApiServer.Fault(400, "bad_request", "\"package\" must be a package name", false);
                Shell.Result r = Shell.run("monkey -p " + pkg + " -c android.intent.category.LAUNCHER 1", 10_000);
                if (r.code != 0 || r.out.contains("No activities found"))
                    throw new ApiServer.Fault(404, "no_match", "cannot launch " + pkg + ": " + (r.out + r.err).trim(), false);
                return true;
            }
            case "wait":
                return true;
            default:
                throw new ApiServer.Fault(400, "unsupported", "unknown action " + type, false);
        }
    }

    private boolean setText(JSONObject target, JSONObject req, Device.Info di, int epoch, JSONObject res) throws Exception {
        String text = req.optString("text");
        UiTree.Node n = resolveNode(target, req, di, epoch);
        res.put("node", n.id);
        if (tree.live(n)) {
            Bundle args = new Bundle();
            args.putCharSequence(AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE, text);
            if (n.info.performAction(AccessibilityNodeInfo.ACTION_SET_TEXT, args)) {
                n.info.refresh();
                CharSequence now = n.info.getText();
                res.put("verified", n.info.isPassword() || (now != null && text.contentEquals(now)));
                return true;
            }
        }
        /* no accessibility node: tap, select all, type */
        gesture(n.b.exactCenterX(), n.b.exactCenterY(), n.b.exactCenterX(), n.b.exactCenterY(), 40, 1);
        SystemClock.sleep(150);
        device.key(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_A, KeyEvent.META_CTRL_ON | KeyEvent.META_CTRL_LEFT_ON);
        device.key(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_A, KeyEvent.META_CTRL_ON | KeyEvent.META_CTRL_LEFT_ON);
        device.press(KeyEvent.KEYCODE_DEL);
        res.put("method", "keys");
        return text.isEmpty() || device.text(text);
    }

    private boolean scroll(JSONObject target, JSONObject req, Device.Info di, int epoch, JSONObject res) throws Exception {
        String dir = req.optString("direction", "down");
        boolean forward = "down".equals(dir) || "right".equals(dir) || "forward".equals(dir);
        Rect area = new Rect(0, 0, di.width, di.height);
        if (target != null) {
            UiTree.Node n = resolveNode(target, req, di, epoch);
            res.put("node", n.id);
            if (tree.live(n) && n.has(UiTree.F_SCROLL) && n.info.performAction(forward
                    ? AccessibilityNodeInfo.ACTION_SCROLL_FORWARD : AccessibilityNodeInfo.ACTION_SCROLL_BACKWARD)) {
                res.put("method", "accessibility");
                return true;
            }
            area = n.b;
        }
        /* a finger swipe: content moves opposite to the finger */
        String finger = "down".equals(dir) || "forward".equals(dir) ? "up" : "up".equals(dir) || "backward".equals(dir)
                ? "down" : "right".equals(dir) ? "left" : "right";
        float[] a = new float[2], b = new float[2];
        swipeWithin(area, finger, req.optDouble("distance", 0.6), a, b);
        res.put("method", "swipe");
        return gesture(a[0], a[1], b[0], b[1], 350, 22);
    }

    private static void swipeWithin(Rect r, String dir, double distance, float[] a, float[] b) {
        float cx = r.exactCenterX(), cy = r.exactCenterY();
        float dx = (float) (r.width() * distance / 2), dy = (float) (r.height() * distance / 2);
        a[0] = b[0] = cx;
        a[1] = b[1] = cy;
        switch (dir) {
            case "up": a[1] = cy + dy; b[1] = cy - dy; break;
            case "down": a[1] = cy - dy; b[1] = cy + dy; break;
            case "left": a[0] = cx + dx; b[0] = cx - dx; break;
            default: a[0] = cx - dx; b[0] = cx + dx; break;
        }
    }

    // ---------------- targets ----------------

    /** A point in display pixels from {"x","y"[,"space":"image","w","h"]}, {"node"} or a selector. */
    private float[] point(JSONObject t, JSONObject req, Device.Info di, int epoch, JSONObject res) throws Exception {
        if (t == null) throw new ApiServer.Fault(400, "bad_request", "\"target\" is required", false);
        if (t.has("x") && t.has("y")) {
            double x = t.getDouble("x"), y = t.getDouble("y");
            if ("image".equals(t.optString("space", "display"))) {
                int w = t.optInt("w"), h = t.optInt("h");
                if (w <= 0 || h <= 0) throw new ApiServer.Fault(400, "bad_request", "image coordinates need w and h", false);
                x = x * di.width / w;
                y = y * di.height / h;
            }
            if (x < 0 || y < 0 || x >= di.width || y >= di.height)
                throw new ApiServer.Fault(400, "bad_request", "the point is outside the display", false);
            return new float[] { (float) x, (float) y };
        }
        UiTree.Node n = resolveNode(t, req, di, epoch);
        res.put("node", n.id);
        Rect b = new Rect(n.b);
        if (!b.intersect(0, 0, di.width, di.height) || b.isEmpty())
            throw new ApiServer.Fault(409, "no_match", "the element is off screen", true);
        return new float[] { b.exactCenterX(), b.exactCenterY() };
    }

    private UiTree.Node resolveNode(JSONObject t, JSONObject req, Device.Info di, int epoch) throws Exception {
        if (t == null) throw new ApiServer.Fault(400, "bad_request", "\"target\" is required", false);
        if (t.has("node")) {
            UiTree.Snapshot s = tree.last;
            int uiId = req.optInt("ui_id", t.optInt("ui_id", -1));
            if (s == null || s.uiId != uiId)
                throw new ApiServer.Fault(409, "stale_state", "node ids belong to one ui_id; fetch /ui again", true);
            int id = t.getInt("node");
            if (id < 0 || id >= s.nodes.size()) throw new ApiServer.Fault(404, "no_match", "no node " + id, false);
            return s.nodes.get(id);
        }
        UiTree.Selector sel = UiTree.Selector.parse(t);
        UiTree.Snapshot s = collect(di, epoch);
        List<UiTree.Node> found = sel.find(s);
        if (sel.index >= 0) {
            if (sel.index >= found.size())
                throw new ApiServer.Fault(404, "no_match", "only " + found.size() + " elements match", true);
            return found.get(sel.index);
        }
        if (found.isEmpty()) throw new ApiServer.Fault(404, "no_match", "no element matches the selector", true);
        if (found.size() > 1) {
            JSONArray c = new JSONArray();
            for (int i = 0; i < Math.min(5, found.size()); i++) c.put(UiTree.nodeJson(found.get(i), -1, s.pkg));
            throw new ApiServer.Fault(409, "ambiguous", found.size() + " elements match; add fields or \"index\"", false)
                    .with("candidates", c);
        }
        UiTree.Node n = found.get(0);
        if (!n.has(UiTree.F_ENABLED)) throw new ApiServer.Fault(409, "disabled", "the element is disabled", true);
        return n;
    }

    // ---------------- input ----------------

    private boolean gesture(float x1, float y1, float x2, float y2, long ms, int moves) {
        long down = SystemClock.uptimeMillis();
        boolean ok = touch(MotionEvent.ACTION_DOWN, down, x1, y1);
        for (int i = 1; ok && i <= moves && (x1 != x2 || y1 != y2); i++) {
            SystemClock.sleep(Math.max(1, ms / moves));
            float f = (float) i / moves;
            ok = touch(MotionEvent.ACTION_MOVE, down, x1 + (x2 - x1) * f, y1 + (y2 - y1) * f);
        }
        if (x1 == x2 && y1 == y2) SystemClock.sleep(ms);
        return touch(MotionEvent.ACTION_UP, down, x2, y2) && ok;
    }

    private boolean touch(int action, long down, float x, float y) {
        MotionEvent e = MotionEvent.obtain(down, SystemClock.uptimeMillis(), action, x, y, 0);
        e.setSource(InputDevice.SOURCE_TOUCHSCREEN);
        boolean ok = device.inject(e);
        e.recycle();
        return ok;
    }

    static int keyCode(Object k) throws ApiServer.Fault {
        if (k instanceof Number) return ((Number) k).intValue();
        String s = k == null ? "" : k.toString().toUpperCase(java.util.Locale.ROOT);
        if (s.matches("\\d+")) return Integer.parseInt(s);
        int c = KeyEvent.keyCodeFromString(s.startsWith("KEYCODE_") ? s : "KEYCODE_" + s);
        if (c == KeyEvent.KEYCODE_UNKNOWN) throw new ApiServer.Fault(400, "bad_request", "unknown key " + k, false);
        return c;
    }

    // ---------------- waits ----------------

    /** {"for": "ui_changed" | "idle" | "selector" | "gone", "selector": {…}, "timeout_ms": 2000} */
    private void waitFor(JSONObject w, long before, JSONObject res) throws Exception {
        String what = w.optString("for", w.has("selector") ? "selector" : "idle");
        long timeout = Math.min(Math.max(w.optLong("timeout_ms", 2000), 0), 30_000);
        long end = SystemClock.uptimeMillis() + timeout;
        UiTree.Selector sel = "selector".equals(what) || "gone".equals(what)
                ? UiTree.Selector.parse(w.optJSONObject("selector")) : null;
        if (("selector".equals(what) || "gone".equals(what)) && sel == null)
            throw new ApiServer.Fault(400, "bad_request", "wait for " + what + " needs \"selector\"", false);
        if ("idle".equals(what)) {
            res.put("verified", tree.waitIdle(timeout));
            return;
        }
        if (!"ui_changed".equals(what) && sel == null)
            throw new ApiServer.Fault(400, "bad_request", "unknown wait " + what, false);
        boolean ok = false;
        UiTree.Snapshot s = null;
        do {
            Device.Info di = device.displayInfo();
            s = collect(di, api.epoch(di));
            if ("ui_changed".equals(what)) ok = UiTree.signature(s) != before;
            else if ("selector".equals(what)) ok = !sel.find(s).isEmpty();
            else ok = sel.find(s).isEmpty();
            if (!ok) SystemClock.sleep(100);
        } while (!ok && SystemClock.uptimeMillis() < end);
        res.put("verified", ok).put("ui_id", s.uiId).put("package", s.pkg);
        if (ok && sel != null && "selector".equals(what)) {
            List<UiTree.Node> f = sel.find(s);
            res.put("match", UiTree.nodeJson(f.get(0), -1, s.pkg));
        }
    }
}
