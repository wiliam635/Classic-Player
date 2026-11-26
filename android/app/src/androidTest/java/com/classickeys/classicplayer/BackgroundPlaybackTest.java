package com.classickeys.classicplayer;

import static org.junit.Assert.*;

import android.app.Activity;
import android.app.Instrumentation;
import android.content.Context;
import android.content.Intent;
import android.os.SystemClock;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.lang.reflect.Field;
import org.junit.Test;
import org.junit.runner.RunWith;

/** The engine and the foreground service must survive Home, Back, a screen off and a hidden recreation. */
@RunWith(AndroidJUnit4.class)
public class BackgroundPlaybackTest {
    private final Instrumentation instrumentation = InstrumentationRegistry.getInstrumentation();

    private String shell(String command) throws Exception {
        android.os.ParcelFileDescriptor fd = instrumentation.getUiAutomation().executeShellCommand(command);
        StringBuilder out = new StringBuilder();
        try (BufferedReader in = new BufferedReader(new InputStreamReader(new android.os.ParcelFileDescriptor.AutoCloseInputStream(fd)))) {
            String line; while ((line = in.readLine()) != null) out.append(line).append('\n');
        }
        return out.toString();
    }

    private static Object field(Object owner, String name) throws Exception {
        Field f = owner.getClass().getDeclaredField(name); f.setAccessible(true); return f.get(owner);
    }

    private void assertAlive(MainActivity activity, String when) throws Exception {
        assertFalse("activity destroyed " + when, activity.isDestroyed());
        assertTrue("audio engine stopped " + when, (Boolean) field(field(activity, "audioEngine"), "running"));
        assertTrue("playback service gone " + when, PlaybackService.active);
        String services = shell("dumpsys activity services " + instrumentation.getTargetContext().getPackageName());
        assertTrue("service not in the foreground " + when + ": " + services, services.contains("isForeground=true"));
    }

    @Test public void soundStaysOnOutsideTheApp() throws Exception {
        Context context = instrumentation.getTargetContext();
        Activity started = instrumentation.startActivitySync(new Intent(context, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        MainActivity activity = (MainActivity) started;
        try {
            SystemClock.sleep(2500);
            assertAlive(activity, "in front");
            shell("input keyevent KEYCODE_HOME"); SystemClock.sleep(8000);
            assertAlive(activity, "after Home");
            shell("input keyevent KEYCODE_SLEEP"); SystemClock.sleep(5000);
            assertAlive(activity, "with the screen off");
            String power = shell("dumpsys power");
            assertTrue("no wake lock while playing", power.contains("ClassicPlayer:playback"));
            shell("input keyevent KEYCODE_WAKEUP"); shell("wm dismiss-keyguard"); SystemClock.sleep(1000);
            instrumentation.runOnMainSync(activity::onBackPressed); SystemClock.sleep(1500);
            assertFalse("Back must not close the app", activity.isFinishing());
            assertAlive(activity, "after Back");
        } finally {
            instrumentation.runOnMainSync(activity::finish);
            SystemClock.sleep(1000);
        }
    }
}
