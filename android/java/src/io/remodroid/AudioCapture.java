package io.remodroid;

import android.annotation.SuppressLint;
import android.content.AttributionSource;
import android.content.Context;
import android.content.ContextWrapper;
import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.MediaCodec;
import android.media.MediaFormat;
import android.media.MediaRecorder;

import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;

/** Captures what the phone plays (the REMOTE_SUBMIX source, like scrcpy's
 *  --audio-source=output) and encodes it to Opus for the WebRTC audio track.
 *  While it records, Android routes media to the submix instead of the
 *  speaker, so the sound moves to the iPhone; stopping gives it back.
 *  Needs Android 12 (the AudioRecord context API); runs as the shell user,
 *  which holds CAPTURE_AUDIO_OUTPUT. */
final class AudioCapture {
    static final int RATE = 48000, CHANNELS = 2;
    private static final int FRAME_BYTES = RATE / 50 * CHANNELS * 2; /* 20 ms of 16-bit stereo */
    private static Context shellContext;

    private volatile boolean running;
    private Thread thread;

    /** Builds the shell context on the main thread: ActivityThread needs a Looper. */
    static void init() {
        if (!supported()) return;
        try {
            context();
        } catch (Throwable e) {
            Ln.w("audio: no shell context", e instanceof java.lang.reflect.InvocationTargetException ? e.getCause() : e);
        }
    }

    static boolean supported() {
        return Hidden.sdk() >= 31 && opusEncoder() != null;
    }

    static String opusEncoder() {
        try {
            return new android.media.MediaCodecList(android.media.MediaCodecList.REGULAR_CODECS)
                    .findEncoderForFormat(MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_OPUS, RATE, CHANNELS));
        } catch (Exception e) {
            return null;
        }
    }

    synchronized void start() {
        if (running || !supported()) return;
        running = true;
        thread = new Thread(this::run, "audio");
        thread.start();
    }

    synchronized void stop() {
        running = false;
        Thread t = thread;
        thread = null;
        if (t != null) {
            try {
                t.join(1000);
            } catch (InterruptedException ignored) { }
        }
    }

    boolean running() { return running; }

    private void run() {
        AudioRecord rec = null;
        MediaCodec enc = null;
        try {
            rec = record();
            MediaFormat f = MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_OPUS, RATE, CHANNELS);
            f.setInteger(MediaFormat.KEY_BIT_RATE, 128000);
            enc = MediaCodec.createByCodecName(opusEncoder());
            enc.configure(f, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
            rec.startRecording();
            enc.start();
            Ln.i("audio: capturing the phone's output (" + enc.getName() + ")");
            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            long t0 = System.nanoTime() / 1000, samples = 0;
            while (running) {
                int in = enc.dequeueInputBuffer(20000);
                if (in >= 0) {
                    ByteBuffer b = enc.getInputBuffer(in);
                    int n = rec.read(b, Math.min(b.remaining(), FRAME_BYTES), AudioRecord.READ_BLOCKING);
                    if (n < 0) throw new IllegalStateException("AudioRecord.read: " + n);
                    enc.queueInputBuffer(in, 0, n, t0 + samples * 1000000 / RATE, 0);
                    samples += n / (CHANNELS * 2);
                }
                for (int out; (out = enc.dequeueOutputBuffer(info, 0)) >= 0; ) {
                    /* the config buffer holds OpusHead, not a packet */
                    if ((info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) == 0 && info.size > 0)
                        Native.audio(enc.getOutputBuffer(out), info.offset, info.size, info.presentationTimeUs);
                    enc.releaseOutputBuffer(out, false);
                }
            }
        } catch (Throwable e) {
            Ln.w("audio capture failed", e instanceof java.lang.reflect.InvocationTargetException ? e.getCause() : e);
            running = false;
        } finally {
            if (rec != null) {
                try { rec.stop(); } catch (Exception ignored) { }
                rec.release();
            }
            if (enc != null) {
                try { enc.stop(); } catch (Exception ignored) { }
                enc.release();
            }
            Ln.i("audio: stopped");
        }
    }

    @SuppressLint({"MissingPermission", "WrongConstant"})
    private static AudioRecord record() throws Exception {
        AudioFormat fmt = new AudioFormat.Builder().setEncoding(AudioFormat.ENCODING_PCM_16BIT).setSampleRate(RATE)
                .setChannelMask(AudioFormat.CHANNEL_IN_STEREO).build();
        int min = AudioRecord.getMinBufferSize(RATE, AudioFormat.CHANNEL_IN_STEREO, AudioFormat.ENCODING_PCM_16BIT);
        AudioRecord.Builder b = new AudioRecord.Builder().setContext(context())
                .setAudioSource(MediaRecorder.AudioSource.REMOTE_SUBMIX).setAudioFormat(fmt);
        if (min > 0) b.setBufferSizeInBytes(8 * min); /* doesn't add latency; reads return as soon as 20 ms is there */
        AudioRecord r = b.build();
        if (r.getState() != AudioRecord.STATE_INITIALIZED) {
            r.release();
            throw new IllegalStateException("AudioRecord not initialized");
        }
        return r;
    }

    /** A context that says it is com.android.shell (uid 2000), the identity the
     *  audio policy checks against CAPTURE_AUDIO_OUTPUT. */
    private static synchronized Context context() throws Exception {
        if (shellContext == null) {
            if (android.os.Looper.myLooper() == null) android.os.Looper.prepare();
            shellContext = new ShellContext(systemContext());
        }
        return shellContext;
    }

    private static Context systemContext() throws Exception {
        Class<?> at = Class.forName("android.app.ActivityThread");
        Constructor<?> c = at.getDeclaredConstructor();
        c.setAccessible(true);
        Object thread = c.newInstance();
        Field cur = at.getDeclaredField("sCurrentActivityThread");
        cur.setAccessible(true);
        cur.set(null, thread);
        Method m = at.getDeclaredMethod("getSystemContext");
        return (Context) m.invoke(thread);
    }

    private static final class ShellContext extends ContextWrapper {
        static final String PKG = "com.android.shell";

        ShellContext(Context base) { super(base); }

        @Override public String getPackageName() { return PKG; }
        @Override public String getOpPackageName() { return PKG; }
        @Override public Context getApplicationContext() { return this; }

        @Override
        public AttributionSource getAttributionSource() {
            return new AttributionSource.Builder(2000 /* Process.SHELL_UID */).setPackageName(PKG).build();
        }
    }
}
