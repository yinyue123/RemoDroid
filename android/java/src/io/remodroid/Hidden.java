package io.remodroid;

import android.os.Build;
import android.os.IBinder;

import java.lang.reflect.Method;

/** Access to hidden system services through reflection (scrcpy is the
 *  reference for which signatures exist on which Android version). */
final class Hidden {
    private Hidden() {}

    static Object service(String name, String iface) {
        try {
            Method get = Class.forName("android.os.ServiceManager").getDeclaredMethod("getService", String.class);
            IBinder b = (IBinder) get.invoke(null, name);
            if (b == null) return null;
            Method asInterface = Class.forName(iface + "$Stub").getMethod("asInterface", IBinder.class);
            return asInterface.invoke(null, b);
        } catch (Exception e) {
            Ln.w("service " + name + " unavailable", e);
            return null;
        }
    }

    /** Finds a public method by name and parameter count (signatures vary between releases). */
    static Method method(Class<?> c, String name, int params) {
        for (Method m : c.getMethods())
            if (m.getName().equals(name) && m.getParameterTypes().length == params) return m;
        return null;
    }

    static Object call(Object target, String name, Object... args) throws Exception {
        Method m = method(target instanceof Class ? (Class<?>) target : target.getClass(), name, args.length);
        if (m == null) throw new NoSuchMethodException(name + "/" + args.length);
        return m.invoke(target instanceof Class ? null : target, args);
    }

    static int sdk() { return Build.VERSION.SDK_INT; }
}
