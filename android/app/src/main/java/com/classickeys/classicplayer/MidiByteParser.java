package com.classickeys.classicplayer;

/** Stateful MIDI 1.0 channel-voice byte parser; survives packets split across receiver callbacks. */
final class MidiByteParser {
    interface Listener {
        void onMessage(int status, int data1, int data2, long timestamp);
    }

    private int runningStatus;
    private int firstData = -1;

    void accept(byte[] data, int offset, int count, long timestamp, Listener listener) {
        if (data == null || listener == null || offset < 0 || count < 0 || offset + count > data.length)
            throw new IllegalArgumentException("Invalid MIDI packet bounds or listener");
        for (int i = offset, end = offset + count; i < end; i++) {
            int value = data[i] & 0xff;
            if (value >= 0xf8) continue; // Realtime bytes may occur between any two data bytes.
            if ((value & 0x80) != 0) {
                if (value < 0xf0) runningStatus = value;
                else runningStatus = 0; // System Common/SysEx cancels channel running status.
                firstData = -1;
                continue;
            }
            if (runningStatus == 0) continue;
            int type = runningStatus & 0xf0;
            int required = type == 0xc0 || type == 0xd0 ? 1 : 2;
            if (firstData < 0) {
                if (required == 1) listener.onMessage(runningStatus, value & 0x7f, -1, timestamp);
                else firstData = value & 0x7f;
                continue;
            }
            int first = firstData;
            firstData = -1;
            listener.onMessage(runningStatus, first, value & 0x7f, timestamp);
        }
    }

    void reset() { runningStatus = 0; firstData = -1; }
}
