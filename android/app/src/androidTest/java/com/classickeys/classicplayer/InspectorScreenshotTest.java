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
    private void shell(String command) throws Exception {
        try (ParcelFileDescriptorReader ignored = new ParcelFileDescriptorReader(
                InstrumentationRegistry.getInstrumentation().getUiAutomation().executeShellCommand(command))) {}
    }
    private static final class ParcelFileDescriptorReader implements Closeable {
        final android.os.ParcelFileDescriptor fd;
        ParcelFileDescriptorReader(android.os.ParcelFileDescriptor fd) throws IOException {
            this.fd = fd;
            try (InputStream in = new android.os.ParcelFileDescriptor.AutoCloseInputStream(fd)) {
                byte[] buffer = new byte[1024]; while (in.read(buffer) != -1) {}
            }
        }
        @Override public void close() {}
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
        shell("wm size 2000x1200"); shell("wm density 240");
        Activity activity = null;
        try {
            activity = instrumentation.startActivitySync(new Intent(context, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            final MainActivity main = (MainActivity) activity;
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
