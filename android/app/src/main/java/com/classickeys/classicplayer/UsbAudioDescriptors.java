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
        int terminalLink;                                  // bTerminalLink (UAC2: leads to the clock source)
        int mult = 1;                                      // transactions per microframe (high bandwidth endpoints)
        int feedbackEndpoint, feedbackMaxPacket, feedbackInterval;   // explicit feedback IN endpoint (async), 0 if none
        int clockId = -1, controlInterface = -1;           // UAC2: where to read/set the sample rate

        boolean supportsRate(int hz) {
            for (int r : rates) if (r == hz) return true;
            return rates.length == 0 && minRate > 0 && hz >= minRate && hz <= maxRate;
        }
        boolean fits16BitStereo() { return pcm && channels == 2 && subframeBytes == 2 && bitResolution == 16; }
        /** Any integer PCM layout the driver can write (16/24/32-bit containers, any channel count). */
        boolean usable() { return pcm && channels >= 1 && subframeBytes >= 2 && subframeBytes <= 4 && bitResolution >= 16 && bitResolution <= subframeBytes * 8; }
        int frameBytes() { return channels * subframeBytes; }
        @Override public String toString() {
            return "UAC" + uacVersion + " if" + interfaceNumber + " alt" + altSetting + " ep0x" + Integer.toHexString(endpointAddress)
                    + " pkt" + maxPacketSize + " " + channels + "ch/" + bitResolution + "bit sync" + syncType;
        }
    }

    private static final int DT_INTERFACE = 4, DT_ENDPOINT = 5, DT_CS_INTERFACE = 0x24;
    private static final int CLASS_AUDIO = 1, SUBCLASS_STREAMING = 2;
    private static final int AS_GENERAL = 1, FORMAT_TYPE = 2;
    private static final int SUBCLASS_CONTROL = 1;

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
                    int w = (d[pos + 4] & 0xff) | ((d[pos + 5] & 0xff) << 8);
                    current.maxPacketSize = w & 0x7ff;
                    current.mult = ((w >> 11) & 3) + 1;
                    current.interval = d[pos + 6] & 0xff;
                } else if ((address & 0x80) != 0 && (attributes & 3) == 1 && currentHasOut && current.feedbackEndpoint == 0) {
                    current.feedbackEndpoint = address;
                    current.feedbackMaxPacket = ((d[pos + 4] & 0xff) | ((d[pos + 5] & 0xff) << 8)) & 0x7ff;
                    current.feedbackInterval = d[pos + 6] & 0xff;
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
        if (len >= 4) alt.terminalLink = d[pos + 3] & 0xff;
        if (alt.uacVersion == 1) {
            if (len >= 7) { int tag = (d[pos + 5] & 0xff) | ((d[pos + 6] & 0xff) << 8); alt.pcm = tag == 1; }
        } else if (len >= 16) {
            // UAC2 AS_GENERAL: bTerminalLink(3) bmControls(4) bFormatType(5) bmFormats(6..9) bNrChannels(10)
            alt.pcm = (d[pos + 5] & 0xff) == 1 && ((d[pos + 6] & 1) != 0);  // format type I + PCM bit
            alt.channels = d[pos + 10] & 0xff;
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

    /** AudioControl topology (UAC2): enough to find the clock source behind a streaming terminal. */
    static final class ControlInfo {
        int controlInterface = -1;
        final java.util.Map<Integer, int[]> entities = new java.util.HashMap<>();   // id -> {kind, source/pins...}
        static final int INPUT_TERMINAL = 2, OUTPUT_TERMINAL = 3, CLOCK_SOURCE = 0x0a, CLOCK_SELECTOR = 0x0b, CLOCK_MULTIPLIER = 0x0c;

        /** Clock source id feeding the given terminal, or -1. */
        int clockFor(int terminalId) {
            int[] e = entities.get(terminalId);
            if (e == null) return -1;
            int id = e[1];                                          // terminals: bCSourceID
            for (int hops = 0; hops < 8; hops++) {
                int[] c = entities.get(id);
                if (c == null) return -1;
                if (c[0] == CLOCK_SOURCE) return id;
                if (c[0] == CLOCK_SELECTOR || c[0] == CLOCK_MULTIPLIER) id = c[1]; else return -1;
            }
            return -1;
        }
    }

    static ControlInfo parseControl(byte[] d) {
        ControlInfo info = new ControlInfo();
        if (d == null) return info;
        boolean inControl = false, uac2 = false;
        int pos = 0;
        while (pos + 2 <= d.length) {
            int len = d[pos] & 0xff, type = d[pos + 1] & 0xff;
            if (len < 2 || pos + len > d.length) break;
            if (type == DT_INTERFACE && len >= 9) {
                inControl = (d[pos + 5] & 0xff) == CLASS_AUDIO && (d[pos + 6] & 0xff) == SUBCLASS_CONTROL;
                uac2 = (d[pos + 7] & 0xff) == 0x20;
                if (inControl && info.controlInterface < 0) info.controlInterface = d[pos + 2] & 0xff;
            } else if (inControl && uac2 && type == DT_CS_INTERFACE && len >= 5) {
                int sub = d[pos + 2] & 0xff, id = d[pos + 3] & 0xff;
                if (sub == ControlInfo.INPUT_TERMINAL && len >= 8) info.entities.put(id, new int[]{sub, d[pos + 7] & 0xff});
                else if (sub == ControlInfo.OUTPUT_TERMINAL && len >= 9) info.entities.put(id, new int[]{sub, d[pos + 8] & 0xff});
                else if (sub == ControlInfo.CLOCK_SOURCE) info.entities.put(id, new int[]{sub, 0});
                else if (sub == ControlInfo.CLOCK_SELECTOR && len >= 6) info.entities.put(id, new int[]{sub, d[pos + 5] & 0xff});
                else if (sub == ControlInfo.CLOCK_MULTIPLIER) info.entities.put(id, new int[]{sub, d[pos + 4] & 0xff});
            }
            pos += len;
        }
        return info;
    }

    /** Rates the driver is willing to run the device at, best first (no resampling for 44.1 kHz). */
    static final int[] PREFERRED_RATES = {44100, 48000, 88200, 96000, 176400, 192000, 32000, 22050, 24000, 16000, 11025, 8000};

    /** Data packets per second: the bus runs 1000 frames (full speed) or 8000 microframes (high speed) per second, bInterval = 2^(n-1) of them. */
    static int packetsPerSecond(int unitsPerSecond, int bInterval) { return Math.max(1, unitsPerSecond >> Math.max(0, Math.min(15, bInterval - 1))); }

    /** Preference score for an alternate at a rate (higher is better); -1 if unusable. */
    static int score(StreamAlt a, int rate, int unitsPerSecond) {
        if (!a.usable() || !a.supportsRate(rate)) return -1;
        int packetsPerSecond = packetsPerSecond(unitsPerSecond, a.interval);
        int perPacket = (rate + packetsPerSecond - 1) / packetsPerSecond + 1;
        if (perPacket * a.frameBytes() > a.maxPacketSize * a.mult) return -1;     // packet too small for this rate
        int rateRank = 0;
        for (int i = 0; i < PREFERRED_RATES.length; i++) if (PREFERRED_RATES[i] == rate) rateRank = PREFERRED_RATES.length - i;
        int channelRank = a.channels == 2 ? 3 : a.channels > 2 ? 2 : 1;
        int bitRank = a.subframeBytes == 2 ? 3 : a.subframeBytes == 3 ? 2 : 1;
        int syncRank = a.syncType == 1 && a.feedbackEndpoint == 0 ? -1 : 0;        // async without feedback cannot be paced
        if (syncRank < 0) return -1;
        return rateRank * 100 + channelRank * 10 + bitRank;
    }

    /** Best alternate/rate pair, or null. result[0] is the alternate, rate in rate[0]. */
    static StreamAlt pick(List<StreamAlt> alts, int unitsPerSecond, int[] rateOut) {
        StreamAlt best = null; int bestScore = -1, bestRate = 0;
        for (StreamAlt a : alts)
            for (int rate : PREFERRED_RATES) {
                int sc = score(a, rate, unitsPerSecond);
                if (sc > bestScore) { bestScore = sc; best = a; bestRate = rate; }
            }
        if (best != null && rateOut != null && rateOut.length > 0) rateOut[0] = bestRate;
        return best;
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
