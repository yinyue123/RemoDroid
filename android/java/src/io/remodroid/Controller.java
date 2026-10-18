package io.remodroid;

import android.os.SystemClock;
import android.view.InputDevice;
import android.view.MotionEvent;

import java.nio.ByteBuffer;

/** Decodes the binary input channel (plan A.7) and injects multi-touch and
 *  scroll events. Coordinates are 0..65535 across the video frame. */
final class Controller {
    private static final int MAX_POINTERS = 10;
    private final Device device;
    private final MotionEvent.PointerProperties[] props = new MotionEvent.PointerProperties[MAX_POINTERS];
    private final MotionEvent.PointerCoords[] coords = new MotionEvent.PointerCoords[MAX_POINTERS];
    private final int[] ids = new int[MAX_POINTERS];      // viewer pointer id per slot
    private final boolean[] used = new boolean[MAX_POINTERS];
    private long downTime;
    private volatile int width = 1080, height = 2400;
    long events;
    /** Uptime of the viewer's last input; the AI API yields to a person (Actions.VIEWER_GRACE_MS). */
    static volatile long lastInputAt;

    Controller(Device device) {
        this.device = device;
        for (int i = 0; i < MAX_POINTERS; i++) {
            props[i] = new MotionEvent.PointerProperties();
            props[i].toolType = MotionEvent.TOOL_TYPE_FINGER;
            coords[i] = new MotionEvent.PointerCoords();
            coords[i].size = 1;
        }
    }

    /** Called with the display size in its current rotation. */
    void setScreen(int w, int h) {
        width = w;
        height = h;
    }

    void handle(byte[] msg) {
        lastInputAt = SystemClock.uptimeMillis();
        ByteBuffer b = ByteBuffer.wrap(msg);
        if (msg.length >= 14 && msg[0] == 0x01) {
            int action = b.get(1) & 0xff, pid = b.get(2) & 0xff;
            float x = (b.getShort(4) & 0xffff) / 65535f * width, y = (b.getShort(6) & 0xffff) / 65535f * height;
            float p = (b.getShort(8) & 0xffff) / 65535f;
            touch(action, pid, x, y, p);
        } else if (msg.length >= 13 && msg[0] == 0x02) {
            float x = (b.getShort(1) & 0xffff) / 65535f * width, y = (b.getShort(3) & 0xffff) / 65535f * height;
            scroll(x, y, b.getShort(5) / 256f, b.getShort(7) / 256f);
        }
    }

    private int slot(int pid, boolean create) {
        for (int i = 0; i < MAX_POINTERS; i++)
            if (used[i] && ids[i] == pid) return i;
        if (!create) return -1;
        for (int i = 0; i < MAX_POINTERS; i++)
            if (!used[i]) {
                used[i] = true;
                ids[i] = pid;
                props[i].id = pid;
                return i;
            }
        return -1;
    }

    private int count() {
        int n = 0;
        for (boolean u : used) if (u) n++;
        return n;
    }

    /** Compacts used slots to the front (MotionEvent wants contiguous arrays) and returns the index of slot s. */
    private int compact(int s) {
        int j = 0, at = -1;
        for (int i = 0; i < MAX_POINTERS; i++) {
            if (!used[i]) continue;
            if (i == s) at = j;
            if (i != j) {
                MotionEvent.PointerProperties tp = props[j]; props[j] = props[i]; props[i] = tp;
                MotionEvent.PointerCoords tc = coords[j]; coords[j] = coords[i]; coords[i] = tc;
                ids[j] = ids[i];
                used[j] = true;
                used[i] = false;
            }
            j++;
        }
        return at;
    }

    private void touch(int action, int pid, float x, float y, float pressure) {
        int s = slot(pid, action == 0);
        if (s < 0) return;
        coords[s].x = x;
        coords[s].y = y;
        coords[s].pressure = action == 1 ? 0 : Math.max(pressure, 0.01f);
        int idx = compact(s);
        int n = count();
        long now = SystemClock.uptimeMillis();
        int a;
        switch (action) {
            case 0: /* down */
                if (n == 1) downTime = now;
                a = n == 1 ? MotionEvent.ACTION_DOWN
                        : MotionEvent.ACTION_POINTER_DOWN | (idx << MotionEvent.ACTION_POINTER_INDEX_SHIFT);
                break;
            case 1: /* up */
                a = n == 1 ? MotionEvent.ACTION_UP
                        : MotionEvent.ACTION_POINTER_UP | (idx << MotionEvent.ACTION_POINTER_INDEX_SHIFT);
                break;
            case 3: a = MotionEvent.ACTION_CANCEL; break;
            default: a = MotionEvent.ACTION_MOVE;
        }
        MotionEvent ev = MotionEvent.obtain(downTime, now, a, n, props, coords, 0, 0, 1f, 1f, 0, 0,
                InputDevice.SOURCE_TOUCHSCREEN, 0);
        device.inject(ev);
        ev.recycle();
        events++;
        if (action == 1 || action == 3) {
            used[idx] = false;
            if (action == 3) for (int i = 0; i < MAX_POINTERS; i++) used[i] = false;
        }
    }

    private void scroll(float x, float y, float h, float v) {
        long now = SystemClock.uptimeMillis();
        MotionEvent.PointerProperties[] pp = { new MotionEvent.PointerProperties() };
        pp[0].toolType = MotionEvent.TOOL_TYPE_MOUSE;
        MotionEvent.PointerCoords[] pc = { new MotionEvent.PointerCoords() };
        pc[0].x = x;
        pc[0].y = y;
        pc[0].setAxisValue(MotionEvent.AXIS_HSCROLL, h);
        pc[0].setAxisValue(MotionEvent.AXIS_VSCROLL, v);
        MotionEvent ev = MotionEvent.obtain(now, now, MotionEvent.ACTION_SCROLL, 1, pp, pc, 0, 0, 1f, 1f, 0, 0,
                InputDevice.SOURCE_MOUSE, 0);
        device.inject(ev);
        ev.recycle();
    }
}
