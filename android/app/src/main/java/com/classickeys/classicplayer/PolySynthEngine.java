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
    private volatile AudioTrack track;
    private Thread renderThread;
    private volatile boolean running;
    private int bufferFrames = 512;
    private volatile AudioDeviceInfo preferredDevice;
    private volatile String outputError = "";

    int bufferFrames() { return bufferFrames; }
    synchronized void setBufferFrames(int frames) {
        if(frames!=128&&frames!=256&&frames!=512&&frames!=1024&&frames!=2048)
            throw new IllegalArgumentException("Invalid buffer size");
        boolean restart=running;
        stop(); bufferFrames=frames;
        if(restart)start();
    }
    String outputStatus() {
        AudioTrack current=track;
        try { return current==null ? "Áudio parado "+outputError :
            "Buffer real: "+current.getBufferSizeInFrames()+" frames"+
            (Build.VERSION.SDK_INT>=24?" · underruns: "+current.getUnderrunCount():"");
        } catch(IllegalStateException e) { return "Reconectando áudio"; }
    }
    boolean isRunning() { return running; }

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
    private static native void nativeSetAnalogControls(int layer,float[] values,int[] flags);
    private static native void nativeActivateHammond(int layer);
    private static native int nativeHammondPresetCount();
    private static native String nativeHammondPresetName(int preset);
    private static native boolean nativeSetHammondPreset(int layer, int preset);
    private static native void nativeSetHammondControls(int layer, int[] bars, int leslie, int percussion, float click, float leakage, float drive, float level);
    private static native void nativeUnloadAll();
    private static native void nativeClearLayer(int layer);
    private static native void nativeSetMaster(float value);
    private static native void nativeSetLayerGain(int layer, float value);
    private static native void nativeSetLayerPan(int layer,float value);
    private static native void nativeSetLayerEnvelope(int layer, float attack, float release);
    private static native void nativeSetLayerTone(int layer,float cutoff,float reverb,float compMix,float chorus);
    private static native void nativeSetLayerEq(int layer,float lowDb,float midDb,float highDb,float lowFrequency,float midFrequency,float highFrequency,float lowQ,float midQ,float highQ,float highPassHz,float lowPassHz);
    private static native void nativeSetLayerCompressor(int layer,float threshold,float ratio,float attackMs,float releaseMs,float makeupDb);
    private static native void nativeSetLayerReverb(int layer,float size,float damping,float width);
    private static native void nativeSetLayerRouting(int layer,int channel,int octave,int lowNote,int highNote,int velocityCurve,boolean sustainEnabled,int mode);
    private static native void nativeSetMasterEffects(float reverb, float chorus);
    private static native int nativePresetCount(int layer);
    private static native String nativePresetName(int layer, int preset);
    private static native boolean nativeSetPreset(int layer, int preset);
    private static native void nativeNoteOn(int note, int velocity,int channel);
    private static native void nativeNoteOff(int note,int channel);
    private static native void nativeControl(int controller, int value,int channel);
    private static native void nativeAllNotesOff();
    private static native void nativeRender(short[] output, int frames);
    private static native float nativeLayerPeak(int layer);
    private static native float nativeMasterPeak();
    private static native int nativeActiveVoices(int layer);
    int activeVoices(int layer) { return nativeActiveVoices(layer); }

    synchronized void start() {
        if (running) return;
        stop();
        track = createTrack();
        outputError="";
        running = true;
        renderThread = new Thread(this::render, "classic-sf2-audio");
        renderThread.setPriority(Thread.MAX_PRIORITY);
        renderThread.start();
    }

    private AudioTrack createTrack() {
        int min = AudioTrack.getMinBufferSize(RATE, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT);
        AudioTrack.Builder builder = new AudioTrack.Builder()
                // MEDIA is routed to USB Audio Class interfaces by Android's
                // normal media policy; GAME can remain pinned to the speaker
                // on several devices even when setPreferredDevice succeeds.
                .setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
                .setAudioFormat(new AudioFormat.Builder().setSampleRate(RATE)
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO).build())
                .setBufferSizeInBytes(Math.max(min, bufferFrames * 4))
                .setTransferMode(AudioTrack.MODE_STREAM);
        if (Build.VERSION.SDK_INT >= 26)
            builder.setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY);
        AudioTrack result = builder.build();
        if(preferredDevice!=null)result.setPreferredDevice(preferredDevice);
        if(Build.VERSION.SDK_INT>=24)result.setBufferSizeInFrames(bufferFrames);
        return result;
    }

    synchronized void stop() {
        running = false;
        // Unblock WRITE_BLOCKING before joining. Never release a track while
        // its writer is still alive, nor let an old writer join a new session.
        AudioTrack current=track;
        if(current!=null)try { current.pause(); } catch(IllegalStateException ignored) { }
        boolean interrupted=false;
        if(renderThread!=null)while(renderThread.isAlive())try { renderThread.join(); }
            catch(InterruptedException e){interrupted=true;}
        renderThread = null;
        if(interrupted)Thread.currentThread().interrupt();
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
    void setAnalogControls(int layer,float[] values,int[] flags){nativeSetAnalogControls(layer,values,flags);}
    void activateHammond(int layer) { nativeActivateHammond(layer); }
    int hammondPresetCount() { return nativeHammondPresetCount(); }
    String hammondPresetName(int preset) { return nativeHammondPresetName(preset); }
    boolean setHammondPreset(int layer, int preset) { return nativeSetHammondPreset(layer, preset); }
    void setHammondControls(int layer,int[] bars,int leslie,int percussion,float click,float leakage,float drive,float level){nativeSetHammondControls(layer,bars,leslie,percussion,click,leakage,drive,level);}
    void setMaster(float value) { nativeSetMaster(value); }
    void clearLayer(int layer) { nativeClearLayer(layer); }
    void setLayerGain(int layer, float value) { nativeSetLayerGain(layer, value); }
    void setLayerPan(int layer,float value){nativeSetLayerPan(layer,value);}
    void setLayerEnvelope(int layer, float attack, float release) { nativeSetLayerEnvelope(layer, attack, release); }
    void setLayerTone(int layer,float cutoff,float reverb,float compMix,float chorus){nativeSetLayerTone(layer,cutoff,reverb,compMix,chorus);}
    void setLayerEq(int layer,float low,float mid,float high,float lowFrequency,float midFrequency,float highFrequency,float lowQ,float midQ,float highQ,float highPassHz,float lowPassHz){nativeSetLayerEq(layer,low,mid,high,lowFrequency,midFrequency,highFrequency,lowQ,midQ,highQ,highPassHz,lowPassHz);}
    void setLayerCompressor(int layer,float threshold,float ratio,float attack,float release,float makeup){nativeSetLayerCompressor(layer,threshold,ratio,attack,release,makeup);}
    void setLayerReverb(int layer,float size,float damping,float width){nativeSetLayerReverb(layer,size,damping,width);}
    void setLayerRouting(int layer,int channel,int octave,int low,int high,int velocityCurve,boolean sustain,int mode){nativeSetLayerRouting(layer,channel,octave,low,high,velocityCurve,sustain,mode);}
    void setMasterEffects(float reverb, float chorus) { nativeSetMasterEffects(reverb, chorus); }
    int presetCount(int layer) { return nativePresetCount(layer); }
    String presetName(int layer, int preset) { return nativePresetName(layer, preset); }
    boolean setPreset(int layer, int preset) { return nativeSetPreset(layer, preset); }
    boolean setPreferredDevice(AudioDeviceInfo device) {
        preferredDevice=device;AudioTrack current=track;
        try{return current!=null&&current.setPreferredDevice(device);}catch(IllegalStateException e){return false;}
    }
    AudioDeviceInfo routedDevice() { return track == null ? null : track.getRoutedDevice(); }
    void noteOn(int note, int velocity,int channel) { nativeNoteOn(note, velocity,channel); }
    void noteOff(int note,int channel) { nativeNoteOff(note,channel); }
    void control(int controller,int value,int channel){nativeControl(controller,value,channel);}
    void setSustain(int channel,boolean on) { nativeControl(64, on ? 127 : 0,channel); }
    void allNotesOff() { nativeAllNotesOff(); }
    float layerPeak(int layer) { return nativeLayerPeak(layer); }
    float masterPeak() { return nativeMasterPeak(); }
    void close() { stop(); nativeUnloadAll(); }

    private void render() {
        Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO);
        short[] output = new short[FRAMES * 2];
        AudioTrack current=track;
        int failures=0;
        try {
        current.play();
        while (running) {
            nativeRender(output, FRAMES);
            if (current != null) {
                int offset = 0;
                while (running && offset < output.length) {
                    int written = current.write(output, offset, output.length - offset, AudioTrack.WRITE_BLOCKING);
                    if (written <= 0) {
                        if(!running)break;
                        android.util.Log.e("ClassicAudio", "AudioTrack write failed: " + written);
                        if(++failures>3)throw new IllegalStateException("AudioTrack error "+written);
                        current.release(); current=null; track=null;
                        if(!running)break;
                        current=createTrack();track=current;current.play();
                        break;
                    }
                    failures=0;
                    offset += written;
                }
            }
        }
        } catch(RuntimeException e) {
            outputError=e.toString();android.util.Log.e("ClassicAudio","Output stopped",e);
        } finally {
            running=false;
            if(current!=null)current.release();
            track=null;
        }
    }
}
