package io.remodroid;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.PixelFormat;
import android.media.Image;
import android.media.ImageReader;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.SystemClock;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;

/** Screenshots for the API (skill-plan.md §6). A mirror display created at the output
 *  size feeds an ImageReader, so the compositor does the scaling on the GPU; the newest
 *  frame is kept and each request compresses it in C (Native.encodeImage). The display
 *  stays up for WARM_MS after the last request. Nothing is written to disk. */
final class Capture {
    static final int JPEG = 0, PNG = 1, WEBP = 3;
    private static final long WARM_MS = 10_000;

    static final class Shot {
        byte[] data;
        String type, backend;
        int width, height;
        Device.Info display;
        long frameAgeMs, encodeMs;
    }

    private final Device device;
    private final Handler handler;
    private final ScheduledExecutorService sched = Executors.newSingleThreadScheduledExecutor();
    private final DisplayMirror mirror = new DisplayMirror();
    private ImageReader reader;
    private Image latest;        // newest frame; guarded by this
    private long latestAt;
    private int width, height, dw, dh, rotation;
    private ScheduledFuture<?> idle;
    private long mirrorBrokenUntil;  // after a mirror failure, use screencap for a while

    Capture(Device device) {
        this.device = device;
        HandlerThread t = new HandlerThread("capture");
        t.start();
        handler = new Handler(t.getLooper());
    }

    static String mime(int format) {
        return format == PNG ? "image/png" : format == WEBP ? "image/webp" : "image/jpeg";
    }

    /** Output size: the display's aspect ratio with the long edge at most maxEdge (even numbers). */
    static int[] outSize(Device.Info di, int maxEdge) {
        int longSide = Math.max(di.width, di.height);
        double scale = Math.min(1.0, (double) maxEdge / longSide);
        int w = Math.max(2, (int) Math.round(di.width * scale) & ~1);
        int h = Math.max(2, (int) Math.round(di.height * scale) & ~1);
        return new int[] { w, h };
    }

    synchronized Shot take(int maxEdge, int format, int quality, long timeoutMs) throws ApiServer.Fault {
        Device.Info di = device.displayInfo();
        int[] size = outSize(di, maxEdge);
        if (idle != null) idle.cancel(false);
        idle = sched.schedule(this::closeIdle, WARM_MS, TimeUnit.MILLISECONDS);
        if (SystemClock.uptimeMillis() >= mirrorBrokenUntil) {
            try {
                return fromMirror(di, size[0], size[1], format, quality, timeoutMs);
            } catch (ApiServer.Fault f) {
                throw f;
            } catch (Exception e) {
                Ln.w("mirror capture failed; using screencap for a minute", e);
                close();
                mirrorBrokenUntil = SystemClock.uptimeMillis() + 60_000;
            }
        }
        return fromScreencap(di, size[0], size[1], format, quality, timeoutMs);
    }

    private Shot fromMirror(Device.Info di, int w, int h, int format, int quality, long timeoutMs) throws Exception {
        if (reader == null || w != width || h != height || di.width != dw || di.height != dh || di.rotation != rotation) {
            close();
            open(di, w, h);
        }
        long end = SystemClock.uptimeMillis() + timeoutMs;
        while (latest == null) {
            long left = end - SystemClock.uptimeMillis();
            if (left <= 0) throw new ApiServer.Fault(503, "capture_unavailable", "no frame from the display", true);
            try { wait(Math.min(left, 50)); } catch (InterruptedException e) { throw new ApiServer.Fault(503, "capture_unavailable", "interrupted", true); }
        }
        Shot s = new Shot();
        s.display = di;
        s.width = w;
        s.height = h;
        s.type = mime(format);
        s.frameAgeMs = SystemClock.uptimeMillis() - latestAt;
        long t0 = System.nanoTime();
        Image.Plane p = latest.getPlanes()[0];
        ByteBuffer px = p.getBuffer();
        int stride = p.getRowStride();
        if (Native.hasImageEncoder()) {
            s.data = Native.encodeImage(px, w, h, stride, format, quality);
            s.backend = "mirror+c";
        }
        if (s.data == null) {
            s.data = javaEncode(px, w, h, stride, format, quality);
            s.backend = "mirror+java";
        }
        s.encodeMs = (System.nanoTime() - t0) / 1_000_000;
        return s;
    }

    private void open(Device.Info di, int w, int h) throws Exception {
        reader = ImageReader.newInstance(w, h, PixelFormat.RGBA_8888, 3);
        final ImageReader r = reader;
        r.setOnImageAvailableListener(ir -> onFrame(r), handler);
        width = w;
        height = h;
        dw = di.width;
        dh = di.height;
        rotation = di.rotation;
        mirror.create("remodroid-shot", di, r.getSurface(), w, h);
        Ln.i("screenshot display " + w + "x" + h);
    }

    private void onFrame(ImageReader r) {
        Image img;
        try {
            img = r.acquireLatestImage();
        } catch (IllegalStateException e) {
            return;
        }
        if (img == null) return;
        synchronized (this) {
            if (r != reader) {
                img.close();
                return;
            }
            if (latest != null) latest.close();
            latest = img;
            latestAt = SystemClock.uptimeMillis();
            notifyAll();
        }
    }

    private synchronized void closeIdle() {
        if (reader != null) Ln.d("screenshot display idle; releasing it");
        close();
    }

    private void close() {
        mirror.release();
        if (latest != null) latest.close();
        latest = null;
        if (reader != null) reader.close();
        reader = null;
    }

    // ---------------- fallbacks ----------------

    private static byte[] javaEncode(ByteBuffer px, int w, int h, int stride, int format, int quality) {
        Bitmap full = Bitmap.createBitmap(stride / 4, h, Bitmap.Config.ARGB_8888);
        px.rewind();
        full.copyPixelsFromBuffer(px);
        Bitmap b = full.getWidth() == w ? full : Bitmap.createBitmap(full, 0, 0, w, h);
        byte[] out = compress(b, format, quality);
        if (b != full) b.recycle();
        full.recycle();
        return out;
    }

    @SuppressWarnings("deprecation")
    private static byte[] compress(Bitmap b, int format, int quality) {
        Bitmap.CompressFormat f = format == PNG ? Bitmap.CompressFormat.PNG
                : format == WEBP ? Bitmap.CompressFormat.WEBP : Bitmap.CompressFormat.JPEG;
        ByteArrayOutputStream out = new ByteArrayOutputStream(256 * 1024);
        b.compress(f, quality, out);
        return out.toByteArray();
    }

    /** Last resort: `screencap -p` through a pipe (no file), scaled and re-encoded in memory. */
    private static Shot fromScreencap(Device.Info di, int w, int h, int format, int quality, long timeoutMs)
            throws ApiServer.Fault {
        try {
            Process p = new ProcessBuilder("screencap", "-p").redirectErrorStream(false).start();
            ByteArrayOutputStream png = new ByteArrayOutputStream(1 << 20);
            byte[] buf = new byte[65536];
            int n;
            long end = SystemClock.uptimeMillis() + timeoutMs;
            while ((n = p.getInputStream().read(buf)) > 0) {
                png.write(buf, 0, n);
                if (SystemClock.uptimeMillis() > end) { p.destroy(); break; }
            }
            p.waitFor(1, TimeUnit.SECONDS);
            Bitmap src = BitmapFactory.decodeByteArray(png.toByteArray(), 0, png.size());
            if (src == null) throw new ApiServer.Fault(503, "capture_unavailable", "screencap returned no image", true);
            long t0 = System.nanoTime();
            Bitmap b = src.getWidth() == w && src.getHeight() == h ? src : Bitmap.createScaledBitmap(src, w, h, true);
            Shot s = new Shot();
            s.display = di;
            s.width = b.getWidth();
            s.height = b.getHeight();
            s.type = mime(format);
            s.data = compress(b, format, quality);
            s.backend = "screencap";
            s.encodeMs = (System.nanoTime() - t0) / 1_000_000;
            if (b != src) b.recycle();
            src.recycle();
            return s;
        } catch (ApiServer.Fault f) {
            throw f;
        } catch (Exception e) {
            throw new ApiServer.Fault(503, "capture_unavailable", "screencap failed: " + e.getMessage(), true);
        }
    }
}
