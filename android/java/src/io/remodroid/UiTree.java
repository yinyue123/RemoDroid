package io.remodroid;

import android.accessibilityservice.AccessibilityServiceInfo;
import android.app.UiAutomation;
import android.graphics.Rect;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Xml;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.accessibility.AccessibilityWindowInfo;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;
import org.xmlpull.v1.XmlPullParser;

import java.io.File;
import java.io.FileInputStream;
import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;

/** The UI hierarchy as JSON (skill-plan.md §5). Fast path: one in-process UiAutomation,
 *  connected the way the uiautomator command connects, kept for IDLE_MS after the last
 *  use (only one can be connected, so this frees it for other tools). Fallback:
 *  `uiautomator dump`, parsed here on the phone. All methods run under this object's lock. */
final class UiTree {
    static final int MAX_NODES = 10_000, MAX_DEPTH = 128;
    private static final long IDLE_MS = 120_000;
    private static final String DUMP = "/data/local/tmp/remodroid/rd-ui.xml";

    static final int F_CLICK = 1, F_LONG = 2, F_CHECKABLE = 4, F_CHECKED = 8, F_EDIT = 16, F_SCROLL = 32,
            F_ENABLED = 64, F_FOCUSED = 128, F_SELECTED = 256, F_PASSWORD = 512, F_VISIBLE = 1024, F_FOCUSABLE = 2048;
    private static final String[] FLAG_NAMES = { "click", "long", "checkable", "checked", "edit", "scroll",
            "enabled", "focused", "selected", "password", "visible", "focusable" };

    static final class Node {
        int id, parent = -1, depth;
        String rid, cls, text, desc, pkg;
        Rect b = new Rect();
        int flags;
        AccessibilityNodeInfo info; // fast path only; used by set_text / scroll
        boolean has(int f) { return (flags & f) != 0; }
    }

    static final class Snapshot {
        int uiId;
        String pkg = "", backend;
        List<Node> nodes = new ArrayList<>();
        boolean truncated, idle = true;
        long ms;
        Device.Info display;
        int epoch;
    }

    private final ScheduledExecutorService sched = Executors.newSingleThreadScheduledExecutor();
    private UiAutomation ua;
    private HandlerThread uaThread;
    private ScheduledFuture<?> idle;
    private String uaError;          // why the fast path is unavailable (null = untried or working)
    private long uaRetryAt;
    private int nextId = 1;
    Snapshot last;

    String backend() { return ua != null ? "uiautomation" : uaError == null ? "uiautomation" : "uiautomator-dump"; }

    // ---------------- collection ----------------

    synchronized Snapshot collect(Device.Info di, int epoch, boolean allWindows, long timeoutMs) throws ApiServer.Fault {
        long t0 = SystemClock.uptimeMillis();
        Snapshot s = new Snapshot();
        s.display = di;
        s.epoch = epoch;
        if (connect()) {
            try {
                fromUiAutomation(s, allWindows, timeoutMs);
            } catch (RuntimeException e) {
                Ln.w("UiAutomation failed; reconnecting next time", e);
                disconnect();
                s.nodes.clear();
            }
        }
        if (s.backend == null) fromDump(s, timeoutMs);
        s.uiId = nextId++;
        s.ms = SystemClock.uptimeMillis() - t0;
        last = s;
        return s;
    }

    private boolean connect() {
        if (ua != null) {
            touch();
            return true;
        }
        if (uaError != null && SystemClock.uptimeMillis() < uaRetryAt) return false;
        try {
            uaThread = new HandlerThread("uiautomation");
            uaThread.start();
            Object conn = Class.forName("android.app.UiAutomationConnection").getDeclaredConstructor().newInstance();
            Class<?> iconn = Class.forName("android.app.IUiAutomationConnection");
            UiAutomation u;
            try { /* what the uiautomator command uses */
                Constructor<UiAutomation> c = UiAutomation.class.getDeclaredConstructor(Looper.class, iconn);
                c.setAccessible(true);
                u = c.newInstance(uaThread.getLooper(), conn);
            } catch (NoSuchMethodException e) { /* releases that dropped it keep (displayId, Looper, connection) */
                Constructor<UiAutomation> c = UiAutomation.class.getDeclaredConstructor(int.class, Looper.class, iconn);
                c.setAccessible(true);
                u = c.newInstance(0, uaThread.getLooper(), conn);
            }
            Method m = Hidden.method(UiAutomation.class, "connect", 1);
            if (m != null) m.invoke(u, UiAutomation.FLAG_DONT_SUPPRESS_ACCESSIBILITY_SERVICES); /* keep TalkBack etc. */
            else UiAutomation.class.getMethod("connect").invoke(u);
            AccessibilityServiceInfo info = u.getServiceInfo();
            info.flags |= AccessibilityServiceInfo.FLAG_INCLUDE_NOT_IMPORTANT_VIEWS
                    | AccessibilityServiceInfo.FLAG_REPORT_VIEW_IDS
                    | AccessibilityServiceInfo.FLAG_RETRIEVE_INTERACTIVE_WINDOWS;
            u.setServiceInfo(info);
            ua = u;
            uaError = null;
            Ln.i("UiAutomation connected");
            touch();
            return true;
        } catch (Throwable e) {
            Throwable cause = e instanceof java.lang.reflect.InvocationTargetException ? e.getCause() : e;
            uaError = String.valueOf(cause);
            uaRetryAt = SystemClock.uptimeMillis() + 30_000;
            Ln.w("UiAutomation unavailable (" + uaError + "); using uiautomator dump");
            if (uaThread != null) uaThread.quitSafely();
            uaThread = null;
            return false;
        }
    }

    private void touch() {
        if (idle != null) idle.cancel(false);
        idle = sched.schedule(this::idleDisconnect, IDLE_MS, TimeUnit.MILLISECONDS);
    }

    private synchronized void idleDisconnect() {
        if (ua != null) Ln.i("UiAutomation idle; disconnecting");
        disconnect();
    }

    private void disconnect() {
        if (ua != null) {
            try {
                Method m = UiAutomation.class.getDeclaredMethod("disconnect");
                m.setAccessible(true);
                m.invoke(ua);
            } catch (Throwable e) {
                Ln.w("UiAutomation disconnect failed", e);
            }
        }
        ua = null;
        if (uaThread != null) uaThread.quitSafely();
        uaThread = null;
        last = null; // its AccessibilityNodeInfos belong to the old connection
    }

    /** True when an AccessibilityNodeInfo for the node is live (fast path). */
    boolean live(Node n) { return ua != null && n.info != null && n.info.refresh(); }

    boolean waitIdle(long timeoutMs) {
        if (ua == null) return true;
        try {
            ua.waitForIdle(150, Math.max(200, timeoutMs));
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    private void fromUiAutomation(Snapshot s, boolean allWindows, long timeoutMs) {
        s.idle = waitIdle(Math.min(timeoutMs, 2000));
        List<AccessibilityNodeInfo> roots = new ArrayList<>();
        if (allWindows) {
            for (AccessibilityWindowInfo w : ua.getWindows()) {
                AccessibilityNodeInfo r = w.getRoot();
                if (r != null) roots.add(r);
            }
        }
        if (roots.isEmpty()) {
            long end = SystemClock.uptimeMillis() + Math.min(timeoutMs, 1500);
            AccessibilityNodeInfo r;
            while ((r = ua.getRootInActiveWindow()) == null && SystemClock.uptimeMillis() < end)
                SystemClock.sleep(50);
            if (r == null) throw new IllegalStateException("no active window");
            roots.add(r);
        }
        s.backend = "uiautomation";
        AccessibilityNodeInfo active = allWindows ? ua.getRootInActiveWindow() : roots.get(0);
        CharSequence pkg = (active != null ? active : roots.get(0)).getPackageName();
        s.pkg = pkg == null ? "" : pkg.toString();
        for (AccessibilityNodeInfo r : roots) walk(s, r, -1, 0);
    }

    private void walk(Snapshot s, AccessibilityNodeInfo info, int parent, int depth) {
        if (info == null) return;
        if (s.nodes.size() >= MAX_NODES || depth > MAX_DEPTH) {
            s.truncated = true;
            return;
        }
        Node n = new Node();
        n.id = s.nodes.size();
        n.parent = parent;
        n.depth = depth;
        n.info = info;
        n.rid = str(info.getViewIdResourceName());
        n.cls = str(info.getClassName());
        n.pkg = str(info.getPackageName());
        n.desc = str(info.getContentDescription());
        info.getBoundsInScreen(n.b);
        if (info.isClickable()) n.flags |= F_CLICK;
        if (info.isLongClickable()) n.flags |= F_LONG;
        if (info.isCheckable()) n.flags |= F_CHECKABLE;
        if (info.isChecked()) n.flags |= F_CHECKED;
        if (info.isEditable()) n.flags |= F_EDIT;
        if (info.isScrollable()) n.flags |= F_SCROLL;
        if (info.isEnabled()) n.flags |= F_ENABLED;
        if (info.isFocused()) n.flags |= F_FOCUSED;
        if (info.isFocusable()) n.flags |= F_FOCUSABLE;
        if (info.isSelected()) n.flags |= F_SELECTED;
        if (info.isPassword()) n.flags |= F_PASSWORD;
        if (info.isVisibleToUser()) n.flags |= F_VISIBLE;
        n.text = n.has(F_PASSWORD) ? null : str(info.getText());
        s.nodes.add(n);
        for (int i = 0; i < info.getChildCount(); i++) walk(s, info.getChild(i), n.id, depth + 1);
    }

    private static String str(CharSequence c) { return c == null || c.length() == 0 ? null : c.toString(); }

    private void fromDump(Snapshot s, long timeoutMs) throws ApiServer.Fault {
        File f = new File(DUMP);
        try {
            f.getParentFile().mkdirs();
            Shell.Result r = Shell.run("uiautomator dump " + DUMP, Math.max(timeoutMs, 3000));
            if (r.code != 0 || !f.exists())
                throw new ApiServer.Fault(503, "tree_unavailable", "uiautomator dump failed: "
                        + (r.out + r.err).trim() + (uaError != null ? " (UiAutomation: " + uaError + ")" : ""), true);
            XmlPullParser p = Xml.newPullParser();
            p.setFeature(XmlPullParser.FEATURE_PROCESS_DOCDECL, false);
            try (FileInputStream in = new FileInputStream(f)) {
                p.setInput(in, "UTF-8");
                List<Integer> stack = new ArrayList<>();
                for (int ev = p.getEventType(); ev != XmlPullParser.END_DOCUMENT; ev = p.next()) {
                    if (ev == XmlPullParser.START_TAG && "node".equals(p.getName())) {
                        int depth = stack.size();
                        if (s.nodes.size() >= MAX_NODES || depth > MAX_DEPTH) {
                            s.truncated = true;
                            stack.add(-1);
                            continue;
                        }
                        Node n = xmlNode(p, s.nodes.size(), depth);
                        for (int i = stack.size() - 1; i >= 0 && n.parent < 0; i--) n.parent = stack.get(i);
                        s.nodes.add(n);
                        stack.add(n.id);
                        if (s.pkg.isEmpty() && n.pkg != null) s.pkg = n.pkg;
                    } else if (ev == XmlPullParser.END_TAG && "node".equals(p.getName()) && !stack.isEmpty()) {
                        stack.remove(stack.size() - 1);
                    }
                }
            }
            s.backend = "uiautomator-dump";
        } catch (ApiServer.Fault e) {
            throw e;
        } catch (Exception e) {
            throw new ApiServer.Fault(503, "tree_unavailable", "cannot read the UI dump: " + e, true);
        } finally {
            f.delete();
        }
    }

    private static Node xmlNode(XmlPullParser p, int id, int depth) {
        Node n = new Node();
        n.id = id;
        n.depth = depth;
        n.rid = emptyNull(p.getAttributeValue(null, "resource-id"));
        n.cls = emptyNull(p.getAttributeValue(null, "class"));
        n.pkg = emptyNull(p.getAttributeValue(null, "package"));
        n.desc = emptyNull(p.getAttributeValue(null, "content-desc"));
        if (attr(p, "clickable")) n.flags |= F_CLICK;
        if (attr(p, "long-clickable")) n.flags |= F_LONG;
        if (attr(p, "checkable")) n.flags |= F_CHECKABLE;
        if (attr(p, "checked")) n.flags |= F_CHECKED;
        if (attr(p, "scrollable")) n.flags |= F_SCROLL;
        if (attr(p, "enabled")) n.flags |= F_ENABLED;
        if (attr(p, "focused")) n.flags |= F_FOCUSED;
        if (attr(p, "focusable")) n.flags |= F_FOCUSABLE;
        if (attr(p, "selected")) n.flags |= F_SELECTED;
        if (attr(p, "password")) n.flags |= F_PASSWORD;
        String vis = p.getAttributeValue(null, "visible-to-user");
        if (vis == null || "true".equals(vis)) n.flags |= F_VISIBLE; /* older dumps list only visible nodes */
        if (n.cls != null && n.cls.contains("EditText")) n.flags |= F_EDIT; /* not in the XML: inferred */
        n.text = n.has(F_PASSWORD) ? null : emptyNull(p.getAttributeValue(null, "text"));
        String b = p.getAttributeValue(null, "bounds"); /* [l,t][r,b] */
        if (b != null) {
            String[] v = b.replace("][", ",").replace("[", "").replace("]", "").split(",");
            if (v.length == 4) {
                try {
                    n.b.set(Integer.parseInt(v[0]), Integer.parseInt(v[1]), Integer.parseInt(v[2]), Integer.parseInt(v[3]));
                } catch (NumberFormatException ignored) { }
            }
        }
        return n;
    }

    private static boolean attr(XmlPullParser p, String name) { return "true".equals(p.getAttributeValue(null, name)); }

    private static String emptyNull(String s) { return s == null || s.isEmpty() ? null : s; }

    // ---------------- JSON ----------------

    /** Compact keeps visible nodes that act or carry a label; parents point at the nearest kept ancestor. */
    static boolean keepCompact(Node n) {
        if (!n.has(F_VISIBLE)) return false;
        return (n.flags & (F_CLICK | F_LONG | F_CHECKABLE | F_EDIT | F_SCROLL)) != 0 || n.text != null || n.desc != null;
    }

    static JSONObject toJson(Snapshot s, boolean compact) throws JSONException {
        JSONObject o = new JSONObject().put("schema", 1).put("ui_id", s.uiId).put("package", s.pkg)
                .put("display", new JSONObject().put("w", s.display.width).put("h", s.display.height)
                        .put("rotation", s.display.rotation).put("epoch", s.epoch))
                .put("backend", s.backend).put("view", compact ? "compact" : "full")
                .put("truncated", s.truncated).put("idle", s.idle).put("collect_ms", s.ms);
        boolean[] kept = new boolean[s.nodes.size()];
        JSONArray arr = new JSONArray();
        for (Node n : s.nodes) {
            kept[n.id] = !compact || keepCompact(n);
            if (!kept[n.id]) continue;
            int p = n.parent;
            while (p >= 0 && !kept[p]) p = s.nodes.get(p).parent;
            arr.put(nodeJson(n, p, s.pkg));
        }
        o.put("count", arr.length()).put("nodes", arr);
        return o;
    }

    static JSONObject nodeJson(Node n, int parent, String pkg) throws JSONException {
        JSONObject j = new JSONObject().put("id", n.id);
        if (parent >= 0) j.put("p", parent);
        if (n.rid != null) j.put("rid", n.rid);
        if (n.cls != null) j.put("cls", shortClass(n.cls));
        if (n.text != null) j.put("text", n.text);
        if (n.desc != null) j.put("desc", n.desc);
        if (n.pkg != null && !n.pkg.equals(pkg)) j.put("pkg", n.pkg);
        j.put("b", new JSONArray().put(n.b.left).put(n.b.top).put(n.b.right).put(n.b.bottom));
        JSONArray f = new JSONArray();
        for (int i = 0; i < FLAG_NAMES.length; i++)
            if ((n.flags & (1 << i)) != 0 && (1 << i) != F_VISIBLE) f.put(FLAG_NAMES[i]);
        if (!n.has(F_VISIBLE)) f.put("hidden");
        j.put("f", f);
        return j;
    }

    static String shortClass(String c) {
        int dot = c.lastIndexOf('.');
        return dot >= 0 && (c.startsWith("android.widget.") || c.startsWith("android.view.")) ? c.substring(dot + 1) : c;
    }

    /** Changes when what the user can see or act on changes (for wait: ui_changed). */
    static long signature(Snapshot s) {
        long h = s.pkg.hashCode();
        for (Node n : s.nodes) {
            if (!n.has(F_VISIBLE)) continue;
            h = h * 31 + (n.rid == null ? 0 : n.rid.hashCode());
            h = h * 31 + (n.text == null ? 0 : n.text.hashCode());
            h = h * 31 + (n.desc == null ? 0 : n.desc.hashCode());
            h = h * 31 + n.b.hashCode();
            h = h * 31 + (n.flags & (F_CHECKED | F_SELECTED | F_FOCUSED | F_ENABLED));
        }
        return h;
    }

    // ---------------- selectors ----------------

    /** {"rid", "text", "text_contains", "desc", "desc_contains", "label", "cls", "pkg",
     *   "clickable", "enabled", "checked", "editable", "scrollable", "within": {…}, "index": n} */
    static final class Selector {
        String rid, text, textContains, desc, descContains, label, cls, pkg;
        Boolean clickable, enabled, checked, editable, scrollable;
        Selector within;
        int index = -1;

        static Selector parse(JSONObject o) throws ApiServer.Fault {
            if (o == null) return null;
            Selector s = new Selector();
            s.rid = o.optString("rid", null);
            s.text = o.optString("text", null);
            s.textContains = o.optString("text_contains", null);
            s.desc = o.optString("desc", null);
            s.descContains = o.optString("desc_contains", null);
            s.label = o.optString("label", null);
            s.cls = o.optString("cls", null);
            s.pkg = o.optString("pkg", null);
            s.clickable = bool(o, "clickable");
            s.enabled = bool(o, "enabled");
            s.checked = bool(o, "checked");
            s.editable = bool(o, "editable");
            s.scrollable = bool(o, "scrollable");
            s.index = o.optInt("index", -1);
            s.within = parse(o.optJSONObject("within"));
            if (s.rid == null && s.text == null && s.textContains == null && s.desc == null && s.descContains == null
                    && s.label == null && s.cls == null && s.within == null)
                throw new ApiServer.Fault(400, "bad_request", "the selector needs rid, text, desc, label, cls or within", false);
            return s;
        }

        private static Boolean bool(JSONObject o, String k) { return o.has(k) ? o.optBoolean(k) : null; }

        boolean matchesSelf(Node n) {
            if (rid != null && (n.rid == null || !(n.rid.equals(rid) || n.rid.endsWith(":id/" + rid)))) return false;
            if (text != null && !text.equals(n.text)) return false;
            if (textContains != null && (n.text == null || !n.text.contains(textContains))) return false;
            if (desc != null && !desc.equals(n.desc)) return false;
            if (descContains != null && (n.desc == null || !n.desc.contains(descContains))) return false;
            if (label != null && !label.equals(n.text) && !label.equals(n.desc)) return false;
            if (cls != null && (n.cls == null || !(n.cls.equals(cls) || shortClass(n.cls).equals(cls)))) return false;
            if (pkg != null && !pkg.equals(n.pkg)) return false;
            if (clickable != null && clickable != n.has(F_CLICK)) return false;
            if (enabled != null && enabled != n.has(F_ENABLED)) return false;
            if (checked != null && checked != n.has(F_CHECKED)) return false;
            if (editable != null && editable != n.has(F_EDIT)) return false;
            if (scrollable != null && scrollable != n.has(F_SCROLL)) return false;
            return true;
        }

        boolean matches(Node n, List<Node> all) {
            if (!matchesSelf(n)) return false;
            if (within == null) return true;
            for (int p = n.parent; p >= 0; p = all.get(p).parent)
                if (within.matches(all.get(p), all)) return true;
            return false;
        }

        /** Visible matches (all matches when none is visible). */
        List<Node> find(Snapshot s) {
            List<Node> vis = new ArrayList<>(), any = new ArrayList<>();
            for (Node n : s.nodes) {
                if (!matches(n, s.nodes)) continue;
                any.add(n);
                if (n.has(F_VISIBLE)) vis.add(n);
            }
            return vis.isEmpty() ? any : vis;
        }
    }
}
