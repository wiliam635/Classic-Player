package com.classickeys.classicplayer;

import android.app.Activity;
import android.app.Instrumentation;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.View;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import org.junit.Test;
import org.junit.runner.RunWith;
import java.io.*;
import java.lang.reflect.Method;
import static org.junit.Assert.*;

/** Emulates the tablet flow: load a layer, tap it, and capture the docked inspector. */
@RunWith(AndroidJUnit4.class)
public class InspectorScreenshotTest {
    private String shell(String command) throws Exception {
        android.os.ParcelFileDescriptor fd = InstrumentationRegistry.getInstrumentation().getUiAutomation().executeShellCommand(command);
        StringBuilder out = new StringBuilder();
        try (BufferedReader in = new BufferedReader(new InputStreamReader(new android.os.ParcelFileDescriptor.AutoCloseInputStream(fd)))) {
            String line; while ((line = in.readLine()) != null) out.append(line).append('\n');
        }
        return out.toString();
    }
    private void shot(String name, File dir) throws Exception {
        Instrumentation i = InstrumentationRegistry.getInstrumentation();
        i.waitForIdleSync();
        Bitmap b = i.getUiAutomation().takeScreenshot();
        assertNotNull(b);
        try (OutputStream out = new FileOutputStream(new File(dir, name))) { assertTrue(b.compress(Bitmap.CompressFormat.PNG, 100, out)); }
        b.recycle();
    }
    private void tap(float x, float y) {
        Instrumentation i = InstrumentationRegistry.getInstrumentation();
        long t = SystemClock.uptimeMillis();
        MotionEvent down = MotionEvent.obtain(t, t, MotionEvent.ACTION_DOWN, x, y, 0); down.setSource(InputDevice.SOURCE_TOUCHSCREEN);
        MotionEvent up = MotionEvent.obtain(t, t + 60, MotionEvent.ACTION_UP, x, y, 0); up.setSource(InputDevice.SOURCE_TOUCHSCREEN);
        i.getUiAutomation().injectInputEvent(down, true); i.getUiAutomation().injectInputEvent(up, true);
        down.recycle(); up.recycle();
    }

    @Test public void tapLayerOpensDockedInspector() throws Exception {
        Instrumentation instrumentation = InstrumentationRegistry.getInstrumentation();
        Context context = instrumentation.getTargetContext();
        String additional = InstrumentationRegistry.getArguments().getString("additionalTestOutputDir");
        File dir = new File(additional == null ? context.getExternalFilesDir(null).getPath() : additional, "audio-regression");
        assertTrue(dir.isDirectory() || dir.mkdirs());
        String log = shell("wm size 2000x1200") + shell("wm density 240") + shell("wm size") + shell("wm density");
        try (FileWriter w = new FileWriter(new File(dir, "wm.txt"))) { w.write(log); }
        Thread.sleep(1500);
        Activity activity = null;
        try {
            activity = instrumentation.startActivitySync(new Intent(context, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            final MainActivity main = (MainActivity) activity;
            Method login = MainActivity.class.getDeclaredMethod("showMixerAfterLogin", String.class);
            login.setAccessible(true);
            instrumentation.runOnMainSync(() -> { try { login.invoke(main, ""); } catch (Exception e) { throw new RuntimeException(e); } });
            Method activate = MainActivity.class.getDeclaredMethod("activateBundledDx7", int.class, int.class, String.class);
            activate.setAccessible(true);
            instrumentation.runOnMainSync(() -> { try { activate.invoke(main, 0, R.raw.dx7_bank_1, "Divine Masquerade 1"); } catch (Exception e) { throw new RuntimeException(e); } });
            Thread.sleep(1500);
            tap(1160, 302); // dismiss the emulator's "Viewing full screen" hint
            Thread.sleep(800);
            shot("inspector-0-mixer.png", dir);
            View content = activity.findViewById(android.R.id.content);
            int[] at = new int[2]; content.getLocationOnScreen(at);
            float density = context.getResources().getDisplayMetrics().density;
            float h = content.getHeight();
            // Artwork area of the first layer card.
            tap(at[0] + 18 * density + 60 * density, at[1] + h * .17f + h * .16f + 24 * density);
            Thread.sleep(2000);
            shot("inspector-1-docked.png", dir);
        } finally {
            if (activity != null) { final Activity a = activity; instrumentation.runOnMainSync(a::finish); }
            shell("wm size reset"); shell("wm density reset");
        }
    }

    private boolean clickText(String exact) {
        android.view.accessibility.AccessibilityNodeInfo root = InstrumentationRegistry.getInstrumentation().getUiAutomation().getRootInActiveWindow();
        if (root == null) return false;
        java.util.List<android.view.accessibility.AccessibilityNodeInfo> found = root.findAccessibilityNodeInfosByText(exact);
        for (android.view.accessibility.AccessibilityNodeInfo n : found) {
            if (n.getText() != null && n.getText().toString().trim().equalsIgnoreCase(exact)) {
                android.graphics.Rect r = new android.graphics.Rect(); n.getBoundsInScreen(r);
                tap(r.centerX(), r.centerY()); return true;
            }
        }
        return false;
    }

    /** Every engine, every tab, plus a scrolled capture of each: the layout can be reviewed without the tablet. */
    @Test public void everyEngineEveryTab() throws Exception {
        Instrumentation instrumentation = InstrumentationRegistry.getInstrumentation();
        Context context = instrumentation.getTargetContext();
        String additional = InstrumentationRegistry.getArguments().getString("additionalTestOutputDir");
        File dir = new File(additional == null ? context.getExternalFilesDir(null).getPath() : additional, "audio-regression");
        assertTrue(dir.isDirectory() || dir.mkdirs());
        shell("wm size 2000x1200"); shell("wm density 240"); Thread.sleep(1500);
        File sf2 = new File(context.getCacheDir(), "inspector-sine.sf2"); SoundFontFixture.write(sf2);
        Activity activity = null;
        StringBuilder report = new StringBuilder();
        try {
            activity = instrumentation.startActivitySync(new Intent(context, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            final MainActivity main = (MainActivity) activity;
            Method login = MainActivity.class.getDeclaredMethod("showMixerAfterLogin", String.class); login.setAccessible(true);
            Method dx7 = MainActivity.class.getDeclaredMethod("activateBundledDx7", int.class, int.class, String.class); dx7.setAccessible(true);
            Method sf = MainActivity.class.getDeclaredMethod("loadSoundFontLibraryFile", int.class, File.class, boolean.class); sf.setAccessible(true);
            Method analog = MainActivity.class.getDeclaredMethod("activateAnalog", int.class); analog.setAccessible(true);
            Method hammond = MainActivity.class.getDeclaredMethod("activateHammond", int.class); hammond.setAccessible(true);
            instrumentation.runOnMainSync(() -> { try {
                login.invoke(main, "");
                dx7.invoke(main, 0, R.raw.dx7_bank_1, "Divine Masquerade 1");
                sf.invoke(main, 1, sf2, true);
                analog.invoke(main, 2);
                hammond.invoke(main, 3);
            } catch (Exception e) { throw new RuntimeException(e); } });
            Thread.sleep(2000);
            tap(1160, 302); Thread.sleep(800);
            shot("all-0-mixer.png", dir);
            View content = activity.findViewById(android.R.id.content);
            int[] at = new int[2]; content.getLocationOnScreen(at);
            float density = context.getResources().getDisplayMetrics().density, h = content.getHeight();
            String[] names = {"DX7", "SF2", "ANALOG", "HAMMOND"};
            String[] normalTabs = {"SOM", "ENVELOPE", "MIDI", "EFEITOS", "VELOCIDADE"};
            String[] analogTabs = {"OSCILADORES", "FILTRO / ADSR", "CONTROLES", "EFEITOS", "MIDI"};
            for (int layer = 0; layer < 4; layer++) {
                tap(at[0] + (18 + layer * 150 + 60) * density, at[1] + h * .17f + h * .16f + 24 * density);
                Thread.sleep(2000);
                String[] tabs = names[layer].equals("ANALOG") ? analogTabs : normalTabs;
                for (int t = 0; t < tabs.length; t++) {
                    boolean ok = t == 0 || clickText(tabs[t]);
                    report.append(names[layer]).append(' ').append(tabs[t]).append(ok ? " ok\n" : " TAB NOT FOUND\n");
                    Thread.sleep(900);
                    String base = "L" + layer + "-" + names[layer] + "-" + t + "-" + tabs[t].replaceAll("[^A-Z]", "");
                    shot(base + ".png", dir);
                    shell("input swipe 1300 900 1300 350 400"); Thread.sleep(700);
                    shot(base + "-scrolled.png", dir);
                    shell("input swipe 1300 350 1300 900 400"); Thread.sleep(500);
                }
                boolean closed = clickText("FECHAR");
                report.append(names[layer]).append(" FECHAR ").append(closed ? "ok\n" : "NOT FOUND\n");
                Thread.sleep(1000);
            }
        } finally {
            try (FileWriter w = new FileWriter(new File(dir, "tabs-report.txt"))) { w.write(report.toString()); }
            if (activity != null) { final Activity a = activity; instrumentation.runOnMainSync(a::finish); }
            shell("wm size reset"); shell("wm density reset");
        }
    }
}
