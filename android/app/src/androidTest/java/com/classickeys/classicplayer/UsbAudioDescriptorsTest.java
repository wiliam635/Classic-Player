package com.classickeys.classicplayer;

import static org.junit.Assert.*;

import java.io.ByteArrayOutputStream;
import java.util.List;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import org.junit.Test;
import org.junit.runner.RunWith;

/** The direct USB driver depends on these parsers; they run on the emulator without any hardware. */
@RunWith(AndroidJUnit4.class)
public class UsbAudioDescriptorsTest {
    private static void put(ByteArrayOutputStream o, int... bytes) { for (int b : bytes) o.write(b); }
    private static void rate(ByteArrayOutputStream o, int hz) { put(o, hz & 0xff, (hz >> 8) & 0xff, (hz >> 16) & 0xff); }

    /** UAC1 stereo 16-bit converter like the TI PCM2902 "USB Audio CODEC": alt 1 = 44.1/48 kHz adaptive. */
    private static byte[] uac1Codec() {
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        put(o, 9, 4, 1, 0, 0, 1, 2, 0, 0);                       // interface 1 alt 0, no endpoints
        put(o, 9, 4, 1, 1, 1, 1, 2, 0, 0);                       // interface 1 alt 1, 1 endpoint
        put(o, 7, 0x24, 1, 1, 1, 1, 0);                          // AS_GENERAL: PCM
        put(o, 14, 0x24, 2, 1, 2, 2, 16, 2); rate(o, 44100); rate(o, 48000);   // FORMAT_TYPE I, 2 discrete rates
        put(o, 9, 5, 0x01, 0x09, 0xc4, 0, 1, 0, 0);              // iso OUT adaptive, 196-byte packets
        put(o, 7, 0x25, 1, 0, 0, 0, 0);                          // CS_ENDPOINT
        return o.toByteArray();
    }

    @Test public void parsesUac1CodecAndPicks44100() {
        List<UsbAudioDescriptors.StreamAlt> alts = UsbAudioDescriptors.parse(uac1Codec());
        assertEquals(1, alts.size());
        UsbAudioDescriptors.StreamAlt a = alts.get(0);
        assertEquals(1, a.interfaceNumber); assertEquals(1, a.altSetting);
        assertEquals(0x01, a.endpointAddress); assertEquals(196, a.maxPacketSize);
        assertEquals(2, a.syncType); assertEquals(2, a.channels); assertEquals(16, a.bitResolution);
        assertArrayEquals(new int[]{44100, 48000}, a.rates);
        assertSame(a, UsbAudioDescriptors.choose(alts, new int[]{44100, 48000}));
        assertTrue(a.supportsRate(48000)); assertFalse(a.supportsRate(96000));
    }

    @Test public void prefersTheFirstRateAndRejectsUnsupported() {
        List<UsbAudioDescriptors.StreamAlt> alts = UsbAudioDescriptors.parse(uac1Codec());
        assertNull(UsbAudioDescriptors.choose(alts, new int[]{96000}));
        assertNotNull(UsbAudioDescriptors.choose(alts, new int[]{96000, 48000}));
    }

    @Test public void ignoresInputOnlyAndGarbage() {
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        put(o, 9, 4, 2, 1, 1, 1, 2, 0, 0);
        put(o, 14, 0x24, 2, 1, 2, 2, 16, 2); rate(o, 44100); rate(o, 48000);
        put(o, 9, 5, 0x81, 0x05, 0xc4, 0, 1, 0, 0);              // iso IN (microphone)
        assertTrue(UsbAudioDescriptors.parse(o.toByteArray()).isEmpty());
        assertTrue(UsbAudioDescriptors.parse(null).isEmpty());
        assertTrue(UsbAudioDescriptors.parse(new byte[]{9, 4, 1}).isEmpty());   // truncated
    }

    @Test public void continuousRangeIsSupported() {
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        put(o, 9, 4, 1, 1, 1, 1, 2, 0, 0);
        put(o, 7, 0x24, 1, 1, 1, 1, 0);
        put(o, 14, 0x24, 2, 1, 2, 2, 16, 0); rate(o, 8000); rate(o, 48000);
        put(o, 9, 5, 0x01, 0x09, 0xc4, 0, 1, 0, 0);
        UsbAudioDescriptors.StreamAlt a = UsbAudioDescriptors.parse(o.toByteArray()).get(0);
        assertTrue(a.supportsRate(44100)); assertFalse(a.supportsRate(96000));
    }

    @Test public void fullSpeedPacketsAlternate44And45Samples() {
        UsbAudioDescriptors.PacketSchedule s = new UsbAudioDescriptors.PacketSchedule(44100, 1000);
        long total = 0; int fortyFives = 0;
        for (int i = 0; i < 1000; i++) { int n = s.next(); assertTrue(n == 44 || n == 45); total += n; if (n == 45) fortyFives++; }
        assertEquals(44100, total);               // exactly one second of audio per 1000 packets
        assertEquals(100, fortyFives);            // one 45-sample packet in ten
    }

    @Test public void highSpeedMicroframesAreExactToo() {
        UsbAudioDescriptors.PacketSchedule s = new UsbAudioDescriptors.PacketSchedule(48000, 8000);
        long total = 0;
        for (int i = 0; i < 8000; i++) total += s.next();
        assertEquals(48000, total);
    }
}
