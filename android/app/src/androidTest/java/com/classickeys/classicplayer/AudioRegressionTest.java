package com.classickeys.classicplayer;

import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.util.Log;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import java.io.*;
import java.lang.reflect.Method;
import java.util.Arrays;
import static org.junit.Assert.*;

/** Offline PCM tests call the actual APK renderer, not a mock DSP. No AudioTrack
 * is started: emulator timings are diagnostic, not tablet latency guarantees. */
@RunWith(AndroidJUnit4.class)
public class AudioRegressionTest {
    private static final int RATE = 48000, BLOCK = 256;
    private PolySynthEngine engine;
    private Context context;
    private File reports, sf2, dx7;
    private Method render;

    @Before public void prepare() throws Exception {
        context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        // AGP pulls this directory BEFORE uninstalling the target APK. Ordinary
        // external-files output disappears when connected tests clean up.
        String additionalOutput = InstrumentationRegistry.getArguments().getString("additionalTestOutputDir");
        reports = additionalOutput == null
                ? new File(context.getExternalFilesDir(null), "audio-regression")
                : new File(additionalOutput, "audio-regression");
        assertTrue(reports.isDirectory() || reports.mkdirs());
        engine = new PolySynthEngine();
        for (int i = 0; i < 6; i++) engine.clearLayer(i);
        engine.allNotesOff();
        engine.setMaster(.7f);
        engine.setMasterEffects(0, 0);
        render = PolySynthEngine.class.getDeclaredMethod("nativeRender", short[].class, int.class);
        render.setAccessible(true);
        sf2 = new File(context.getCacheDir(), "regression-sine.sf2");
        SoundFontFixture.write(sf2);
        dx7 = new File(context.getCacheDir(), "regression-dx7.syx");
        try (InputStream in = context.getResources().openRawResource(R.raw.dx7_bank_1);
             OutputStream out = new FileOutputStream(dx7)) {
            byte[] buf = new byte[8192]; int n;
            while ((n = in.read(buf)) != -1) out.write(buf, 0, n);
        }
    }

    @After public void cleanup() { if (engine != null) engine.close(); }

    @Test public void sustainedRetriggersReleaseEveryVoice() throws Exception {
        activate(1,0);activate(2,1);activate(1,2);
        for(int cycle=0;cycle<4;cycle++){
            engine.setSustain(0,true);
            for(int n=0;n<300;n++){
                int note=48+n%24;engine.noteOn(note,70,0);pcm(1024);engine.noteOff(note,0);
                for(int layer=0;layer<3;layer++)assertTrue(engine.activeVoices(layer)<=engine.voiceBudget(layer));
            }
            engine.setSustain(0,false);pcm(RATE*5);
            for(int layer=0;layer<3;layer++)assertEquals("Leaked voices, layer "+layer,0,engine.activeVoices(layer));
            engine.noteOn(72,100,0);assertTrue(rms(pcm(RATE/10))>.0001);engine.noteOff(72,0);pcm(RATE*3);
        }
    }

    @Test public void fullSustainedPoolAcceptsNewKey() throws Exception {
        activate(1,0);engine.setSustain(0,true);
        assertEquals("A single active layer receives the full app budget",256,engine.voiceBudget(0));
        for(int n=0;n<300;n++){engine.noteOn(60,60,0);pcm(256);engine.noteOff(60,0);}
        assertEquals(256,engine.activeVoices(0));
        engine.noteOn(84,100,0);engine.setSustain(0,false);pcm(RATE*3);
        assertEquals("Newest held key must survive voice stealing",1,engine.activeVoices(0));
        assertTrue(rms(pcm(RATE/10))>.0001);
        engine.noteOff(84,0);pcm(RATE*3);assertEquals(0,engine.activeVoices(0));
    }

    @Test public void soundFontPedalIsIsolatedByMidiChannel() throws Exception {
        activate(1,0);engine.setSustain(0,true);
        engine.noteOn(60,90,0);engine.noteOn(60,90,1);pcm(256);
        engine.noteOff(60,0);engine.noteOff(60,1);pcm(RATE*3);
        assertEquals(1,engine.activeVoices(0));
        engine.setSustain(0,false);pcm(RATE*3);assertEquals(0,engine.activeVoices(0));
    }

    @Test public void threeEnginesNativeOutputSurvivesBufferRestarts() throws Exception {
        activate(1,0);activate(2,1);activate(1,2);
        for(int frames:new int[]{128,256,512,1024,2048,512}){
            engine.setBufferFrames(frames);engine.start();
            engine.noteOn(60,80,0);
            android.os.SystemClock.sleep(150);
            assertTrue(engine.outputStatus(),engine.isRunning());
            int[] info=engine.outputInfo();
            assertEquals("Native callback output must be exercised",48000,info[0]);
            assertTrue("Output buffer must be negotiated",info[1]>0);
            assertTrue("Device burst must be known",info[2]>0);
            assertTrue("Buffer capacity must be reported",info[10]>=info[2]);
            assertTrue("Buffer must retain at least one device burst when capacity allows",
                info[1]>=Math.min(info[10],info[2]));
            assertEquals("Android 10 must use AAudio",2,info[4]);
            report("native-output-"+frames,"{\"info\":"+Arrays.toString(info)+",\"outputLatencyMs\":"+
                engine.outputLatencyMillis()+"}");
            assertTrue("Renderer stopped producing audio",engine.masterPeak()>0);
            engine.noteOff(60,0);engine.stop();assertFalse(engine.isRunning());
        }
    }

    private void activate(int type, int layer) {
        if (type == 1) assertTrue(engine.loadLayer(layer, sf2.getAbsolutePath()));
        if (type == 2) assertTrue(engine.loadDx7(layer, dx7.getAbsolutePath()));
        if (type == 3) engine.activateAnalog(layer);
        if (type == 4) engine.activateHammond(layer);
        engine.setLayerGain(layer, .5f);
        engine.setLayerEnvelope(layer, .005f, .05f);
        engine.setLayerTone(layer, 100, 0, 0, 0);
        engine.setLayerEq(layer, 0, 0, 0, 220, 1200, 4200, .707f, 1, .707f, 20, 20000);
        engine.setLayerRouting(layer, -1, 0, 0, 127, 0, true, 0);
    }

    private short[] pcm(int frames) throws Exception {
        short[] out = new short[frames * 2];
        for (int offset = 0; offset < frames; offset += BLOCK) {
            int count = Math.min(BLOCK, frames - offset);
            short[] block = new short[count * 2];
            render.invoke(null, block, count);
            System.arraycopy(block, 0, out, offset * 2, block.length);
        }
        return out;
    }

    private static double rms(short[] pcm) {
        double sum = 0;
        for (short sample : pcm) sum += (double) sample * sample;
        return Math.sqrt(sum / Math.max(1, pcm.length)) / 32768.0;
    }

    private void report(String name, String data) throws Exception {
        Log.i("AudioRegression", name + " " + data);
        try (Writer out = new OutputStreamWriter(new FileOutputStream(new File(reports, name + ".json")), "UTF-8")) {
            out.write(data);
        }
    }

    private void noteLifecycle(int type, boolean sustain) throws Exception {
        activate(type, 0);
        engine.noteOn(60, 100, 0);
        short[] held = pcm(RATE / 4);
        if (sustain) engine.setSustain(0, true);
        engine.noteOff(60, 0);
        short[] afterKey = pcm(RATE / 4);
        if (sustain) engine.setSustain(0, false);
        short[] release = pcm(RATE * 3);
        short[] tail = Arrays.copyOfRange(release, release.length - RATE / 2, release.length);
        String name = "engine-" + type + (sustain ? "-sustain" : "-noteoff");
        SoundFontFixture.wav(new File(reports, name + ".wav"), held, afterKey, release);
        report(name, "{\"heldRms\":" + rms(held) + ",\"afterKeyRms\":" + rms(afterKey)
                + ",\"tailRms\":" + rms(tail) + ",\"tailLimit\":0.0005}");
        assertTrue(name + " produced no sound", rms(held) > .0005);
        if (sustain) assertTrue(name + " pedal did not hold note", rms(afterKey) > .0005);
        assertTrue(name + " note remains sounding after release", rms(tail) < .0005);
    }

    @Test public void sf2NoteOff() throws Exception { noteLifecycle(1, false); }
    @Test public void dx7NoteOff() throws Exception { noteLifecycle(2, false); }
    @Test public void analogNoteOff() throws Exception { noteLifecycle(3, false); }
    @Test public void hammondNoteOff() throws Exception { noteLifecycle(4, false); }
    @Test public void sf2Sustain() throws Exception { noteLifecycle(1, true); }
    @Test public void dx7Sustain() throws Exception { noteLifecycle(2, true); }
    @Test public void analogSustain() throws Exception { noteLifecycle(3, true); }
    @Test public void hammondSustain() throws Exception { noteLifecycle(4, true); }

    @Test public void soundFontLoadAndPreset() throws Exception {
        long start = System.nanoTime(); activate(1, 0);
        long elapsed = System.nanoTime() - start;
        assertEquals(1, engine.presetCount(0));
        assertTrue(engine.setPreset(0, 0));
        engine.noteOn(60, 100, 0);
        assertTrue(rms(pcm(RATE / 4)) > .0005);
        report("sf2-load", "{\"syntheticFixture\":true,\"bytes\":" + sf2.length()
                + ",\"loadMs\":" + elapsed / 1e6 + "}");
    }

    @Test public void failedSoundFontImportPreservesLayer() throws Exception {
        activate(1, 0);
        assertFalse(engine.loadLayer(0, new File(context.getCacheDir(), "missing-font.sf2").getAbsolutePath()));
        assertEquals(1, engine.engineType(0));
        assertEquals(1, engine.presetCount(0));
        engine.noteOn(60, 100, 0);
        assertTrue(rms(pcm(RATE / 4)) > .0005);
    }

    @Test public void sixLayers128InputNotesAndFaders() throws Exception {
        // Stress traffic, NOT proof of 128 surviving DSP voices: layered SF2
        // regions and voice-stealing require separate voice-count diagnostics.
        for (int layer = 0; layer < 6; layer++) activate(1 + layer % 4, layer);
        for (int note = 0; note < 128; note++) engine.noteOn(note, 80, 0);
        int blocks = 256, clipped = 0, maxJump = 0;
        long total = 0, max = 0;
        short[] audio = new short[blocks * BLOCK * 2];
        short[] block = new short[BLOCK * 2];
        for (int b = 0; b < blocks; b++) {
            for (int layer = 0; layer < 6; layer++) engine.setLayerGain(layer, .1f + .4f * (b % 64) / 63f);
            long start = System.nanoTime(); render.invoke(null, block, BLOCK);
            long ns = System.nanoTime() - start; total += ns; max = Math.max(max, ns);
            System.arraycopy(block, 0, audio, b * block.length, block.length);
        }
        for (int i = 0; i < audio.length; i++) {
            if (Math.abs((int)audio[i]) >= 32760) clipped++;
            if (i >= 2) maxJump = Math.max(maxJump, Math.abs((int)audio[i] - audio[i - 2]));
        }
        SoundFontFixture.wav(new File(reports, "six-layers-faders.wav"), audio);
        report("stress", "{\"inputNotesPerLayer\":128,\"layers\":6,\"emulatorOnly\":true,"
                + "\"meanRenderMs\":" + total / (blocks * 1e6) + ",\"maxRenderMs\":" + max / 1e6
                + ",\"blockBudgetMs\":" + BLOCK * 1000.0 / RATE + ",\"clippedSamples\":" + clipped
                + ",\"maxAdjacentSampleJump\":" + maxJump + "}");
        assertTrue("Stress test produced silence", rms(audio) > .0005);
        for (int note = 0; note < 128; note++) engine.noteOff(note, 0);
        pcm(RATE * 3);
    }

    @Test public void globalVoiceBudgetIsEvenlyDistributedAndRebalanced() throws Exception {
        int[] types={1,2,3,4,1,2};
        int budgetTotal=0;
        for(int layer=0;layer<types.length;layer++){
            activate(types[layer],layer);
            int budget=engine.voiceBudget(layer);
            assertEquals("Equal split with remainder assigned in layer order",layer<4?43:42,budget);
            budgetTotal+=budget;
        }
        assertEquals(256,budgetTotal);

        // Fill each pool with sustained, distinct notes, then assert that the
        // actual renderer never exceeds its per-layer share.
        engine.setSustain(0,true);
        for(int note=0;note<64;note++){
            engine.noteOn(note,90,0);
            pcm(128);
        }
        int activeTotal=0;
        for(int layer=0;layer<types.length;layer++){
            int active=engine.activeVoices(layer);
            assertTrue("Layer "+layer+" exceeded budget",active<=engine.voiceBudget(layer));
            activeTotal+=active;
        }
        assertTrue("Combined active voice count exceeded app limit",activeTotal<=256);

        engine.clearLayer(5);
        budgetTotal=0;
        for(int layer=0;layer<5;layer++){
            int expected=layer==0?52:51;
            assertEquals("Removing a layer redistributes the budget",expected,engine.voiceBudget(layer));
            assertTrue("Active voices must be trimmed after redistribution",
                    engine.activeVoices(layer)<=engine.voiceBudget(layer));
            budgetTotal+=engine.voiceBudget(layer);
        }
        assertEquals(256,budgetTotal);
        assertEquals(0,engine.voiceBudget(5));
        engine.allNotesOff();
        pcm(RATE*3);
    }

    @Test public void twoSoundFontsThenDx7LayerLoadsAndRenders() throws Exception {
        activate(1,0);
        activate(1,1);
        activate(2,2);
        assertEquals(86,engine.voiceBudget(0));
        assertEquals(85,engine.voiceBudget(1));
        assertEquals(85,engine.voiceBudget(2));
        assertEquals(1,engine.engineType(0));
        assertEquals(1,engine.engineType(1));
        assertEquals(2,engine.engineType(2));
        engine.noteOn(60,100,0);
        assertTrue("SF2 + DX7 stack produced silence",rms(pcm(RATE/4))>.0005);
        engine.noteOff(60,0);
        pcm(RATE*3);
    }

    @Test public void activityLaunchScreenshot() throws Exception {
        android.app.Instrumentation instrumentation = InstrumentationRegistry.getInstrumentation();
        android.app.Activity activity = instrumentation.startActivitySync(
                new Intent(context, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        try {
            instrumentation.waitForIdleSync();
            Bitmap screenshot = instrumentation.getUiAutomation().takeScreenshot();
            assertNotNull(screenshot);
            try (OutputStream out = new FileOutputStream(new File(reports, "mixer.png"))) {
                assertTrue(screenshot.compress(Bitmap.CompressFormat.PNG, 100, out));
            }
            screenshot.recycle();
        } finally { instrumentation.runOnMainSync(activity::finish); instrumentation.waitForIdleSync(); }
    }
}
