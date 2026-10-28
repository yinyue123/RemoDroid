package io.remodroid;

import android.net.LocalServerSocket;
import android.net.LocalSocket;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Iterator;
import java.util.concurrent.SynchronousQueue;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;

/** The AI control API on the phone (skill-plan.md §4), on its own abstract socket
 *  (localabstract:remodroid-api). The router agent opens one ADB stream per request:
 *
 *    → {"op": "ui" | "screenshot" | "action" | "capabilities", "args": {…}}\n
 *    ← {"status": 200, "type": "image/jpeg", "len": 123456, "headers": {…}}\n  then len body bytes
 *
 *  and the connection closes. Closing it early cancels the request. */
final class ApiServer {
    static final int SCHEMA = 1;
    private static final int MAX_REQUEST = 1 << 20;

    /** An API error: HTTP status, a code from skill-plan.md §8 and whether retrying can help. */
    static final class Fault extends Exception {
        final int status;
        final String code;
        final boolean retryable;
        JSONObject extra;

        Fault(int status, String code, String msg, boolean retryable) {
            super(msg);
            this.status = status;
            this.code = code;
            this.retryable = retryable;
        }

        Fault with(String key, Object value) {
            try {
                if (extra == null) extra = new JSONObject();
                extra.put(key, value);
            } catch (Exception ignored) { }
            return this;
        }
    }

    private final String name;
    private final Device device;
    private final UiTree tree = new UiTree();
    private final Capture capture;
    private final Actions actions;
    private final ThreadPoolExecutor pool = new ThreadPoolExecutor(1, 4, 30, TimeUnit.SECONDS, new SynchronousQueue<>());
    private int epoch, ew = -1, eh = -1, er = -1;

    ApiServer(Device device, String name) {
        this.device = device;
        this.name = name;
        this.capture = new Capture(device);
        this.actions = new Actions(device, tree, this);
    }

    /** Increments whenever the display size or rotation changes, so stale coordinates are refused. */
    synchronized int epoch(Device.Info di) {
        if (di.width != ew || di.height != eh || di.rotation != er) {
            if (ew >= 0) epoch++;
            ew = di.width;
            eh = di.height;
            er = di.rotation;
        }
        return epoch;
    }

    void start() {
        Thread t = new Thread(this::acceptLoop, "api");
        t.setDaemon(true);
        t.start();
    }

    private void acceptLoop() {
        LocalServerSocket ss;
        try {
            ss = new LocalServerSocket(name);
        } catch (Exception e) {
            Ln.e("cannot listen on @" + name + "; the AI API is off", e);
            return;
        }
        Ln.i("AI API on @" + name);
        for (;;) {
            try {
                LocalSocket s = ss.accept();
                try {
                    pool.execute(() -> serve(s));
                } catch (java.util.concurrent.RejectedExecutionException e) {
                    respond(s.getOutputStream(), error(new Fault(503, "busy", "too many requests at once", true)));
                    s.close();
                }
            } catch (Exception e) {
                Ln.w("API accept failed", e);
            }
        }
    }

    private static final class Response {
        int status = 200;
        String type = "application/json";
        byte[] body;
        JSONObject headers = new JSONObject();
    }

    private void serve(LocalSocket s) {
        try (LocalSocket sock = s) {
            InputStream in = sock.getInputStream();
            ByteArrayOutputStream line = new ByteArrayOutputStream();
            int c;
            while ((c = in.read()) >= 0 && c != '\n') {
                line.write(c);
                if (line.size() > MAX_REQUEST) throw new Fault(413, "too_large", "request too large", false);
            }
            Response r;
            String op = "?";
            long t0 = System.nanoTime();
            try {
                JSONObject req = new JSONObject(line.toString("UTF-8"));
                op = req.optString("op");
                JSONObject args = req.optJSONObject("args");
                r = handle(op, args == null ? new JSONObject() : args);
            } catch (Fault f) {
                r = error(f);
            } catch (org.json.JSONException e) {
                r = error(new Fault(400, "bad_request", "bad JSON: " + e.getMessage(), false));
            } catch (Exception e) {
                Ln.w("API " + op + " failed", e);
                r = error(new Fault(500, "io_error", String.valueOf(e), true));
            }
            Ln.d("API " + op + " -> " + r.status + " " + r.body.length + " B in " + (System.nanoTime() - t0) / 1_000_000 + " ms");
            respond(sock.getOutputStream(), r);
        } catch (Exception e) {
            Ln.d("API connection: " + e);
        }
    }

    private static void respond(OutputStream out, Response r) throws Exception {
        JSONObject h = new JSONObject().put("status", r.status).put("type", r.type).put("len", r.body.length)
                .put("headers", r.headers);
        out.write((h.toString() + "\n").getBytes(StandardCharsets.UTF_8));
        out.write(r.body);
        out.flush();
    }

    private static Response json(int status, JSONObject o) {
        Response r = new Response();
        r.status = status;
        r.body = o.toString().getBytes(StandardCharsets.UTF_8);
        return r;
    }

    private static Response error(Fault f) {
        JSONObject o = new JSONObject();
        try {
            o.put("error", f.code).put("msg", f.getMessage()).put("retryable", f.retryable);
            if (f.extra != null)
                for (Iterator<String> it = f.extra.keys(); it.hasNext(); ) {
                    String k = it.next();
                    o.put(k, f.extra.get(k));
                }
        } catch (Exception ignored) { }
        return json(f.status, o);
    }

    private Response handle(String op, JSONObject a) throws Exception {
        switch (op) {
            case "capabilities": return json(200, capabilities());
            case "ui": {
                String view = a.optString("view", "compact");
                if (!"compact".equals(view) && !"full".equals(view))
                    throw new Fault(400, "bad_request", "view is compact or full", false);
                Device.Info di = device.displayInfo();
                UiTree.Snapshot snap = tree.collect(di, epoch(di), "all".equals(a.optString("windows")),
                        clamp(a.optLong("timeout_ms", 5000), 500, 30_000));
                return json(200, UiTree.toJson(snap, "compact".equals(view)));
            }
            case "screenshot": return screenshot(a);
            case "action": return json(200, actions.run(a));
            default: throw new Fault(404, "not_found", "unknown op " + op, false);
        }
    }

    private Response screenshot(JSONObject a) throws Exception {
        String fmt = a.optString("format", "jpeg");
        int format = "png".equals(fmt) ? Capture.PNG : "webp".equals(fmt) ? Capture.WEBP
                : "jpeg".equals(fmt) || "jpg".equals(fmt) ? Capture.JPEG : -1;
        if (format < 0) throw new Fault(400, "bad_request", "format is jpeg, png or webp", false);
        if (format == Capture.PNG && a.has("quality"))
            throw new Fault(400, "bad_request", "quality applies to jpeg and webp only", false);
        int maxEdge = a.optInt("max_edge", 1280), quality = a.optInt("quality", format == Capture.WEBP ? 70 : 75);
        if (maxEdge < 160 || maxEdge > 4096) throw new Fault(400, "bad_request", "max_edge must be 160..4096", false);
        if (quality < 10 || quality > 100) throw new Fault(400, "bad_request", "quality must be 10..100", false);
        Capture.Shot s = capture.take(maxEdge, format, quality, clamp(a.optLong("timeout_ms", 3000), 200, 30_000));
        Response r = new Response();
        r.type = s.type;
        r.body = s.data;
        r.headers.put("X-RD-Width", s.width).put("X-RD-Height", s.height)
                .put("X-RD-Display-W", s.display.width).put("X-RD-Display-H", s.display.height)
                .put("X-RD-Rotation", s.display.rotation).put("X-RD-Epoch", epoch(s.display))
                .put("X-RD-Backend", s.backend).put("X-RD-Frame-Age-Ms", s.frameAgeMs).put("X-RD-Encode-Ms", s.encodeMs)
                .put("X-RD-Screen", device.isInteractive() ? "on" : "asleep");
        return r;
    }

    private JSONObject capabilities() throws Exception {
        Device.Info di = device.displayInfo();
        return new JSONObject().put("schema", SCHEMA).put("model", android.os.Build.MODEL)
                .put("android", android.os.Build.VERSION.RELEASE).put("sdk", Hidden.sdk())
                .put("display", new JSONObject().put("w", di.width).put("h", di.height).put("rotation", di.rotation)
                        .put("epoch", epoch(di)))
                .put("ui", new JSONObject().put("backend", tree.backend()).put("views", new JSONArray().put("compact").put("full")))
                .put("screenshot", new JSONObject().put("formats", new JSONArray().put("jpeg").put("png").put("webp"))
                        .put("encoder", Native.hasImageEncoder() ? "c" : "java"))
                .put("actions", new JSONArray().put("tap").put("long_press").put("swipe").put("scroll").put("key")
                        .put("text").put("set_text").put("back").put("home").put("recents").put("launch").put("wait"))
                .put("swipe_path", true) /* swipe {"path": [points]}: one continuous touch */
                .put("viewer_active", Controller.lastInputAt > 0
                        && android.os.SystemClock.uptimeMillis() - Controller.lastInputAt < Actions.VIEWER_GRACE_MS);
    }

    private static long clamp(long v, long lo, long hi) { return Math.max(lo, Math.min(hi, v)); }
}
