package com.classickeys.classicplayer;

import android.content.SharedPreferences;

/** Persistent CC/channel mapping. Channel -1 represents older channel-unspecified mappings. */
final class MidiLearnMapping {
    final int cc;
    final int channel;

    private MidiLearnMapping(int cc, int channel) {
        this.cc = cc;
        this.channel = channel;
    }

    static String key(int layer, int target) { return "layer_" + layer + "_" + target; }

    static MidiLearnMapping load(SharedPreferences preferences, int layer, int target) {
        String key = key(layer, target);
        return new MidiLearnMapping(preferences.getInt(key, -1), preferences.getInt(key + "_channel", -1));
    }

    static void save(SharedPreferences preferences, int layer, int target, int cc, int channel) {
        if (cc < 0 || cc > 127 || channel < 0 || channel > 15)
            throw new IllegalArgumentException("CC must be 0–127 and MIDI channel 0–15");
        String key = key(layer, target);
        preferences.edit().putInt(key, cc).putInt(key + "_channel", channel)
                .putBoolean(key + "_relative", false).apply();
    }

    static void clear(SharedPreferences preferences, int layer, int target) {
        String key = key(layer, target);
        preferences.edit().remove(key).remove(key + "_channel").remove(key + "_relative").apply();
    }

    boolean matches(int incomingCc, int incomingChannel) {
        return cc == incomingCc && (channel < 0 || channel == incomingChannel);
    }

    static float normalizedValue(int value) {
        return Math.max(0, Math.min(127, value)) / 127f;
    }

    String label() {
        return cc < 0 ? "LEARN" : "CC " + cc + (channel < 0 ? "" : " CH " + (channel + 1));
    }
}
