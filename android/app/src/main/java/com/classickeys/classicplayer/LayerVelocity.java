package com.classickeys.classicplayer;

import android.content.SharedPreferences;

/** Per-layer velocity and key-range settings that mirror the desktop routing (persisted in the "velocity" preferences). */
final class LayerVelocity {
    static final int LAYERS = 6;
    final int[] low = {1,1,1,1,1,1}, high = {127,127,127,127,127,127}, outMin = {1,1,1,1,1,1}, outMax = {127,127,127,127,127,127};
    final int[] timbreFloor = {1,1,1,1,1,1}, noteLow = {0,0,0,0,0,0}, noteHigh = {127,127,127,127,127,127};
    final float[] slope = new float[LAYERS];
    /** Additional ADSR stage: 0 ms decay bypasses it. */
    final float[] decayMs = new float[LAYERS];
    final float[] sustainPercent = {100f,100f,100f,100f,100f,100f};
    final boolean[] sensitivityOff = new boolean[LAYERS];
    /** Hand-drawn curve (128 outputs), null when the layer uses the shaped curve. */
    final int[][] custom = new int[LAYERS][];
    private final SharedPreferences prefs;

    LayerVelocity(SharedPreferences prefs) {
        this.prefs = prefs;
        for (int i = 0; i < LAYERS; i++) {
            low[i] = prefs.getInt("low_" + i, 1); high[i] = prefs.getInt("high_" + i, 127);
            outMin[i] = prefs.getInt("out_min_" + i, 1); outMax[i] = prefs.getInt("out_max_" + i, 127);
            timbreFloor[i] = prefs.getInt("floor_" + i, 1); slope[i] = prefs.getFloat("slope_" + i, 0f);
            sensitivityOff[i] = prefs.getBoolean("off_" + i, false);
            decayMs[i] = prefs.getFloat("decay_" + i, 0f); sustainPercent[i] = prefs.getFloat("sustain_" + i, 100f);
            noteLow[i] = prefs.getInt("note_low_" + i, 0); noteHigh[i] = prefs.getInt("note_high_" + i, 127);
            String csv = prefs.getString("drawn_" + i, null);
            if (csv != null) { String[] parts = csv.split(","); if (parts.length == 128) { int[] table = new int[128]; try { for (int v = 0; v < 128; v++) table[v] = Math.max(1, Math.min(127, Integer.parseInt(parts[v].trim()))); custom[i] = table; } catch (NumberFormatException ignored) { } } }
        }
    }

    void save(int i) {
        prefs.edit().putInt("low_" + i, low[i]).putInt("high_" + i, high[i]).putInt("out_min_" + i, outMin[i]).putInt("out_max_" + i, outMax[i])
                .putInt("floor_" + i, timbreFloor[i]).putFloat("slope_" + i, slope[i]).putBoolean("off_" + i, sensitivityOff[i])
                .putFloat("decay_" + i, decayMs[i]).putFloat("sustain_" + i, sustainPercent[i])
                .putInt("note_low_" + i, noteLow[i]).putInt("note_high_" + i, noteHigh[i]).apply();
    }

    void apply(PolySynthEngine engine, int i) {
        if (engine == null || i < 0 || i >= LAYERS) return;
        engine.setLayerVelocity(i, low[i], high[i], sensitivityOff[i], outMin[i], outMax[i], slope[i], timbreFloor[i]);
        engine.setVelocityTable(i, custom[i]);
        engine.setLayerAdsr(i, decayMs[i], sustainPercent[i]);
    }

    /** Stores a drawn curve (or clears it with null). */
    void setCustom(int i, int[] table) {
        custom[i] = table;
        if (table == null) { prefs.edit().remove("drawn_" + i).apply(); return; }
        StringBuilder sb = new StringBuilder();
        for (int v = 0; v < 128; v++) { if (v > 0) sb.append(','); sb.append(table[v]); }
        prefs.edit().putString("drawn_" + i, sb.toString()).apply();
    }

    /** Back to the linear curve: output 1..127 with no slope. */
    void resetCurve(int i) { outMin[i] = 1; outMax[i] = 127; slope[i] = 0f; setCustom(i, null); }
}
