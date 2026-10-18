package io.remodroid;

import java.nio.ByteBuffer;

/** JNI bridge to libremodroid.so (android/native/jni.c). Callbacks arrive on
 *  the native thread; keep them short and hand work to other threads. */
public final class Native {
    private Native() {}

    interface Listener {
        void onCtrl(String json);
        void onLink(boolean up);
        void onSession(String sid, int state, String codec, String profile, String quality, String reason, boolean audio);
        void onChannelOpen(String label);
        void onMessage(String label, boolean binary, byte[] data);
        void onKeyframe();
        void onBitrate(int kbps);
    }

    public static final int SESSION_CONNECTING = 0, SESSION_CONNECTED = 1, SESSION_ENDED = 2;

    static volatile Listener listener;

    static native boolean start(String listen, boolean direct, boolean h265, boolean audio, int maxKbps, int logLevel);
    static native void sendCtrl(String json);
    static native void dcSend(String label, boolean binary, byte[] data);
    static native void dcOpen(String label);
    /** One encoded access unit from MediaCodec; prefix = parameter sets for keyframes (or null). */
    static native void video(ByteBuffer buf, int offset, int size, byte[] prefix, long ptsUs, boolean keyframe);
    /** One Opus packet from MediaCodec (AudioCapture). */
    static native void audio(ByteBuffer buf, int offset, int size, long ptsUs);
    static native void endSession(String reason);
    static native void setMaxKbps(int kbps);
    static native void setDirect(boolean direct);
    static native String stats();
    /** True when the C screenshot encoder (AndroidBitmap_compress, Android 11+) is available. */
    static native boolean hasImageEncoder();
    /** Compresses RGBA_8888 pixels in C; format 0 = JPEG, 1 = PNG, 3 = WebP. Null on failure. */
    static native byte[] encodeImage(ByteBuffer rgba, int width, int height, int rowStride, int format, int quality);

    // ---- called from native ----
    static void onCtrl(String json) { Listener l = listener; if (l != null) l.onCtrl(json); }
    static void onLink(boolean up) { Listener l = listener; if (l != null) l.onLink(up); }
    static void onSession(String sid, int state, String codec, String profile, String quality, String reason, boolean audio) {
        Listener l = listener; if (l != null) l.onSession(sid, state, codec, profile, quality, reason, audio);
    }
    static void onChannelOpen(String label) { Listener l = listener; if (l != null) l.onChannelOpen(label); }
    static void onMessage(String label, boolean binary, byte[] data) { Listener l = listener; if (l != null) l.onMessage(label, binary, data); }
    static void onKeyframe() { Listener l = listener; if (l != null) l.onKeyframe(); }
    static void onBitrate(int kbps) { Listener l = listener; if (l != null) l.onBitrate(kbps); }
}
