package io.remodroid;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.security.MessageDigest;
import java.util.HashMap;
import java.util.Map;

/** File control (A.6 file.*) and data (A.8 binary chunks: u32 xfer | u32 seq | payload). */
final class Files {
    interface Out {
        void ctrl(JSONObject msg);
        void chunk(byte[] data);
        boolean canSend();   /* data channel not backed up */
    }

    private static final int CHUNK = 16 * 1024;
    private final Config cfg;
    private final Out out;

    private static final class Upload {
        File tmp, dest;
        FileOutputStream fos;
        long size, got;
        String sha;
        MessageDigest md;
        boolean scan;
        int nextSeq;
    }

    private final Map<Integer, Upload> uploads = new HashMap<>();
    private final Map<Integer, Thread> pulls = new HashMap<>();

    Files(Config cfg, Out out) {
        this.cfg = cfg;
        this.out = out;
    }

    static boolean allowed(String path) {
        try {
            String c = new File(path).getCanonicalPath();
            return c.startsWith("/sdcard/") || c.startsWith("/storage/emulated/0/") || c.startsWith("/data/local/tmp/")
                    || c.equals("/sdcard") || c.equals("/storage/emulated/0");
        } catch (IOException e) {
            return false;
        }
    }

    private static String hex(byte[] d) {
        StringBuilder sb = new StringBuilder();
        for (byte b : d) sb.append(String.format("%02x", b));
        return sb.toString();
    }

    /** Returns the reply payload (ack d), or throws Fault. */
    JSONObject control(String t, JSONObject d) throws Exception {
        String path = d.optString("path", "");
        if (!path.isEmpty() && !allowed(path)) throw new Fault("forbidden", "path must be under /sdcard or /data/local/tmp");
        switch (t) {
            case "file.list": {
                File[] list = new File(path).listFiles();
                if (list == null) throw new Fault("not_found", "no such folder");
                JSONArray arr = new JSONArray();
                for (File f : list)
                    arr.put(new JSONObject().put("name", f.getName()).put("size", f.length())
                            .put("mtime", f.lastModified()).put("dir", f.isDirectory()));
                return new JSONObject().put("entries", arr);
            }
            case "file.stat": {
                File f = new File(path);
                if (!f.exists()) throw new Fault("not_found", "no such file");
                return new JSONObject().put("size", f.length()).put("mtime", f.lastModified()).put("dir", f.isDirectory());
            }
            case "file.delete":
                if (!new File(path).delete()) throw new Fault("io_error", "cannot delete");
                return new JSONObject();
            case "file.mkdir":
                if (!new File(path).mkdirs() && !new File(path).isDirectory()) throw new Fault("io_error", "cannot create");
                return new JSONObject();
            case "file.push": {
                int xfer = d.getInt("xfer");
                long size = d.getLong("size");
                if (size > (long) cfg.maxMb * 1024 * 1024) throw new Fault("too_large", "over the " + cfg.maxMb + " MB limit");
                File dest = new File(path);
                if (dest.exists() && !d.optBoolean("overwrite", cfg.overwrite)) dest = unique(dest);
                File parent = dest.getParentFile();
                if (parent != null) parent.mkdirs();
                Upload u = new Upload();
                u.dest = dest;
                u.tmp = new File(dest.getPath() + ".part");
                u.fos = new FileOutputStream(u.tmp);
                u.size = size;
                u.sha = d.optString("sha256", "");
                u.md = MessageDigest.getInstance("SHA-256");
                u.scan = d.optBoolean("media_scan", cfg.mediaScan);
                uploads.put(xfer, u);
                return new JSONObject().put("xfer", xfer).put("path", dest.getPath());
            }
            case "file.done": {
                int xfer = d.getInt("xfer");
                Upload u = uploads.remove(xfer);
                if (u == null) throw new Fault("not_found", "unknown transfer");
                u.fos.close();
                String got = hex(u.md.digest());
                if (u.got != u.size || (!u.sha.isEmpty() && !u.sha.equalsIgnoreCase(got))) {
                    u.tmp.delete();
                    throw new Fault("io_error", "size or checksum mismatch");
                }
                if (!u.tmp.renameTo(u.dest)) throw new Fault("io_error", "cannot move the file into place");
                if (u.scan) Shell.runQuiet("am broadcast -a android.intent.action.MEDIA_SCANNER_SCAN_FILE -d 'file://"
                        + u.dest.getPath().replace("'", "") + "'");
                Ln.i("received " + u.dest + " (" + u.size + " bytes)");
                return new JSONObject().put("xfer", xfer).put("path", u.dest.getPath()).put("verified", true);
            }
            case "file.cancel": {
                int xfer = d.getInt("xfer");
                Upload u = uploads.remove(xfer);
                if (u != null) {
                    u.fos.close();
                    u.tmp.delete();
                }
                Thread p = pulls.remove(xfer);
                if (p != null) p.interrupt();
                return new JSONObject();
            }
            case "file.pull": {
                int xfer = d.getInt("xfer");
                File f = new File(path);
                if (!f.isFile()) throw new Fault("not_found", "no such file");
                startPull(xfer, f);
                return new JSONObject().put("xfer", xfer).put("size", f.length()).put("sha256", sha256(f));
            }
            default:
                throw new Fault("unsupported", t);
        }
    }

    private static File unique(File f) {
        String name = f.getName(), base = name, ext = "";
        int dot = name.lastIndexOf('.');
        if (dot > 0) {
            base = name.substring(0, dot);
            ext = name.substring(dot);
        }
        for (int i = 1; ; i++) {
            File c = new File(f.getParentFile(), base + " (" + i + ")" + ext);
            if (!c.exists()) return c;
        }
    }

    private static String sha256(File f) throws Exception {
        MessageDigest md = MessageDigest.getInstance("SHA-256");
        try (FileInputStream in = new FileInputStream(f)) {
            byte[] b = new byte[65536];
            int n;
            while ((n = in.read(b)) > 0) md.update(b, 0, n);
        }
        return hex(md.digest());
    }

    void chunk(byte[] msg) {
        if (msg.length < 8) return;
        ByteBuffer b = ByteBuffer.wrap(msg);
        int xfer = b.getInt(0), seq = b.getInt(4);
        Upload u = uploads.get(xfer);
        if (u == null || seq != u.nextSeq) return;
        try {
            u.fos.write(msg, 8, msg.length - 8);
            u.md.update(msg, 8, msg.length - 8);
            u.got += msg.length - 8;
            u.nextSeq++;
            if (u.got / (256 * 1024) != (u.got - (msg.length - 8)) / (256 * 1024))
                out.ctrl(new JSONObject().put("t", "file.progress").put("d", new JSONObject().put("xfer", xfer).put("bytes", u.got)));
        } catch (Exception e) {
            Ln.w("upload write failed", e);
            uploads.remove(xfer);
        }
    }

    private void startPull(int xfer, File f) {
        Thread t = new Thread(() -> {
            try (FileInputStream in = new FileInputStream(f)) {
                byte[] buf = new byte[CHUNK];
                int seq = 0, n;
                while ((n = in.read(buf)) > 0 && !Thread.currentThread().isInterrupted()) {
                    while (!out.canSend()) Thread.sleep(5);
                    byte[] m = new byte[8 + n];
                    ByteBuffer.wrap(m).putInt(xfer).putInt(seq++);
                    System.arraycopy(buf, 0, m, 8, n);
                    out.chunk(m);
                }
                out.ctrl(new JSONObject().put("t", "file.done").put("d", new JSONObject().put("xfer", xfer)));
            } catch (InterruptedException ignored) {
            } catch (Exception e) {
                Ln.w("pull failed", e);
            } finally {
                pulls.remove(xfer);
            }
        }, "pull-" + xfer);
        pulls.put(xfer, t);
        t.start();
    }

    void cancelAll() {
        for (Upload u : uploads.values()) {
            try { u.fos.close(); } catch (IOException ignored) { }
            u.tmp.delete();
        }
        uploads.clear();
        for (Thread t : pulls.values()) t.interrupt();
        pulls.clear();
    }

    static final class Fault extends Exception {
        final String code;
        Fault(String code, String msg) {
            super(msg);
            this.code = code;
        }
    }
}
