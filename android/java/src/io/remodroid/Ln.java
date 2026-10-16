package io.remodroid;

/** Logs to stderr; the router agent collects it (diag / server_stderr). */
public final class Ln {
    public static final int ERROR = 0, WARN = 1, INFO = 2, DEBUG = 3;
    static int level = INFO;

    private Ln() {}

    static int parse(String s) {
        if (s == null) return INFO;
        switch (s) {
            case "error": return ERROR;
            case "warn": return WARN;
            case "debug": case "trace": return DEBUG;
            default: return INFO;
        }
    }

    private static void log(int lvl, char tag, String msg, Throwable t) {
        if (lvl > level) return;
        System.err.println("[remodroid] " + tag + " " + msg);
        if (t != null && level >= DEBUG) t.printStackTrace();
        else if (t != null) System.err.println("[remodroid] " + tag + "   " + t);
    }

    public static void e(String m) { log(ERROR, 'E', m, null); }
    public static void e(String m, Throwable t) { log(ERROR, 'E', m, t); }
    public static void w(String m) { log(WARN, 'W', m, null); }
    public static void w(String m, Throwable t) { log(WARN, 'W', m, t); }
    public static void i(String m) { log(INFO, 'I', m, null); }
    public static void d(String m) { log(DEBUG, 'D', m, null); }
}
