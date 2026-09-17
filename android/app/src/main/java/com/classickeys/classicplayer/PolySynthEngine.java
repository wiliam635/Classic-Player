package com.classickeys.classicplayer;

import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioTrack;
import android.media.AudioDeviceInfo;

/** Small, dependency-free polyphonic monitor used while the SoundFont engine is integrated. */
final class PolySynthEngine {
    private static final int RATE = 48000;
    // Matches the SoundFont engine used by the Windows/macOS builds.
    private static final int VOICES = 512;
    private final Voice[] voices = new Voice[VOICES];
    private final float[] layerGain = {1f, 1f, 1f, 1f, 1f, 1f};
    private AudioTrack track;
    private Thread renderThread;
    private volatile boolean running;
    private volatile float master = 0.5f;
    private volatile boolean sustain;
    private long serial;

    PolySynthEngine() { for (int i = 0; i < VOICES; i++) voices[i] = new Voice(); }

    void start() {
        if (running) return;
        int min = AudioTrack.getMinBufferSize(RATE, AudioFormat.CHANNEL_OUT_STEREO,
                AudioFormat.ENCODING_PCM_16BIT);
        track = new AudioTrack(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build(),
                new AudioFormat.Builder().setSampleRate(RATE).setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO).build(), Math.max(min, RATE / 5),
                AudioTrack.MODE_STREAM, AudioTrack.WRITE_NON_BLOCKING);
        running = true;
        track.play();
        renderThread = new Thread(this::render, "classic-audio");
        renderThread.start();
    }

    void stop() {
        running = false;
        if (renderThread != null) try { renderThread.join(300); } catch (InterruptedException ignored) { }
        if (track != null) { track.pause(); track.flush(); track.release(); track = null; }
    }

    void setMaster(float value) { master = Math.max(0f, Math.min(1f, value)); }
    synchronized void setLayerGain(int layer, float value) {
        if (layer >= 0 && layer < layerGain.length) layerGain[layer] = Math.max(0f, Math.min(1f, value));
    }
    boolean setPreferredDevice(AudioDeviceInfo device) {
        return track != null && device != null && track.setPreferredDevice(device);
    }
    synchronized void noteOn(int note, int velocity) {
        if (velocity <= 0) { noteOff(note); return; }
        Voice target = null;
        for (Voice v : voices) if (v.active && v.note == note) { target = v; break; }
        if (target == null) for (Voice v : voices) if (!v.active) { target = v; break; }
        if (target == null) {
            target = voices[0];
            for (Voice v : voices) if (v.serial < target.serial) target = v;
        }
        target.note = note; target.phase = 0; target.level = velocity / 127f;
        target.serial = ++serial; target.down = true; target.active = true;
    }
    synchronized void noteOff(int note) {
        for (Voice v : voices) if (v.active && v.note == note) {
            v.down = false;
            if (!sustain) v.active = false;
        }
    }
    synchronized void setSustain(boolean on) {
        sustain = on;
        if (!on) for (Voice v : voices) if (v.active && !v.down) v.active = false;
    }
    synchronized void allNotesOff() { for (Voice v : voices) v.active = false; }

    private void render() {
        short[] out = new short[1024 * 2];
        while (running) {
            for (int i = 0; i < 1024; i++) {
                float sample = 0;
                for (Voice v : voices) if (v.active) {
                    sample += (float)Math.sin(v.phase) * v.level * 0.08f * layerGain[0];
                    v.phase += 2 * Math.PI * Math.pow(2, (v.note - 69) / 12.0) / RATE;
                    if (v.phase > 2 * Math.PI) v.phase -= 2 * Math.PI;
                }
                short s = (short)(Math.max(-1f, Math.min(1f, sample * master)) * 32767);
                out[i * 2] = s; out[i * 2 + 1] = s;
            }
            if (track != null) track.write(out, 0, out.length, AudioTrack.WRITE_NON_BLOCKING);
        }
    }

    private static final class Voice { int note; double phase; float level; long serial; boolean active; boolean down; }
}
