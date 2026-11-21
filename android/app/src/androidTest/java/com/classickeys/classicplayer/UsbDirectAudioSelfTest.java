package com.classickeys.classicplayer;

import static org.junit.Assert.*;

import androidx.test.ext.junit.runners.AndroidJUnit4;
import org.junit.Test;
import org.junit.runner.RunWith;

/** Runs the direct USB driver against a fake device (no hardware): pacing, packet sizes and content. */
@RunWith(AndroidJUnit4.class)
public class UsbDirectAudioSelfTest {
    @Test public void fakeDeviceReceivesExactRatePacketsWithAudio() throws Exception {
        PolySynthEngine engine = new PolySynthEngine();
        try {
            for (int i = 0; i < 6; i++) engine.clearLayer(i);
            engine.allNotesOff();
            engine.setMaster(.7f);
            engine.activateAnalog(0);
            engine.setLayerGain(0, .5f);
            engine.setLayerRouting(0, -1, 0, 0, 127, 0, true, 0);
            engine.noteOn(60, 100, 0);
            long[] r = UsbDirectAudio.selfTest(1500);
            // {frames, longest packet bytes, URBs with audio, errors, URBs done, packets submitted}
            // The fake device paces itself with sloppy emulator timers, so the exactness check is
            // against the number of packets submitted (1 ms each), not the wall clock.
            long expected = r[5] * 44100L / 1000;
            assertTrue("frames " + r[0] + " vs " + expected + " for " + r[5] + " packets", Math.abs(r[0] - expected) <= 2);
            assertTrue("fake device barely ran: " + r[0], r[0] > 44100L * 1500 / 1000 / 3);
            assertTrue("a 44.1 kHz packet is 44 or 45 frames (176/180 bytes), got " + r[1], r[1] == 176 || r[1] == 180);
            assertTrue("no audio reached the device", r[2] > 10);
            assertEquals("driver reported errors", 0, r[3]);
            assertTrue("URBs did not cycle: " + r[4], r[4] > 100);
        } finally { engine.close(); }
    }
}
