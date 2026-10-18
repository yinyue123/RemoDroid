package io.remodroid;

import android.view.KeyEvent;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;

/** One viewer session: the ctrl channel protocol (plan A.6), screen power and
 *  locking policy (§4.9), clipboard sync (§4.8) and stats. */
final class Session implements ScreenEncoder.Listener {
    private final Server server;
    private final Device device;
    private final Config cfg;
    private final Controller controller;
    private final ScreenEncoder encoder;
    private final Files files;
    private final AudioCapture audio = new AudioCapture();
    private volatile boolean audioNegotiated, audioWanted = true; /* audioWanted: the viewer's Sound switch */
    private final ScheduledExecutorService exec = Executors.newSingleThreadScheduledExecutor();
    private final Map<Integer, Shell.Logcat> logcats = new HashMap<>();
    final String sid;
    private String codec = "h264", profile = "baseline";
    private JSONObject quality = new JSONObject();
    private volatile boolean active = true, ctrlOpen, panelOff;
    private volatile String lastClipSent, lastClipSet;
    private volatile double fps;
    private volatile int targetKbps;
    private int nextStream = 1;
    private ScheduledFuture<?> statsTask;

    Session(Server server, Device device, Config cfg, String sid) {
        this.server = server;
        this.device = device;
        this.cfg = cfg;
        this.sid = sid;
        this.controller = new Controller(device);
        this.encoder = new ScreenEncoder(device, this);
        this.files = new Files(cfg, new FilesOut(this));
    }

    /** Where file control replies and pulled chunks go. Static on purpose: JDK 21 javac
     *  marks inner-class constructors in a way d8 8.2 cannot read. */
    private static final class FilesOut implements Files.Out {
        private final Session s;

        FilesOut(Session s) { this.s = s; }

        public void ctrl(JSONObject m) { s.send(m); }
        public void chunk(byte[] d) { Native.dcSend("file", true, d); }
        public boolean canSend() { return s.active; }
    }

    static int shortSide(String preset) {
        switch (preset == null ? "" : preset) {
            case "1080p": return 1080;
            case "540p": return 540;
            case "360p": return 360;
            default: return 720;
        }
    }

    // ---------------- lifecycle (native thread) ----------------

    void connected(String codec, String profile, String qualityJson, boolean audioNegotiated) {
        this.codec = codec;
        this.profile = profile;
        this.audioNegotiated = audioNegotiated;
        try {
            quality = new JSONObject(qualityJson == null ? "{}" : qualityJson);
        } catch (Exception e) {
            quality = new JSONObject();
        }
        exec.execute(() -> {
            server.cancelPendingLock();
            device.wake();
            int kbps = Math.min(quality.optInt("kbps", 2500), cfg.maxKbps);
            if ("manual".equals(quality.optString("mode", cfg.videoMode))) Native.setMaxKbps(quality.optInt("kbps", cfg.maxKbps));
            else Native.setMaxKbps(Math.min(quality.optInt("kbps", cfg.maxKbps), cfg.maxKbps));
            encoder.configure(codec, profile, shortSide(quality.optString("preset", cfg.preset)),
                    Math.min(quality.optInt("fps", cfg.maxFps), cfg.maxFps), kbps);
            encoder.start();
            updateAudio();
        });
        statsTask = exec.scheduleAtFixedRate(this::sendStats, 1, 1, TimeUnit.SECONDS);
    }

    void ended(String reason) {
        active = false;
        exec.execute(() -> {
            encoder.stop();
            audio.stop();
            for (Shell.Logcat l : logcats.values()) l.stop();
            logcats.clear();
            files.cancelAll();
            if (panelOff) {
                device.setPanel(true);
                panelOff = false;
                server.cleanup("panel_on");
            }
            server.scheduleLock();
        });
        if (statsTask != null) statsTask.cancel(false);
        exec.shutdown();
    }

    // ---------------- encoder events ----------------

    @Override
    public void onFirstFrame() {
        if (cfg.screenOffOnConnect && active) exec.execute(() -> setPanel(false));
    }

    @Override
    public void onSizeChanged(int w, int h, int rotation) {
        Device.Info di = device.displayInfo();
        controller.setScreen(di.width, di.height);
        try {
            send(new JSONObject().put("t", "screen.changed")
                    .put("d", new JSONObject().put("w", w).put("h", h).put("rotation", rotation)));
        } catch (Exception ignored) { }
    }

    @Override
    public void onFrameStats(double fps, double encodeMs) { this.fps = fps; }

    void keyframe() { encoder.requestKeyframe(); }

    void bitrate(int kbps) {
        targetKbps = kbps;
        if (!"manual".equals(quality.optString("mode", cfg.videoMode))) encoder.setBitrate(kbps);
    }

    private void setPanel(boolean on) {
        if (device.setPanel(on)) {
            panelOff = !on;
            server.cleanup(on ? "panel_on" : "panel_off");
            server.statusChanged();
            try {
                send(new JSONObject().put("t", "screen.state").put("d", new JSONObject()
                        .put("panel", on ? "on" : "off").put("locked", false)));
            } catch (Exception ignored) { }
        }
    }

    boolean panelOff() { return panelOff; }

    // ---------------- sound ----------------

    /** Capture runs only while it has somewhere to go: the viewer took the track, the router's
     *  [phone] audio setting is on, and the viewer hasn't switched Sound off. Stopping gives the
     *  sound back to the phone's speaker. */
    private void updateAudio() {
        boolean want = active && audioNegotiated && cfg.audio && audioWanted;
        if (want && !audio.running()) audio.start();
        else if (!want && audio.running()) audio.stop();
    }

    void configChanged() {
        try {
            exec.execute(this::updateAudio);
        } catch (java.util.concurrent.RejectedExecutionException ignored) { } /* the session just ended */
    }

    private void sendAudioState() {
        try {
            send(new JSONObject().put("t", "audio.state").put("d", new JSONObject().put("on", audio.running())
                    .put("available", audioNegotiated && AudioCapture.supported()).put("enabled", cfg.audio)));
        } catch (Exception ignored) { }
    }

    // ---------------- data channels ----------------

    void channelOpen(String label) {
        if (!"ctrl".equals(label)) return;
        ctrlOpen = true;
        Device.Info di = device.displayInfo();
        try {
            JSONArray codecs = new JSONArray();
            if (ScreenEncoder.hasHardwareHevc()) codecs.put("h265");
            codecs.put("h264");
            send(new JSONObject().put("t", "session.ready").put("d", new JSONObject().put("proto", 1)
                    .put("screen", new JSONObject().put("w", di.width).put("h", di.height).put("rotation", di.rotation))
                    .put("codec", codec)
                    .put("caps", new JSONObject().put("codecs", codecs).put("max_fps", cfg.maxFps)
                            .put("audio", audioNegotiated && AudioCapture.supported()))));
        } catch (Exception ignored) { }
        if (cfg.clipboardSync) {
            String c = device.getClipboard();
            if (c != null) clipChanged(c);
        }
    }

    void message(String label, boolean binary, byte[] data) {
        if ("input".equals(label)) {
            controller.handle(data);
        } else if ("file".equals(label)) {
            files.chunk(data);
        } else if ("ctrl".equals(label)) {
            try {
                JSONObject m = new JSONObject(new String(data, "UTF-8"));
                exec.execute(() -> control(m));
            } catch (Exception e) {
                Ln.w("bad ctrl message", e);
            }
        }
    }

    void send(JSONObject m) {
        if (!active) return;
        try {
            Native.dcSend("ctrl", false, m.toString().getBytes("UTF-8"));
        } catch (Exception ignored) { }
    }

    private void reply(JSONObject req, JSONObject d) {
        try {
            JSONObject r = new JSONObject().put("t", "ack").put("d", d == null ? new JSONObject() : d);
            if (req.has("id")) r.put("id", req.get("id"));
            send(r);
        } catch (Exception ignored) { }
    }

    private void error(JSONObject req, String code, String msg) {
        try {
            JSONObject r = new JSONObject().put("t", "error").put("d", new JSONObject().put("code", code).put("msg", msg));
            if (req.has("id")) r.put("id", req.get("id"));
            send(r);
        } catch (Exception ignored) { }
    }

    private static final Map<String, Integer> NAV = new HashMap<>();
    static {
        NAV.put("back", KeyEvent.KEYCODE_BACK);
        NAV.put("home", KeyEvent.KEYCODE_HOME);
        NAV.put("recents", KeyEvent.KEYCODE_APP_SWITCH);
        NAV.put("power", KeyEvent.KEYCODE_POWER);
        NAV.put("volume_up", KeyEvent.KEYCODE_VOLUME_UP);
        NAV.put("volume_down", KeyEvent.KEYCODE_VOLUME_DOWN);
    }

    private void control(JSONObject m) {
        String t = m.optString("t");
        JSONObject d = m.optJSONObject("d");
        if (d == null) d = new JSONObject();
        if ("key".equals(t) || "nav".equals(t) || "text".equals(t)) Controller.lastInputAt = android.os.SystemClock.uptimeMillis();
        try {
            switch (t) {
                case "quality.set": {
                    String oldPreset = quality.optString("preset", cfg.preset);
                    int oldFps = quality.optInt("fps", cfg.maxFps);
                    quality = d;
                    if (!d.optString("codec", "auto").equals("auto") && !d.optString("codec").equals(codec)) {
                        error(m, "unsupported", "the codec changes on the next connection");
                    }
                    boolean manual = "manual".equals(d.optString("mode"));
                    int kbps = Math.min(d.optInt("kbps", cfg.maxKbps), cfg.maxKbps);
                    Native.setMaxKbps(kbps);
                    if (manual) encoder.setBitrate(kbps);
                    if (!d.optString("preset", oldPreset).equals(oldPreset) || d.optInt("fps", oldFps) != oldFps)
                        encoder.configure(codec, profile, shortSide(d.optString("preset", oldPreset)),
                                Math.min(d.optInt("fps", oldFps), cfg.maxFps), manual ? kbps : Math.max(targetKbps, 1000));
                    send(new JSONObject().put("t", "quality.state").put("d", new JSONObject(d.toString())
                            .put("width", encoder.width).put("height", encoder.height).put("codec", codec)));
                    reply(m, null);
                    break;
                }
                case "key":
                    if ("press".equals(d.optString("action"))) device.press(d.getInt("code"));
                    else device.key("up".equals(d.optString("action")) ? KeyEvent.ACTION_UP : KeyEvent.ACTION_DOWN,
                            d.getInt("code"), d.optInt("meta", 0));
                    reply(m, null);
                    break;
                case "nav": {
                    String b = d.optString("button");
                    if ("notifications".equals(b)) Shell.runQuiet("cmd statusbar expand-notifications");
                    else if (NAV.containsKey(b)) device.press(NAV.get(b));
                    else { error(m, "bad_request", "unknown button"); break; }
                    reply(m, null);
                    break;
                }
                case "text":
                    if (device.text(d.optString("text"))) reply(m, null);
                    else error(m, "io_error", "text injection failed");
                    break;
                case "rotate": {
                    String to = d.optString("to", "auto");
                    if ("auto".equals(to)) Shell.runQuiet("settings put system accelerometer_rotation 1");
                    else Shell.runQuiet("settings put system accelerometer_rotation 0; settings put system user_rotation "
                            + ("landscape".equals(to) ? 1 : 0));
                    reply(m, null);
                    break;
                }
                case "clip.set": {
                    String text = d.optString("text");
                    lastClipSet = text;
                    if (!device.setClipboard(text)) { error(m, "io_error", "cannot set the clipboard"); break; }
                    if (d.optBoolean("paste", cfg.autoPaste)) device.press(KeyEvent.KEYCODE_PASTE);
                    reply(m, null);
                    break;
                }
                case "audio.set":
                    audioWanted = d.optBoolean("on", true);
                    updateAudio();
                    sendAudioState();
                    reply(m, new JSONObject().put("on", audio.running()));
                    break;
                case "screen.power":
                    setPanel(d.optBoolean("on", true));
                    reply(m, null);
                    break;
                case "shell.exec": {
                    Shell.Result r = Shell.run(d.optString("cmd", "true"), d.optLong("timeout_ms", 10000));
                    reply(m, new JSONObject().put("exit_code", r.code).put("stdout", r.out).put("stderr", r.err)
                            .put("truncated", r.truncated).put("ms", r.ms));
                    break;
                }
                case "logcat.start": {
                    int stream = nextStream++;
                    List<String> filter = new ArrayList<>(), buffers = new ArrayList<>();
                    JSONArray f = d.optJSONArray("filter"), b = d.optJSONArray("buffers");
                    for (int i = 0; f != null && i < f.length(); i++) filter.add(f.getString(i));
                    for (int i = 0; b != null && i < b.length(); i++) buffers.add(b.getString(i));
                    Shell.Logcat lc = new Shell.Logcat(filter, d.optString("format"), d.optInt("tail", 200), buffers,
                            lines -> {
                                try {
                                    send(new JSONObject().put("t", "logcat.lines").put("d", new JSONObject()
                                            .put("stream", stream).put("lines", new JSONArray(lines))));
                                } catch (Exception ignored) { }
                            },
                            () -> {
                                try {
                                    send(new JSONObject().put("t", "logcat.ended").put("d", new JSONObject()
                                            .put("stream", stream).put("reason", "stopped")));
                                } catch (Exception ignored) { }
                            });
                    logcats.put(stream, lc);
                    reply(m, new JSONObject().put("stream", stream));
                    break;
                }
                case "logcat.stop": {
                    Shell.Logcat lc = logcats.remove(d.optInt("stream"));
                    if (lc != null) lc.stop();
                    reply(m, null);
                    break;
                }
                case "ping":
                    send(new JSONObject().put("t", "pong").put("id", m.opt("id"))
                            .put("d", new JSONObject().put("ts", d.opt("ts")).put("phone_ts", System.currentTimeMillis())));
                    break;
                default:
                    if (t.startsWith("file.")) reply(m, files.control(t, d));
                    else error(m, "unsupported", t);
            }
        } catch (Files.Fault f) {
            error(m, f.code, f.getMessage());
        } catch (Exception e) {
            Ln.w("ctrl " + t + " failed", e);
            error(m, "io_error", String.valueOf(e.getMessage()));
        }
    }

    // ---------------- clipboard / stats ----------------

    void clipChanged(String text) {
        if (!ctrlOpen || !cfg.clipboardSync || text == null || text.equals(lastClipSent) || text.equals(lastClipSet)) return;
        lastClipSent = text;
        try {
            send(new JSONObject().put("t", "clip.changed").put("d", new JSONObject().put("text", text)));
        } catch (Exception ignored) { }
    }

    private void sendStats() {
        if (!ctrlOpen) return;
        try {
            JSONObject s = new JSONObject(Native.stats());
            JSONObject d = new JSONObject().put("fps", Math.round(fps)).put("kbps", s.optInt("acked_kbps", targetKbps))
                    .put("target_kbps", s.optInt("target_kbps")).put("rtt_ms", s.optInt("rtt_ms"))
                    .put("loss_pct", s.optDouble("loss_pct", 0)).put("codec", codec)
                    .put("width", encoder.width).put("height", encoder.height)
                    .put("path", "relay".equals(s.optString("local")) ? "relay" : server.pathName())
                    .put("panel", panelOff ? "off" : "on").put("inputs", controller.events).put("audio", audio.running());
            send(new JSONObject().put("t", "stats").put("d", d));
        } catch (Exception ignored) { }
    }
}
