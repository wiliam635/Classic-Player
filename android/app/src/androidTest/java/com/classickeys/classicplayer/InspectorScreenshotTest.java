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
}
