package com.classickeys.classicplayer;

import static org.junit.Assert.*;

import android.app.Instrumentation;
import android.content.Context;
import android.content.Intent;
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import org.junit.Test;
import org.junit.runner.RunWith;

/** Real finger drags on the EQ graph: the point follows the finger and the panel neither closes nor is rebuilt. */
@RunWith(AndroidJUnit4.class)
public class EqGraphTouchTest {
    private final Instrumentation instrumentation = InstrumentationRegistry.getInstrumentation();

    private void drag(float x0, float y0, float x1, float y1) {
        long start = SystemClock.uptimeMillis();
        send(start, MotionEvent.ACTION_DOWN, x0, y0);
        for (int i = 1; i <= 12; i++) { SystemClock.sleep(16); send(start, MotionEvent.ACTION_MOVE, x0 + (x1 - x0) * i / 12f, y0 + (y1 - y0) * i / 12f); }
        send(start, MotionEvent.ACTION_UP, x1, y1);
        instrumentation.waitForIdleSync(); SystemClock.sleep(500);
    }
    private void send(long start, int action, float x, float y) {
        MotionEvent e = MotionEvent.obtain(start, SystemClock.uptimeMillis(), action, x, y, 0);
        e.setSource(InputDevice.SOURCE_TOUCHSCREEN);
        instrumentation.getUiAutomation().injectInputEvent(e, true); e.recycle();
    }
    private static <T> T find(View root, Class<T> type) {
        if (type.isInstance(root)) return type.cast(root);
        if (root instanceof ViewGroup) for (int i = 0; i < ((ViewGroup) root).getChildCount(); i++) { T hit = find(((ViewGroup) root).getChildAt(i), type); if (hit != null) return hit; }
        return null;
    }

    @Test public void popupEditor() throws Exception { run(false); }
    @Test public void dockedEditor() throws Exception { run(true); }

    private void run(boolean docked) throws Exception {
        Context context = instrumentation.getTargetContext();
        // Tablet-sized screen, without the emulator's "viewing full screen" overlay eating the first touch.
        for (String command : new String[]{"wm size 2000x1200", "wm density 240", "settings put secure immersive_mode_confirmations confirmed"})
            try (java.io.InputStream in = new android.os.ParcelFileDescriptor.AutoCloseInputStream(instrumentation.getUiAutomation().executeShellCommand(command))) { while (in.read() >= 0) { } }
        SystemClock.sleep(1500);
        final MainActivity main = (MainActivity) instrumentation.startActivitySync(new Intent(context, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        try {
            Method login = MainActivity.class.getDeclaredMethod("showMixerAfterLogin", String.class); login.setAccessible(true);
            Method render = MainActivity.class.getDeclaredMethod("renderEffectEditorBody", int.class, String.class, LinearLayout.class, int.class, Runnable.class, Runnable.class); render.setAccessible(true);
            final EditorUi.Panel[] panel = new EditorUi.Panel[1];
            instrumentation.runOnMainSync(() -> { try {
                login.invoke(main, "");
                if (docked) EditorUi.Panel.dockNext = true;
                panel[0] = new EditorUi.Panel(main, "EQ · LAYER 1", "");
                EditorUi.compact = true;
                render.invoke(main, 0, "EQ", panel[0].body, 0, (Runnable) () -> {}, (Runnable) () -> {});
                panel[0].show();
            } catch (Exception e) { throw new RuntimeException(e); } });
            instrumentation.waitForIdleSync(); SystemClock.sleep(1500);
            final EffectGraphs.EqGraph graph = find(panel[0].body, EffectGraphs.EqGraph.class);
            assertNotNull("no EQ graph", graph);
            instrumentation.runOnMainSync(() -> graph.requestRectangleOnScreen(new android.graphics.Rect(0, 0, graph.getWidth(), graph.getHeight()), true));
            SystemClock.sleep(600);
            int[] at = new int[2]; graph.getLocationOnScreen(at);
            float left = at[0] + EditorUi.dp(context, 40), top = at[1] + EditorUi.dp(context, 34);
            float width = graph.getWidth() - EditorUi.dp(context, 58), height = graph.getHeight() - EditorUi.dp(context, 68);

            // 1. Grab point 2 (1200 Hz, 0 dB) and take it to 1/2 of the width, a quarter from the top (+9 dB).
            float px = left + width * (float) (Math.log10(1200 / 20.0) / 3.0), py = top + height * .5f;
            drag(px, py, left + width * .5f, top + height * .25f);
            assertTrue("panel closed after dragging a point", panel[0].dialog.isShowing());
            assertSame("panel was rebuilt", graph, find(panel[0].body, EffectGraphs.EqGraph.class));
            assertEquals("frequency under the finger", 632.5, graph.freq[1], 25);
            assertEquals("gain under the finger", 9.0, graph.gain[1], .8);
            Field screenField = MainActivity.class.getDeclaredField("screen"); screenField.setAccessible(true);
            Object screen = screenField.get(main);
            Field mid = screen.getClass().getDeclaredField("eqMid"); mid.setAccessible(true);
            assertEquals("engine setting", graph.gain[1], ((float[]) mid.get(screen))[0], .01);

            // 2. Drag the curve away from any point, finishing far outside the graph and the panel.
            drag(left + width * .93f, top + height * .85f, 5, top + height * .85f);
            assertTrue("panel closed after dragging out of the graph", panel[0].dialog.isShowing());
            assertSame("panel was rebuilt", graph, find(panel[0].body, EffectGraphs.EqGraph.class));
            assertEquals("band 3 follows to its lowest frequency", 1000.0, graph.freq[2], 1);
            instrumentation.runOnMainSync(() -> panel[0].dialog.dismiss());
        } finally {
            instrumentation.runOnMainSync(main::finish);
            SystemClock.sleep(800);
        }
    }
}
