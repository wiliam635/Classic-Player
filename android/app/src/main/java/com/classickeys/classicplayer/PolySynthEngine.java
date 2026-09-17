package com.classickeys.classicplayer;

import android.media.AudioAttributes;
import android.media.AudioDeviceInfo;
import android.media.AudioFormat;
import android.media.AudioTrack;
import android.os.Build;
import android.os.Process;

/** Native SoundFont renderer used by all six Android mixer layers. */
final class PolySynthEngine {
    private static final int RATE = 48000;
    // 128 frames at 48 kHz is 2.67 ms. The previous 512-frame render block,
    // combined with a doubled platform buffer, was noticeably slow on tablets.
    // 256 frames is still low latency (5.3 ms at 48 kHz), but gives slower
    // Android tablets enough time to render layered SoundFonts without gaps.
    private static final int FRAMES = 256;
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
    private static native void nativeActivateHammond(int layer);
    private static native int nativeHammondPresetCount();
    private static native String nativeHammondPresetName(int preset);
    private static native boolean nativeSetHammondPreset(int layer, int preset);
    private static native void nativeUnloadAll();
    private static native void nativeClearLayer(int layer);
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
        AudioTrack.Builder builder = new AudioTrack.Builder()
                .setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_GAME)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
                .setAudioFormat(new AudioFormat.Builder().setSampleRate(RATE)
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO).build())
                .setBufferSizeInBytes(Math.max(min, FRAMES * 4))
                .setTransferMode(AudioTrack.MODE_STREAM);
        if (Build.VERSION.SDK_INT >= 26)
            builder.setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY);
        track = builder.build();
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
    void activateHammond(int layer) { nativeActivateHammond(layer); }
    int hammondPresetCount() { return nativeHammondPresetCount(); }
    String hammondPresetName(int preset) { return nativeHammondPresetName(preset); }
    boolean setHammondPreset(int layer, int preset) { return nativeSetHammondPreset(layer, preset); }
    void setMaster(float value) { nativeSetMaster(value); }
    void clearLayer(int layer) { nativeClearLayer(layer); }
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
        Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO);
        short[] output = new short[FRAMES * 2];
        while (running) {
            nativeRender(output, FRAMES);
            AudioTrack current = track;
            if (current != null) current.write(output, 0, output.length, AudioTrack.WRITE_BLOCKING);
        }
    }
}
