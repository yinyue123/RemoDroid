package io.remodroid;

import java.io.BufferedReader;
import java.io.InputStreamReader;

/**
 * Crash safety (plan §4.9): a separate process that waits for the server to
 * exit (its stdin closes). If the panel was left off, it turns it back on, and
 * when configured it puts the phone to sleep so it locks. scrcpy uses the same
 * pattern.
 */
public final class CleanUp {
    public static void main(String[] args) {
        boolean panelOff = false, lock = true;
        try (BufferedReader r = new BufferedReader(new InputStreamReader(System.in))) {
            String line;
            while ((line = r.readLine()) != null) {
                if (line.equals("panel_off")) panelOff = true;
                else if (line.equals("panel_on")) panelOff = false;
                else if (line.startsWith("lock_on_exit=")) lock = line.endsWith("1");
            }
        } catch (Exception ignored) { }
        Device d = new Device();
        if (panelOff) d.setPanel(true);
        if (lock) d.sleep();
        Ln.i("cleanup done (panel restored: " + panelOff + ", locked: " + lock + ")");
    }
}
