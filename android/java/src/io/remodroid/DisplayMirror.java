package io.remodroid;

import android.graphics.Rect;
import android.os.IBinder;
import android.view.Surface;

import java.lang.reflect.Method;

/** A virtual display that mirrors the main display into a Surface, scaled by the
 *  compositor to the Surface's size. Shared by the video encoder (ScreenEncoder)
 *  and screenshots (Capture). */
final class DisplayMirror {
    private Object virtualDisplay;   // android.hardware.display.VirtualDisplay (Android 14+)
    private IBinder scDisplay;       // SurfaceControl display (older releases)

    void create(String name, Device.Info di, Surface surface, int width, int height) throws Exception {
        if (Hidden.sdk() >= 34) {
            Class<?> dm = Class.forName("android.hardware.display.DisplayManager");
            Method m = dm.getMethod("createVirtualDisplay", String.class, int.class, int.class, int.class, Surface.class);
            virtualDisplay = m.invoke(null, name, width, height, 0, surface);
            return;
        }
        Class<?> sc = Class.forName("android.view.SurfaceControl");
        boolean secure = Hidden.sdk() < 30 || (Hidden.sdk() == 30 && !"S".equals(android.os.Build.VERSION.CODENAME));
        scDisplay = (IBinder) sc.getMethod("createDisplay", String.class, boolean.class).invoke(null, name, secure);
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

    void release() {
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
}
