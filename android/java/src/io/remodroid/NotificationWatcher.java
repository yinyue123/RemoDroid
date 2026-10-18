package io.remodroid;

import org.json.JSONObject;

import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.function.Consumer;

/** Watches the phone's notifications and reports each new one (notifications feature).
 *
 *  The shell user has no notification-listener stream, but `dumpsys notification --noredact`
 *  shows current notifications with content, so this polls it and diffs. Each new notification
 *  becomes a {@code {kind:"notification"}} event the agent forwards to the server (Bark) and to
 *  connected viewers (WebSocket). Filtering ("all but noise") happens here so quiet/ongoing
 *  notifications never leave the phone. */
final class NotificationWatcher {
    private static final int MAX_SEEN = 1000, MAX_PER_POLL = 25, MAX_LEN = 500;

    private final Config cfg;
    private final Consumer<JSONObject> emit;
    private final ScheduledExecutorService sched = Executors.newSingleThreadScheduledExecutor(r -> {
        Thread t = new Thread(r, "notify");
        t.setDaemon(true);
        return t;
    });
    /* key -> signature of the last version we reported; insertion-ordered so we can cap it */
    private final LinkedHashMap<String, String> seen = new LinkedHashMap<String, String>(16, 0.75f, false) {
        protected boolean removeEldestEntry(Map.Entry<String, String> e) { return size() > MAX_SEEN; }
    };
    private boolean primed;
    private int interval;
    private ScheduledFuture<?> task;

    NotificationWatcher(Config cfg, Consumer<JSONObject> emit) {
        this.cfg = cfg;
        this.emit = emit;
    }

    /** (Re)applies the pushed [notify] config; safe to call whenever config changes. */
    synchronized void reconfigure() {
        int want = cfg.notifyEnabled ? Math.max(1, cfg.notifyIntervalSeconds) : 0;
        if (want == interval)
            return;
        interval = want;
        if (task != null) {
            task.cancel(false);
            task = null;
        }
        if (interval > 0) {
            Ln.i("notifications: watching every " + interval + "s (min importance " + cfg.notifyMinImportance + ")");
            task = sched.scheduleWithFixedDelay(this::poll, 0, interval, TimeUnit.SECONDS);
        } else {
            Ln.i("notifications: off");
        }
    }

    private void poll() {
        try {
            Shell.Result r = Shell.run("dumpsys notification --noredact", 8000);
            if (r.code != 0 || r.out == null || r.out.isEmpty())
                return;
            Set<String> ignore = split(cfg.notifyIgnore);
            List<JSONObject> fresh = new ArrayList<>();
            for (Rec rec : parse(r.out)) {
                if (rec.key == null || rec.pkg == null)
                    continue;
                if ((rec.flags & 0x42) != 0)                    // FLAG_ONGOING_EVENT | FLAG_FOREGROUND_SERVICE
                    continue;
                if (rec.importance < cfg.notifyMinImportance)   // NONE0 MIN1 LOW2 DEFAULT3 HIGH4
                    continue;
                if (ignore.contains(rec.pkg))
                    continue;
                if ((rec.title == null || rec.title.isEmpty()) && (rec.text == null || rec.text.isEmpty()))
                    continue;
                String sig = rec.when + "\u0000" + rec.title + "\u0000" + rec.text;
                if (sig.equals(seen.put(rec.key, sig)))         // same version already reported
                    continue;
                if (primed)
                    fresh.add(rec.json());
            }
            if (!primed) {
                primed = true;                                   // first poll only seeds the baseline
                return;
            }
            for (int i = 0; i < fresh.size() && i < MAX_PER_POLL; i++)
                emit.accept(fresh.get(i));
            if (fresh.size() > MAX_PER_POLL)
                Ln.w("notifications: " + fresh.size() + " at once; forwarded " + MAX_PER_POLL);
        } catch (Exception e) {
            Ln.w("notifications: poll failed", e);
        }
    }

    // ---------------- parsing dumpsys notification --noredact ----------------

    static final class Rec {
        String pkg, key, title, text;
        long when;
        int importance = 3, flags;

        JSONObject json() {
            try {
                return new JSONObject().put("pkg", pkg).put("title", title == null ? "" : title)
                        .put("text", text == null ? "" : text).put("key", key).put("when", when)
                        .put("importance", importance);
            } catch (Exception e) {
                return new JSONObject();
            }
        }
    }

    /** Each "NotificationRecord(...)" block becomes a Rec; robust to field order and extra lines. */
    static List<Rec> parse(String dump) {
        List<Rec> out = new ArrayList<>();
        Rec cur = null;
        for (String line : dump.split("\n")) {
            String t = line.trim();
            if (t.startsWith("NotificationRecord(")) {
                if (cur != null)
                    out.add(cur);
                cur = new Rec();
                cur.pkg = field(t, "pkg=");
                String imp = field(t, "importance=");           // numeric on the record line
                if (imp != null)
                    try { cur.importance = Integer.parseInt(imp); } catch (NumberFormatException ignored) { }
                cur.flags = hexField(t, "flags=0x");            // inside Notification(... flags=0x.. )
                continue;
            }
            if (cur == null)
                continue;
            if (t.startsWith("key=")) {
                cur.key = t.substring(4).trim();
            } else if (t.startsWith("flags=0x")) {
                cur.flags |= hexField(t, "flags=0x");
            } else if (t.contains("when=") && cur.when == 0) {
                String w = field(t, "when=");
                if (w != null)
                    try { cur.when = Long.parseLong(w); } catch (NumberFormatException ignored) { }
            } else if (t.startsWith("android.title=") && cur.title == null) {
                cur.title = extra(t.substring("android.title=".length()));
            } else if ((t.startsWith("android.text=") || t.startsWith("android.bigText=")) && cur.text == null) {
                cur.text = extra(t.substring(t.indexOf('=') + 1));
            }
        }
        if (cur != null)
            out.add(cur);
        return out;
    }

    /** A space-terminated token after `name` on a line (e.g. pkg=, importance=). */
    private static String field(String line, String name) {
        int i = line.indexOf(name);
        if (i < 0)
            return null;
        int s = i + name.length(), e = s;
        while (e < line.length() && line.charAt(e) != ' ' && line.charAt(e) != ')')
            e++;
        return e > s ? line.substring(s, e) : null;
    }

    private static int hexField(String line, String name) {
        int i = line.indexOf(name);
        if (i < 0)
            return 0;
        int s = i + name.length(), e = s;
        while (e < line.length() && isHex(line.charAt(e)))
            e++;
        try { return e > s ? (int) Long.parseLong(line.substring(s, e), 16) : 0; } catch (NumberFormatException x) { return 0; }
    }

    private static boolean isHex(char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }

    /** An extras value: "String (the text)" -> "the text"; "null" -> null. */
    private static String extra(String v) {
        v = v.trim();
        if (v.isEmpty() || v.equals("null") || v.startsWith("null "))
            return null;
        int lp = v.indexOf('(');              // "<Type> (value)"; the ) may be on a later (dropped) line
        if (lp >= 0) {
            v = v.substring(lp + 1);
            if (v.endsWith(")"))
                v = v.substring(0, v.length() - 1);
        }
        v = v.trim();
        return v.length() > MAX_LEN ? v.substring(0, MAX_LEN) + "…" : v;
    }

    private static Set<String> split(String csv) {
        java.util.HashSet<String> set = new java.util.HashSet<>();
        if (csv != null)
            for (String p : csv.split(","))
                if (!p.trim().isEmpty())
                    set.add(p.trim());
        return Collections.unmodifiableSet(set);
    }
}
