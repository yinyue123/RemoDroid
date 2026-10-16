package io.remodroid;

import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.TimeUnit;
import java.util.function.Consumer;

/** shell.exec and logcat streaming (plan A.6); the server already runs as the shell user. */
final class Shell {
    private Shell() {}

    static final class Result {
        int code;
        String out, err;
        boolean truncated;
        long ms;
    }

    static Result run(String cmd, long timeoutMs) throws Exception {
        long t0 = System.nanoTime();
        Process p = new ProcessBuilder("sh", "-c", cmd).start();
        Result r = new Result();
        ByteArrayOutputStream out = new ByteArrayOutputStream(), err = new ByteArrayOutputStream();
        Thread a = pump(p.getInputStream(), out, r), b = pump(p.getErrorStream(), err, r);
        if (!p.waitFor(timeoutMs, TimeUnit.MILLISECONDS)) {
            p.destroyForcibly();
            r.code = -1;
        } else {
            r.code = p.exitValue();
        }
        a.join(500);
        b.join(500);
        r.out = out.toString("UTF-8");
        r.err = err.toString("UTF-8");
        r.ms = (System.nanoTime() - t0) / 1_000_000;
        return r;
    }

    private static Thread pump(InputStream in, ByteArrayOutputStream to, Result r) {
        Thread t = new Thread(() -> {
            byte[] b = new byte[8192];
            int n;
            try {
                while ((n = in.read(b)) > 0) {
                    if (to.size() < 1024 * 1024) to.write(b, 0, Math.min(n, 1024 * 1024 - to.size()));
                    else r.truncated = true;
                }
            } catch (Exception ignored) { }
        });
        t.start();
        return t;
    }

    static void runQuiet(String cmd) {
        try {
            new ProcessBuilder("sh", "-c", cmd).redirectErrorStream(true).start();
        } catch (Exception e) {
            Ln.w("exec failed: " + cmd, e);
        }
    }

    /** Streams logcat lines in batches (every 100 ms or 64 KB) until stopped. */
    static final class Logcat {
        private Process p;
        private volatile boolean stopped;

        Logcat(List<String> filter, String format, int tail, List<String> buffers, Consumer<List<String>> sink,
               Runnable onEnd) throws Exception {
            List<String> cmd = new ArrayList<>();
            cmd.add("logcat");
            cmd.add("-v");
            cmd.add(format == null || format.isEmpty() ? "threadtime" : format);
            if (tail > 0) {
                cmd.add("-T");
                cmd.add(String.valueOf(tail));
            }
            for (String b : buffers) {
                cmd.add("-b");
                cmd.add(b);
            }
            cmd.addAll(filter);
            p = new ProcessBuilder(cmd).redirectErrorStream(true).start();
            new Thread(() -> {
                List<String> batch = new ArrayList<>();
                long last = System.nanoTime();
                int bytes = 0;
                try (BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream()))) {
                    String line;
                    while (!stopped && (line = r.readLine()) != null) {
                        batch.add(line);
                        bytes += line.length();
                        long now = System.nanoTime();
                        if (bytes > 64 * 1024 || now - last > 100_000_000L) {
                            sink.accept(batch);
                            batch = new ArrayList<>();
                            bytes = 0;
                            last = now;
                        }
                    }
                } catch (Exception ignored) { }
                if (!batch.isEmpty()) sink.accept(batch);
                onEnd.run();
            }, "logcat").start();
        }

        void stop() {
            stopped = true;
            p.destroy();
        }
    }
}
