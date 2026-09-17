package com.classickeys.classicplayer;

import android.media.AudioAttributes;
import android.media.AudioDeviceInfo;
import android.media.AudioFormat;
import android.media.AudioTrack;

/** Native SoundFont renderer used by all six Android mixer layers. */
final class PolySynthEngine {
    private static final int RATE = 48000;
    private static final int FRAMES = 512;
    private AudioTrack track;
    private Thread renderThread;
    private volatile boolean running;

    static { System.loadLibrary("classic_player_native"); }

    private static native boolean nativeLoadLayer(int layer, String absolutePath);
    private static native boolean nativeLoadDx7(int layer, String absolutePath);
    private static native int nativeEngineType(int layer);
    private static native int nativeDx7PatchCount(int layer);
    private static native String nativeDx7PatchName(int layer, int patch);
    private static native boolean nativeSetDx7Patch(int layer, int patch);
    private static native void nativeActivateAnalog(int layer);
    private static native int nativeAnalogPresetCount();
    private static native String nativeAnalogPresetName(int preset);
    private static native boolean nativeSetAnalogPreset(int layer, int preset);
    private static native void nativeUnloadAll();
    private static native void nativeSetMaster(float value);
    private static native void nativeSetLayerGain(int layer, float value);
    private static native int nativePresetCount(int layer);
    private static native String nativePresetName(int layer, int preset);
    private static native boolean nativeSetPreset(int layer, int preset);
    private static native void nativeNoteOn(int note, int velocity);
    private static native void nativeNoteOff(int note);
    private static native void nativeControl(int controller, int value);
    private static native void nativeAllNotesOff();
    private static native void nativeRender(short[] output, int frames);
    private static native float nativeLayerPeak(int layer);
    private static native float nativeMasterPeak();

    void start() {
        if (running) return;
        int min = AudioTrack.getMinBufferSize(RATE, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT);
        track = new AudioTrack(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_GAME)
                .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build(),
                new AudioFormat.Builder().setSampleRate(RATE).setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO).build(), Math.max(min * 2, FRAMES * 4),
                AudioTrack.MODE_STREAM, AudioTrack.WRITE_BLOCKING);
        running = true;
        track.play();
        renderThread = new Thread(this::render, "classic-sf2-audio");
        renderThread.setPriority(Thread.MAX_PRIORITY);
        renderThread.start();
    }

    void stop() {
        running = false;
        if (renderThread != null) try { renderThread.join(500); } catch (InterruptedException ignored) { }
        renderThread = null;
        if (track != null) { track.pause(); track.flush(); track.release(); track = null; }
    }

    boolean loadLayer(int layer, String absolutePath) { return absolutePath != null && nativeLoadLayer(layer, absolutePath); }
    boolean loadDx7(int layer, String absolutePath) { return absolutePath != null && nativeLoadDx7(layer, absolutePath); }
    int engineType(int layer) { return nativeEngineType(layer); }
    int dx7PatchCount(int layer) { return nativeDx7PatchCount(layer); }
    String dx7PatchName(int layer, int patch) { return nativeDx7PatchName(layer, patch); }
    boolean setDx7Patch(int layer, int patch) { return nativeSetDx7Patch(layer, patch); }
    void activateAnalog(int layer) { nativeActivateAnalog(layer); }
    int analogPresetCount() { return nativeAnalogPresetCount(); }
    String analogPresetName(int preset) { return nativeAnalogPresetName(preset); }
    boolean setAnalogPreset(int layer, int preset) { return nativeSetAnalogPreset(layer, preset); }
    void setMaster(float value) { nativeSetMaster(value); }
    void setLayerGain(int layer, float value) { nativeSetLayerGain(layer, value); }
    int presetCount(int layer) { return nativePresetCount(layer); }
    String presetName(int layer, int preset) { return nativePresetName(layer, preset); }
    boolean setPreset(int layer, int preset) { return nativeSetPreset(layer, preset); }
    boolean setPreferredDevice(AudioDeviceInfo device) { return track != null && device != null && track.setPreferredDevice(device); }
    void noteOn(int note, int velocity) { nativeNoteOn(note, velocity); }
    void noteOff(int note) { nativeNoteOff(note); }
    void setSustain(boolean on) { nativeControl(64, on ? 127 : 0); }
    void allNotesOff() { nativeAllNotesOff(); }
    float layerPeak(int layer) { return nativeLayerPeak(layer); }
    float masterPeak() { return nativeMasterPeak(); }
    void close() { stop(); nativeUnloadAll(); }

    private void render() {
        short[] output = new short[FRAMES * 2];
        while (running) {
            nativeRender(output, FRAMES);
            AudioTrack current = track;
            if (current != null) current.write(output, 0, output.length, AudioTrack.WRITE_BLOCKING);
        }
    }
}
