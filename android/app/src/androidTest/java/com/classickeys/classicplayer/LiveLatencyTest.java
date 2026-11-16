package com.classickeys.classicplayer;

import android.content.Context;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import org.junit.Test;
import org.junit.runner.RunWith;
import java.io.*;
import java.util.Locale;

/** Live (real audio stream) key-to-voice latency in several situations. Emulator timings are diagnostic only. */
@RunWith(AndroidJUnit4.class)
public class LiveLatencyTest {
    private PolySynthEngine engine;
    private File sf2, dx7;
    private final StringBuilder report = new StringBuilder("{\n");

    private void log(String name, double ms) { report.append(String.format(Locale.US, "  \"%s\": %.1f,\n", name, ms)); }

    private double keyToVoice(int layer) throws Exception {
        long t0 = System.nanoTime();
        engine.noteOn(60, 100, 0);
        while (engine.activeVoices(layer) == 0 && System.nanoTime() - t0 < 12_000_000_000L) Thread.sleep(1);
        double ms = (System.nanoTime() - t0) / 1e6;
        engine.noteOff(60, 0);
        Thread.sleep(300);
        return ms;
    }

    private void activate(int type, int layer) {
        if (type == 1) engine.loadLayer(layer, sf2.getAbsolutePath());
        if (type == 2) engine.loadDx7(layer, dx7.getAbsolutePath());
        if (type == 3) engine.activateAnalog(layer);
        if (type == 4) engine.activateHammond(layer);
        engine.setLayerGain(layer, .5f);
        engine.setLayerRouting(layer, -1, 0, 0, 127, 0, true, 0);
    }

    @Test public void measure() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        String additional = InstrumentationRegistry.getArguments().getString("additionalTestOutputDir");
        File dir = new File(additional == null ? context.getExternalFilesDir(null).getPath() : additional, "audio-regression");
        dir.mkdirs();
        sf2 = new File(context.getCacheDir(), "latency-sine.sf2"); SoundFontFixture.write(sf2);
        dx7 = new File(context.getCacheDir(), "latency-dx7.syx");
        try (InputStream in = context.getResources().openRawResource(R.raw.dx7_bank_1); OutputStream out = new FileOutputStream(dx7)) {
            byte[] b = new byte[8192]; int n; while ((n = in.read(b)) != -1) out.write(b, 0, n);
        }
        engine = new PolySynthEngine();
        try {
            for (int i = 0; i < 6; i++) engine.clearLayer(i);
            engine.setMaster(.7f);
            long t = System.nanoTime();
            activate(1, 0);
            log("load_sf2_ms", (System.nanoTime() - t) / 1e6);
            t = System.nanoTime(); activate(2, 1); log("load_dx7_ms", (System.nanoTime() - t) / 1e6);
            t = System.nanoTime(); activate(4, 2); log("activate_hammond_ms", (System.nanoTime() - t) / 1e6);
            t = System.nanoTime(); engine.start(); log("start_stream_ms", (System.nanoTime() - t) / 1e6);
            Thread.sleep(500);
            report.append("  \"output\": \"" + engine.outputStatus().replace("\"", "'") + "\",\n");
            log("1_cold_first_note_sf2", keyToVoice(0));
            log("1_second_note_sf2", keyToVoice(0));
            log("1_first_note_dx7", keyToVoice(1));
            // pause/resume like onPause/onResume
            t = System.nanoTime(); engine.stop(); log("stop_ms", (System.nanoTime() - t) / 1e6);
            t = System.nanoTime(); engine.start(); log("restart_ms", (System.nanoTime() - t) / 1e6);
            log("2_note_right_after_restart", keyToVoice(0));
            // polyphony cap changes (budget redistribution under the synth lock)
            t = System.nanoTime(); for (int i = 0; i < 6; i++) engine.setLayerPolyphony(i, 64); log("set_poly_6x_ms", (System.nanoTime() - t) / 1e6);
            log("3_note_after_poly_change", keyToVoice(0));
            // reload a layer (mute/unmute) while playing on another layer
            Thread loader = new Thread(() -> { engine.clearLayer(0); activate(1, 0); });
            final double[] during = new double[1];
            loader.start();
            during[0] = keyToVoice(1);
            loader.join();
            log("4_note_on_other_layer_while_reloading", during[0]);
            // pads strip audio path
            log("5_note_after_idle_5s", sleepThen(5000));
            int[] q = engine.midiQueueDiagnostics();
            report.append("  \"queue_max_wait_us\": " + q[5] + ",\n  \"queue_peak\": " + q[1] + "\n}\n");
        } finally {
            engine.close();
            try (FileWriter w = new FileWriter(new File(dir, "latency.json"))) { w.write(report.toString()); }
        }
    }

    private double sleepThen(long ms) throws Exception { Thread.sleep(ms); return keyToVoice(0); }
}
