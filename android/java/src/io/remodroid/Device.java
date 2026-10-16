package io.remodroid;

import android.content.ClipData;
import android.graphics.Rect;
import android.os.Binder;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Parcel;
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;

import java.io.BufferedReader;
import java.io.FileReader;
import java.lang.reflect.Method;

/** The phone itself: display info, input injection, clipboard, screen power,
 *  battery. Everything goes through hidden APIs available to the shell user. */
final class Device {
    static final int POWER_MODE_OFF = 0, POWER_MODE_NORMAL = 2;
    static final String PKG = "com.android.shell";

    private final Object displayManager = Hidden.service("display", "android.hardware.display.IDisplayManager");
    private final Object inputManager = Hidden.service("input", "android.hardware.input.IInputManager");
    private final Object powerManager = Hidden.service("power", "android.os.IPowerManager");
    private final Object clipboard = Hidden.service("clipboard", "android.content.IClipboard");
    private Method injectMethod;
    private Object injectTarget;
    private Class<?> displayControl; // Android 14+

    // ---------------- display ----------------

    static final class Info {
        int width, height, rotation, layerStack;
    }

    Info displayInfo() {
        try {
            Object di = Hidden.call(displayManager, "getDisplayInfo", 0);
            Class<?> c = di.getClass();
            Info i = new Info();
            i.width = c.getDeclaredField("logicalWidth").getInt(di);
            i.height = c.getDeclaredField("logicalHeight").getInt(di);
            i.rotation = c.getDeclaredField("rotation").getInt(di);
            i.layerStack = c.getDeclaredField("layerStack").getInt(di);
            return i;
        } catch (Exception e) {
            Ln.e("cannot read the display info", e);
            Info i = new Info();
            i.width = 1080;
            i.height = 2400;
            return i;
        }
    }

    // ---------------- input ----------------

    boolean inject(InputEvent ev) {
        try {
            if (injectMethod == null) {
                injectMethod = Hidden.method(inputManager.getClass(), "injectInputEvent", 2);
                injectTarget = inputManager;
                if (injectMethod == null) { /* fall back to android.hardware.input.InputManager(Global) */
                    Class<?> im = Class.forName(Hidden.sdk() >= 34 ? "android.hardware.input.InputManagerGlobal"
                                                                   : "android.hardware.input.InputManager");
                    injectTarget = im.getMethod("getInstance").invoke(null);
                    injectMethod = im.getMethod("injectInputEvent", InputEvent.class, int.class);
                }
            }
            return (Boolean) injectMethod.invoke(injectTarget, ev, 0 /* INJECT_INPUT_EVENT_MODE_ASYNC */);
        } catch (Exception e) {
            Ln.w("input injection failed", e);
            return false;
        }
    }

    boolean key(int action, int code, int meta) {
        long now = SystemClock.uptimeMillis();
        return inject(new KeyEvent(now, now, action, code, 0, meta, KeyCharacterMap.VIRTUAL_KEYBOARD, 0, 0,
                InputDevice.SOURCE_KEYBOARD));
    }

    boolean press(int code) { return key(KeyEvent.ACTION_DOWN, code, 0) & key(KeyEvent.ACTION_UP, code, 0); }

    /** ASCII through key events; anything else via the clipboard and a paste. */
    boolean text(String s) {
        KeyEvent[] evs = KeyCharacterMap.load(KeyCharacterMap.VIRTUAL_KEYBOARD).getEvents(s.toCharArray());
        if (evs != null) {
            for (KeyEvent e : evs) inject(e);
            return true;
        }
        if (!setClipboard(s)) return false;
        return press(KeyEvent.KEYCODE_PASTE);
    }

    // ---------------- power ----------------

    boolean isInteractive() {
        try {
            return (Boolean) Hidden.call(powerManager, "isInteractive");
        } catch (Exception e) {
            return true;
        }
    }

    void wake() { if (!isInteractive()) press(KeyEvent.KEYCODE_WAKEUP); }

    void sleep() { press(KeyEvent.KEYCODE_SLEEP); }

    private IBinder displayToken() throws Exception {
        if (Hidden.sdk() >= 34) {
            if (displayControl == null) {
                /* DisplayControl lives in services.jar and needs libandroid_servers (scrcpy's approach). */
                Class<?> f = Class.forName("com.android.internal.os.ClassLoaderFactory");
                Method create = f.getDeclaredMethod("createClassLoader", String.class, String.class, String.class,
                        ClassLoader.class, int.class, boolean.class, String.class);
                ClassLoader cl = (ClassLoader) create.invoke(null, "/system/framework/services.jar", null, null,
                        ClassLoader.getSystemClassLoader(), 0, true, null);
                displayControl = cl.loadClass("com.android.server.display.DisplayControl");
                Method load = Runtime.class.getDeclaredMethod("loadLibrary0", Class.class, String.class);
                load.setAccessible(true);
                load.invoke(Runtime.getRuntime(), displayControl, "android_servers");
            }
            long[] ids = (long[]) displayControl.getMethod("getPhysicalDisplayIds").invoke(null);
            return (IBinder) displayControl.getMethod("getPhysicalDisplayToken", long.class).invoke(null, ids[0]);
        }
        Class<?> sc = Class.forName("android.view.SurfaceControl");
        if (Hidden.sdk() >= 29) {
            try {
                return (IBinder) sc.getMethod("getInternalDisplayToken").invoke(null);
            } catch (NoSuchMethodException e) {
                long[] ids = (long[]) sc.getMethod("getPhysicalDisplayIds").invoke(null);
                return (IBinder) sc.getMethod("getPhysicalDisplayToken", long.class).invoke(null, ids[0]);
            }
        }
        return (IBinder) sc.getMethod("getBuiltInDisplay", int.class).invoke(null, 0);
    }

    /** Turns the physical panel on/off; capture keeps running (plan §4.9). */
    boolean setPanel(boolean on) {
        try {
            Class<?> sc = Class.forName("android.view.SurfaceControl");
            sc.getMethod("setDisplayPowerMode", IBinder.class, int.class)
                    .invoke(null, displayToken(), on ? POWER_MODE_NORMAL : POWER_MODE_OFF);
            Ln.i("panel " + (on ? "on" : "off"));
            return true;
        } catch (Exception e) {
            Ln.w("cannot change the display power mode", e);
            return false;
        }
    }

    // ---------------- clipboard ----------------

    String getClipboard() {
        try {
            Method m;
            Object clip;
            int sdk = Hidden.sdk();
            if ((m = Hidden.method(clipboard.getClass(), "getPrimaryClip", 4)) != null)
                clip = m.invoke(clipboard, PKG, null, 0, 0);
            else if ((m = Hidden.method(clipboard.getClass(), "getPrimaryClip", 3)) != null)
                clip = m.invoke(clipboard, PKG, null, 0);
            else if ((m = Hidden.method(clipboard.getClass(), "getPrimaryClip", 2)) != null)
                clip = m.invoke(clipboard, PKG, 0);
            else
                clip = Hidden.call(clipboard, "getPrimaryClip", PKG);
            if (!(clip instanceof ClipData) || ((ClipData) clip).getItemCount() == 0) return null;
            CharSequence t = ((ClipData) clip).getItemAt(0).getText();
            return t == null ? null : t.toString();
        } catch (Exception e) {
            Ln.w("cannot read the clipboard", e);
            return null;
        }
    }

    boolean setClipboard(String text) {
        try {
            ClipData clip = ClipData.newPlainText(null, text);
            Method m;
            if ((m = Hidden.method(clipboard.getClass(), "setPrimaryClip", 5)) != null)
                m.invoke(clipboard, clip, PKG, null, 0, 0);
            else if ((m = Hidden.method(clipboard.getClass(), "setPrimaryClip", 4)) != null)
                m.invoke(clipboard, clip, PKG, null, 0);
            else if ((m = Hidden.method(clipboard.getClass(), "setPrimaryClip", 3)) != null)
                m.invoke(clipboard, clip, PKG, 0);
            else
                Hidden.call(clipboard, "setPrimaryClip", clip, PKG);
            return true;
        } catch (Exception e) {
            Ln.w("cannot set the clipboard", e);
            return false;
        }
    }

    /** Registers a clipboard-change listener without compiling against the hidden
     *  AIDL interface: our Binder answers the single one-way callback. */
    boolean watchClipboard(Runnable onChange) {
        try {
            Binder b = new ClipListener(onChange);
            Class<?> stub = Class.forName("android.content.IOnPrimaryClipChangedListener$Stub");
            IInterface listener = (IInterface) stub.getMethod("asInterface", IBinder.class).invoke(null, b);
            Method m;
            if ((m = Hidden.method(clipboard.getClass(), "addPrimaryClipChangedListener", 5)) != null)
                m.invoke(clipboard, listener, PKG, null, 0, 0);
            else if ((m = Hidden.method(clipboard.getClass(), "addPrimaryClipChangedListener", 4)) != null)
                m.invoke(clipboard, listener, PKG, null, 0);
            else if ((m = Hidden.method(clipboard.getClass(), "addPrimaryClipChangedListener", 3)) != null)
                m.invoke(clipboard, listener, PKG, 0);
            else
                Hidden.call(clipboard, "addPrimaryClipChangedListener", listener, PKG);
            return true;
        } catch (Exception e) {
            Ln.w("cannot watch the clipboard", e);
            return false;
        }
    }

    /** Receives IOnPrimaryClipChangedListener.dispatchPrimaryClipChanged(). */
    private static final class ClipListener extends Binder {
        private final Runnable onChange;

        ClipListener(Runnable onChange) { this.onChange = onChange; }

        @Override
        protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) {
            if (code == IBinder.FIRST_CALL_TRANSACTION) {
                onChange.run();
                return true;
            }
            return false;
        }
    }

    // ---------------- battery ----------------

    private static String read(String path) {
        try (BufferedReader r = new BufferedReader(new FileReader(path))) {
            return r.readLine();
        } catch (Exception e) {
            return null;
        }
    }

    int batteryLevel() {
        String s = read("/sys/class/power_supply/battery/capacity");
        return s == null ? -1 : Integer.parseInt(s.trim());
    }

    boolean charging() {
        String s = read("/sys/class/power_supply/battery/status");
        return s != null && (s.startsWith("Charging") || s.startsWith("Full"));
    }

    double batteryTemp() {
        String s = read("/sys/class/power_supply/battery/temp");
        return s == null ? -1 : Integer.parseInt(s.trim()) / 10.0;
    }

    static Rect rect(int w, int h) { return new Rect(0, 0, w, h); }
}
