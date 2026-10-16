package io.remodroid;

import android.graphics.Rect;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.os.Bundle;
import android.os.IBinder;
import android.view.Surface;

import java.lang.reflect.Method;
import java.nio.ByteBuffer;

/** Mirrors the main display into a hardware MediaCodec encoder and hands every
 *  access unit to the native WebRTC stack (plan §4.1, §5.5, §6). */
final class ScreenEncoder {
    interface Listener {
        void onFirstFrame();
        void onSizeChanged(int w, int h, int rotation);
        void onFrameStats(double fps, double encodeMs);
    }

    private final Device device;
    private final Listener listener;
    private volatile boolean running;
    private Thread thread;
    private MediaCodec codec;
    private Object virtualDisplay;   // android.hardware.display.VirtualDisplay (Android 14+)
    private IBinder scDisplay;       // SurfaceControl display (older releases)
    private Surface surface;
    private volatile boolean restart;
    private volatile int bitrateKbps;
    private String mime = MediaFormat.MIMETYPE_VIDEO_HEVC;
    private String profile = "main";
    private int shortSide = 720, fps = 60;
    int width, height, rotation;

    ScreenEncoder(Device device, Listener listener) {
        this.device = device;
        this.listener = listener;
    }

    /** True when the phone has a hardware encoder for H.265 (decides the SDP offer). */
    static boolean hasHardwareHevc() {
        return pickEncoder(MediaFormat.MIMETYPE_VIDEO_HEVC) != null;
    }

    static String pickEncoder(String mime) {
        MediaCodecList list = new MediaCodecList(MediaCodecList.REGULAR_CODECS);
        for (MediaCodecInfo i : list.getCodecInfos()) {
            if (!i.isEncoder()) continue;
            boolean hw = Hidden.sdk() >= 29 ? i.isHardwareAccelerated()
                    : !(i.getName().startsWith("OMX.google.") || i.getName().startsWith("c2.android."));
            if (!hw) continue;
            for (String t : i.getSupportedTypes())
                if (t.equalsIgnoreCase(mime)) return i.getName();
        }
        return null;
    }

    synchronized void configure(String codecName, String profile, int shortSide, int fps, int kbps) {
        this.mime = "h265".equals(codecName) ? MediaFormat.MIMETYPE_VIDEO_HEVC : MediaFormat.MIMETYPE_VIDEO_AVC;
        this.profile = profile == null ? "main" : profile;
        this.shortSide = shortSide;
        this.fps = fps;
        this.bitrateKbps = kbps;
        restart = true;
        if (codec != null) codec.signalEndOfInputStream();
    }

    void start() {
        running = true;
        thread = new Thread(this::loop, "encoder");
        thread.start();
    }

    void stop() {
        running = false;
        MediaCodec c = codec;
        if (c != null) {
            try { c.signalEndOfInputStream(); } catch (Exception ignored) { }
        }
        if (thread != null) {
            try { thread.join(2000); } catch (InterruptedException ignored) { }
        }
    }

    void setBitrate(int kbps) {
        bitrateKbps = kbps;
        MediaCodec c = codec;
        if (c == null) return;
        try {
            Bundle b = new Bundle();
            b.putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, kbps * 1000);
            c.setParameters(b);
        } catch (Exception e) {
            Ln.w("cannot change the bitrate", e);
        }
    }

    void requestKeyframe() {
        MediaCodec c = codec;
        if (c == null) return;
        try {
            Bundle b = new Bundle();
            b.putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0);
            c.setParameters(b);
        } catch (Exception e) {
            Ln.w("cannot request a keyframe", e);
        }
    }

    private void computeSize(Device.Info di) {
        int w = di.width, h = di.height;
        boolean portrait = h >= w;
        int s = Math.min(shortSide, Math.min(w, h));
        int l = Math.round((float) Math.max(w, h) * s / Math.min(w, h));
        s &= ~15; /* hardware encoders want multiples of 16 */
        l &= ~15;
        width = portrait ? s : l;
        height = portrait ? l : s;
        rotation = di.rotation;
    }

    private MediaFormat format() {
        MediaFormat f = MediaFormat.createVideoFormat(mime, width, height);
        f.setInteger(MediaFormat.KEY_BIT_RATE, bitrateKbps * 1000);
        f.setInteger(MediaFormat.KEY_FRAME_RATE, fps);
        f.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
        f.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 10);           /* keyframes mostly on request (PLI) */
        f.setLong(MediaFormat.KEY_REPEAT_PREVIOUS_FRAME_AFTER, 100_000); /* keep frames flowing on a still screen */
        f.setInteger(MediaFormat.KEY_PRIORITY, 0);                      /* real time */
        if (Hidden.sdk() >= 30) f.setInteger(MediaFormat.KEY_LATENCY, 1);
        if (Hidden.sdk() >= 29) f.setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0);
        f.setFloat("max-fps-to-encoder", fps);
        if (mime.equals(MediaFormat.MIMETYPE_VIDEO_HEVC)) {
            f.setInteger(MediaFormat.KEY_PROFILE, MediaCodecInfo.CodecProfileLevel.HEVCProfileMain);
            f.setInteger(MediaFormat.KEY_LEVEL, MediaCodecInfo.CodecProfileLevel.HEVCMainTierLevel41);
        } else if ("high".equals(profile)) {
            f.setInteger(MediaFormat.KEY_PROFILE, Hidden.sdk() >= 27
                    ? MediaCodecInfo.CodecProfileLevel.AVCProfileConstrainedHigh : MediaCodecInfo.CodecProfileLevel.AVCProfileHigh);
            f.setInteger(MediaFormat.KEY_LEVEL, MediaCodecInfo.CodecProfileLevel.AVCLevel42);
        } else {
            f.setInteger(MediaFormat.KEY_PROFILE, Hidden.sdk() >= 27
                    ? MediaCodecInfo.CodecProfileLevel.AVCProfileConstrainedBaseline : MediaCodecInfo.CodecProfileLevel.AVCProfileBaseline);
            f.setInteger(MediaFormat.KEY_LEVEL, MediaCodecInfo.CodecProfileLevel.AVCLevel42);
        }
        return f;
    }

    private void createDisplay(Device.Info di) throws Exception {
        if (Hidden.sdk() >= 34) {
            Class<?> dm = Class.forName("android.hardware.display.DisplayManager");
            Method m = dm.getMethod("createVirtualDisplay", String.class, int.class, int.class, int.class, Surface.class);
            virtualDisplay = m.invoke(null, "remodroid", width, height, 0, surface);
            return;
        }
        Class<?> sc = Class.forName("android.view.SurfaceControl");
        boolean secure = Hidden.sdk() < 30 || (Hidden.sdk() == 30 && !"S".equals(android.os.Build.VERSION.CODENAME));
        scDisplay = (IBinder) sc.getMethod("createDisplay", String.class, boolean.class).invoke(null, "remodroid", secure);
        sc.getMethod("openTransaction").invoke(null);
        try {
            sc.getMethod("setDisplaySurface", IBinder.class, Surface.class).invoke(null, scDisplay, surface);
            sc.getMethod("setDisplayProjection", IBinder.class, int.class, Rect.class, Rect.class)
                    .invoke(null, scDisplay, 0, new Rect(0, 0, di.width, di.height), new Rect(0, 0, width, height));
            sc.getMethod("setDisplayLayerStack", IBinder.class, int.class).invoke(null, scDisplay, di.layerStack);
        } finally {
            sc.getMethod("closeTransaction").invoke(null);
        }
    }

    private void destroyDisplay() {
        try {
            if (virtualDisplay != null) virtualDisplay.getClass().getMethod("release").invoke(virtualDisplay);
            if (scDisplay != null)
                Class.forName("android.view.SurfaceControl").getMethod("destroyDisplay", IBinder.class).invoke(null, scDisplay);
        } catch (Exception e) {
            Ln.w("cannot release the virtual display", e);
        }
        virtualDisplay = null;
        scDisplay = null;
    }

    private void loop() {
        boolean first = true;
        while (running) {
            Device.Info di = device.displayInfo();
            synchronized (this) {
                restart = false;
                computeSize(di);
            }
            String name = pickEncoder(mime);
            try {
                codec = name != null ? MediaCodec.createByCodecName(name) : MediaCodec.createEncoderByType(mime);
                codec.configure(format(), null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
                surface = codec.createInputSurface();
                createDisplay(di);
                codec.start();
                Ln.i("encoding " + mime + " " + profile + " " + width + "x" + height + "@" + fps + " "
                        + bitrateKbps + " kbps on " + codec.getName());
                listener.onSizeChanged(width, height, rotation);
                first = drain(di, first);
            } catch (Exception e) {
                Ln.e("encoder failed", e);
                try { Thread.sleep(500); } catch (InterruptedException ignored) { }
            } finally {
                destroyDisplay();
                if (codec != null) {
                    try { codec.stop(); } catch (Exception ignored) { }
                    codec.release();
                    codec = null;
                }
                if (surface != null) surface.release();
                surface = null;
            }
        }
    }

    /** Output loop; returns when the encoder must be rebuilt (stop, size or rotation change). */
    private boolean drain(Device.Info started, boolean first) {
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        byte[] csd = null;
        long statsAt = System.nanoTime();
        int frames = 0;
        long checkAt = 0;
        while (running && !restart) {
            int i = codec.dequeueOutputBuffer(info, 50_000);
            long now = System.nanoTime();
            if (now - checkAt > 500_000_000L) { /* rotation / resolution changes */
                checkAt = now;
                Device.Info di = device.displayInfo();
                if (di.width != started.width || di.height != started.height || di.rotation != started.rotation) {
                    Ln.i("display changed: " + di.width + "x" + di.height + " rotation " + di.rotation);
                    return first;
                }
            }
            if (i < 0) continue;
            try {
                if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) return first;
                ByteBuffer buf = codec.getOutputBuffer(i);
                if ((info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0) {
                    csd = new byte[info.size];
                    buf.position(info.offset);
                    buf.get(csd);
                    continue;
                }
                if (info.size == 0) continue;
                boolean key = (info.flags & MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0;
                /* parameter sets go in front of every keyframe so the viewer can resync */
                Native.video(buf, info.offset, info.size, key ? csd : null, info.presentationTimeUs, key);
                frames++;
                if (first) {
                    first = false;
                    listener.onFirstFrame();
                }
            } finally {
                codec.releaseOutputBuffer(i, false);
            }
            if (now - statsAt >= 1_000_000_000L) {
                listener.onFrameStats(frames * 1e9 / (now - statsAt), 0);
                frames = 0;
                statsAt = now;
            }
        }
        return first;
    }
}
