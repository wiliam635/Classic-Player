package com.classickeys.classicplayer;

import static org.junit.Assert.*;

import java.io.ByteArrayOutputStream;
import java.util.List;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import org.junit.Test;
import org.junit.runner.RunWith;

/** Phase 4: UAC2 topology, async/feedback descriptors, alternate selection and the fake-device driver in other formats. */
@RunWith(AndroidJUnit4.class)
public class UsbDirectFormatsTest {
    private static void put(ByteArrayOutputStream o, int... bytes) { for (int b : bytes) o.write(b); }

    /** UAC2 interface: clock 5 -> input terminal 1 (USB streaming), 24-bit in 4-byte slots, async OUT with feedback IN. */
    private static byte[] uac2Async(boolean withFeedback) {
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        put(o, 9, 4, 0, 0, 0, 1, 1, 0x20, 0);                                        // audio control, UAC2
        put(o, 8, 0x24, 0x0a, 5, 1, 3, 0, 0);                                        // clock source id 5
        put(o, 17, 0x24, 2, 1, 0x01, 0x01, 0, 5, 2, 3, 0, 0, 0, 0, 0, 0, 0, 0);      // input terminal 1, clock 5, 2 channels
        put(o, 9, 4, 1, 0, 0, 1, 2, 0x20, 0);                                        // streaming alt 0
        put(o, 9, 4, 1, 1, 2, 1, 2, 0x20, 0);                                        // streaming alt 1
        put(o, 16, 0x24, 1, 1, 0, 1, 1, 0, 0, 0, 2, 3, 0, 0, 0, 0);                  // AS_GENERAL: link 1, PCM, 2 channels
        put(o, 6, 0x24, 2, 1, 4, 24);                                                // type I, 4-byte slots, 24 bits
        put(o, 7, 5, 0x01, 0x05, 0x88, 0x01, 4);                                     // iso OUT async, 392 bytes, every 4th microframe
        if (withFeedback) put(o, 7, 5, 0x81, 0x11, 4, 0, 4);                         // iso IN feedback
        return o.toByteArray();
    }

    @Test public void parsesUac2ClockTopologyAndFeedback() {
        byte[] d = uac2Async(true);
        List<UsbAudioDescriptors.StreamAlt> alts = UsbAudioDescriptors.parse(d);
        assertEquals(1, alts.size());
        UsbAudioDescriptors.StreamAlt a = alts.get(0);
        assertEquals(2, a.uacVersion); assertEquals(2, a.channels); assertEquals(4, a.subframeBytes); assertEquals(24, a.bitResolution);
        assertTrue(a.pcm); assertEquals(1, a.terminalLink); assertEquals(1, a.syncType);
        assertEquals(392, a.maxPacketSize); assertEquals(4, a.interval);
        assertEquals(0x81, a.feedbackEndpoint); assertEquals(4, a.feedbackMaxPacket);
        UsbAudioDescriptors.ControlInfo c = UsbAudioDescriptors.parseControl(d);
        assertEquals(0, c.controlInterface);
        assertEquals(5, c.clockFor(a.terminalLink));
    }

    @Test public void picksStandardRateWithoutResamplingWhenPossible() {
        List<UsbAudioDescriptors.StreamAlt> alts = UsbAudioDescriptors.parse(uac2Async(true));
        UsbAudioDescriptors.StreamAlt a = alts.get(0);
        a.rates = new int[]{48000, 96000};
        int[] rate = new int[1];
        assertSame(a, UsbAudioDescriptors.pick(alts, 8000, rate));
        assertEquals(48000, rate[0]);                      // 44.1 kHz not offered: closest preferred standard rate
        a.rates = new int[]{44100, 48000, 96000};
        UsbAudioDescriptors.pick(alts, 8000, rate);
        assertEquals(44100, rate[0]);
    }

    @Test public void asyncWithoutFeedbackIsRejected() {
        List<UsbAudioDescriptors.StreamAlt> alts = UsbAudioDescriptors.parse(uac2Async(false));
        alts.get(0).rates = new int[]{48000};
        assertNull(UsbAudioDescriptors.pick(alts, 8000, new int[1]));
    }

    @Test public void packetThatCannotCarryTheRateIsSkipped() {
        List<UsbAudioDescriptors.StreamAlt> alts = UsbAudioDescriptors.parse(uac2Async(true));
        UsbAudioDescriptors.StreamAlt a = alts.get(0);
        a.rates = new int[]{192000};                       // 192 frames/ms * 8 bytes does not fit in 392 bytes
        assertNull(UsbAudioDescriptors.pick(alts, 8000, new int[1]));
    }

    private static void startNote(PolySynthEngine engine) {
        for (int i = 0; i < 6; i++) engine.clearLayer(i);
        engine.allNotesOff();
        engine.setMaster(.7f);
        engine.activateAnalog(0);
        engine.setLayerGain(0, .5f);
        engine.setLayerRouting(0, -1, 0, 0, 127, 0, true, 0);
        engine.noteOn(60, 100, 0);
    }

    @Test public void resampled48kHz24BitReachesTheDevice() {
        PolySynthEngine engine = new PolySynthEngine();
        try {
            startNote(engine);
            long[] r = UsbDirectAudio.selfTest(1200, 48000, 2, 3, 0, 1000, 1000, false);
            assertEquals("48000/1000 is exactly 48 frames of 6 bytes", 288, r[1]);
            assertEquals(r[5] * 48L, r[0]);
            assertTrue("no audio reached the device", r[2] > 10);
            assertEquals(0, r[3]);
        } finally { engine.close(); }
    }

    @Test public void sixChannelHighSpeedKeepsExtraChannelsSilent() {
        PolySynthEngine engine = new PolySynthEngine();
        try {
            startNote(engine);
            long[] r = UsbDirectAudio.selfTest(1200, 96000, 6, 4, 0, 8000, 8000, false);
            assertEquals("96000/8000 = 12 frames of 24 bytes", 288, r[1]);
            assertEquals(r[5] * 12L, r[0]);
            assertEquals("channels 3-6 must stay silent", 0, r[6]);
            assertTrue("no audio reached the device", r[2] > 10);
            assertEquals(0, r[3]);
        } finally { engine.close(); }
    }

    @Test public void monoDeviceGetsAudio() {
        PolySynthEngine engine = new PolySynthEngine();
        try {
            startNote(engine);
            long[] r = UsbDirectAudio.selfTest(1000, 44100, 1, 2, 0, 1000, 1000, false);
            assertTrue("mono 44.1 kHz packet is 44 or 45 frames of 2 bytes, got " + r[1], r[1] == 88 || r[1] == 90);
            assertTrue(r[2] > 10);
            assertEquals(0, r[3]);
        } finally { engine.close(); }
    }

    @Test public void asyncFeedbackSetsThePacketSize() {
        PolySynthEngine engine = new PolySynthEngine();
        try {
            startNote(engine);
            // The device claims 48.5 frames per millisecond (3-byte 10.14 value), 1% above nominal.
            long[] r = UsbDirectAudio.selfTest(1500, 48000, 2, 2, 48.5, 1000, 1000, false);
            assertTrue("feedback never applied: " + r[7], r[7] > 3);
            double perPacket = r[0] / (double) r[5];
            assertTrue("frames per packet " + perPacket + " should follow the feedback (48.5), not nominal 48", perPacket > 48.3 && perPacket < 48.6);
            assertEquals(0, r[3]);
        } finally { engine.close(); }
    }

    @Test public void highSpeedFourByteFeedbackWorksToo() {
        PolySynthEngine engine = new PolySynthEngine();
        try {
            startNote(engine);
            // 16.16 value per microframe: 6.06 frames/microframe = 48.5 kHz, packets every microframe.
            long[] r = UsbDirectAudio.selfTest(1500, 48000, 2, 2, 6.0625, 8000, 8000, true);
            assertTrue("feedback never applied: " + r[7], r[7] > 3);
            double perPacket = r[0] / (double) r[5];
            assertTrue("frames per packet " + perPacket + " should approach 6.0625", perPacket > 6.03 && perPacket < 6.08);
        } finally { engine.close(); }
    }
}
