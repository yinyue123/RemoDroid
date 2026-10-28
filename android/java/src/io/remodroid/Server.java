package io.remodroid;

import android.os.Looper;

import org.json.JSONObject;

import java.io.OutputStream;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;

/**
 * Phone server, started by the router agent through adb:
 *   CLASSPATH=/data/local/tmp/remodroid/remodroid-server.jar app_process /system/bin io.remodroid.Server \
 *       socket=remodroid lib=/data/local/tmp/remodroid/libremodroid.so log=info
 */
public final class Server implements Native.Listener {
    private final Config cfg = new Config();
    private final Device device = new Device();
    private final ScheduledExecutorService sched = Executors.newSingleThreadScheduledExecutor();
    private volatile Session session;
    private ScheduledFuture<?> pendingLock;
    private OutputStream cleanup;
    private NotificationWatcher notify;
    private String jar;
    private int lastBattery = -2;
    private String lastScreen = "";

    public static void main(String[] args) throws Exception {
        String socket = "remodroid", lib = "/data/local/tmp/remodroid/libremodroid.so", log = "info";
        for (String a : args) {
            if (a.startsWith("socket=")) socket = a.substring(7);
            else if (a.startsWith("lib=")) lib = a.substring(4);
            else if (a.startsWith("log=")) log = a.substring(4);
        }
        Ln.level = Ln.parse(log);
        Looper.prepareMainLooper(); /* some framework calls expect a Looper */
        Server s = new Server();
        s.jar = System.getProperty("java.class.path");
        System.load(lib);
        Native.listener = s;
        s.notify = new NotificationWatcher(s.cfg, s::onNotification); /* before the link, so the first config finds it */
        boolean h265 = ScreenEncoder.hasHardwareHevc(), audio = AudioCapture.supported();
        Ln.i("remodroid server starting (Android " + android.os.Build.VERSION.RELEASE + ", SDK " + Hidden.sdk()
                + ", hardware H.265: " + h265 + ", audio: " + audio + ")");
        if (!Native.start("abstract:" + socket, false, h265, audio, s.cfg.maxKbps, Ln.level)) {
            Ln.e("cannot start the native stack (is another server running?)");
            System.exit(1);
        }
        AudioCapture.init();
        new ApiServer(s.device, socket + "-api").start();
        s.startCleanup();
        s.device.watchClipboard(s::onClipboard);
        s.sched.scheduleAtFixedRate(s::reportStatus, 5, 30, TimeUnit.SECONDS);
        Looper.loop();
    }

    // ---------------- cleanup process (plan §4.9 crash safety) ----------------

    private void startCleanup() {
        try {
            /* setsid: survive the SIGHUP adbd sends to the server's process group */
            String cmd = "c=" + CleanUp.class.getName() + "; command -v setsid >/dev/null"
                    + " && exec setsid app_process /system/bin $c || exec app_process /system/bin $c";
            Process p = new ProcessBuilder("sh", "-c", cmd).redirectOutput(ProcessBuilder.Redirect.INHERIT)
                    .redirectError(ProcessBuilder.Redirect.INHERIT).start();
            cleanup = p.getOutputStream();
            cleanup("lock_on_exit=" + (cfg.lockOnDisconnect ? 1 : 0));
        } catch (Exception e) {
            Ln.w("cannot start the cleanup process", e);
        }
    }

    synchronized void cleanup(String line) {
        if (cleanup == null) return;
        try {
            cleanup.write((line + "\n").getBytes());
            cleanup.flush();
        } catch (Exception ignored) { }
    }

    // ---------------- locking after disconnect ----------------

    synchronized void scheduleLock() {
        if (!cfg.lockOnDisconnect) return;
        cancelPendingLock();
        int grace = Math.max(0, cfg.lockGraceSeconds);
        Ln.i("locking in " + grace + " s unless the viewer reconnects");
        pendingLock = sched.schedule(() -> {
            if (session != null) return;
            device.sleep();
            event("phone.locked", null);
            statusChanged();
        }, grace, TimeUnit.SECONDS);
    }

    synchronized void cancelPendingLock() {
        if (pendingLock != null) pendingLock.cancel(false);
        pendingLock = null;
    }

    // ---------------- status to the agent ----------------

    String pathName() { return "wifi".equals(cfg.path) ? "wifi" : "tunnel"; }

    void statusChanged() { sched.execute(this::reportStatus); }

    private void reportStatus() {
        try {
            int level = device.batteryLevel();
            double temp = device.batteryTemp();
            Session s = session;
            String screen = s != null && s.panelOff() ? "off" : device.isInteractive() ? "on" : "asleep";
            JSONObject d = new JSONObject()
                    .put("battery", new JSONObject().put("level", level).put("charging", device.charging()).put("temp_c", temp))
                    .put("screen", screen).put("path", pathName());
            Native.sendCtrl(new JSONObject().put("t", "status").put("d", d).toString());
            if (temp >= cfg.overheatC && lastBattery != -1) event("phone.overheat", new JSONObject().put("temp_c", temp));
            lastBattery = level;
            lastScreen = screen;
        } catch (Exception e) {
            Ln.w("status failed", e);
        }
    }

    private void event(String kind, JSONObject info) {
        try {
            Native.sendCtrl(new JSONObject().put("t", "event").put("d", new JSONObject().put("kind", kind)
                    .put("info", info == null ? new JSONObject() : info)).toString());
        } catch (Exception ignored) { }
    }

    private void onNotification(JSONObject info) {
        try {
            Native.sendCtrl(new JSONObject().put("t", "event").put("d",
                    new JSONObject().put("kind", "notification").put("info", info)).toString());
        } catch (Exception ignored) { }
    }

    private void onClipboard() {
        Session s = session;
        if (s != null) sched.execute(() -> s.clipChanged(device.getClipboard()));
    }

    // ---------------- native callbacks ----------------

    @Override
    public void onLink(boolean up) {
        if (!up) return;
        try {
            JSONObject d = new JSONObject().put("proto", 1).put("version", "0.3.1")
                    .put("android", android.os.Build.VERSION.RELEASE).put("sdk", Hidden.sdk())
                    .put("model", android.os.Build.MODEL)
                    .put("codecs", ScreenEncoder.hasHardwareHevc() ? new org.json.JSONArray().put("h265").put("h264")
                                                                    : new org.json.JSONArray().put("h264"))
                    .put("max_fps", 120).put("audio", AudioCapture.supported());
            Native.sendCtrl(new JSONObject().put("t", "ready").put("d", d).toString());
        } catch (Exception ignored) { }
        statusChanged();
    }

    @Override
    public void onCtrl(String json) {
        try {
            JSONObject m = new JSONObject(json);
            switch (m.optString("t")) {
                case "config":
                    cfg.apply(m.getJSONObject("d"));
                    Native.setDirect("wifi".equals(cfg.path));
                    Native.setMaxKbps(cfg.maxKbps);
                    cleanup("lock_on_exit=" + (cfg.lockOnDisconnect ? 1 : 0));
                    Session cur = session;
                    if (cur != null) cur.configChanged();
                    if (notify != null) notify.reconfigure();
                    Ln.i("config applied");
                    break;
                case "cmd": {
                    String c = m.getJSONObject("d").optString("cmd");
                    if ("phone.wake".equals(c)) sched.execute(device::wake);
                    else if ("phone.sleep".equals(c)) sched.execute(() -> { device.sleep(); event("phone.locked", null); });
                    break;
                }
                default:
                    Ln.d("ignoring ctrl " + m.optString("t"));
            }
        } catch (Exception e) {
            Ln.w("bad ctrl from the agent", e);
        }
    }

    @Override
    public void onSession(String sid, int state, String codec, String profile, String quality, String reason, boolean audio) {
        if (state == Native.SESSION_CONNECTING) {
            Ln.i("session " + sid + " connecting");
        } else if (state == Native.SESSION_CONNECTED) {
            Session s = new Session(this, device, cfg, sid);
            session = s;
            s.connected(codec, profile, quality, audio);
            event("session.connected", null);
        } else {
            Session s = session;
            session = null;
            Ln.i("session " + sid + " ended: " + reason);
            if (s != null) s.ended(reason);
        }
    }

    @Override
    public void onChannelOpen(String label) {
        Session s = session;
        if (s != null) s.channelOpen(label);
    }

    @Override
    public void onMessage(String label, boolean binary, byte[] data) {
        Session s = session;
        if (s != null) s.message(label, binary, data);
    }

    @Override
    public void onKeyframe() {
        Session s = session;
        if (s != null) s.keyframe();
    }

    @Override
    public void onBitrate(int kbps) {
        Session s = session;
        if (s != null) s.bitrate(kbps);
    }
}
