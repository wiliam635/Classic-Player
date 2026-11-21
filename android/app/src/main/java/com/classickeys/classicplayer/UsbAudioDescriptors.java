package com.classickeys.classicplayer;

import java.util.ArrayList;
import java.util.List;

/**
 * Reads the USB Audio Class descriptors of a device (UsbDeviceConnection.getRawDescriptors())
 * and finds an isochronous OUT streaming alternate setting the direct USB driver can use.
 * Pure Java on purpose: it is unit-tested on the emulator with hand-built descriptors.
 *
 * UAC1 (class-compliant converters such as the PCM2902 "USB Audio CODEC") lists its sample rates
 * in the descriptors. UAC2 keeps them in the clock source, so rates stay empty there until the
 * driver asks the device (GET_RANGE); such alternates are reported but not auto-selected.
 */
final class UsbAudioDescriptors {
    /** One AudioStreaming alternate setting that has an isochronous OUT endpoint. */
    static final class StreamAlt {
        int interfaceNumber, altSetting, uacVersion;       // uacVersion: 1 or 2
        int endpointAddress, maxPacketSize, interval;      // interval: bInterval as in the descriptor
        int channels, subframeBytes, bitResolution;
        int syncType;                                      // 0 none, 1 async, 2 adaptive, 3 sync
        int[] rates = new int[0];                          // discrete rates in Hz
        int minRate, maxRate;                              // continuous range (UAC1 bSamFreqType 0)
        boolean pcm = true;

        boolean supportsRate(int hz) {
            for (int r : rates) if (r == hz) return true;
            return rates.length == 0 && minRate > 0 && hz >= minRate && hz <= maxRate;
        }
        boolean fits16BitStereo() { return pcm && channels == 2 && subframeBytes == 2 && bitResolution == 16; }
        @Override public String toString() {
            return "UAC" + uacVersion + " if" + interfaceNumber + " alt" + altSetting + " ep0x" + Integer.toHexString(endpointAddress)
                    + " pkt" + maxPacketSize + " " + channels + "ch/" + bitResolution + "bit sync" + syncType;
        }
    }

    private static final int DT_INTERFACE = 4, DT_ENDPOINT = 5, DT_CS_INTERFACE = 0x24;
    private static final int CLASS_AUDIO = 1, SUBCLASS_STREAMING = 2;
    private static final int AS_GENERAL = 1, FORMAT_TYPE = 2;

    private UsbAudioDescriptors() { }

    static List<StreamAlt> parse(byte[] d) {
        List<StreamAlt> out = new ArrayList<>();
        if (d == null) return out;
        StreamAlt current = null;       // alternate being filled, null outside AudioStreaming
        boolean currentHasOut = false;
        int pos = 0;
        while (pos + 2 <= d.length) {
            int len = d[pos] & 0xff, type = d[pos + 1] & 0xff;
            if (len < 2 || pos + len > d.length) break;
            if (type == DT_INTERFACE && len >= 9) {
                finish(out, current, currentHasOut);
                int iface = d[pos + 2] & 0xff, alt = d[pos + 3] & 0xff;
                int cls = d[pos + 5] & 0xff, sub = d[pos + 6] & 0xff, proto = d[pos + 7] & 0xff;
                current = null; currentHasOut = false;
                if (cls == CLASS_AUDIO && sub == SUBCLASS_STREAMING && alt > 0) {
                    current = new StreamAlt();
                    current.interfaceNumber = iface; current.altSetting = alt;
                    current.uacVersion = proto == 0x20 ? 2 : 1;
                }
            } else if (current != null && type == DT_CS_INTERFACE && len >= 3) {
                int sub = d[pos + 2] & 0xff;
                if (sub == AS_GENERAL) parseGeneral(d, pos, len, current);
                else if (sub == FORMAT_TYPE) parseFormat(d, pos, len, current);
            } else if (current != null && type == DT_ENDPOINT && len >= 7) {
                int address = d[pos + 2] & 0xff, attributes = d[pos + 3] & 0xff;
                boolean isoOut = (address & 0x80) == 0 && (attributes & 3) == 1;
                if (isoOut && !currentHasOut) {
                    currentHasOut = true;
                    current.endpointAddress = address;
                    current.syncType = (attributes >> 2) & 3;
                    current.maxPacketSize = (d[pos + 4] & 0xff) | ((d[pos + 5] & 0xff) << 8);
                    current.interval = d[pos + 6] & 0xff;
                }
            }
            pos += len;
        }
        finish(out, current, currentHasOut);
        return out;
    }

    private static void finish(List<StreamAlt> out, StreamAlt alt, boolean hasOut) {
        if (alt != null && hasOut && alt.channels > 0) out.add(alt);
    }

    private static void parseGeneral(byte[] d, int pos, int len, StreamAlt alt) {
        if (alt.uacVersion == 1) {
            if (len >= 7) { int tag = (d[pos + 5] & 0xff) | ((d[pos + 6] & 0xff) << 8); alt.pcm = tag == 1; }
        } else if (len >= 16) {
            alt.pcm = (d[pos + 5] & 0xff) == 1 && ((d[pos + 7] & 1) != 0);  // bFormatType I + PCM bit
            alt.channels = d[pos + 11] & 0xff;
        }
    }

    private static void parseFormat(byte[] d, int pos, int len, StreamAlt alt) {
        int formatType = d[pos + 3] & 0xff;
        if (formatType != 1) { alt.pcm = false; return; }   // only Type I (PCM) is supported
        if (alt.uacVersion == 1 && len >= 8) {
            alt.channels = d[pos + 4] & 0xff;
            alt.subframeBytes = d[pos + 5] & 0xff;
            alt.bitResolution = d[pos + 6] & 0xff;
            int types = d[pos + 7] & 0xff;
            if (types == 0 && len >= 14) {
                alt.minRate = rate24(d, pos + 8);
                alt.maxRate = rate24(d, pos + 11);
            } else {
                int count = Math.min(types, (len - 8) / 3);
                alt.rates = new int[count];
                for (int i = 0; i < count; i++) alt.rates[i] = rate24(d, pos + 8 + 3 * i);
            }
        } else if (alt.uacVersion == 2 && len >= 6) {
            alt.subframeBytes = d[pos + 4] & 0xff;
            alt.bitResolution = d[pos + 5] & 0xff;
        }
    }

    private static int rate24(byte[] d, int p) {
        return (d[p] & 0xff) | ((d[p + 1] & 0xff) << 8) | ((d[p + 2] & 0xff) << 16);
    }

    /**
     * Picks the alternate for 16-bit stereo output. A rate of 44100 is preferred (the synth
     * renders at 44.1 kHz, so no resampling), then 48000. Returns null if nothing fits.
     */
    static StreamAlt choose(List<StreamAlt> alts, int[] preferredRates) {
        for (int rate : preferredRates)
            for (StreamAlt a : alts)
                if (a.uacVersion == 1 && a.fits16BitStereo() && a.supportsRate(rate)) return a;
        return null;
    }

    /**
     * Samples to put in each USB packet for a given rate, with the fractional remainder carried
     * over (full speed: 1 packet per 1 ms, so 44.1 kHz alternates 44 and 45 samples; high speed:
     * packets are 125 microseconds long).
     */
    static final class PacketSchedule {
        private final long numerator, denominator;
        private long remainder;
        PacketSchedule(int rateHz, int packetsPerSecond) {
            numerator = rateHz; denominator = packetsPerSecond;
        }
        /** Next packet's frame count. */
        int next() {
            remainder += numerator;
            int frames = (int) (remainder / denominator);
            remainder -= (long) frames * denominator;
            return frames;
        }
    }
}
