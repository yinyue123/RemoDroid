package io.remodroid;

import org.json.JSONObject;

/** Settings pushed by the router agent from agent.ini ([phone], [video], [files]). */
final class Config {
    volatile boolean screenOffOnConnect = true;
    volatile boolean lockOnDisconnect = true;
    volatile int lockGraceSeconds = 10;
    volatile boolean clipboardSync = true;
    volatile boolean autoPaste = false;
    volatile int overheatC = 45;
    volatile String rotation = "auto";
    volatile String path = "auto";
    volatile boolean forceRelay = false;
    volatile String videoMode = "auto";
    volatile String preset = "720p";
    volatile String codec = "auto";
    volatile int maxKbps = 20000;
    volatile int maxFps = 60;
    volatile String uploadDir = "/sdcard/DCIM/RemoDroid";
    volatile boolean mediaScan = true;
    volatile boolean overwrite = false;
    volatile int maxMb = 2048;

    void apply(JSONObject d) {
        JSONObject p = d.optJSONObject("phone"), v = d.optJSONObject("video"), f = d.optJSONObject("files");
        if (p != null) {
            screenOffOnConnect = p.optBoolean("screen_off_on_connect", screenOffOnConnect);
            lockOnDisconnect = p.optBoolean("lock_on_disconnect", lockOnDisconnect);
            lockGraceSeconds = p.optInt("lock_grace_seconds", lockGraceSeconds);
            clipboardSync = p.optBoolean("clipboard_sync", clipboardSync);
            autoPaste = p.optBoolean("auto_paste", autoPaste);
            overheatC = p.optInt("overheat_c", overheatC);
            rotation = p.optString("rotation", rotation);
            path = p.optString("path", path);
            forceRelay = p.optBoolean("force_relay", forceRelay);
        }
        if (v != null) {
            videoMode = v.optString("mode", videoMode);
            preset = v.optString("preset", preset);
            codec = v.optString("codec", codec);
            maxKbps = v.optInt("max_bitrate_kbps", maxKbps);
            maxFps = v.optInt("max_fps", maxFps);
        }
        if (f != null) {
            uploadDir = f.optString("upload_dir", uploadDir);
            mediaScan = f.optBoolean("media_scan", mediaScan);
            overwrite = f.optBoolean("overwrite", overwrite);
            maxMb = f.optInt("max_mb", maxMb);
        }
    }
}
