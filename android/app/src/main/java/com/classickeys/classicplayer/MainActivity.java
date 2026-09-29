package com.classickeys.classicplayer;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.Dialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.content.pm.ActivityInfo;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.StateListDrawable;
import android.media.AudioDeviceInfo;
import android.media.AudioDeviceCallback;
import android.media.AudioManager;
import android.media.midi.MidiDeviceInfo;
import android.media.midi.MidiDevice;
import android.media.midi.MidiInputPort;
import android.media.midi.MidiOutputPort;
import android.media.midi.MidiManager;
import android.media.midi.MidiReceiver;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.content.Intent;
import android.net.Uri;
import android.provider.Settings;
import android.provider.OpenableColumns;
import android.view.MotionEvent;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.view.Gravity;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ArrayAdapter;
import android.widget.ListView;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.TextView;
import java.io.IOException;
import java.io.InputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.ByteArrayOutputStream;
import java.io.OutputStream;
import org.json.JSONObject;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;

/**
 * First Android surface for Classic Player. The audio monitor runs alongside
 * the UI so MIDI/SoundFont integration can be tested without blocking it.
 */
public final class MainActivity extends Activity {
    private static final int[] PAD_COLOURS={
            0xfff3e4d8,0xffeef0d8,0xffdff1d7,0xffd8f0e5,
            0xffd9f0f1,0xffdbe8f5,0xffe3def5,0xffefdff1,
            0xfff5e1d5,0xffe7edcf,0xffd8eceb,0xffe9e0f5
    };
    private static String padSampleKey(boolean continuous,int pad){return (continuous?"pad_continuous_":"pad_drum_")+pad;}
    private static String padNameKey(boolean continuous,int pad){return (continuous?"pad_continuous_name_":"pad_drum_name_")+pad;}
    private static int padColour(int pad){return PAD_COLOURS[Math.floorMod(pad,PAD_COLOURS.length)];}
    private ClassicPlayerView screen;
    private MidiManager midiManager;
    private PolySynthEngine audioEngine;
    private MidiDevice midiDevice;
    private MidiOutputPort midiInput;
    private int pendingLayer = -1;
    private int pendingEngine = 1;
    private final java.util.concurrent.ExecutorService fontImporter = java.util.concurrent.Executors.newSingleThreadExecutor();
    private final Object importLifecycleLock = new Object();
    private final java.util.concurrent.atomic.AtomicIntegerArray muteGenerations = new java.util.concurrent.atomic.AtomicIntegerArray(6);
    private volatile boolean closing;
    private final String[] sf2Uris = new String[6];
    private SoundFontLayer[] soundFontLayers;
    private LicenseManager licenseManager;
    private AudioOutputManager audioOutputManager;
    private final AudioDeviceCallback audioDeviceCallback = new AudioDeviceCallback() {
        @Override public void onAudioDevicesAdded(AudioDeviceInfo[] added) { refreshMidiDevices(); restorePreferredAudioDevice(); if(screen!=null)screen.postDelayed(MainActivity.this::restorePreferredAudioDevice,1000); }
        @Override public void onAudioDevicesRemoved(AudioDeviceInfo[] removed) { refreshMidiDevices(); restorePreferredAudioDevice(); }
    };
    private PadEngine padEngine;
    private int pendingPad = -1;
    private boolean pendingContinuous;
    private boolean padLayerActive;
    private boolean continuousPadActive;
    private int pendingLearnTarget = -1;
    private volatile int pendingLayerLearn = -1,pendingLayerLearnTarget = -1;
    private Button pendingLayerLearnButton;
    private volatile int pendingPadMidiLearn=-1,pendingPadCcLearn=-1;
    private int padLayerIndex=-1;
    private String pendingEffectPresetJson;
    private int pendingEffectPresetLayer=-1;
    private String pendingEffectPresetType="";
    private String pendingLayerPresetJson;
    private int pendingLayerPresetLayer=-1;
    private int midiIndex;
    private int midiRunningStatus;
    private int midiFirstData = -1;
    private boolean isPadEngine(int layer){return screen!=null&&layer>=0&&layer<6&&screen.engineNames[layer].contains("PADS");}
    // Keep the same per-channel key state used by the desktop engines. It is
    // essential for distinguishing a real release from a sustain-pedal change.
    private final boolean[][] midiHeld = new boolean[16][128];
    private final boolean[] midiSustain = new boolean[16];
    private final boolean[][] padCcDown=new boolean[16][128];
    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private final MidiReceiver midiReceiver = new MidiReceiver() {
        @Override public synchronized void onSend(byte[] data, int offset, int count, long timestamp) {
            final int end = offset + count;
            for (int i = offset; i < end; i++) {
                int value = data[i] & 0xff;
                if (value >= 0xf8) continue; // MIDI realtime may appear between data bytes.
                if ((value & 0x80) != 0) {
                    if (value < 0xf0) midiRunningStatus = value;
                    else midiRunningStatus = 0;
                    midiFirstData = -1;
                    continue;
                }
                if (midiRunningStatus == 0) continue;
                int type = midiRunningStatus & 0xf0;
                int required = (type == 0xc0 || type == 0xd0) ? 1 : 2;
                if (required == 1) continue; // Program/pressure do not drive a note here.
                if (midiFirstData < 0) { midiFirstData = value & 0x7f; continue; }
                int first = midiFirstData;
                midiFirstData = -1;
                processMidiMessage(type, midiRunningStatus & 0x0f, first, value & 0x7f);
            }
        }
        @Override public void onFlush() { panicMidiState(); }
    };

    private void processMidiMessage(int type, int channel, int first, int second) {
        if (audioEngine == null) return;
        if (type == 0xb0) {
            screen.setMidiSignal();
            screen.setLastMidiControl(first,channel,second);
            if(pendingLayerLearn>=0&&pendingLayerLearnTarget>=0){
                int layer=pendingLayerLearn,target=pendingLayerLearnTarget;
                pendingLayerLearn=-1;pendingLayerLearnTarget=-1;
                boolean relative=(second==1||second==127);
                getSharedPreferences("midi_learn",MODE_PRIVATE).edit().putInt("layer_"+layer+"_"+target,first).putBoolean("layer_"+layer+"_"+target+"_relative",relative).apply();
                Button learnedButton=pendingLayerLearnButton;pendingLayerLearnButton=null;
                runOnUiThread(()->{if(learnedButton!=null)learnedButton.setText("CC "+first);});
                screen.setMidiStatus("MIDI: layer "+(layer+1)+" CC "+first+(relative?" relativo":" absoluto")+" aprendido");return;
            }
            if(pendingPadCcLearn>=0&&first!=64){
                getSharedPreferences("pads",MODE_PRIVATE).edit().putInt(pendingPadCcLearn==12?"pad_stop_cc":"pad_cc_"+pendingPadCcLearn,first).apply();
                screen.setMidiStatus(pendingPadCcLearn==12?"MIDI: STOP CC "+first+" aprendido":"MIDI: PAD "+(pendingPadCcLearn+1)+" CC "+first+" aprendido");pendingPadCcLearn=-1;return;
            }
            // Learn is handled before the reserved MIDI CC filters so STOP
            // can intentionally use CC 120/123 when a controller provides it.
            if (pendingLearnTarget >= 0) {
                getSharedPreferences("midi_learn",MODE_PRIVATE).edit().putInt("cc_"+pendingLearnTarget,first).apply();
                screen.setMidiStatus("MIDI: CC "+first+" aprendido"); pendingLearnTarget=-1; return;
            }
            SharedPreferences layerLearn=getSharedPreferences("midi_learn",MODE_PRIVATE);
            boolean learnedLayerControl=false;
            for(int layer=0;layer<6;layer++)for(int target=0;target<5;target++)
                if(layerLearn.getInt("layer_"+layer+"_"+target,-1)==first){applyLayerMidiLearn(layer,target,second);learnedLayerControl=true;}
            if(learnedLayerControl)return;
            if(padLayerActive&&continuousPadActive&&first!=64){
                SharedPreferences padMap=getSharedPreferences("pads",MODE_PRIVATE);int stopCc=padMap.getInt("pad_stop_cc",-1);
                boolean down=second>=64,edge=down&&!padCcDown[channel][first];padCcDown[channel][first]=down;
                if(first==stopCc&&down){padEngine.stopAll(true);screen.setMidiStatus("MIDI: pads parados");return;}
                if(edge)for(int pad=0;pad<12;pad++)if(padMap.getInt("pad_cc_"+pad,-1)==first){padEngine.trigger(pad,true);return;}
            }
            if(first!=64&&first!=120&&first!=123){
                SharedPreferences learn=getSharedPreferences("midi_learn",MODE_PRIVATE);for(int target=0;target<8;target++)if(learn.getInt("cc_"+target,-1)==first){if(target==7){padEngine.stopAll();screen.setMidiStatus("MIDI: pads parados");}else screen.setLearnedVolume(target,second/127f);return;}
            } else if (getSharedPreferences("midi_learn",MODE_PRIVATE).getInt("cc_7",-1)==first) {
                padEngine.stopAll(); screen.setMidiStatus("MIDI: pads parados"); return;
            }
            if (first == 64) {
                boolean wasDown = midiSustain[channel];
                midiSustain[channel] = second >= 64;
                audioEngine.setSustain(channel,midiSustain[channel]);
                // Releasing the pedal must release every key that is no
                // longer physically held. The native engines are channel
                // agnostic, so send the complete pending note set.
                if (wasDown && !midiSustain[channel])
                    for (int note = 0; note < 128; note++)
                        if (!midiHeld[channel][note]) audioEngine.noteOff(note,channel);
            } else if (first == 120 || first == 123) {
                panicMidiState();
            } else audioEngine.control(first,second,channel);
            return;
        }
        if (type != 0x80 && type != 0x90) return;
        screen.setMidiSignal();
        if (type == 0x90 && second > 0) {
            if(pendingPadMidiLearn>=0){getSharedPreferences("pads",MODE_PRIVATE).edit().putInt("pad_note_"+pendingPadMidiLearn,first).apply();screen.setMidiStatus("MIDI: PAD "+(pendingPadMidiLearn+1)+" nota "+first+" aprendida");pendingPadMidiLearn=-1;return;}
            // Some controllers resend Note On before a missing release. Kill
            // the previous instance first so the voice can never accumulate.
            if (midiHeld[channel][first]) audioEngine.noteOff(first,channel);
            midiHeld[channel][first] = true;
            if(padLayerActive){SharedPreferences padMap=getSharedPreferences("pads",MODE_PRIVATE);for(int pad=0;pad<(continuousPadActive?12:8);pad++)if(padMap.getInt("pad_note_"+pad,36+pad)==first){padEngine.trigger(pad,continuousPadActive);break;}}
            audioEngine.noteOn(first, second,channel);
        }
        else {
            // Note On with velocity zero is the MIDI-standard equivalent of
            // Note Off. Both paths must reach every internal engine.
            midiHeld[channel][first] = false;
            audioEngine.noteOff(first,channel);
            screen.setLastNoteOff(first);
        }
    }

    private void applyLayerMidiLearn(int layer,int target,int value){
        if(android.os.Looper.myLooper()!=Looper.getMainLooper()){
            mainHandler.post(()->applyLayerMidiLearn(layer,target,value));return;
        }
        SharedPreferences mappings=getSharedPreferences("midi_learn",MODE_PRIVATE);
        boolean relative=target!=4&&mappings.getBoolean("layer_"+layer+"_"+target+"_relative",false);
        float step=relative?(value>0&&value<64?value:value>64?-(128-value):0)/127f:0;
        switch(target){
            case 0:screen.setLearnedVolume(layer,relative?clamp01(screen.layerVolumes[layer]+step):value/127f);break;
            case 1:screen.setLayerTone(layer,relative?clamp100(screen.layerCutoff[layer]+step*100):value*100f/127f,screen.layerReverb[layer],screen.layerCompMix[layer],screen.layerChorus[layer]);break;
            case 2:screen.setLayerTone(layer,screen.layerCutoff[layer],relative?clamp01(screen.layerReverb[layer]+step):value/127f,screen.layerCompMix[layer],screen.layerChorus[layer]);break;
            case 3:screen.setLayerTone(layer,screen.layerCutoff[layer],screen.layerReverb[layer],relative?clamp01(screen.layerCompMix[layer]+step):value/127f,screen.layerChorus[layer]);break;
            case 4:setLayerMuted(layer,value>=64);break;
        }
        screen.setMidiStatus("MIDI: CC "+(relative?"relativo":"aplicado")+" à layer "+(layer+1)+" · valor "+value);
    }

    private static float clamp01(float value){return Math.max(0f,Math.min(1f,value));}
    private static float clamp100(float value){return Math.max(0f,Math.min(100f,value));}

    private void panicMidiState() {
        for (int channel = 0; channel < 16; channel++)
            java.util.Arrays.fill(midiHeld[channel], false);
        java.util.Arrays.fill(midiSustain, false);
        if (audioEngine != null) audioEngine.allNotesOff();
    }
    private final MidiManager.DeviceCallback midiCallback = new MidiManager.DeviceCallback() {
        @Override public void onDeviceAdded(MidiDeviceInfo device) { refreshMidiDevices(); if(screen!=null)screen.postDelayed(MainActivity.this::restorePreferredAudioDevice,1000); }
        @Override public void onDeviceRemoved(MidiDeviceInfo device) {
            if(midiDevice!=null&&midiDevice.getInfo().getId()==device.getId())closeMidi();
            refreshMidiDevices();
        }
    };

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
        final Window window = getWindow();
        window.setFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON,
                WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        screen = new ClassicPlayerView(this);
        setContentView(screen);
        midiManager = (MidiManager) getSystemService(MIDI_SERVICE);
        audioEngine = new PolySynthEngine(this);
        audioEngine.setBufferFrames(getSharedPreferences("audio",MODE_PRIVATE).getInt("buffer_frames",512));
        audioEngine.setMaster(screen.faderGain(screen.masterVolume));
        audioEngine.setMasterEffects(screen.masterReverb,screen.masterChorus);
        licenseManager = new LicenseManager(this);
        audioOutputManager = new AudioOutputManager(this);
        padEngine = new PadEngine(this);
        padEngine.setMaster(screen.faderGain(screen.masterVolume));
        SharedPreferences padPrefs=getSharedPreferences("pads",MODE_PRIVATE);
        padEngine.setFadeSeconds(padPrefs.getFloat("crossfade_seconds",1f));
        SharedPreferences storedLayers=getSharedPreferences("layers",MODE_PRIVATE);
        boolean legacyContinuous=false;
        for(int i=0;i<6;i++){int engine=storedLayers.getInt("engine_"+i,0);if(engine==5||engine==6)legacyContinuous=engine==6;}
        SharedPreferences.Editor padMigration=padPrefs.edit();
        for(int p=0;p<12;p++)if(padPrefs.contains("pad_"+p)){
            String legacy=padPrefs.getString("pad_"+p,null);String target=padSampleKey(legacyContinuous,p);
            if(legacy!=null&&!padPrefs.contains(target))padMigration.putString(target,legacy);
            padMigration.remove("pad_"+p);
        }
        padMigration.apply();
        for(int p=0;p<12;p++){
            String drumPath=padPrefs.getString(padSampleKey(false,p),null);if(drumPath!=null)padEngine.load(p,drumPath,false,padPrefs.getString(padNameKey(false,p),null));
            String continuousPath=padPrefs.getString(padSampleKey(true,p),null);if(continuousPath!=null)padEngine.load(p,continuousPath,true,padPrefs.getString(padNameKey(true,p),null));
        }
        soundFontLayers = new SoundFontLayer[6];
        for (int i = 0; i < soundFontLayers.length; i++) soundFontLayers[i] = new SoundFontLayer(this);
        android.content.SharedPreferences prefs = getSharedPreferences("layers", MODE_PRIVATE);
        for (int i = 0; i < sf2Uris.length; i++) {
            int engine = prefs.getInt("engine_" + i, 1);
            boolean muted=screen.muted[i];
            if (engine == 2) {
                String dx7Path=prefs.getString("dx7_"+i,null);
                String dx7Name=prefs.getString("name_"+i,"Banco DX7");
                screen.setLayerName(i,dx7Name); screen.setEngineName(i,"DX7");
                if(!muted&&dx7Path!=null&&audioEngine.loadDx7(i,dx7Path)){
                    int patch=prefs.getInt("dx7_patch_"+i,0); audioEngine.setDx7Patch(i,patch);
                    screen.setPresetName(i,audioEngine.dx7PatchName(i,patch));
                }else screen.setPresetName(i,muted?"MUTE · DX7":"");
                continue;
            }
            if(engine==3){
                int preset=prefs.getInt("analog_preset_"+i,0);
                if(!muted){audioEngine.activateAnalog(i);audioEngine.setAnalogPreset(i,preset);}
                screen.setLayerName(i,"Classic Keys Analog"); screen.setEngineName(i,"ANALOG"); screen.setPresetName(i,(muted?"MUTE · ":"")+audioEngine.analogPresetName(preset));
                continue;
            }
            if(engine==4){
                int preset=prefs.getInt("hammond_preset_"+i,0);
                if(!muted){audioEngine.activateHammond(i);audioEngine.setHammondPreset(i,preset);}
                screen.setLayerName(i,"Classic Keys Hammond"); screen.setEngineName(i,"HAMMOND"); screen.setPresetName(i,(muted?"MUTE · ":"")+audioEngine.hammondPresetName(preset));
                if(!prefs.contains("hammond_bar_"+i+"_0"))screen.setHammondPresetDefaults(i,preset);
                continue;
            }
            if(engine==5||engine==6){
                padLayerActive=true;continuousPadActive=engine==6;padLayerIndex=i;padEngine.setEnabled(!muted,engine==6);audioEngine.clearLayer(i);screen.setLayerName(i,engine==6?"Pads contínuos":"Drum Pads");screen.setEngineName(i,engine==6?"CONT. PADS":"DRUM PADS");screen.setPresetName(i,(muted?"MUTE · ":"")+(continuousPadActive?"12 pads":"8 pads"));continue;
            }
            sf2Uris[i] = prefs.getString("sf2_" + i, null);
            String name = displaySafeSoundFontName(prefs.getString("name_" + i, null));
            if (sf2Uris[i] != null) {
                Uri saved = Uri.parse(sf2Uris[i]);
                soundFontLayers[i].load(saved, name);
                screen.setLayerName(i,name==null?"SF2 carregado":name);
                screen.setEngineName(i,"SF2");
                if(muted){
                    soundFontLayers[i].setPreset(prefs.getInt("preset_"+i,0));
                    screen.setPresetName(i,"MUTE · SF2");
                }else{
                    String cachedPath = restoreSoundFont(sf2Uris[i], i);
                    if (cachedPath != null && audioEngine.loadLayer(i, cachedPath)) {
                    int preset = prefs.getInt("preset_" + i, 0);
                    if (audioEngine.setPreset(i, preset)) {
                        soundFontLayers[i].setPreset(preset);
                        screen.setPresetName(i, audioEngine.presetName(i, preset));
                    }
                    }
                }
            } else if (name != null) screen.setLayerName(i, name);
        }
        for(int i=0;i<6;i++)screen.applyLoadedEditorState(i);
        String account = licenseManager.userName();
        if (account == null || account.isEmpty()) account = licenseManager.userEmail();
        if (account != null && !account.isEmpty()) screen.setAccount(account + (licenseManager.userEmail().isEmpty() || account.equals(licenseManager.userEmail()) ? "" : " · " + licenseManager.userEmail()));
        if (!licenseManager.isUsableOffline()) showLoginScreen();
    }

    @Override public void onResume() {
        super.onResume();
        hideSystemBars();
        if (audioEngine != null) audioEngine.start();
        restorePreferredAudioDevice();
        if(screen!=null)screen.postDelayed(this::restorePreferredAudioDevice,1000);
        AudioManager audioManager = (AudioManager) getSystemService(AUDIO_SERVICE);
        if (audioManager != null) audioManager.registerAudioDeviceCallback(audioDeviceCallback, null);
        if (midiManager != null) midiManager.registerDeviceCallback(midiCallback, null);
        refreshMidiDevices();
        if (licenseManager != null && licenseManager.isActivated()) revalidateLicenseAsync();
    }

    @Override public void onPause() {
        AudioManager audioManager = (AudioManager) getSystemService(AUDIO_SERVICE);
        if (audioManager != null) audioManager.unregisterAudioDeviceCallback(audioDeviceCallback);
        if (midiManager != null) midiManager.unregisterDeviceCallback(midiCallback);
        if (audioEngine != null) audioEngine.allNotesOff();
        if (audioEngine != null) audioEngine.stop();
        if (padEngine != null) padEngine.stopImmediately();
        closeMidi();
        super.onPause();
    }

    @Override protected void onDestroy() {
        if (padEngine != null) padEngine.stopImmediately();
        closing = true;
        if (audioEngine != null) {
            final PolySynthEngine retiringEngine = audioEngine;
            retiringEngine.stop();
            // Do not let a finishing import publish after native teardown.
            synchronized (importLifecycleLock) { retiringEngine.close(); }
        }
        fontImporter.shutdown();
        super.onDestroy();
    }

    private void hideSystemBars() {
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
    }

    private void refreshMidiDevices() {
        if (screen == null) return;
        MidiDeviceInfo[] devices=midiInputDevices();
        int count=devices.length;
        screen.setMidiStatus(count == 0 ? "MIDI: nenhuma entrada disponível" :
                "MIDI: " + count + (count == 1 ? " entrada" : " entradas"));
        AudioDeviceInfo routed = audioEngine == null ? null : audioEngine.routedDevice();
        screen.setAudioStatus(routed == null ? "ÁUDIO: saída do sistema" : "ÁUDIO: " + routed.getProductName());
        if (count > 0 && midiDevice == null) openMidi(devices[Math.min(midiIndex, count - 1)]);
    }

    private MidiDeviceInfo[] midiInputDevices(){
        if(midiManager==null)return new MidiDeviceInfo[0];
        java.util.ArrayList<MidiDeviceInfo> available=new java.util.ArrayList<>();
        for(MidiDeviceInfo device:midiManager.getDevices())
            for(MidiDeviceInfo.PortInfo port:device.getPorts())
                if(port.getType()==MidiDeviceInfo.PortInfo.TYPE_OUTPUT){available.add(device);break;}
        return available.toArray(new MidiDeviceInfo[0]);
    }

    private void restorePreferredAudioDevice() {
        if (audioOutputManager == null || audioEngine == null) return;
        android.content.SharedPreferences preferences = getSharedPreferences("audio", MODE_PRIVATE);
        int id = preferences.getInt("output_id", -1);
        AudioDeviceInfo device = audioOutputManager.deviceById(id);
        // Android assigns a new transient ID after a USB reconnection.
        String savedName = preferences.getString("output_name", "");
        int savedType = preferences.getInt("output_type", -1);
        String currentName = device == null || device.getProductName() == null
                ? "" : device.getProductName().toString();
        if (!savedName.isEmpty() && (device == null || device.getType() != savedType
                || !savedName.equals(currentName)))
            device = audioOutputManager.deviceByIdentity(savedType, savedName);
        boolean automaticUsbSelection = false;
        // Prefer a connected CK61 over a legacy/default tablet route. Keep an
        // endpoint explicitly selected in this version unless it disappeared.
        if (!preferences.getBoolean("output_user_selected", false) || device == null) {
            AudioDeviceInfo usbOutput = audioOutputManager.automaticUsbOutput();
            if (usbOutput != null) {
                device = usbOutput;
                automaticUsbSelection = true;
            }
        }
        if (device == null) {
            AudioDeviceInfo routed=audioEngine.routedDevice();
            screen.setAudioStatus(routed==null?"ÁUDIO: saída do sistema":"ÁUDIO: "+routed.getProductName());
            return;
        }

        String outputName = device.getProductName() == null
                ? "Saída USB" : device.getProductName().toString();
        if (audioEngine.setPreferredDevice(device)) {
            if (automaticUsbSelection) {
                // Remember the automatically selected USB route like an
                // explicit choice, so connecting another output cannot steal it.
                preferences.edit().putInt("output_id", device.getId())
                        .putInt("output_type", device.getType())
                        .putString("output_name", outputName)
                        .putBoolean("output_user_selected", false).apply();
            }
            screen.setAudioStatus("ÁUDIO: " + outputName + " (ID " + device.getId() + ")");
        } else if (device.getType() == AudioDeviceInfo.TYPE_USB_DEVICE
                || device.getType() == AudioDeviceInfo.TYPE_USB_HEADSET) {
            screen.setAudioStatus("ÁUDIO USB detectado · falha ao abrir: " + outputName);
            android.util.Log.w("ClassicAudio", "Could not route audio to USB device "
                    + outputName + " (ID " + device.getId() + ")");
        }
    }

    private void showAudioBufferChooser() {
        final int[] frames={128,256,512,1024,2048};
        String[] labels={"128 frames","256 frames","512 frames","1024 frames","2048 frames"};
        int selected=2;for(int i=0;i<frames.length;i++)if(frames[i]==audioEngine.bufferFrames())selected=i;
        new AlertDialog.Builder(this).setTitle("BUFFER DE SAÍDA")
            .setSingleChoiceItems(labels,selected,(dialog,which)->{
                audioEngine.setBufferFrames(frames[which]);
                getSharedPreferences("audio",MODE_PRIVATE).edit().putInt("buffer_frames",frames[which]).apply();
                screen.invalidate();dialog.dismiss();
            }).setNeutralButton("SOBRE",(dialog,which)->new AlertDialog.Builder(this)
                .setTitle("BUFFER DE SAÍDA")
                .setMessage("Escolha o tamanho inicial pedido. O Android pode arredondá-lo para múltiplos do burst da rota; o tamanho efetivo aparece em ÁUDIO/MIDI. O ajuste automático só aumenta o buffer se houver underruns.")
                .setPositiveButton("FECHAR",null).show())
            .setNegativeButton("CANCELAR",null).show();
    }

    private void showAudioOutputChooser() {
        if (audioOutputManager == null) return;
        java.util.List<String> names = audioOutputManager.outputs();
        if (names.isEmpty()) {
            new AlertDialog.Builder(this).setTitle("SAÍDA DE ÁUDIO")
                    .setMessage("Nenhuma saída de áudio foi encontrada. Reconecte a interface USB e tente novamente.")
                    .setPositiveButton("OK", null).show();
            return;
        }
        new AlertDialog.Builder(this).setTitle("ESCOLHER SAÍDA DE ÁUDIO")
                .setItems(names.toArray(new String[0]), (dialog, which) -> {
                    AudioDeviceInfo device = audioOutputManager.deviceAt(which);
                    if (device != null && audioEngine != null && audioEngine.setPreferredDevice(device)) {
                        getSharedPreferences("audio", MODE_PRIVATE).edit().putInt("output_id", device.getId())
                                .putInt("output_type", device.getType()).putString("output_name", device.getProductName().toString())
                                .putBoolean("output_user_selected", true).apply();
                        screen.setAudioStatus("ÁUDIO: " + names.get(which));
                    } else {
                        new AlertDialog.Builder(this).setTitle("SAÍDA DE ÁUDIO")
                                .setMessage("O Android não permitiu selecionar esta saída. Desconecte e reconecte a interface USB.")
                                .setPositiveButton("OK", null).show();
                    }
                }).setNegativeButton("CANCELAR", null).show();
    }
    private void showMidiLearnChooser(){
        String[] targets={"VOLUME LAYER 1","VOLUME LAYER 2","VOLUME LAYER 3","VOLUME LAYER 4","VOLUME LAYER 5","VOLUME LAYER 6","VOLUME MASTER","PARAR PADS CONTÍNUOS"};
        new AlertDialog.Builder(this).setTitle("MIDI LEARN").setItems(targets,(d,which)->{pendingLearnTarget=which;screen.setMidiStatus("MIDI: mova agora o controle CC · CANCELAR para sair");}).setNeutralButton("LIMPAR MAPEAMENTOS",(d,w)->getSharedPreferences("midi_learn",MODE_PRIVATE).edit().clear().apply()).setNegativeButton("CANCELAR",(d,w)->{pendingLearnTarget=-1;screen.setMidiStatus("MIDI: pronto");}).show();
    }

    private void openMidi(MidiDeviceInfo info) {
        if (midiManager == null) return;
        closeMidi();
        midiManager.openDevice(info, device -> {
            if(device==null){screen.setMidiStatus("MIDI: falha ao abrir entrada");return;}
            midiDevice = device;
            MidiDeviceInfo.PortInfo[] ports = info.getPorts();
            for (MidiDeviceInfo.PortInfo port : ports) {
                if (port.getType() == MidiDeviceInfo.PortInfo.TYPE_OUTPUT) {
                    midiInput = device.openOutputPort(port.getPortNumber());
                    if (midiInput != null){midiInput.connect(midiReceiver);screen.setMidiStatus("MIDI: entrada conectada · "+info.getId());}
                    else screen.setMidiStatus("MIDI: porta indisponível · "+info.getId());
                    break;
                }
            }
        }, mainHandler);
    }
    private void showMidiDeviceChooser(){
        if(midiManager==null)return;MidiDeviceInfo[] devices=midiInputDevices();
        if(devices.length==0){new AlertDialog.Builder(this).setTitle("CONTROLADOR MIDI").setMessage("O Android não disponibilizou nenhuma entrada MIDI. Uma interface de áudio USB pode aparecer separadamente sem oferecer uma porta MIDI.").setPositiveButton("OK",null).show();return;}
        String[] names=new String[devices.length];for(int i=0;i<devices.length;i++){String name=devices[i].getProperties().getString(MidiDeviceInfo.PROPERTY_NAME);names[i]=(name==null?"Dispositivo MIDI":name)+" (ID "+devices[i].getId()+")";}
        new AlertDialog.Builder(this).setTitle("ESCOLHER CONTROLADOR MIDI").setItems(names,(d,which)->{midiIndex=which;openMidi(devices[which]);screen.setMidiStatus("MIDI USB: "+names[which]);}).setNegativeButton("CANCELAR",null).show();
    }

    private void closeMidi() {
        closeMidiInput();
        if (midiDevice != null) { try { midiDevice.close(); } catch (IOException ignored) {} midiDevice = null; }
    }

    private void closeMidiInput() {
        panicMidiState();
        midiRunningStatus = 0;
        midiFirstData = -1;
        if (midiInput != null) { try { midiInput.close(); } catch (IOException ignored) {} midiInput = null; }
    }

    private void revalidateLicenseAsync() {
        new Thread(() -> {
            Boolean result = validateOnline();
            if (Boolean.FALSE.equals(result)) runOnUiThread(this::showLoginScreen);
        }, "license-refresh").start();
    }

    /** null means transport failure (keep the offline grace period); false means server rejection. */
    private Boolean validateOnline() {
        try {
            HttpURLConnection c = (HttpURLConnection) new URL("https://licenca.classickeys.com.br/v1/license/validate").openConnection();
            c.setRequestMethod("POST"); c.setConnectTimeout(8000); c.setReadTimeout(8000); c.setDoOutput(true);
            c.setRequestProperty("Content-Type", "application/json"); c.setRequestProperty("Authorization", "Bearer " + licenseManager.accessToken());
            String deviceId = Settings.Secure.getString(getContentResolver(), Settings.Secure.ANDROID_ID);
            if (deviceId == null || deviceId.isEmpty()) deviceId = "android-" + android.os.Build.MODEL;
            String body = "{\"device_id\":\"" + jsonEscape(deviceId) + "\",\"device_name\":\""
                    + jsonEscape(android.os.Build.MANUFACTURER + " " + android.os.Build.MODEL) + "\",\"platform\":\"Android\"}";
            c.getOutputStream().write(body.getBytes(StandardCharsets.UTF_8));
            int code = c.getResponseCode();
            if (code == 401 || code == 403) { licenseManager.clear(); return false; }
            if (code < 200 || code >= 300) return null;
            InputStream in = c.getInputStream(); byte[] bytes = new byte[4096]; int n = in.read(bytes);
            String responseBody = n < 0 ? "" : new String(bytes, 0, n, StandardCharsets.UTF_8);
            if (responseBody.contains("\"valid\":false")) { licenseManager.clear(); return false; }
            licenseManager.refreshOfflineWindow();
            return true;
        } catch (Exception ignored) { return null; }
    }

    private void openSf2Picker(int layer) {
        pendingLayer = layer;
        pendingEngine = 1;
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        // Many Android file managers report .sf2 as octet-stream (or no known
        // MIME type). Showing all files makes SoundFonts on Downloads/USB visible.
        i.setType("*/*");
        i.putExtra(Intent.EXTRA_MIME_TYPES, new String[]{
                "audio/x-soundfont", "audio/sf2", "application/octet-stream"
        });
        startActivityForResult(i, 700);
    }

    private void openDx7Picker(int layer) {
        final String[] choices = {"BANCO DX7 1 · DIVINE MASQUERADE", "BANCO DX7 2 · DIVINE MASQUERADE", "IMPORTAR BANCO .SYX"};
        new AlertDialog.Builder(this).setTitle("BANCO DE TIMBRES DX7").setItems(choices, (dialog, which) -> {
            if (which == 0) { activateBundledDx7(layer, R.raw.dx7_bank_1, "Divine Masquerade 1"); if(audioEngine.dx7PatchCount(layer)>0)openDx7Editor(layer); }
            else if (which == 1) { activateBundledDx7(layer, R.raw.dx7_bank_2, "Divine Masquerade 2"); if(audioEngine.dx7PatchCount(layer)>0)openDx7Editor(layer); }
            else openExternalDx7Picker(layer);
        }).setNegativeButton("CANCELAR", null).show();
    }

    private void openExternalDx7Picker(int layer) {
        pendingLayer = layer; pendingEngine = 2;
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE); i.setType("application/octet-stream");
        startActivityForResult(i, 701);
    }

    private void activateBundledDx7(int layer, int resourceId, String displayName) {
        File target = new File(getFilesDir(), "dx7_" + layer + "_" + resourceId + ".syx");
        try (InputStream in = getResources().openRawResource(resourceId);
             FileOutputStream out = new FileOutputStream(target)) {
            byte[] buffer = new byte[4096]; int n;
            while ((n = in.read(buffer)) > 0) out.write(buffer, 0, n);
        } catch (Exception e) {
            screen.setAudioStatus("ÁUDIO: falha ao abrir banco DX7 interno"); return;
        }
        if (audioEngine == null || !audioEngine.loadDx7(layer, target.getAbsolutePath())) {
            screen.setAudioStatus("ÁUDIO: falha ao preparar DX7 (formato ou memória)"); return;
        }
        deactivatePadLayer(layer);
        screen.setLayerName(layer, displayName); screen.setEngineName(layer, "DX7");
        screen.setPresetName(layer, audioEngine.dx7PatchName(layer, 0));
        getSharedPreferences("layers", MODE_PRIVATE).edit().putInt("engine_" + layer, 2)
                .putString("dx7_" + layer, target.getAbsolutePath()).putString("name_" + layer, displayName)
                .putInt("dx7_patch_" + layer, 0).apply();
        screen.setAudioStatus("ÁUDIO: " + displayName + " carregado");
    }

    private void chooseLayerSource(int layer) {
        EditorUi.Panel panel=new EditorUi.Panel(this,"ADICIONAR MOTOR · LAYER "+(layer+1),"Escolha o motor sonoro desta camada.");
        String[] names={"SOUNDFONT 2 (.sf2)","DX7 SYSEX (.syx)","CLASSIC KEYS ANALOG","HAMMOND / LESLIE","DRUM PADS","PADS CONTÍNUOS"};
        for(int i=0;i<names.length;i+=2){LinearLayout row=EditorUi.gridRow(panel.body);for(int j=i;j<Math.min(i+2,names.length);j++){final int which=j;Button b=EditorUi.button(this,names[which],()->{
            panel.dialog.dismiss();if(which==0)openSf2Picker(layer);else if(which==1)openDx7Picker(layer);
            else if(which==2){activateAnalog(layer);openAnalogEditor(layer);}
            else if(which==3){activateHammond(layer);openHammondEditor(layer);}
            else openPadEditor(layer,which==5);
            });row.addView(b,new LinearLayout.LayoutParams(0,EditorUi.dp(this,30),1));}}
        EditorUi.addButton(panel.footer,"CANCELAR",panel.dialog::dismiss);panel.show();
    }

    private void openLayerEditor(int layer) {
        if (screen.muted[layer]) { screen.setAudioStatus("DESMUTE A LAYER "+(layer+1)+" PARA EDITAR O MOTOR"); return; }
        String engine=screen.engineName(layer);
        if(engine.equals("DX7"))openDx7Editor(layer);
        else if(engine.equals("ANALOG"))openAnalogEditor(layer);
        else if(engine.equals("HAMMOND"))openHammondEditor(layer);
        else if(engine.contains("PADS"))openPadEditor(layer,engine.startsWith("CONT"));
        else openSoundFontEditor(layer);
    }

    private void showLayerActions(final int layer) {
        final String engine = screen.engineName(layer);
        EditorUi.Panel panel=new EditorUi.Panel(this,"LAYER "+(layer+1)+" · "+engine,"Escolha o que deseja ajustar.");
        String[] actions={"EDITAR MOTOR ATUAL","TROCAR MOTOR","LIMPAR LAYER"};for(int i=0;i<actions.length;i+=2){LinearLayout row=EditorUi.gridRow(panel.body);for(int j=i;j<Math.min(i+2,actions.length);j++){final int action=j;Button b=EditorUi.button(this,actions[action],()->{panel.dialog.dismiss();if(action==1){panicAndChooseLayerSource(layer);return;}if(action==2){clearLayer(layer);return;}openLayerEditor(layer);});row.addView(b,new LinearLayout.LayoutParams(0,EditorUi.dp(this,30),1));}}
        LinearLayout move=EditorUi.gridRow(panel.body);
        Button left=EditorUi.button(this,"← MOVER LAYER",()->{screen.moveLayer(layer,-1);panel.dialog.dismiss();});
        Button right=EditorUi.button(this,"MOVER LAYER →",()->{screen.moveLayer(layer,1);panel.dialog.dismiss();});
        left.setEnabled(screen.canMoveLayer(layer,-1));right.setEnabled(screen.canMoveLayer(layer,1));
        move.addView(left,new LinearLayout.LayoutParams(0,EditorUi.dp(this,30),1));
        move.addView(right,new LinearLayout.LayoutParams(0,EditorUi.dp(this,30),1));
        EditorUi.addButton(panel.footer,"FECHAR",panel.dialog::dismiss);panel.show();
    }

    private void panicAndChooseLayerSource(int layer) {
        if (audioEngine != null) audioEngine.allNotesOff();
        deactivatePadLayer(layer);
        chooseLayerSource(layer);
    }

    /** Mute is a resource state: unload the motor, then restore it off the UI/audio threads. */
    private void setLayerMuted(int layer,boolean muted) {
        if(layer<0||layer>=6||screen==null)return;
        if(android.os.Looper.myLooper()!=android.os.Looper.getMainLooper()){
            mainHandler.post(()->setLayerMuted(layer,muted));return;
        }
        if(screen.muted[layer]==muted)return;
        screen.muted[layer]=muted;
        getSharedPreferences("layers",MODE_PRIVATE).edit().putBoolean("muted_"+layer,muted).apply();
        final int generation=muteGenerations.incrementAndGet(layer);
        if(muted){
            if(audioEngine!=null){audioEngine.setLayerGain(layer,0f);audioEngine.clearLayer(layer);}
            if(isPadEngine(layer)&&padEngine!=null)padEngine.setEnabled(false,screen.engineName(layer).startsWith("CONT"));
            screen.applyLayerGains();
            screen.setAudioStatus("LAYER "+(layer+1)+" mutada · motor parado e vozes liberadas");
            screen.invalidate();
            return;
        }

        final int savedEngine=getSharedPreferences("layers",MODE_PRIVATE).getInt("engine_"+layer,0);
        if(savedEngine==5||savedEngine==6||screen.engineName(layer).contains("PADS")){
            if(padEngine!=null)padEngine.setEnabled(true,savedEngine==6);
            screen.applyLayerGains();
            screen.setAudioStatus("LAYER "+(layer+1)+" reativada");
            screen.invalidate();
            return;
        }
        if(savedEngine==0||"VAZIA".equals(screen.engineName(layer))){
            screen.applyLayerGains();screen.invalidate();return;
        }

        screen.applyLayerGains();
        screen.setAudioStatus("REATIVANDO LAYER "+(layer+1)+"…");
        try{
            fontImporter.execute(()->{
                String presetName=null;
                boolean loaded=false,stale=false;
                synchronized(importLifecycleLock){
                    if(closing||muteGenerations.get(layer)!=generation)return;
                    presetName=restoreLayerMotor(layer,savedEngine);
                    loaded=presetName!=null;
                    if(loaded&&(closing||muteGenerations.get(layer)!=generation)){
                        if(audioEngine!=null)audioEngine.clearLayer(layer);
                        stale=true;
                    }
                }
                if(stale||closing)return;
                final boolean success=loaded;
                final String restoredName=presetName;
                mainHandler.post(()->{
                    if(closing||isFinishing()||isDestroyed()||muteGenerations.get(layer)!=generation)return;
                    if(!success){
                        screen.muted[layer]=true;
                        muteGenerations.incrementAndGet(layer);
                        getSharedPreferences("layers",MODE_PRIVATE).edit().putBoolean("muted_"+layer,true).apply();
                        if(audioEngine!=null)audioEngine.clearLayer(layer);
                        screen.applyLayerGains();
                        screen.setAudioStatus("FALHA AO REATIVAR LAYER "+(layer+1)+" · continua mutada");
                    }else{
                        screen.applyLoadedEditorState(layer);
                        screen.setPresetName(layer,restoredName);
                        screen.setAudioStatus("LAYER "+(layer+1)+" reativada");
                    }
                    screen.invalidate();
                });
            });
        }catch(java.util.concurrent.RejectedExecutionException ignored){
            screen.muted[layer]=true;
            getSharedPreferences("layers",MODE_PRIVATE).edit().putBoolean("muted_"+layer,true).apply();
            if(audioEngine!=null)audioEngine.clearLayer(layer);
            screen.applyLayerGains();
            screen.setAudioStatus("NÃO FOI POSSÍVEL REATIVAR A LAYER");
        }
    }

    private String restoreLayerMotor(int layer,int engine){
        if(audioEngine==null)return null;
        SharedPreferences prefs=getSharedPreferences("layers",MODE_PRIVATE);
        if(engine==1){
            String stored=prefs.getString("sf2_"+layer,null);
            String path=restoreSoundFont(stored,layer);
            if(path==null||!audioEngine.loadLayer(layer,path))return null;
            int count=audioEngine.presetCount(layer),selected=prefs.getInt("preset_"+layer,0);
            if(count<=0)return null;
            if(selected<0||selected>=count)selected=0;
            if(!audioEngine.setPreset(layer,selected))audioEngine.setPreset(layer,0);
            sf2Uris[layer]=path;
            prefs.edit().putString("sf2_"+layer,path).apply();
            String name=prefs.getString("name_"+layer,"SF2 carregado");
            soundFontLayers[layer].load(Uri.fromFile(new File(path)),name);
            soundFontLayers[layer].setPreset(selected);
            String preset=audioEngine.presetName(layer,selected);
            return preset==null||preset.isEmpty()?"Preset "+(selected+1):preset;
        }
        if(engine==2){
            String path=prefs.getString("dx7_"+layer,null);
            if(path==null||!audioEngine.loadDx7(layer,path))return null;
            int count=audioEngine.dx7PatchCount(layer),selected=prefs.getInt("dx7_patch_"+layer,0);
            if(count<=0)return null;
            if(selected<0||selected>=count)selected=0;
            if(!audioEngine.setDx7Patch(layer,selected))audioEngine.setDx7Patch(layer,0);
            String name=audioEngine.dx7PatchName(layer,selected);
            return name==null||name.isEmpty()?"DX7 · Timbre "+(selected+1):name;
        }
        if(engine==3){
            int preset=prefs.getInt("analog_preset_"+layer,0);
            audioEngine.activateAnalog(layer);
            if(!audioEngine.setAnalogPreset(layer,preset))audioEngine.setAnalogPreset(layer,0);
            return audioEngine.analogPresetName(preset);
        }
        if(engine==4){
            int preset=prefs.getInt("hammond_preset_"+layer,0);
            audioEngine.activateHammond(layer);
            if(!audioEngine.setHammondPreset(layer,preset))audioEngine.setHammondPreset(layer,0);
            return audioEngine.hammondPresetName(preset);
        }
        return null;
    }

    /** A newly chosen motor replaces the muted one; do not reload the old motor first. */
    private void prepareLayerMotorReplacement(int layer){
        if(layer<0||layer>=6||!screen.muted[layer])return;
        screen.muted[layer]=false;
        muteGenerations.incrementAndGet(layer);
        getSharedPreferences("layers",MODE_PRIVATE).edit().putBoolean("muted_"+layer,false).apply();
        screen.applyLayerGains();
        screen.invalidate();
    }

    private void deactivatePadLayer(int layer) {
        if(!isPadEngine(layer))return;
        boolean continuous=screen.engineName(layer).startsWith("CONT");
        if(padEngine!=null){padEngine.setEnabled(false,continuous);padEngine.stopImmediately(continuous);}
        if(padLayerIndex==layer){padLayerActive=false;continuousPadActive=false;padLayerIndex=-1;}
    }

    private void clearLayer(int layer) {
        if(layer<0||layer>=6)return;
        muteGenerations.incrementAndGet(layer);
        deactivatePadLayer(layer);
        if (audioEngine != null) { audioEngine.allNotesOff(); audioEngine.clearLayer(layer); }
        screen.muted[layer]=false;
        getSharedPreferences("layers", MODE_PRIVATE).edit()
                .remove("engine_"+layer).remove("sf2_"+layer).remove("dx7_"+layer)
                .remove("name_"+layer).remove("preset_"+layer).remove("dx7_patch_"+layer)
                .remove("analog_preset_"+layer).remove("hammond_preset_"+layer)
                .putBoolean("muted_"+layer,false).apply();
        screen.setLayerName(layer,"Sem SoundFont"); screen.setEngineName(layer,"VAZIA"); screen.setPresetName(layer,"");
        screen.applyLayerGains();
    }

    private void activateAnalog(int layer) {
        prepareLayerMotorReplacement(layer);
        deactivatePadLayer(layer);
        audioEngine.activateAnalog(layer); audioEngine.setAnalogPreset(layer,0);
        screen.setLayerName(layer,"Classic Keys Analog"); screen.setEngineName(layer,"ANALOG"); screen.setPresetName(layer,audioEngine.analogPresetName(0));
        getSharedPreferences("layers",MODE_PRIVATE).edit().putInt("engine_"+layer,3).putInt("analog_preset_"+layer,0).putString("name_"+layer,"Classic Keys Analog").apply();
        screen.setAnalogPresetDefaults(layer,0);
    }

    private void openAnalogEditor(final int layer) {
        int count=audioEngine.analogPresetCount(); String[] presets=new String[count];
        for(int i=0;i<count;++i)presets[i]=audioEngine.analogPresetName(i);
        int selectedPreset=getSharedPreferences("layers",MODE_PRIVATE).getInt("analog_preset_"+layer,0);
        showPresetEditor(layer, "LAYER "+(layer+1)+" · CLASSIC KEYS ANALOG", "Selecione o banco e o timbre desta layer", presets, selectedPreset,
                which -> { if(audioEngine.setAnalogPreset(layer,which)){screen.setPresetName(layer,audioEngine.analogPresetName(which));screen.setAnalogPresetDefaults(layer,which);} }, "TROCAR MOTOR", () -> panicAndChooseLayerSource(layer));
    }

    private void activateHammond(int layer) {
        prepareLayerMotorReplacement(layer);
        deactivatePadLayer(layer);
        audioEngine.activateHammond(layer); audioEngine.setHammondPreset(layer,0);
        screen.setLayerName(layer,"Classic Keys Hammond"); screen.setEngineName(layer,"HAMMOND"); screen.setPresetName(layer,audioEngine.hammondPresetName(0));
        screen.setHammondPresetDefaults(layer,0);
        getSharedPreferences("layers",MODE_PRIVATE).edit().putInt("engine_"+layer,4).putInt("hammond_preset_"+layer,0).putString("name_"+layer,"Classic Keys Hammond").apply();
    }
    private void openHammondEditor(final int layer) {
        int count=audioEngine.hammondPresetCount(); String[] presets=new String[count];
        for(int i=0;i<count;++i)presets[i]=audioEngine.hammondPresetName(i);
        int selected=getSharedPreferences("layers",MODE_PRIVATE).getInt("hammond_preset_"+layer,0);
        showPresetEditor(layer, "LAYER "+(layer+1)+" · HAMMOND / LESLIE", "Selecione o banco e o registro desta layer", presets, selected,
                which -> {audioEngine.setHammondPreset(layer,which);screen.setHammondPresetDefaults(layer,which);screen.setPresetName(layer,audioEngine.hammondPresetName(which));getSharedPreferences("layers",MODE_PRIVATE).edit().putInt("hammond_preset_"+layer,which).apply();}, "TROCAR MOTOR", () -> panicAndChooseLayerSource(layer));
    }

    private void openPadEditor(final int layer, final boolean continuous) {
        prepareLayerMotorReplacement(layer);
        if (padLayerIndex >= 0 && padLayerIndex != layer) deactivatePadLayer(padLayerIndex);
        audioEngine.clearLayer(layer);padLayerActive=true;continuousPadActive=continuous;if(padEngine!=null)padEngine.setEnabled(true,continuous);
        padLayerIndex=layer;SharedPreferences padSettings=getSharedPreferences("pads",MODE_PRIVATE);padEngine.setFadeSeconds(padSettings.getFloat("crossfade_seconds",1f));screen.applyLoadedEditorState(layer);
        screen.setLayerName(layer,continuous?"Pads contínuos":"Drum Pads");
        screen.setEngineName(layer,continuous?"CONT. PADS":"DRUM PADS"); screen.setPresetName(layer,continuous?"12 pads":"8 pads");
        getSharedPreferences("layers",MODE_PRIVATE).edit().putInt("engine_"+layer,continuous?6:5).putString("name_"+layer,continuous?"Pads contínuos":"Drum Pads").apply();
        showPadEditorScreen(layer, continuous);
    }

    private void showPadEditorScreen(final int layer, final boolean continuous) {
        final int padCount=continuous?12:8;
        final int padColumns=continuous?3:2;
        final EditorUi.Panel panel=new EditorUi.Panel(this,(continuous?"PADS CONTÍNUOS":"DRUM PADS")+" · LAYER "+(layer+1),padCount+" pads · toque para tocar · LOAD à esquerda · LIMPAR à direita");
        String[] tabs=continuous?new String[]{"PADS","MIDI","CROSSFADE / EQ"}:new String[]{"PADS","MIDI","CONFIGURAÇÕES"};
        class PadNav{void show(int page){panel.setTabs(tabs,page,this::show);panel.body.removeAllViews();
            if(page==0){
                for(int rowIndex=0;rowIndex<4;rowIndex++){LinearLayout row=EditorUi.gridRow(panel.body);for(int col=0;col<padColumns;col++){final int pad=rowIndex*padColumns+col;LinearLayout cell=EditorUi.column(MainActivity.this);cell.setPadding(EditorUi.dp(MainActivity.this,2),EditorUi.dp(MainActivity.this,2),EditorUi.dp(MainActivity.this,2),EditorUi.dp(MainActivity.this,2));
                    boolean loaded=padEngine.loaded(pad,continuous);String padName=padEngine.name(pad,continuous);Button trigger=EditorUi.button(MainActivity.this,loaded?padName:"PAD "+(pad+1),()->{if(padEngine.loaded(pad,continuous))padEngine.trigger(pad,continuous);else openPadPicker(pad,continuous);});if(loaded)stylePadTrigger(trigger,pad);row.addView(cell,new LinearLayout.LayoutParams(0,-2,1));cell.addView(trigger,new LinearLayout.LayoutParams(-1,EditorUi.dp(MainActivity.this,36)));
                    LinearLayout controls=EditorUi.gridRow(cell);Button load=EditorUi.button(MainActivity.this,"LOAD",()->openPadPicker(pad,continuous));Button clear=EditorUi.button(MainActivity.this,"LIMPAR",()->{clearPadAudio(pad,continuous);trigger.setText("PAD "+(pad+1));trigger.setBackground(EditorUi.background(0xff1d2c35));trigger.setTextColor(EditorUi.TEXT);});controls.addView(load,new LinearLayout.LayoutParams(0,EditorUi.dp(MainActivity.this,27),1));controls.addView(clear,new LinearLayout.LayoutParams(0,EditorUi.dp(MainActivity.this,27),1));
                }}
                if(continuous){
                    LinearLayout actions=EditorUi.gridRow(panel.body);EditorUi.addButton(actions,"STOP",()->padEngine.stopAll(true));
                    LinearLayout stopLearn=EditorUi.gridRow(panel.body);EditorUi.addButton(stopLearn,pendingPadCcLearn==12?"CANCELAR LEARN STOP":"LEARN STOP",MainActivity.this::beginPadStopLearn);
                    addPadCrossfadeControl(panel.body);
                }
                addPadLayerLearnRow(panel.body,layer,0,"LEARN VOLUME");
                addPadLayerLearnRow(panel.body,layer,4,"LEARN MUTE");
                if(continuous){LinearLayout eq=EditorUi.gridRow(panel.body);EditorUi.addButton(eq,"EDITAR EQ / FILTROS",()->showEffectEditor(layer,"EQ"));}
            }else if(page==1){
                SharedPreferences mapping=getSharedPreferences("pads",MODE_PRIVATE);String title=continuous?"CC DE DISPARO":"NOTA MIDI DE DISPARO";TextView caption=EditorUi.label(MainActivity.this,title+" · LEARN captura o próximo controle",9);caption.setTextColor(EditorUi.MUTED);panel.body.addView(caption,new LinearLayout.LayoutParams(-1,EditorUi.dp(MainActivity.this,21)));
                for(int rowIndex=0;rowIndex<(continuous?6:4);rowIndex++){LinearLayout row=EditorUi.gridRow(panel.body);for(int col=0;col<2;col++){final int pad=rowIndex*2+col;int learned=mapping.getInt((continuous?"pad_cc_":"pad_note_")+pad,continuous?-1:36+pad);LinearLayout item=EditorUi.column(MainActivity.this);row.addView(item,new LinearLayout.LayoutParams(0,-2,1));TextView label=EditorUi.label(MainActivity.this,"PAD "+(pad+1)+" · "+(learned<0?"SEM MAPA":(continuous?"CC ":"NOTA ")+learned),9);label.setGravity(Gravity.START|Gravity.CENTER_VERTICAL);item.addView(label,new LinearLayout.LayoutParams(-1,EditorUi.dp(MainActivity.this,22)));LinearLayout buttons=EditorUi.gridRow(item);EditorUi.addButton(buttons,pendingPadMidiLearn==pad||pendingPadCcLearn==pad?"CANCELAR":"LEARN",()->beginPadMappingLearn(pad,continuous));EditorUi.addButton(buttons,"LIMPAR",()->clearPadMapping(pad,continuous));}}
                if(continuous){LinearLayout stop=EditorUi.gridRow(panel.body);int cc=mapping.getInt("pad_stop_cc",-1);EditorUi.addButton(stop,pendingPadCcLearn==12?"CANCELAR STOP LEARN":cc<0?"LEARN STOP CC":"STOP · CC "+cc,()->beginPadStopLearn());EditorUi.addButton(stop,"LIMPAR STOP",()->clearPadMapping(12,true));}
                addPadLayerLearnRow(panel.body,layer,0,"LEARN VOLUME");addPadLayerLearnRow(panel.body,layer,4,"LEARN MUTE");
            }else{
                if(continuous){TextView label=EditorUi.label(MainActivity.this,"CROSSFADE ENTRE CAMADAS DE ÁUDIO",10);label.setGravity(Gravity.START|Gravity.CENTER_VERTICAL);panel.body.addView(label,new LinearLayout.LayoutParams(-1,EditorUi.dp(MainActivity.this,22)));SeekBar fade=new SeekBar(MainActivity.this);fade.setMax(998);fade.setProgress(Math.max(0,Math.min(998,Math.round((padEngine.fadeSeconds()-.02f)*100))));fade.setContentDescription("Duração do crossfade dos pads contínuos");panel.body.addView(fade,new LinearLayout.LayoutParams(-1,EditorUi.dp(MainActivity.this,32)));TextView value=EditorUi.label(MainActivity.this,String.format(java.util.Locale.ROOT,"%.2f s",padEngine.fadeSeconds()),9);value.setTextColor(EditorUi.MUTED);panel.body.addView(value,new LinearLayout.LayoutParams(-1,EditorUi.dp(MainActivity.this,18)));fade.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener(){public void onProgressChanged(SeekBar s,int n,boolean user){if(user){float seconds=.02f+n/100f;padEngine.setFadeSeconds(seconds);getSharedPreferences("pads",MODE_PRIVATE).edit().putFloat("crossfade_seconds",seconds).apply();value.setText(String.format(java.util.Locale.ROOT,"%.2f s",seconds));}}public void onStartTrackingTouch(SeekBar s){}public void onStopTrackingTouch(SeekBar s){}});
                    LinearLayout eq=EditorUi.gridRow(panel.body);EditorUi.addButton(eq,"EDITAR EQ / FILTROS…",()->showEffectEditor(layer,"EQ"));
                }else{TextView info=EditorUi.label(MainActivity.this,"Drum Pads: 12 disparos one-shot, volumen y mute MIDI Learn.",10);info.setTextColor(EditorUi.MUTED);panel.body.addView(info,new LinearLayout.LayoutParams(-1,EditorUi.dp(MainActivity.this,32)));}
                LinearLayout actions=EditorUi.gridRow(panel.body);EditorUi.addButton(actions,"PARAR TODOS",()->padEngine.stopAll(continuous));EditorUi.addButton(actions,"CARREGAR ÁUDIO…",()->choosePadToLoad(continuous));
            }
        }}
        new PadNav().show(0);EditorUi.addButton(panel.footer,"TROCAR MOTOR",()->{panel.dialog.dismiss();panicAndChooseLayerSource(layer);});EditorUi.addButton(panel.footer,"FECHAR",panel.dialog::dismiss);panel.show();
    }
    private void stylePadTrigger(Button button,int pad){
        StateListDrawable states=new StateListDrawable();
        states.addState(new int[]{android.R.attr.state_pressed},EditorUi.background(0xffffd84a));
        states.addState(new int[]{},EditorUi.background(padColour(pad)));
        button.setBackground(states);button.setTextColor(Color.rgb(21,25,29));
    }
    private void clearPadAudio(int pad,boolean continuous){
        if(pad<0||pad>=12)return;
        SharedPreferences prefs=getSharedPreferences("pads",MODE_PRIVATE);
        String key=padSampleKey(continuous,pad),path=prefs.getString(key,null);
        prefs.edit().remove(key).remove(padNameKey(continuous,pad)).apply();
        if(padEngine!=null)padEngine.load(pad,null,continuous,null);
        if(path!=null)try{
            File cached=new File(path).getCanonicalFile();
            File expected=new File(new File(new File(getFilesDir(),"pads"),continuous?"continuous":"drum"),"pad-"+pad+".audio").getCanonicalFile();
            if(cached.equals(expected))cached.delete();
        }catch(IOException ignored){}
        screen.invalidate();
        screen.setAudioStatus("ÁUDIO: PAD "+(pad+1)+" limpo");
    }
    private void beginPadMappingLearn(int pad,boolean continuous){if(continuous){if(pendingPadCcLearn==pad)pendingPadCcLearn=-1;else{pendingPadCcLearn=pad;pendingPadMidiLearn=-1;}}else{if(pendingPadMidiLearn==pad)pendingPadMidiLearn=-1;else{pendingPadMidiLearn=pad;pendingPadCcLearn=-1;}}screen.setMidiStatus("MIDI: toque agora o pad/controlador que deseja aprender");}
    private void addPadCrossfadeControl(LinearLayout body){
        TextView label=EditorUi.label(this,"CROSSFADE",10);label.setGravity(Gravity.START|Gravity.CENTER_VERTICAL);
        body.addView(label,new LinearLayout.LayoutParams(-1,EditorUi.dp(this,20)));
        LinearLayout row=EditorUi.gridRow(body);
        TextView value=EditorUi.label(this,String.format(java.util.Locale.ROOT,"%.2f s",padEngine.fadeSeconds()),9);
        row.addView(value,new LinearLayout.LayoutParams(EditorUi.dp(this,58),EditorUi.dp(this,31)));
        SeekBar fade=new SeekBar(this);fade.setMax(998);
        fade.setProgress(Math.max(0,Math.min(998,Math.round((padEngine.fadeSeconds()-.02f)*100))));
        fade.setContentDescription("Duração do crossfade dos pads contínuos");
        row.addView(fade,new LinearLayout.LayoutParams(0,EditorUi.dp(this,31),1));
        fade.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener(){
            public void onProgressChanged(SeekBar s,int n,boolean user){if(user){float seconds=.02f+n/100f;padEngine.setFadeSeconds(seconds);getSharedPreferences("pads",MODE_PRIVATE).edit().putFloat("crossfade_seconds",seconds).apply();value.setText(String.format(java.util.Locale.ROOT,"%.2f s",seconds));}}
            public void onStartTrackingTouch(SeekBar s){}
            public void onStopTrackingTouch(SeekBar s){}
        });
    }
    private void beginPadStopLearn(){pendingPadCcLearn=pendingPadCcLearn==12?-1:12;pendingPadMidiLearn=-1;screen.setMidiStatus(pendingPadCcLearn==12?"MIDI: mova agora o controle STOP":"MIDI: STOP Learn cancelado");}
    private void clearPadMapping(int target,boolean continuous){SharedPreferences.Editor e=getSharedPreferences("pads",MODE_PRIVATE).edit();if(target==12)e.remove("pad_stop_cc");else e.remove((continuous?"pad_cc_":"pad_note_")+target);e.apply();if(pendingPadMidiLearn==target)pendingPadMidiLearn=-1;if(pendingPadCcLearn==target)pendingPadCcLearn=-1;}
    private void addPadLayerLearnRow(LinearLayout body,int layer,int target,String title){LinearLayout row=EditorUi.gridRow(body);TextView label=EditorUi.label(this,title,9);label.setGravity(Gravity.START|Gravity.CENTER_VERTICAL);row.addView(label,new LinearLayout.LayoutParams(0,EditorUi.dp(this,29),1));int cc=getSharedPreferences("midi_learn",MODE_PRIVATE).getInt("layer_"+layer+"_"+target,-1);EditorUi.addButton(row,cc<0?"LEARN CC":"CC "+cc,()->{pendingLayerLearn=layer;pendingLayerLearnTarget=target;screen.setMidiStatus("MIDI: mova o controle para aprender");});}
    private void choosePadToLoad(boolean continuous){
        String[] pads=new String[continuous?12:8];for(int i=0;i<pads.length;i++)pads[i]="PAD "+(i+1);
        new AlertDialog.Builder(this).setTitle("ESCOLHA O PAD").setItems(pads,(d,p)->openPadPicker(p,continuous)).show();
    }
    private void openPadPicker(int pad,boolean continuous){
        pendingPad=pad;pendingContinuous=continuous;Intent i=new Intent(Intent.ACTION_OPEN_DOCUMENT);i.addCategory(Intent.CATEGORY_OPENABLE);i.setType("audio/*");startActivityForResult(i,710);
    }
    private void saveLiveSlot(final int bank,final int slot){
        EditText input=new EditText(this);input.setHint("Nome da programação");input.setSingleLine(true);
        new AlertDialog.Builder(this).setTitle("SALVAR NO LIVE SET "+(slot+1)).setView(input)
                .setPositiveButton("SALVAR",(d,w)->{
                    SharedPreferences layers=getSharedPreferences("layers",MODE_PRIVATE),live=getSharedPreferences("live_set",MODE_PRIVATE);SharedPreferences.Editor e=live.edit();
                    String root="bank_"+bank+"_slot_"+slot;e.putInt(root+"_version",2).putFloat(root+"_master",screen.masterVolume);for(int layer=0;layer<6;layer++){String p=root+"_"+layer+"_";e.putInt(p+"engine",layers.getInt("engine_"+layer,0));e.putString(p+"sf2",layers.getString("sf2_"+layer,null));e.putString(p+"dx7",layers.getString("dx7_"+layer,null));e.putString(p+"name",layers.getString("name_"+layer,null));e.putInt(p+"preset",layers.getInt("preset_"+layer,0));e.putInt(p+"dx7_patch",layers.getInt("dx7_patch_"+layer,0));e.putInt(p+"analog",layers.getInt("analog_preset_"+layer,0));e.putInt(p+"hammond",layers.getInt("hammond_preset_"+layer,0));e.putFloat(p+"volume",screen.layerVolumes[layer]).putFloat(p+"pan",screen.layerPan[layer]).putBoolean(p+"muted",screen.muted[layer]).putBoolean(p+"solo",screen.solo[layer]);}
                    SharedPreferences pads=getSharedPreferences("pads",MODE_PRIVATE);e.putFloat(root+"_pad_fade",pads.getFloat("crossfade_seconds",1f)).putInt(root+"_pad_stop_cc",pads.getInt("pad_stop_cc",-1));for(int pad=0;pad<12;pad++){e.putString(root+"_drum_pad_"+pad,pads.getString(padSampleKey(false,pad),null)).putString(root+"_continuous_pad_"+pad,pads.getString(padSampleKey(true,pad),null)).putString(root+"_drum_pad_name_"+pad,pads.getString(padNameKey(false,pad),null)).putString(root+"_continuous_pad_name_"+pad,pads.getString(padNameKey(true,pad),null)).putInt(root+"_pad_note_"+pad,pads.getInt("pad_note_"+pad,36+pad)).putInt(root+"_pad_cc_"+pad,pads.getInt("pad_cc_"+pad,-1));}
                    e.putInt(root+"_version",8).putFloat(root+"_reverb",screen.masterReverb).putFloat(root+"_chorus",screen.masterChorus);
                    SharedPreferences midiLearn=getSharedPreferences("midi_learn",MODE_PRIVATE);
                    for(int layer=0;layer<6;layer++)for(int target=0;target<5;target++){
                        String key="layer_"+layer+"_"+target;
                        e.putInt(root+"_learn_"+layer+"_"+target,midiLearn.getInt(key,-1))
                                .putBoolean(root+"_learn_relative_"+layer+"_"+target,midiLearn.getBoolean(key+"_relative",false));
                    }
                    for(int layer=0;layer<6;layer++){String p=root+"_"+layer+"_";
                        e.putFloat(p+"attack",screen.layerAttack[layer]).putFloat(p+"release",screen.layerRelease[layer]).putFloat(p+"eqLow",screen.eqLow[layer]).putFloat(p+"eqMid",screen.eqMid[layer]).putFloat(p+"eqHigh",screen.eqHigh[layer])
                         .putFloat(p+"eqLowFreq",screen.eqLowFreq[layer]).putFloat(p+"eqMidFreq",screen.eqMidFreq[layer]).putFloat(p+"eqHighFreq",screen.eqHighFreq[layer]).putFloat(p+"eqLowQ",screen.eqLowQ[layer]).putFloat(p+"eqMidQ",screen.eqMidQ[layer]).putFloat(p+"eqHighQ",screen.eqHighQ[layer]).putFloat(p+"eqHighPass",screen.eqHighPass[layer]).putFloat(p+"eqLowPass",screen.eqLowPass[layer])
                         .putFloat(p+"compThreshold",screen.compressorThreshold[layer]).putFloat(p+"compRatio",screen.compressorRatio[layer]).putFloat(p+"compAttack",screen.compressorAttackMs[layer]).putFloat(p+"compRelease",screen.compressorReleaseMs[layer]).putFloat(p+"compMakeup",screen.compressorMakeupDb[layer])
                         .putFloat(p+"cutoff",screen.layerCutoff[layer]).putFloat(p+"reverbSend",screen.layerReverb[layer]).putFloat(p+"compMix",screen.layerCompMix[layer]).putFloat(p+"chorus",screen.layerChorus[layer]).putFloat(p+"reverbSize",screen.reverbSize[layer]).putFloat(p+"reverbDamping",screen.reverbDamping[layer]).putFloat(p+"reverbWidth",screen.reverbWidth[layer])
                         .putInt(p+"routeChannel",screen.routeChannel[layer]).putInt(p+"routeOctave",screen.routeOctave[layer]).putInt(p+"routeVelocity",screen.routeVelocity[layer]).putInt(p+"routeMode",screen.routeMode[layer]).putBoolean(p+"routeSustain",screen.routeSustain[layer])
                         .putInt(p+"hammondLeslie",screen.hammondLeslie[layer]).putInt(p+"hammondPercussion",screen.hammondPercussion[layer]);
                        for(int bar=0;bar<9;bar++)e.putInt(p+"hammondBar"+bar,screen.hammondBars[layer][bar]);
                        for(int control=0;control<4;control++)e.putFloat(p+"hammondExtra"+control,screen.hammondExtras[layer][control]);
                        for(int control=0;control<19;control++)e.putFloat(p+"analogControl"+control,screen.analogControls[layer][control]);
                        e.putFloat(p+"analogOsc1Tune",screen.analogOsc1Tune[layer]);
                        for(int osc=0;osc<3;osc++)e.putInt(p+"analogWave"+osc,screen.analogWaves[layer][osc]).putBoolean(p+"analogEnabled"+osc,screen.analogOscillatorEnabled[layer][osc]);
                        e.putBoolean(p+"analogPink",screen.analogPinkNoise[layer]).putBoolean(p+"analogMono",screen.analogMonophonic[layer]);
                    }
                    for(int position=0;position<6;position++)e.putInt(root+"_layer_order_"+position,screen.layerOrder[position]);
                    String name=input.getText().toString().trim();if(name.isEmpty())name="PROGRAMA "+(slot+1);e.putBoolean(root+"_valid",true).putString(root+"_name",name).apply();screen.setLiveName(slot,name);
                }).setNegativeButton("CANCELAR",null).show();
    }
    private void loadLiveSlot(int bank,int slot){
        SharedPreferences live=getSharedPreferences("live_set",MODE_PRIVATE);String root="bank_"+bank+"_slot_"+slot;if(!live.getBoolean(root+"_valid",false)){new AlertDialog.Builder(this).setMessage("Este slot está vazio. Ative SALVAR SLOT e toque nele para guardar o programa atual.").setPositiveButton("OK",null).show();return;}
        int version=live.getInt(root+"_version",1);boolean legacyContinuous=false;for(int layer=0;layer<6;layer++){int engine=live.getInt(root+"_"+layer+"_engine",0);if(engine==5||engine==6)legacyContinuous=engine==6;}SharedPreferences.Editor e=getSharedPreferences("layers",MODE_PRIVATE).edit();
        screen.masterReverb=live.getFloat(root+"_reverb",0f);screen.masterChorus=live.getFloat(root+"_chorus",0f);if(audioEngine!=null)audioEngine.setMasterEffects(screen.masterReverb,screen.masterChorus);
        e.putFloat("master_volume",live.getFloat(root+"_master",screen.masterVolume))
            .putFloat("master_reverb",screen.masterReverb).putFloat("master_chorus",screen.masterChorus);
        if(version>=7){SharedPreferences.Editor learned=getSharedPreferences("midi_learn",MODE_PRIVATE).edit();
            for(int layer=0;layer<6;layer++)for(int target=0;target<5;target++){
                int cc=live.getInt(root+"_learn_"+layer+"_"+target,-1);String key="layer_"+layer+"_"+target;
                if(cc<0)learned.remove(key);else learned.putInt(key,cc);
                learned.putBoolean(key+"_relative",cc>=0&&live.getBoolean(root+"_learn_relative_"+layer+"_"+target,false));
            }learned.apply();}
        for(int layer=0;layer<6;layer++){String p=root+"_"+layer+"_";e.putInt("engine_"+layer,live.getInt(p+"engine",0)).putString("sf2_"+layer,live.getString(p+"sf2",null)).putString("dx7_"+layer,live.getString(p+"dx7",null)).putString("name_"+layer,live.getString(p+"name",null)).putInt("preset_"+layer,live.getInt(p+"preset",0)).putInt("dx7_patch_"+layer,live.getInt(p+"dx7_patch",0)).putInt("analog_preset_"+layer,live.getInt(p+"analog",0)).putInt("hammond_preset_"+layer,live.getInt(p+"hammond",0))
            .putFloat("control_volume_"+layer,live.getFloat(p+"volume",screen.layerVolumes[layer])).putFloat("control_pan_"+layer,live.getFloat(p+"pan",0)).putBoolean("muted_"+layer,live.getBoolean(p+"muted",false)).putBoolean("solo_"+layer,live.getBoolean(p+"solo",false))
            .putFloat("control_attack_"+layer,live.getFloat(p+"attack",screen.layerAttack[layer])).putFloat("control_release_"+layer,live.getFloat(p+"release",screen.layerRelease[layer]))
            .putFloat("eq_low_db_"+layer,version>=4?live.getFloat(p+"eqLow",screen.eqLow[layer]):(float)(20*Math.log10(Math.max(.125f,live.getFloat(p+"eqLow",1f)))))
            .putFloat("eq_mid_db_"+layer,version>=4?live.getFloat(p+"eqMid",screen.eqMid[layer]):(float)(20*Math.log10(Math.max(.125f,live.getFloat(p+"eqMid",1f)))))
            .putFloat("eq_high_db_"+layer,version>=4?live.getFloat(p+"eqHigh",screen.eqHigh[layer]):(float)(20*Math.log10(Math.max(.125f,live.getFloat(p+"eqHigh",1f)))))
            .putFloat("eq_low_freq_"+layer,live.getFloat(p+"eqLowFreq",220)).putFloat("eq_mid_freq_"+layer,live.getFloat(p+"eqMidFreq",1200)).putFloat("eq_high_freq_"+layer,live.getFloat(p+"eqHighFreq",4200)).putFloat("eq_low_q_"+layer,live.getFloat(p+"eqLowQ",.707f)).putFloat("eq_mid_q_"+layer,live.getFloat(p+"eqMidQ",1)).putFloat("eq_high_q_"+layer,live.getFloat(p+"eqHighQ",.707f)).putFloat("eq_highpass_"+layer,live.getFloat(p+"eqHighPass",20)).putFloat("eq_lowpass_"+layer,live.getFloat(p+"eqLowPass",20000))
            .putFloat("comp_threshold_"+layer,live.getFloat(p+"compThreshold",.126f)).putFloat("comp_ratio_"+layer,live.getFloat(p+"compRatio",4)).putFloat("comp_attack_"+layer,live.getFloat(p+"compAttack",10)).putFloat("comp_release_"+layer,live.getFloat(p+"compRelease",120)).putFloat("comp_makeup_"+layer,live.getFloat(p+"compMakeup",0))
            .putFloat("control_cutoff_"+layer,live.getFloat(p+"cutoff",100)).putFloat("control_reverb_"+layer,live.getFloat(p+"reverbSend",0)).putFloat("control_comp_"+layer,live.getFloat(p+"compMix",0)).putFloat("control_chorus_"+layer,live.getFloat(p+"chorus",0)).putFloat("reverb_size_"+layer,live.getFloat(p+"reverbSize",55)).putFloat("reverb_damping_"+layer,live.getFloat(p+"reverbDamping",45)).putFloat("reverb_width_"+layer,live.getFloat(p+"reverbWidth",100))
            .putInt("route_channel_"+layer,live.getInt(p+"routeChannel",-1)).putInt("route_octave_"+layer,live.getInt(p+"routeOctave",0)).remove("route_low_"+layer).remove("route_high_"+layer).putInt("route_velocity_"+layer,live.getInt(p+"routeVelocity",0)).putInt("route_mode_"+layer,live.getInt(p+"routeMode",0)).putBoolean("route_sustain_"+layer,live.getBoolean(p+"routeSustain",true))
            .putInt("hammond_leslie_"+layer,live.getInt(p+"hammondLeslie",1)).putInt("hammond_percussion_"+layer,live.getInt(p+"hammondPercussion",0));
            for(int bar=0;bar<9;bar++)e.putInt("hammond_bar_"+layer+"_"+bar,live.getInt(p+"hammondBar"+bar,6));
            for(int control=0;control<4;control++)e.putFloat(new String[]{"hammond_click_","hammond_leakage_","hammond_drive_","hammond_level_"}[control]+layer,live.getFloat(p+"hammondExtra"+control,new float[]{.15f,.12f,.12f,.8f}[control]));
            for(int control=0;control<19;control++)e.putFloat("analog_control_"+layer+"_"+control,live.getFloat(p+"analogControl"+control,screen.analogControls[layer][control]));
            e.putFloat("analog_osc1_tune_"+layer,live.getFloat(p+"analogOsc1Tune",screen.analogOsc1Tune[layer]));
            for(int osc=0;osc<3;osc++)e.putInt("analog_wave_"+layer+"_"+osc,live.getInt(p+"analogWave"+osc,screen.analogWaves[layer][osc])).putBoolean("analog_enabled_"+layer+"_"+osc,live.getBoolean(p+"analogEnabled"+osc,screen.analogOscillatorEnabled[layer][osc]));
            e.putBoolean("analog_pink_"+layer,live.getBoolean(p+"analogPink",screen.analogPinkNoise[layer])).putBoolean("analog_mono_"+layer,live.getBoolean(p+"analogMono",screen.analogMonophonic[layer]));
        }
        for(int position=0;position<6;position++)e.putInt("layer_order_"+position,live.getInt(root+"_layer_order_"+position,screen.layerOrder[position]));
        e.apply();screen.masterVolume=live.getFloat(root+"_master",screen.masterVolume);SharedPreferences.Editor pads=getSharedPreferences("pads",MODE_PRIVATE).edit().putFloat("crossfade_seconds",live.getFloat(root+"_pad_fade",1f)).putInt("pad_stop_cc",live.getInt(root+"_pad_stop_cc",-1));for(int pad=0;pad<12;pad++){
            if(version>=6){String drum=live.getString(root+"_drum_pad_"+pad,null),continuous=live.getString(root+"_continuous_pad_"+pad,null);if(drum==null)pads.remove(padSampleKey(false,pad));else pads.putString(padSampleKey(false,pad),drum);if(continuous==null)pads.remove(padSampleKey(true,pad));else pads.putString(padSampleKey(true,pad),continuous);}
            else{String path=live.getString(root+"_pad_"+pad,null);pads.remove(padSampleKey(!legacyContinuous,pad));if(path==null)pads.remove(padSampleKey(legacyContinuous,pad));else pads.putString(padSampleKey(legacyContinuous,pad),path);}
            for(boolean continuous:new boolean[]{false,true}){String name=version>=8?live.getString(root+(continuous?"_continuous_pad_name_":"_drum_pad_name_")+pad,null):null;if(name==null)pads.remove(padNameKey(continuous,pad));else pads.putString(padNameKey(continuous,pad),name);}
            pads.putInt("pad_note_"+pad,live.getInt(root+"_pad_note_"+pad,36+pad)).putInt("pad_cc_"+pad,live.getInt(root+"_pad_cc_"+pad,-1));}pads.apply();recreate();
    }

    private void showLoginScreen() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL); root.setGravity(Gravity.CENTER);
        root.setPadding(48, 24, 48, 24); root.setBackgroundColor(Color.rgb(7, 16, 25));
        TextView title = new TextView(this); title.setText("CLASSIC PLAYER\nLICENÇA");
        title.setTextColor(Color.rgb(233,239,240)); title.setTextSize(26); title.setGravity(Gravity.CENTER);
        root.addView(title, new LinearLayout.LayoutParams(-1, -2));
        TextView hint = new TextView(this); hint.setText("Entre com o e-mail e a senha da sua conta para liberar este dispositivo.");
        hint.setTextColor(Color.rgb(180,195,200)); hint.setGravity(Gravity.CENTER); hint.setPadding(0, 24, 0, 18);
        root.addView(hint, new LinearLayout.LayoutParams(-1, -2));
        EditText email = new EditText(this); email.setHint("E-mail"); email.setSingleLine(true); email.setTextColor(Color.WHITE); email.setHintTextColor(Color.GRAY);
        root.addView(email, new LinearLayout.LayoutParams(-1, -2));
        EditText password = new EditText(this); password.setHint("Senha"); password.setSingleLine(true); password.setInputType(0x81); password.setTextColor(Color.WHITE); password.setHintTextColor(Color.GRAY);
        root.addView(password, new LinearLayout.LayoutParams(-1, -2));
        Button login = new Button(this); login.setText("ENTRAR E ATIVAR");
        LinearLayout.LayoutParams bp = new LinearLayout.LayoutParams(-2, -2); bp.gravity = Gravity.CENTER; bp.topMargin = 24; root.addView(login, bp);
        TextView status = new TextView(this); status.setTextColor(Color.rgb(255,110,100)); status.setGravity(Gravity.CENTER); root.addView(status, new LinearLayout.LayoutParams(-1, -2));
        setContentView(root);
        login.setOnClickListener(v -> {
            login.setEnabled(false); status.setText("Autenticando…");
            new Thread(() -> {
                String result = authenticate(email.getText().toString().trim(), password.getText().toString());
                runOnUiThread(() -> { login.setEnabled(true); if (result == null) { status.setText("Falha no login. Verifique os dados e a conexão."); return; } showMixerAfterLogin(result); });
            }, "license-login").start();
        });
    }

    private String authenticate(String email, String password) {
        try {
            HttpURLConnection c = (HttpURLConnection) new URL("https://licenca.classickeys.com.br/v1/auth/login").openConnection();
            c.setRequestMethod("POST"); c.setConnectTimeout(10000); c.setReadTimeout(10000); c.setDoOutput(true); c.setRequestProperty("Content-Type", "application/json");
            String deviceId = Settings.Secure.getString(getContentResolver(), Settings.Secure.ANDROID_ID);
            if (deviceId == null || deviceId.isEmpty()) deviceId = "android-" + android.os.Build.MODEL;
            String body = "{\"email\":\"" + jsonEscape(email) + "\",\"password\":\"" + jsonEscape(password)
                    + "\",\"device_id\":\"" + jsonEscape(deviceId) + "\",\"device_name\":\""
                    + jsonEscape(android.os.Build.MANUFACTURER + " " + android.os.Build.MODEL) + "\",\"platform\":\"Android\"}";
            c.getOutputStream().write(body.getBytes(StandardCharsets.UTF_8));
            InputStream in = c.getResponseCode() >= 400 ? c.getErrorStream() : c.getInputStream(); if (in == null || c.getResponseCode() >= 400) return null;
            byte[] bytes = new byte[8192]; int n = in.read(bytes); String json = n < 0 ? "" : new String(bytes, 0, n, StandardCharsets.UTF_8);
            String token = jsonValue(json, "access_token"); if (token == null) return null;
            licenseManager.storeSession(token, jsonValue(json, "display_name"), jsonValue(json, "email")); return token;
        } catch (Exception ignored) { return null; }
    }
    private static String jsonEscape(String s) { return s.replace("\\", "\\\\").replace("\"", "\\\""); }
    private static String jsonValue(String json, String key) { java.util.regex.Matcher m = java.util.regex.Pattern.compile("\\\"" + key + "\\\"\\s*:\\s*\\\"([^\\\"]*)").matcher(json); return m.find() ? m.group(1) : null; }
    private void showMixerAfterLogin(String ignored) {
        String name = licenseManager.userName();
        if (name == null || name.isEmpty()) name = licenseManager.userEmail();
        screen.setAccount(name + (licenseManager.userEmail().isEmpty() || name.equals(licenseManager.userEmail()) ? "" : " · " + licenseManager.userEmail()));
        setContentView(screen);
    }

    @Override protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if(requestCode==720){
            if(resultCode==RESULT_OK&&data!=null&&data.getData()!=null&&pendingEffectPresetJson!=null){
                try(OutputStream out=getContentResolver().openOutputStream(data.getData())){if(out!=null)out.write(pendingEffectPresetJson.getBytes(java.nio.charset.StandardCharsets.UTF_8));screen.setAudioStatus("Preset "+pendingEffectPresetType+" exportado");}
                catch(Exception ex){new AlertDialog.Builder(this).setMessage("Não foi possível salvar o preset selecionado.").setPositiveButton("OK",null).show();}
            }
            pendingEffectPresetJson=null;pendingEffectPresetLayer=-1;pendingEffectPresetType="";return;
        }
        if(requestCode==721){
            if(resultCode==RESULT_OK&&data!=null&&data.getData()!=null){
                try(InputStream in=getContentResolver().openInputStream(data.getData());ByteArrayOutputStream bytes=new ByteArrayOutputStream()){
                    if(in==null)throw new IOException("Arquivo indisponível");byte[] buffer=new byte[8192];int count;while((count=in.read(buffer))>=0)bytes.write(buffer,0,count);
                    JSONObject preset=new JSONObject(new String(bytes.toByteArray(),java.nio.charset.StandardCharsets.UTF_8));
                    if(!"ClassicPlayerAndroidEffectPreset".equals(preset.optString("format"))||!pendingEffectPresetType.equals(preset.optString("effect")))throw new IOException("O arquivo não é um preset compatível de "+pendingEffectPresetType+".");
                    applyEffectPresetJson(pendingEffectPresetLayer,pendingEffectPresetType,preset);screen.setAudioStatus("Preset "+pendingEffectPresetType+" importado");
                }catch(Exception ex){new AlertDialog.Builder(this).setTitle("PRESET INVÁLIDO").setMessage(ex.getMessage()==null?"Não foi possível importar este preset.":ex.getMessage()).setPositiveButton("OK",null).show();}
            }
            pendingEffectPresetLayer=-1;pendingEffectPresetType="";return;
        }
        if(requestCode==722){
            if(resultCode==RESULT_OK&&data!=null&&data.getData()!=null&&pendingLayerPresetJson!=null){try(OutputStream out=getContentResolver().openOutputStream(data.getData())){if(out==null)throw new IOException("Destino indisponível");out.write(pendingLayerPresetJson.getBytes(java.nio.charset.StandardCharsets.UTF_8));screen.setAudioStatus("Preset da layer exportado");}catch(Exception ex){new AlertDialog.Builder(this).setMessage("Não foi possível salvar o preset da layer.").setPositiveButton("OK",null).show();}}
            pendingLayerPresetJson=null;pendingLayerPresetLayer=-1;return;
        }
        if(requestCode==723){
            if(resultCode==RESULT_OK&&data!=null&&data.getData()!=null){try(InputStream in=getContentResolver().openInputStream(data.getData());ByteArrayOutputStream bytes=new ByteArrayOutputStream()){if(in==null)throw new IOException("Arquivo indisponível");byte[] buffer=new byte[8192];int count;while((count=in.read(buffer))>=0)bytes.write(buffer,0,count);applyLayerPresetJson(pendingLayerPresetLayer,new JSONObject(new String(bytes.toByteArray(),java.nio.charset.StandardCharsets.UTF_8)));screen.setAudioStatus("Preset da layer importado");}catch(Exception ex){new AlertDialog.Builder(this).setTitle("PRESET INVÁLIDO").setMessage(ex.getMessage()==null?"Não foi possível importar este preset.":ex.getMessage()).setPositiveButton("OK",null).show();}}
            pendingLayerPresetLayer=-1;return;
        }
        if(requestCode==710&&resultCode==RESULT_OK&&data!=null&&data.getData()!=null&&pendingPad>=0){
            boolean continuous=pendingContinuous;String path=cachePad(data.getData(),pendingPad,continuous);if(path!=null){String name=padDisplayName(data.getData(),pendingPad);padEngine.load(pendingPad,path,continuous,name);getSharedPreferences("pads",MODE_PRIVATE).edit().putString(padSampleKey(continuous,pendingPad),path).putString(padNameKey(continuous,pendingPad),name).apply();screen.setAudioStatus("ÁUDIO: PAD "+(pendingPad+1)+" carregado");screen.invalidate();}pendingPad=-1;return;
        }
        if (requestCode == 701 && resultCode == RESULT_OK && data != null && data.getData() != null && pendingLayer >= 0) {
            Uri uri=data.getData(); final int layer=pendingLayer;
            String cachedPath=cacheDocument(uri,layer,"syx");
            if(cachedPath==null||audioEngine==null||!audioEngine.loadDx7(layer,cachedPath)){
                screen.setAudioStatus("ÁUDIO: falha ao preparar DX7 (formato ou memória)"); pendingLayer=-1; return;
            }
            prepareLayerMotorReplacement(layer);
            deactivatePadLayer(layer);
            String name=uri.getLastPathSegment()==null?"Banco DX7":uri.getLastPathSegment();
            screen.setLayerName(layer,name); screen.setEngineName(layer,"DX7");
            screen.setPresetName(layer,audioEngine.dx7PatchName(layer,0));
            getSharedPreferences("layers",MODE_PRIVATE).edit().putInt("engine_"+layer,2)
                    .putString("dx7_"+layer,cachedPath).putString("name_"+layer,name).putInt("dx7_patch_"+layer,0).apply();
            screen.setAudioStatus("ÁUDIO: banco DX7 carregado"); pendingLayer=-1; openDx7Editor(layer); return;
        }
        if (requestCode == 700 && resultCode == RESULT_OK && data != null && data.getData() != null && pendingLayer >= 0) {
            Uri uri = data.getData();
            try { getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION); }
            catch (SecurityException ignored) { }
            final int layer = pendingLayer;
            pendingLayer = -1;
            final PolySynthEngine importingEngine = audioEngine;
            screen.setAudioStatus("ÁUDIO: importando SF2…");
            fontImporter.execute(() -> {
            if (closing) return;
            String cachedPath = cacheSoundFont(uri, layer);
            final boolean loaded;
            synchronized (importLifecycleLock) {
                loaded = cachedPath != null && importingEngine != null && !closing
                        && importingEngine.loadLayer(layer, cachedPath);
            }
            runOnUiThread(() -> {
            if (closing || isFinishing() || isDestroyed()) return;
            if (!loaded) {
                screen.setAudioStatus("ÁUDIO: falha ao abrir o arquivo SF2");
                pendingLayer = -1;
                return;
            }
            prepareLayerMotorReplacement(layer);
            deactivatePadLayer(layer);
            String name = soundFontDisplayName(uri);
            screen.setLayerName(layer, name);
            screen.setEngineName(layer, "SF2");
            String firstPreset = audioEngine.presetCount(layer) > 0 ? audioEngine.presetName(layer, 0) : "Preset 1";
            screen.setPresetName(layer, firstPreset);
            soundFontLayers[layer].load(uri, name);
            soundFontLayers[layer].setPreset(0);
            getSharedPreferences("layers", MODE_PRIVATE).edit()
                    .putString("sf2_" + layer, cachedPath)
                    .putString("name_" + layer, name)
                    .putInt("preset_" + layer, 0)
                    .putInt("engine_" + layer, 1)
                    .apply();
            sf2Uris[layer] = cachedPath;
            screen.applyLoadedEditorState(layer);
        restorePreferredAudioDevice();
            pendingLayer = -1;
            openSoundFontEditor(layer);
            });
            });
        }
    }

    /** Android document URIs are not file paths; make a private copy for the native SF2 renderer. */
    private String restoreSoundFont(String stored, int layer) {
        if(stored==null||stored.isEmpty())return null;
        File existing = new File(stored);
        if (existing.isFile()) return existing.getAbsolutePath();
        try { return cacheSoundFont(Uri.parse(stored), layer); }
        catch (Exception ignored) { return null; }
    }

    /** Use the document provider's human-readable filename instead of opaque URI IDs (e.g. msf:27442). */
    private String soundFontDisplayName(Uri uri) {
        String name = null;
        if (uri != null) {
            try (Cursor cursor = getContentResolver().query(uri,
                    new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
                if (cursor != null && cursor.moveToFirst()) {
                    int column = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                    if (column >= 0) name = cursor.getString(column);
                }
            } catch (Exception ignored) { }
            if (name == null || name.trim().isEmpty()) name = uri.getLastPathSegment();
        }
        return displaySafeSoundFontName(name);
    }

    private String displaySafeSoundFontName(String name) {
        if (name == null || name.trim().isEmpty() || name.matches("(?i)msf:\\d+")) {
            return "SoundFont carregado";
        }
        return name;
    }

    private String cacheSoundFont(Uri source, int layer) {
        return cacheDocument(source, layer, "sf2");
    }

    private String padDisplayName(Uri source,int pad){
        String name=null;
        try(android.database.Cursor cursor=getContentResolver().query(source,new String[]{OpenableColumns.DISPLAY_NAME},null,null,null)){
            if(cursor!=null&&cursor.moveToFirst())name=cursor.getString(0);
        }catch(Exception ignored){}
        if(name==null||name.trim().isEmpty())name=source.getLastPathSegment();
        if(name==null||name.trim().isEmpty())return "PAD "+(pad+1);
        name=new File(name).getName().trim();
        int extension=name.lastIndexOf('.');
        if(extension>0)name=name.substring(0,extension);
        return name.isEmpty()?"PAD "+(pad+1):name;
    }

    private String cachePad(Uri source,int pad,boolean continuous){
        File directory=new File(new File(getFilesDir(),"pads"),continuous?"continuous":"drum");if(!directory.exists()&&!directory.mkdirs())return null;
        File target=new File(directory,"pad-"+pad+".audio");
        try(InputStream input=getContentResolver().openInputStream(source);FileOutputStream output=new FileOutputStream(target,false)){if(input==null)return null;byte[] buffer=new byte[65536];int read;while((read=input.read(buffer))>=0)output.write(buffer,0,read);return target.getAbsolutePath();}catch(Exception ignored){return null;}
    }

    private String cacheDocument(Uri source, int layer, String extension) {
        if (source == null || layer < 0 || layer >= 6) return null;
        File directory = new File(getFilesDir(), "soundfonts");
        if (!directory.exists() && !directory.mkdirs()) return null;
        File target = new File(directory, "layer-" + layer + "." + extension);
        try (InputStream input = getContentResolver().openInputStream(source);
             FileOutputStream output = new FileOutputStream(target, false)) {
            if (input == null) return null;
            byte[] block = new byte[64 * 1024];
            int count;
            while ((count = input.read(block)) >= 0) output.write(block, 0, count);
            output.flush();
            return target.getAbsolutePath();
        } catch (Exception ignored) {
            if (target.exists()) target.delete();
            return null;
        }
    }

    private void openSoundFontEditor(final int layer) {
        if (layer < 0 || layer >= 6 || audioEngine == null) return;
        final int count = audioEngine.presetCount(layer);
        if (count <= 0) { openSf2Picker(layer); return; }
        final ArrayList<String> presets = new ArrayList<>();
        for (int preset = 0; preset < count; ++preset) {
            String name = audioEngine.presetName(layer, preset);
            presets.add(String.format("%03d  %s", preset + 1, name == null || name.isEmpty() ? "Preset" : name));
        }
        int selectedPreset = soundFontLayers[layer].preset();
        showPresetEditor(layer, "LAYER " + (layer + 1) + " · TIMBRE SF2", "Selecione um preset do SoundFont", presets.toArray(new String[0]), selectedPreset, which -> {
                    if (audioEngine.setPreset(layer, which)) {
                        soundFontLayers[layer].setPreset(which);
                        screen.setPresetName(layer, audioEngine.presetName(layer, which));
                        getSharedPreferences("layers", MODE_PRIVATE).edit().putInt("preset_" + layer, which).apply();
                    }
                }, "TROCAR SF2", () -> openSf2Picker(layer));
    }

    private void openDx7Editor(final int layer) {
        if (audioEngine == null) return;
        int count=audioEngine.dx7PatchCount(layer);
        if(count<=0){openDx7Picker(layer);return;}
        String[] names=new String[count];
        for(int i=0;i<count;i++)names[i]=String.format(java.util.Locale.ROOT,"%02d: %s",i+1,audioEngine.dx7PatchName(layer,i));
        showPresetEditor(layer,"DX7 · LAYER "+(layer+1),"Selecione o banco e o timbre desta camada.",names,
            getSharedPreferences("layers",MODE_PRIVATE).getInt("dx7_patch_"+layer,0),which->{
                if(audioEngine.setDx7Patch(layer,which)){
                    screen.setPresetName(layer,audioEngine.dx7PatchName(layer,which));
                    getSharedPreferences("layers",MODE_PRIVATE).edit().putInt("dx7_patch_"+layer,which).apply();
                }
            },"IMPORTAR DX7",()->openExternalDx7Picker(layer));
    }

    private interface PresetSelection { void apply(int index); }

    private void showPresetEditor(final int layer,String title,String subtitle,String[] items,int selected,
                                  PresetSelection selection,String secondaryLabel,Runnable secondaryAction) {
        EditorUi.Panel panel=new EditorUi.Panel(this,title,subtitle);
        final String[] tabNames=screen.engineName(layer).equals("ANALOG")?new String[]{"OSCILADORES","FILTRO / ADSR","CONTROLES","EFEITOS","MIDI"}:new String[]{"LAYER","CONTROLES","EFEITOS","MIDI"};
        class PageNav { void show(int page){panel.setTabs(tabNames,page,this::show);renderLayerEditorPage(layer,panel,page,items,selected,selection,secondaryLabel,secondaryAction);} }
        PageNav nav=new PageNav();nav.show(0);
        EditorUi.addButton(panel.footer,"FECHAR",panel.dialog::dismiss);
        panel.show();
    }

    private void renderLayerEditorPage(final int layer,EditorUi.Panel panel,int page,String[] items,int selected,
                                       PresetSelection selection,String secondaryLabel,Runnable secondaryAction){
        LinearLayout body=panel.body;body.removeAllViews();String engine=screen.engineName(layer);boolean analog=engine.equals("ANALOG");int controlsPage=analog?2:1,effectsPage=analog?3:2,midiPage=analog?4:3;SharedPreferences prefs=getSharedPreferences("layers",MODE_PRIVATE);
        if(page==0){
            if("DX7".equals(engine)){
                String path=prefs.getString("dx7_"+layer,"");boolean first=path.endsWith("_"+R.raw.dx7_bank_1+".syx"),second=path.endsWith("_"+R.raw.dx7_bank_2+".syx");
                String[] banks=first||second?new String[]{"Divine Masquerade 1","Divine Masquerade 2"}:new String[]{"Divine Masquerade 1","Divine Masquerade 2",prefs.getString("name_"+layer,"Banco importado")};
                EditorUi.selector(body,"BANCO DX7",banks,first?0:second?1:2,which->{if(which<2){panel.dialog.dismiss();activateBundledDx7(layer,which==0?R.raw.dx7_bank_1:R.raw.dx7_bank_2,which==0?"Divine Masquerade 1":"Divine Masquerade 2");openDx7Editor(layer);}});
            }else if("SF2".equals(engine)){
                TextView loaded=EditorUi.label(this,screen.layerNames[layer],11);loaded.setGravity(Gravity.START|Gravity.CENTER_VERTICAL);loaded.setSingleLine(true);loaded.setEllipsize(android.text.TextUtils.TruncateAt.MIDDLE);body.addView(loaded,new LinearLayout.LayoutParams(-1,EditorUi.dp(this,22)));
            }
            EditorUi.selector(body,"TIMBRE",items,selected,which->{selection.apply(which);if(analog)renderLayerEditorPage(layer,panel,page,items,which,selection,secondaryLabel,secondaryAction);});
            LinearLayout source=EditorUi.gridRow(body);if(secondaryLabel!=null)EditorUi.addButton(source,secondaryLabel,()->{panel.dialog.dismiss();secondaryAction.run();});if(secondaryLabel==null||!secondaryLabel.equals("TROCAR MOTOR"))EditorUi.addButton(source,"TROCAR MOTOR",()->{panel.dialog.dismiss();panicAndChooseLayerSource(layer);});
            if("HAMMOND".equals(engine))buildHammondControls(layer,body);
            if(analog)buildAnalogOscillatorControls(layer,body);
            LinearLayout presets=EditorUi.gridRow(body);EditorUi.addButton(presets,"EXPORTAR PRESET…",()->exportLayerPreset(layer));EditorUi.addButton(presets,"IMPORTAR PRESET…",()->importLayerPreset(layer));
            if (!analog && ("SF2".equals(engine) || "DX7".equals(engine))) {
                TextView routing=EditorUi.label(this,"MIDI · MODO, CANAL E FAIXA",10);routing.setTextColor(EditorUi.MUTED);body.addView(routing);
                buildRoutingControls(layer,body);
                TextView quick=EditorUi.label(this,"CONTROLES DA LAYER",10);quick.setTextColor(EditorUi.MUTED);body.addView(quick);
                LinearLayout row=EditorUi.knobRow(body);
                EditorUi.knob(row,"VOLUME",screen.layerVolumes[layer]*100f,0,100," %",v->screen.setLearnedVolume(layer,v/100f));
                EditorUi.knob(row,"ATTACK ms",screen.layerAttack[layer]*1000f,.1f,100," ms",v->screen.setLayerEnvelope(layer,v/1000f,screen.layerRelease[layer]));
                EditorUi.knob(row,"RELEASE ms",screen.layerRelease[layer]*1000f,1,100," ms",v->screen.setLayerEnvelope(layer,screen.layerAttack[layer],v/1000f));
                row=EditorUi.knobRow(body);
                EditorUi.knob(row,"CUTOFF",screen.layerCutoff[layer],0,100," %",v->screen.setLayerTone(layer,v,screen.layerReverb[layer],screen.layerCompMix[layer],screen.layerChorus[layer]));
                EditorUi.knob(row,"REVERB",screen.layerReverb[layer]*100f,0,100," %",v->screen.setLayerTone(layer,screen.layerCutoff[layer],v/100f,screen.layerCompMix[layer],screen.layerChorus[layer]));
                EditorUi.knob(row,"COMP",screen.layerCompMix[layer]*100f,0,100," %",v->screen.setLayerTone(layer,screen.layerCutoff[layer],screen.layerReverb[layer],v/100f,screen.layerChorus[layer]));
                row=EditorUi.gridRow(body);EditorUi.addButton(row,"EDITAR REVERB",()->showEffectEditor(layer,"REVERB"));EditorUi.addButton(row,"EDITAR COMP",()->showEffectEditor(layer,"COMP"));EditorUi.addButton(row,"EDITAR EQ",()->showEffectEditor(layer,"EQ"));
            }
        }else if(analog&&page==1){
            buildAnalogFilterControls(layer,body);
        }else if(page==controlsPage){
            LinearLayout row=EditorUi.knobRow(body);EditorUi.knob(row,"VOLUME",screen.layerVolumes[layer]*100f,0,100," %",v->screen.setLearnedVolume(layer,v/100f));
            EditorUi.knob(row,"ATTACK ms",screen.layerAttack[layer]*1000f,.1f,100," ms",v->screen.setLayerEnvelope(layer,v/1000f,screen.layerRelease[layer]));
            EditorUi.knob(row,"RELEASE ms",screen.layerRelease[layer]*1000f,1,100," ms",v->screen.setLayerEnvelope(layer,screen.layerAttack[layer],v/1000f));
            row=EditorUi.knobRow(body);EditorUi.knob(row,"CUTOFF",screen.layerCutoff[layer],0,100," %",v->screen.setLayerTone(layer,v,screen.layerReverb[layer],screen.layerCompMix[layer],screen.layerChorus[layer]));
            EditorUi.knob(row,"REVERB",screen.layerReverb[layer]*100f,0,100," %",v->screen.setLayerTone(layer,screen.layerCutoff[layer],v/100f,screen.layerCompMix[layer],screen.layerChorus[layer]));
            EditorUi.knob(row,"COMP",screen.layerCompMix[layer]*100f,0,100," %",v->screen.setLayerTone(layer,screen.layerCutoff[layer],screen.layerReverb[layer],v/100f,screen.layerChorus[layer]));
            row=EditorUi.knobRow(body);EditorUi.knob(row,"PAN",screen.layerPan[layer]*100,-100,100,"",v->screen.setLayerPan(layer,v/100));
            EditorUi.knob(row,"CHORUS",screen.layerChorus[layer]*100,0,100," %",v->screen.setLayerTone(layer,screen.layerCutoff[layer],screen.layerReverb[layer],screen.layerCompMix[layer],v/100));
            row=EditorUi.knobRow(body);EditorUi.knob(row,"EQ LOW dB",screen.eqLow[layer],-18,18," dB",v->screen.setLayerEq(layer,v,screen.eqMid[layer],screen.eqHigh[layer],screen.eqLowFreq[layer],screen.eqMidFreq[layer],screen.eqHighFreq[layer],screen.eqLowQ[layer],screen.eqMidQ[layer],screen.eqHighQ[layer],screen.eqHighPass[layer],screen.eqLowPass[layer]));
            EditorUi.knob(row,"EQ MID dB",screen.eqMid[layer],-18,18," dB",v->screen.setLayerEq(layer,screen.eqLow[layer],v,screen.eqHigh[layer],screen.eqLowFreq[layer],screen.eqMidFreq[layer],screen.eqHighFreq[layer],screen.eqLowQ[layer],screen.eqMidQ[layer],screen.eqHighQ[layer],screen.eqHighPass[layer],screen.eqLowPass[layer]));
            EditorUi.knob(row,"EQ HIGH dB",screen.eqHigh[layer],-18,18," dB",v->screen.setLayerEq(layer,screen.eqLow[layer],screen.eqMid[layer],v,screen.eqLowFreq[layer],screen.eqMidFreq[layer],screen.eqHighFreq[layer],screen.eqLowQ[layer],screen.eqMidQ[layer],screen.eqHighQ[layer],screen.eqHighPass[layer],screen.eqLowPass[layer]));
            EditorUi.selector(body,"MUTE",new String[]{"SOM ATIVO","MUTE"},screen.muted[layer]?1:0,i->setLayerMuted(layer,i==1));
            EditorUi.selector(body,"SOLO",new String[]{"NORMAL","SOLO"},screen.solo[layer]?1:0,i->{screen.solo[layer]=i==1;prefs.edit().putBoolean("solo_"+layer,screen.solo[layer]).apply();screen.applyLayerGains();screen.invalidate();});
        }else if(page==effectsPage){
            TextView note=EditorUi.label(this,"Ajuste os efeitos desta layer. Toque para abrir os parâmetros.",11);note.setTextColor(EditorUi.MUTED);body.addView(note,new LinearLayout.LayoutParams(-1,EditorUi.dp(this,26)));
            LinearLayout row=EditorUi.gridRow(body);EditorUi.addButton(row,"REVERB…",()->showEffectEditor(layer,"REVERB"));EditorUi.addButton(row,"COMP…",()->showEffectEditor(layer,"COMP"));
            row=EditorUi.gridRow(body);EditorUi.addButton(row,"CHORUS…",()->showEffectEditor(layer,"CHORUS"));EditorUi.addButton(row,"EQ…",()->showEffectEditor(layer,"EQ"));
        }else if(page==midiPage){
            buildRoutingControls(layer,body);
            TextView heading=EditorUi.label(this,"MIDI LEARN · clique LEARN e mova o controle no teclado",10);heading.setTextColor(EditorUi.MUTED);heading.setGravity(Gravity.START|Gravity.CENTER_VERTICAL);body.addView(heading,new LinearLayout.LayoutParams(-1,EditorUi.dp(this,26)));
            String[] targets={"VOLUME","CUTOFF","REVERB","COMP","MUTE"};
            for(int target=0;target<targets.length;target++){
                final int t=target;LinearLayout line=EditorUi.gridRow(body);TextView label=EditorUi.label(this,targets[t],9);label.setGravity(Gravity.START|Gravity.CENTER_VERTICAL);line.addView(label,new LinearLayout.LayoutParams(0,EditorUi.dp(this,29),1));
                int cc=getSharedPreferences("midi_learn",MODE_PRIVATE).getInt("layer_"+layer+"_"+t,-1);final Button[] learn={null};learn[0]=EditorUi.button(this,pendingLayerLearn==layer&&pendingLayerLearnTarget==t?"CANCELAR LEARN":cc<0?"LEARN":"CC "+cc,()->{if(pendingLayerLearn==layer&&pendingLayerLearnTarget==t){pendingLayerLearn=-1;pendingLayerLearnTarget=-1;pendingLayerLearnButton=null;learn[0].setText(cc<0?"LEARN":"CC "+cc);screen.setMidiStatus("MIDI: learn cancelado");}else{pendingLayerLearn=layer;pendingLayerLearnTarget=t;pendingLayerLearnButton=learn[0];learn[0].setText("CANCELAR LEARN");screen.setMidiStatus(midiInput==null?"MIDI: aguardando entrada MIDI; verifique o controlador":"MIDI: mova o controle para aprender");}});line.addView(learn[0],new LinearLayout.LayoutParams(0,EditorUi.dp(this,29),1));
                EditorUi.addButton(line,"LIMPAR",()->{getSharedPreferences("midi_learn",MODE_PRIVATE).edit().remove("layer_"+layer+"_"+t).apply();if(pendingLayerLearn==layer&&pendingLayerLearnTarget==t){pendingLayerLearn=-1;pendingLayerLearnTarget=-1;}learn[0].setText("LEARN");screen.setMidiStatus("MIDI Learn apagado");});
            }
        }
    }

    private void buildRoutingControls(int layer,LinearLayout body){
        final int l=layer;final int channelSelection=screen.routeChannel[l]+1;LinearLayout row=EditorUi.gridRow(body);
        String[] channels=new String[17];channels[0]="OMNI";for(int i=1;i<17;i++)channels[i]="CH "+i;
        EditorUi.selectorCell(row,"CANAL MIDI",channels,channelSelection,i->screen.setLayerRouting(l,i-1,screen.routeOctave[l],screen.routeVelocity[l],screen.routeSustain[l]));
        String[] octaves={"−4","−3","−2","−1","0","+1","+2","+3","+4"};EditorUi.selectorCell(row,"OITAVA",octaves,screen.routeOctave[l]+4,i->screen.setLayerRouting(l,screen.routeChannel[l],i-4,screen.routeVelocity[l],screen.routeSustain[l]));
        row=EditorUi.gridRow(body);String[] modes={"POLIFÔNICO","MONO LEGATO","PORTAMENTO"};EditorUi.selectorCell(row,"MODO",modes,screen.routeMode[l],i->screen.setLayerRouting(l,screen.routeChannel[l],screen.routeOctave[l],screen.routeVelocity[l],screen.routeSustain[l],i));
        String[] velocity={"LINEAR","SUAVE","FORTE"};EditorUi.selectorCell(row,"VELOCIDADE",velocity,screen.routeVelocity[l],i->screen.setLayerRouting(l,screen.routeChannel[l],screen.routeOctave[l],i,screen.routeSustain[l]));
        EditorUi.selectorCell(row,"SUSTAIN",new String[]{"LIGADO","DESLIGADO"},screen.routeSustain[l]?0:1,i->screen.setLayerRouting(l,screen.routeChannel[l],screen.routeOctave[l],screen.routeVelocity[l],i==0));
        row=EditorUi.gridRow(body);TextView device=EditorUi.label(this,screen.midiStatus,9);device.setTextColor(EditorUi.MUTED);device.setGravity(Gravity.CENTER_VERTICAL|Gravity.START);row.addView(device,new LinearLayout.LayoutParams(0,EditorUi.dp(this,32),2));EditorUi.addButton(row,"ESCOLHER ENTRADA MIDI",this::showMidiDeviceChooser);
    }

    private void buildHammondControls(int layer,LinearLayout body){
        final int l=layer;String[] barNames={"16′","5⅓′","8′","4′","2⅔′","2′","1⅗′","1⅓′","1′"};
        for(int rowIndex=0;rowIndex<3;rowIndex++){LinearLayout row=EditorUi.gridRow(body);for(int col=0;col<3;col++){
            final int bar=rowIndex*3+col;LinearLayout cell=EditorUi.column(this);cell.setPadding(EditorUi.dp(this,3),0,EditorUi.dp(this,3),0);TextView label=EditorUi.label(this,barNames[bar]+"  "+screen.hammondBars[l][bar],9);cell.addView(label,new LinearLayout.LayoutParams(-1,EditorUi.dp(this,17)));
            SeekBar control=new SeekBar(this);control.setMax(8);control.setProgress(Math.max(0,screen.hammondBars[l][bar]));control.setContentDescription("Drawbar "+barNames[bar]);control.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener(){public void onProgressChanged(SeekBar s,int value,boolean user){if(user){screen.setHammondBar(l,bar,value);label.setText(barNames[bar]+"  "+value);}}public void onStartTrackingTouch(SeekBar s){}public void onStopTrackingTouch(SeekBar s){}});cell.addView(control,new LinearLayout.LayoutParams(-1,EditorUi.dp(this,34)));LinearLayout.LayoutParams cp=new LinearLayout.LayoutParams(0,-2,1);row.addView(cell,cp);
        }}
        LinearLayout opts=EditorUi.gridRow(body);EditorUi.selectorCell(opts,"LESLIE",new String[]{"STOP","CHORALE","FAST"},screen.hammondLeslie[l],i->screen.setHammondOption(l,true,i));EditorUi.selectorCell(opts,"PERCUSSÃO",new String[]{"OFF","2ND","3RD"},screen.hammondPercussion[l],i->screen.setHammondOption(l,false,i));
        LinearLayout knobs=EditorUi.knobRow(body);String[] labels={"KEY CLICK","LEAKAGE","DRIVE","LEVEL"};for(int i=0;i<4;i++){final int index=i;EditorUi.knob(knobs,labels[i],screen.hammondExtras[l][i]*100,0,100," %",v->screen.setHammondExtra(l,index,v/100f));}
    }

    private void buildAnalogOscillatorControls(int layer,LinearLayout body){
        final int l=layer;String[] shapes={"TRIANGLE","SAW","SQUARE","PULSE","SINE"};
        LinearLayout waves=EditorUi.gridRow(body);for(int osc=0;osc<3;osc++){final int index=osc;EditorUi.selectorCell(waves,"OSC "+(osc+1)+" WAVE",shapes,screen.analogWaves[l][osc],v->screen.setAnalogWave(l,index,v));}
        LinearLayout enables=EditorUi.gridRow(body);for(int osc=0;osc<3;osc++){final int index=osc;EditorUi.selectorCell(enables,"OSC "+(osc+1),new String[]{"OFF","ON"},screen.analogOscillatorEnabled[l][osc]?1:0,v->screen.setAnalogOption(l,index,v==1));}
        LinearLayout levels=EditorUi.knobRow(body);for(int osc=0;osc<3;osc++){final int index=osc;EditorUi.knob(levels,"OSC "+(osc+1)+" LEVEL",screen.analogControls[l][osc],0,100," %",v->screen.setAnalogControl(l,index,v));}
        LinearLayout tuning=EditorUi.knobRow(body);EditorUi.knob(tuning,"OSC 1 TUNE",screen.analogOsc1Tune[l],-24,24," st",v->screen.setAnalogOsc1Tune(l,v));EditorUi.knob(tuning,"OSC 2 TUNE",screen.analogControls[l][3],-24,24," st",v->screen.setAnalogControl(l,3,v));EditorUi.knob(tuning,"OSC 3 TUNE",screen.analogControls[l][4],-24,24," st",v->screen.setAnalogControl(l,4,v));
        LinearLayout noise=EditorUi.knobRow(body);EditorUi.knob(noise,"NOISE",screen.analogControls[l][5],0,100," %",v->screen.setAnalogControl(l,5,v));
        LinearLayout options=EditorUi.gridRow(body);EditorUi.selectorCell(options,"NOISE COLOR",new String[]{"WHITE","PINK"},screen.analogPinkNoise[l]?1:0,v->screen.setAnalogOption(l,3,v==1));EditorUi.selectorCell(options,"VOICES",new String[]{"POLY","MONO"},screen.analogMonophonic[l]?1:0,v->screen.setAnalogOption(l,4,v==1));
    }

    private void buildAnalogFilterControls(int layer,LinearLayout body){
        final int l=layer;float[] mins={0,0,0,1,1,0,1,0,0,0,0,0,0},maxs={100,100,100,2000,5000,100,5000,20,12,100,100,100,100};
        String[] labels={"CUTOFF","EMPHASIS","FILTER ENV","AMP ATTACK ms","AMP DECAY ms","AMP SUSTAIN","AMP RELEASE ms","LFO RATE Hz","LFO PITCH","LFO FILTER","DRIVE","KEY TRACK","MOD WHEEL"};
        int[] ids={6,7,8,9,10,11,12,13,14,15,16,17,18};String[] units={" %"," %"," %"," ms"," ms"," %"," ms"," Hz"," st"," %"," %"," %"," %"};
        for(int start=0;start<ids.length;start+=3){LinearLayout row=EditorUi.knobRow(body);for(int pos=start;pos<Math.min(start+3,ids.length);pos++){final int control=ids[pos];EditorUi.knob(row,labels[pos],screen.analogControls[l][control],mins[pos],maxs[pos],units[pos],v->screen.setAnalogControl(l,control,v));}}
    }

    private void showEffectEditor(final int layer,String effect){
        final EditorUi.Panel panel=new EditorUi.Panel(this,effect+" · LAYER "+(layer+1),"Ajustes compactos · todos os valores afetam esta layer.");
        final String[] tabNames={"AJUSTAR","PRESETS","ARQUIVO"};
        class EffectNav { void show(int page){panel.setTabs(tabNames,page,this::show);renderEffectEditorPage(layer,effect,panel,page,()->show(0),()->show(1));} }
        new EffectNav().show(0);
        EditorUi.addButton(panel.footer,"VOLTAR À LAYER",panel.dialog::dismiss);panel.show();
    }

    private void renderEffectEditorPage(final int layer,final String effect,EditorUi.Panel panel,int page,Runnable showAdjustments,Runnable showPresets){
        LinearLayout body=panel.body;body.removeAllViews();
        if(page==2){
            TextView tip=EditorUi.label(this,"Presets portáteis do Android · formato JSON",10);tip.setTextColor(EditorUi.MUTED);body.addView(tip,new LinearLayout.LayoutParams(-1,EditorUi.dp(this,22)));
            LinearLayout row=EditorUi.gridRow(body);EditorUi.addButton(row,"EXPORTAR PRESET",()->exportEffectPreset(layer,effect));EditorUi.addButton(row,"IMPORTAR PRESET",()->importEffectPreset(layer,effect));return;
        }
        if(page==1){
            if(effect.equals("REVERB")){
                String[] names={"Piano Íntimo","Sala Clara","Worship Hall","Ambient Grande"};float[][] values={{32,55,70,14},{45,75,85,18},{72,68,100,28},{92,82,100,42}};
                for(int i=0;i<names.length;i+=2){LinearLayout row=EditorUi.gridRow(body);for(int j=i;j<Math.min(i+2,names.length);j++){
                    final int preset=j;
                    Button button=EditorUi.button(this,names[j],()->{applyReverbPreset(layer,values[preset]);showPresets.run();});
                    float[] v=values[j];
                    if(Math.abs(screen.reverbSize[layer]-v[0])<.01f&&Math.abs(screen.reverbDamping[layer]-(100-v[1]))<.01f
                            &&Math.abs(screen.reverbWidth[layer]-v[2])<.01f&&Math.abs(screen.layerReverb[layer]-v[3]/100f)<.001f)
                        button.setBackground(EditorUi.background(0xff126e69));
                    LinearLayout.LayoutParams bp=new LinearLayout.LayoutParams(0,EditorUi.dp(this,29),1);bp.setMargins(2,1,2,1);row.addView(button,bp);
                }}
            }else if(effect.equals("COMP")){
                String[] names={"Piano Natural","Piano Presença","Worship Suave","Worship Sustentado"};float[][] values={{-9,2.5f,17,238,1,45},{-18,3.5f,7,180,4,55},{-14,2,25,260,2,50},{-22,4,35,360,4.5f,65}};
                for(int i=0;i<names.length;i+=2){LinearLayout row=EditorUi.gridRow(body);for(int j=i;j<Math.min(i+2,names.length);j++){final int preset=j;EditorUi.addButton(row,names[j],()->{applyCompressorPreset(layer,values[preset]);showAdjustments.run();android.widget.Toast.makeText(this,"Compressor: "+names[preset],android.widget.Toast.LENGTH_SHORT).show();});}}
            }else{
                TextView info=EditorUi.label(this,"O EQ é paramétrico; salve seus ajustes como preset para reutilizar.",10);info.setTextColor(EditorUi.MUTED);body.addView(info,new LinearLayout.LayoutParams(-1,EditorUi.dp(this,40)));
                LinearLayout row=EditorUi.gridRow(body);EditorUi.addButton(row,"SALVAR EQ…",()->exportEffectPreset(layer,effect));EditorUi.addButton(row,"CARREGAR EQ…",()->importEffectPreset(layer,effect));
            }
            return;
        }
        if(effect.equals("EQ")){
            EditorUi.ResponseGraph graph=new EditorUi.ResponseGraph(this,false,screen.eqLow[layer],screen.eqMid[layer],screen.eqHigh[layer]);
            graph.setParametricValues(screen.eqLowFreq[layer],screen.eqMidFreq[layer],screen.eqHighFreq[layer],screen.eqLowQ[layer],screen.eqMidQ[layer],screen.eqHighQ[layer],screen.eqHighPass[layer],screen.eqLowPass[layer]);
            LinearLayout.LayoutParams gp=new LinearLayout.LayoutParams(-1,EditorUi.dp(this,84));gp.setMargins(0,2,0,3);body.addView(graph,gp);
            LinearLayout row=EditorUi.knobRow(body);addEqKnob(layer,row,graph,"LOW Hz",0);addEqKnob(layer,row,graph,"LOW dB",1);addEqKnob(layer,row,graph,"LOW Q",2);
            row=EditorUi.knobRow(body);addEqKnob(layer,row,graph,"MID Hz",3);addEqKnob(layer,row,graph,"MID dB",4);addEqKnob(layer,row,graph,"MID Q",5);
            row=EditorUi.knobRow(body);addEqKnob(layer,row,graph,"HIGH Hz",6);addEqKnob(layer,row,graph,"HIGH dB",7);addEqKnob(layer,row,graph,"HIGH Q",8);
            row=EditorUi.knobRow(body);addEqKnob(layer,row,graph,"LOW CUT",9);addEqKnob(layer,row,graph,"HIGH CUT",10);
            LinearLayout files=EditorUi.gridRow(body);EditorUi.addButton(files,"EXPORTAR EQ",()->exportEffectPreset(layer,effect));EditorUi.addButton(files,"IMPORTAR EQ",()->importEffectPreset(layer,effect));return;
        }
        if(effect.equals("COMP")){
            final EditorUi.ResponseGraph graph=new EditorUi.ResponseGraph(this,true,screen.compressorThreshold[layer],screen.compressorRatio[layer],0);
            LinearLayout.LayoutParams gp=new LinearLayout.LayoutParams(-1,EditorUi.dp(this,84));gp.setMargins(0,2,0,3);body.addView(graph,gp);
            LinearLayout row=EditorUi.knobRow(body);EditorUi.knob(row,"THRESHOLD dB",(float)(20*Math.log10(Math.max(.001f,screen.compressorThreshold[layer]))),-60,0," dB",v->{screen.setLayerCompressor(layer,(float)Math.pow(10,v/20f),screen.compressorRatio[layer],screen.compressorAttackMs[layer],screen.compressorReleaseMs[layer],screen.compressorMakeupDb[layer]);graph.setValues(screen.compressorThreshold[layer],screen.compressorRatio[layer],0);});
            EditorUi.knob(row,"RATIO",screen.compressorRatio[layer],1,20,":1",v->{screen.setLayerCompressor(layer,screen.compressorThreshold[layer],v,screen.compressorAttackMs[layer],screen.compressorReleaseMs[layer],screen.compressorMakeupDb[layer]);graph.setValues(screen.compressorThreshold[layer],screen.compressorRatio[layer],0);});
            EditorUi.knob(row,"MIX",screen.layerCompMix[layer]*100,0,100," %",v->screen.setLayerTone(layer,screen.layerCutoff[layer],screen.layerReverb[layer],v/100,screen.layerChorus[layer]));
            row=EditorUi.knobRow(body);EditorUi.knob(row,"ATTACK ms",screen.compressorAttackMs[layer],.1f,100," ms",v->screen.setLayerCompressor(layer,screen.compressorThreshold[layer],screen.compressorRatio[layer],v,screen.compressorReleaseMs[layer],screen.compressorMakeupDb[layer]));
            EditorUi.knob(row,"RELEASE ms",screen.compressorReleaseMs[layer],5,1000," ms",v->screen.setLayerCompressor(layer,screen.compressorThreshold[layer],screen.compressorRatio[layer],screen.compressorAttackMs[layer],v,screen.compressorMakeupDb[layer]));
            EditorUi.knob(row,"MAKEUP dB",screen.compressorMakeupDb[layer],0,24," dB",v->screen.setLayerCompressor(layer,screen.compressorThreshold[layer],screen.compressorRatio[layer],screen.compressorAttackMs[layer],screen.compressorReleaseMs[layer],v));
            LinearLayout files=EditorUi.gridRow(body);EditorUi.addButton(files,"EXPORTAR COMP",()->exportEffectPreset(layer,effect));EditorUi.addButton(files,"IMPORTAR COMP",()->importEffectPreset(layer,effect));return;
        }
        if(effect.equals("REVERB")){
            LinearLayout row=EditorUi.knobRow(body);EditorUi.knob(row,"TEMPO",screen.reverbSize[layer],0,100," %",v->screen.setLayerReverb(layer,v,screen.reverbDamping[layer],screen.reverbWidth[layer]));
            EditorUi.knob(row,"DIFUSÃO",100-screen.reverbDamping[layer],0,100," %",v->screen.setLayerReverb(layer,screen.reverbSize[layer],100-v,screen.reverbWidth[layer]));
            EditorUi.knob(row,"LARGURA",screen.reverbWidth[layer],0,100," %",v->screen.setLayerReverb(layer,screen.reverbSize[layer],screen.reverbDamping[layer],v));
            EditorUi.knob(row,"MIX",screen.layerReverb[layer]*100,0,100," %",v->screen.setLayerTone(layer,screen.layerCutoff[layer],v/100,screen.layerCompMix[layer],screen.layerChorus[layer]));
            LinearLayout files=EditorUi.gridRow(body);EditorUi.addButton(files,"EXPORTAR REVERB",()->exportEffectPreset(layer,effect));EditorUi.addButton(files,"IMPORTAR REVERB",()->importEffectPreset(layer,effect));return;
        }
        LinearLayout row=EditorUi.knobRow(body);EditorUi.knob(row,"CHORUS MIX",screen.layerChorus[layer]*100,0,100," %",v->screen.setLayerTone(layer,screen.layerCutoff[layer],screen.layerReverb[layer],screen.layerCompMix[layer],v/100));
    }

    private void addEqKnob(final int layer,LinearLayout row,EditorUi.ResponseGraph graph,String label,int control){
        float value;float min;float max;String unit="";
        switch(control){case 0:value=screen.eqLowFreq[layer];min=40;max=2000;unit=" Hz";break;case 1:value=screen.eqLow[layer];min=-18;max=18;unit=" dB";break;case 2:value=screen.eqLowQ[layer];min=.1f;max=4;break;
            case 3:value=screen.eqMidFreq[layer];min=60;max=12000;unit=" Hz";break;case 4:value=screen.eqMid[layer];min=-18;max=18;unit=" dB";break;case 5:value=screen.eqMidQ[layer];min=.1f;max=20;break;
            case 6:value=screen.eqHighFreq[layer];min=1000;max=20000;unit=" Hz";break;case 7:value=screen.eqHigh[layer];min=-18;max=18;unit=" dB";break;case 8:value=screen.eqHighQ[layer];min=.1f;max=4;break;case 9:value=screen.eqHighPass[layer];min=20;max=250;unit=" Hz";break;default:value=screen.eqLowPass[layer];min=2000;max=20000;unit=" Hz";}
        final float initial=value,lower=min,upper=max;EditorUi.knob(row,label,initial,lower,upper,unit,v->{float low=screen.eqLow[layer],mid=screen.eqMid[layer],high=screen.eqHigh[layer],lf=screen.eqLowFreq[layer],mf=screen.eqMidFreq[layer],hf=screen.eqHighFreq[layer],lq=screen.eqLowQ[layer],mq=screen.eqMidQ[layer],hq=screen.eqHighQ[layer],hp=screen.eqHighPass[layer],lp=screen.eqLowPass[layer];switch(control){case 0:lf=v;break;case 1:low=v;break;case 2:lq=v;break;case 3:mf=v;break;case 4:mid=v;break;case 5:mq=v;break;case 6:hf=v;break;case 7:high=v;break;case 8:hq=v;break;case 9:hp=v;break;default:lp=v;}screen.setLayerEq(layer,low,mid,high,lf,mf,hf,lq,mq,hq,hp,lp);graph.setValues(low,mid,high);graph.setParametricValues(lf,mf,hf,lq,mq,hq,hp,lp);});
    }

    private void applyReverbPreset(int layer,float[] preset){screen.setLayerReverb(layer,preset[0],100-preset[1],preset[2]);screen.setLayerTone(layer,screen.layerCutoff[layer],preset[3]/100f,screen.layerCompMix[layer],screen.layerChorus[layer]);}
    private void applyCompressorPreset(int layer,float[] preset){screen.setLayerCompressor(layer,(float)Math.pow(10,preset[0]/20f),preset[1],preset[2],preset[3],preset[4]);screen.setLayerTone(layer,screen.layerCutoff[layer],screen.layerReverb[layer],preset[5]/100f,screen.layerChorus[layer]);}

    private void exportEffectPreset(int layer,String effect){
        try{
            JSONObject json=new JSONObject();json.put("format","ClassicPlayerAndroidEffectPreset");json.put("version",1);json.put("effect",effect);
            if(effect.equals("EQ")){json.put("lowGainDb",screen.eqLow[layer]);json.put("midGainDb",screen.eqMid[layer]);json.put("highGainDb",screen.eqHigh[layer]);json.put("lowFrequency",screen.eqLowFreq[layer]);json.put("midFrequency",screen.eqMidFreq[layer]);json.put("highFrequency",screen.eqHighFreq[layer]);json.put("lowQ",screen.eqLowQ[layer]);json.put("midQ",screen.eqMidQ[layer]);json.put("highQ",screen.eqHighQ[layer]);json.put("highPassHz",screen.eqHighPass[layer]);json.put("lowPassHz",screen.eqLowPass[layer]);}
            else if(effect.equals("COMP")){json.put("thresholdDb",20*Math.log10(Math.max(.001f,screen.compressorThreshold[layer])));json.put("ratio",screen.compressorRatio[layer]);json.put("attackMs",screen.compressorAttackMs[layer]);json.put("releaseMs",screen.compressorReleaseMs[layer]);json.put("makeupDb",screen.compressorMakeupDb[layer]);json.put("mix",screen.layerCompMix[layer]);}
            else if(effect.equals("REVERB")){json.put("size",screen.reverbSize[layer]);json.put("diffusion",100-screen.reverbDamping[layer]);json.put("width",screen.reverbWidth[layer]);json.put("mix",screen.layerReverb[layer]);}
            else json.put("mix",screen.layerChorus[layer]);
            pendingEffectPresetJson=json.toString(2);pendingEffectPresetLayer=layer;pendingEffectPresetType=effect;
            Intent intent=new Intent(Intent.ACTION_CREATE_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("application/json").putExtra(Intent.EXTRA_TITLE,"Classic Player "+effect+".json");startActivityForResult(intent,720);
        }catch(Exception ignored){new AlertDialog.Builder(this).setMessage("Não foi possível preparar o preset.").setPositiveButton("OK",null).show();}
    }

    private void importEffectPreset(int layer,String effect){pendingEffectPresetLayer=layer;pendingEffectPresetType=effect;Intent intent=new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("application/json");startActivityForResult(intent,721);}

    private void applyEffectPresetJson(int layer,String effect,JSONObject json)throws Exception{
        if(layer<0||layer>=6)throw new IOException("Layer inválida.");
        if(effect.equals("EQ")){
            screen.setLayerEq(layer,(float)json.getDouble("lowGainDb"),(float)json.getDouble("midGainDb"),(float)json.getDouble("highGainDb"),(float)json.getDouble("lowFrequency"),(float)json.getDouble("midFrequency"),(float)json.getDouble("highFrequency"),(float)json.getDouble("lowQ"),(float)json.getDouble("midQ"),(float)json.getDouble("highQ"),(float)json.getDouble("highPassHz"),(float)json.getDouble("lowPassHz"));
        }else if(effect.equals("COMP")){
            float db=(float)json.getDouble("thresholdDb"),mix=(float)json.getDouble("mix");screen.setLayerCompressor(layer,(float)Math.pow(10,Math.max(-60,Math.min(0,db))/20f),(float)json.getDouble("ratio"),(float)json.getDouble("attackMs"),(float)json.getDouble("releaseMs"),(float)json.getDouble("makeupDb"));screen.setLayerTone(layer,screen.layerCutoff[layer],screen.layerReverb[layer],Math.max(0,Math.min(1,mix)),screen.layerChorus[layer]);
        }else if(effect.equals("REVERB")){
            screen.setLayerReverb(layer,(float)json.getDouble("size"),100-(float)json.getDouble("diffusion"),(float)json.getDouble("width"));screen.setLayerTone(layer,screen.layerCutoff[layer],(float)json.getDouble("mix"),screen.layerCompMix[layer],screen.layerChorus[layer]);
        }else screen.setLayerTone(layer,screen.layerCutoff[layer],screen.layerReverb[layer],screen.layerCompMix[layer],(float)json.getDouble("mix"));
    }

    private void exportLayerPreset(int layer){
        try{
            SharedPreferences prefs=getSharedPreferences("layers",MODE_PRIVATE);JSONObject json=new JSONObject();json.put("format","ClassicPlayerAndroidLayerPreset");json.put("version",1);json.put("engine",screen.engineName(layer));json.put("sourceName",screen.layerNames[layer]);json.put("sourcePath",prefs.getString("sf2_"+layer,prefs.getString("dx7_"+layer,"")));
            String presetKey=screen.engineName(layer).equals("SF2")?"preset_":screen.engineName(layer).equals("DX7")?"dx7_patch_":screen.engineName(layer).equals("ANALOG")?"analog_preset_":"hammond_preset_";json.put("preset",prefs.getInt(presetKey+layer,0));
            json.put("volume",screen.layerVolumes[layer]).put("pan",screen.layerPan[layer]).put("attack",screen.layerAttack[layer]).put("release",screen.layerRelease[layer]).put("cutoff",screen.layerCutoff[layer]).put("reverbSend",screen.layerReverb[layer]).put("compMix",screen.layerCompMix[layer]).put("chorus",screen.layerChorus[layer]).put("mute",screen.muted[layer]).put("solo",screen.solo[layer]);
            json.put("eqLow",screen.eqLow[layer]).put("eqMid",screen.eqMid[layer]).put("eqHigh",screen.eqHigh[layer]).put("eqLowFreq",screen.eqLowFreq[layer]).put("eqMidFreq",screen.eqMidFreq[layer]).put("eqHighFreq",screen.eqHighFreq[layer]).put("eqLowQ",screen.eqLowQ[layer]).put("eqMidQ",screen.eqMidQ[layer]).put("eqHighQ",screen.eqHighQ[layer]).put("eqHighPass",screen.eqHighPass[layer]).put("eqLowPass",screen.eqLowPass[layer]);
            json.put("compThreshold",screen.compressorThreshold[layer]).put("compRatio",screen.compressorRatio[layer]).put("compAttack",screen.compressorAttackMs[layer]).put("compRelease",screen.compressorReleaseMs[layer]).put("compMakeup",screen.compressorMakeupDb[layer]).put("reverbSize",screen.reverbSize[layer]).put("reverbDamping",screen.reverbDamping[layer]).put("reverbWidth",screen.reverbWidth[layer]);
            json.put("routeChannel",screen.routeChannel[layer]).put("routeOctave",screen.routeOctave[layer]).put("routeVelocity",screen.routeVelocity[layer]).put("routeMode",screen.routeMode[layer]).put("routeSustain",screen.routeSustain[layer]);
            json.put("hammondLeslie",screen.hammondLeslie[layer]).put("hammondPercussion",screen.hammondPercussion[layer]);for(int i=0;i<9;i++)json.put("hammondBar"+i,screen.hammondBars[layer][i]);for(int i=0;i<4;i++)json.put("hammondExtra"+i,screen.hammondExtras[layer][i]);
            json.put("analogPink",screen.analogPinkNoise[layer]).put("analogMono",screen.analogMonophonic[layer]).put("analogOsc1Tune",screen.analogOsc1Tune[layer]);for(int i=0;i<19;i++)json.put("analogControl"+i,screen.analogControls[layer][i]);for(int i=0;i<3;i++)json.put("analogWave"+i,screen.analogWaves[layer][i]).put("analogEnabled"+i,screen.analogOscillatorEnabled[layer][i]);
            JSONObject learn=new JSONObject();SharedPreferences cc=getSharedPreferences("midi_learn",MODE_PRIVATE);for(int i=0;i<5;i++){String key="layer_"+layer+"_"+i;learn.put("cc"+i,cc.getInt(key,-1)).put("relative"+i,cc.getBoolean(key+"_relative",false));}json.put("midiLearn",learn);
            pendingLayerPresetJson=json.toString(2);pendingLayerPresetLayer=layer;Intent intent=new Intent(Intent.ACTION_CREATE_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("application/json").putExtra(Intent.EXTRA_TITLE,"Classic Player Layer "+(layer+1)+".json");startActivityForResult(intent,722);
        }catch(Exception ignored){new AlertDialog.Builder(this).setMessage("Não foi possível preparar o preset da layer.").setPositiveButton("OK",null).show();}
    }
    private void importLayerPreset(int layer){pendingLayerPresetLayer=layer;Intent intent=new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("application/json");startActivityForResult(intent,723);}
    private void applyLayerPresetJson(int layer,JSONObject json)throws Exception{
        if(layer<0||layer>=6||!"ClassicPlayerAndroidLayerPreset".equals(json.optString("format")))throw new IOException("Arquivo inválido: não é um preset de layer Classic Player Android.");
        if(!screen.engineName(layer).equals(json.optString("engine")))throw new IOException("Este preset é do motor "+json.optString("engine")+". Troque o motor da layer antes de importar.");
        SharedPreferences prefs=getSharedPreferences("layers",MODE_PRIVATE);String engine=screen.engineName(layer);int preset=json.optInt("preset",0);
        if(engine.equals("SF2")){
            String sourcePath=json.optString("sourcePath","");String sourceName=json.optString("sourceName","");String currentPath=prefs.getString("sf2_"+layer,"");
            if(!sourcePath.isEmpty()&&!sourcePath.equals(currentPath)){
                File sourceFile=new File(sourcePath);
                if(sourceFile.isFile()){
                    if(audioEngine==null||!audioEngine.loadLayer(layer,sourcePath))throw new IOException("Não foi possível carregar o SoundFont indicado neste preset.");
                    String loadedName=displaySafeSoundFontName(sourceName.isEmpty()?sourceFile.getName():sourceName);
                    sf2Uris[layer]=sourcePath;soundFontLayers[layer].load(Uri.fromFile(sourceFile),loadedName);screen.setLayerName(layer,loadedName);
                    prefs.edit().putString("sf2_"+layer,sourcePath).putString("name_"+layer,loadedName).apply();screen.applyLoadedEditorState(layer);
                }else if(!sourceName.isEmpty()&&!sourceName.equals(screen.layerNames[layer])){
                    throw new IOException("O preset informa o SoundFont '"+sourceName+"', mas não contém o arquivo SF2. Carregue esse SF2 na layer e importe o preset novamente.");
                }
            }
            if(audioEngine!=null&&audioEngine.setPreset(layer,preset)){soundFontLayers[layer].setPreset(preset);screen.setPresetName(layer,audioEngine.presetName(layer,preset));prefs.edit().putInt("preset_"+layer,preset).apply();}
        }
        else if(engine.equals("DX7")){if(audioEngine!=null&&audioEngine.setDx7Patch(layer,preset)){screen.setPresetName(layer,audioEngine.dx7PatchName(layer,preset));prefs.edit().putInt("dx7_patch_"+layer,preset).apply();}}
        else if(engine.equals("ANALOG")){if(audioEngine!=null&&audioEngine.setAnalogPreset(layer,preset)){screen.setPresetName(layer,audioEngine.analogPresetName(preset));screen.setAnalogPresetDefaults(layer,preset);}}
        else if(engine.equals("HAMMOND")){if(audioEngine!=null)audioEngine.setHammondPreset(layer,preset);prefs.edit().putInt("hammond_preset_"+layer,preset).apply();screen.setPresetName(layer,audioEngine==null?"Hammond":audioEngine.hammondPresetName(preset));}
        screen.setLayerEnvelope(layer,(float)json.optDouble("attack",screen.layerAttack[layer]),(float)json.optDouble("release",screen.layerRelease[layer]));screen.setLearnedVolume(layer,(float)json.optDouble("volume",screen.layerVolumes[layer]));screen.setLayerPan(layer,(float)json.optDouble("pan",screen.layerPan[layer]));screen.setLayerTone(layer,(float)json.optDouble("cutoff",screen.layerCutoff[layer]),(float)json.optDouble("reverbSend",screen.layerReverb[layer]),(float)json.optDouble("compMix",screen.layerCompMix[layer]),(float)json.optDouble("chorus",screen.layerChorus[layer]));
        screen.setLayerEq(layer,(float)json.optDouble("eqLow",screen.eqLow[layer]),(float)json.optDouble("eqMid",screen.eqMid[layer]),(float)json.optDouble("eqHigh",screen.eqHigh[layer]),(float)json.optDouble("eqLowFreq",screen.eqLowFreq[layer]),(float)json.optDouble("eqMidFreq",screen.eqMidFreq[layer]),(float)json.optDouble("eqHighFreq",screen.eqHighFreq[layer]),(float)json.optDouble("eqLowQ",screen.eqLowQ[layer]),(float)json.optDouble("eqMidQ",screen.eqMidQ[layer]),(float)json.optDouble("eqHighQ",screen.eqHighQ[layer]),(float)json.optDouble("eqHighPass",screen.eqHighPass[layer]),(float)json.optDouble("eqLowPass",screen.eqLowPass[layer]));
        screen.setLayerCompressor(layer,(float)json.optDouble("compThreshold",screen.compressorThreshold[layer]),(float)json.optDouble("compRatio",screen.compressorRatio[layer]),(float)json.optDouble("compAttack",screen.compressorAttackMs[layer]),(float)json.optDouble("compRelease",screen.compressorReleaseMs[layer]),(float)json.optDouble("compMakeup",screen.compressorMakeupDb[layer]));screen.setLayerReverb(layer,(float)json.optDouble("reverbSize",screen.reverbSize[layer]),(float)json.optDouble("reverbDamping",screen.reverbDamping[layer]),(float)json.optDouble("reverbWidth",screen.reverbWidth[layer]));
        screen.setLayerRouting(layer,json.optInt("routeChannel",screen.routeChannel[layer]),json.optInt("routeOctave",screen.routeOctave[layer]),json.optInt("routeVelocity",screen.routeVelocity[layer]),json.optBoolean("routeSustain",screen.routeSustain[layer]),json.optInt("routeMode",screen.routeMode[layer]));
        boolean requestedMute=json.optBoolean("mute",false);screen.solo[layer]=json.optBoolean("solo",false);prefs.edit().putBoolean("solo_"+layer,screen.solo[layer]).apply();
        if(engine.equals("HAMMOND")){screen.hammondLeslie[layer]=json.optInt("hammondLeslie",screen.hammondLeslie[layer]);screen.hammondPercussion[layer]=json.optInt("hammondPercussion",screen.hammondPercussion[layer]);for(int i=0;i<9;i++)screen.hammondBars[layer][i]=json.optInt("hammondBar"+i,screen.hammondBars[layer][i]);for(int i=0;i<4;i++)screen.hammondExtras[layer][i]=(float)json.optDouble("hammondExtra"+i,screen.hammondExtras[layer][i]);screen.persistHammond(layer);screen.applyHammond(layer);}
        if(engine.equals("ANALOG")){screen.setAnalogOsc1Tune(layer,(float)json.optDouble("analogOsc1Tune",screen.analogOsc1Tune[layer]));for(int i=0;i<19;i++)screen.setAnalogControl(layer,i,(float)json.optDouble("analogControl"+i,screen.analogControls[layer][i]));for(int i=0;i<3;i++){screen.setAnalogWave(layer,i,json.optInt("analogWave"+i,screen.analogWaves[layer][i]));screen.setAnalogOption(layer,i,json.optBoolean("analogEnabled"+i,screen.analogOscillatorEnabled[layer][i]));}screen.setAnalogOption(layer,3,json.optBoolean("analogPink",screen.analogPinkNoise[layer]));screen.setAnalogOption(layer,4,json.optBoolean("analogMono",screen.analogMonophonic[layer]));}
        JSONObject learn=json.optJSONObject("midiLearn");if(learn!=null){SharedPreferences.Editor cc=getSharedPreferences("midi_learn",MODE_PRIVATE).edit();for(int i=0;i<5;i++){String key="layer_"+layer+"_"+i;int value=learn.optInt("cc"+i,-1);if(value<0)cc.remove(key);else cc.putInt(key,value);cc.putBoolean(key+"_relative",value>=0&&learn.optBoolean("relative"+i,false));}cc.apply();}
        setLayerMuted(layer,requestedMute);screen.applyLayerGains();screen.invalidate();
    }

    private final class ClassicPlayerView extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final String[] names = new String[8];
        private String midiStatus = "MIDI USB: procurando...";
        private String audioStatus = "ÁUDIO: procurando...";
        private String account = "";
        private boolean midiSignal;
        private int lastNoteOff = -1;
        private String lastMidiControl = "";
        // Desktop builds open directly on the mixer; keep the same workflow on Android.
        private boolean liveSet = false;
        private boolean settings = false;
        private int outputIndex = 0;
        private int selected = 0;
        private boolean savingLiveSlot;
        private int liveBank;
        private int flashingPadLayer=-1,flashingPad=-1;
        private long padFlashExpires;
        private final int[] layerOrder={0,1,2,3,4,5};
        private final float[] layerVolumes = {0.8f, 0.8f, 0.8f, 0.8f, 0.8f, 0.8f};
        private final float[] layerPan={0,0,0,0,0,0};
        private final float[] layerAttack = {0.005f,0.005f,0.005f,0.005f,0.005f,0.005f};
        private final float[] layerRelease = {0.05f,0.05f,0.05f,0.05f,0.05f,0.05f};
        private final float[] layerCutoff={100,100,100,100,100,100},layerReverb={0,0,0,0,0,0},layerCompMix={0,0,0,0,0,0},layerChorus={0,0,0,0,0,0};
        private final float[] reverbSize={55,55,55,55,55,55},reverbDamping={45,45,45,45,45,45},reverbWidth={100,100,100,100,100,100};
        private final float[] compressorAttackMs={10,10,10,10,10,10},compressorReleaseMs={120,120,120,120,120,120},compressorMakeupDb={0,0,0,0,0,0};
        private final float[] eqLow = {0,0,0,0,0,0}, eqMid = {0,0,0,0,0,0}, eqHigh = {0,0,0,0,0,0};
        private final float[] eqLowFreq={220,220,220,220,220,220},eqMidFreq={1200,1200,1200,1200,1200,1200},eqHighFreq={4200,4200,4200,4200,4200,4200};
        private final float[] eqLowQ={.707f,.707f,.707f,.707f,.707f,.707f},eqMidQ={1,1,1,1,1,1},eqHighQ={.707f,.707f,.707f,.707f,.707f,.707f},eqHighPass={20,20,20,20,20,20},eqLowPass={20000,20000,20000,20000,20000,20000};
        private final float[] compressorThreshold = {.126f,.126f,.126f,.126f,.126f,.126f}, compressorRatio = {4f,4f,4f,4f,4f,4f};
        private final int[] routeChannel={-1,-1,-1,-1,-1,-1},routeOctave={0,0,0,0,0,0},routeVelocity={0,0,0,0,0,0},routeMode={0,0,0,0,0,0};
        private final boolean[] routeSustain={true,true,true,true,true,true};
        private final int[][] hammondBars=new int[6][9];private final int[] hammondLeslie=new int[6],hammondPercussion=new int[6];
        private final float[][] hammondExtras=new float[6][4];
        // Analog editor: the same 19 desktop parameters plus oscillator
        // waveforms/enables, pink noise, and mono/poly mode.
        private final float[][] analogControls=new float[6][19];
        private final float[] analogOsc1Tune=new float[6];
        private final int[][] analogWaves=new int[6][3];
        private final boolean[][] analogOscillatorEnabled=new boolean[6][3];
        private final boolean[] analogPinkNoise=new boolean[6],analogMonophonic=new boolean[6];
        private float masterReverb, masterChorus;
        private final boolean[] muted = new boolean[6];
        private final boolean[] solo = new boolean[6];
        private float masterVolume = 0.8f;
        private final String[] layerNames = {"SEM SOUNDFONT", "SEM SOUNDFONT", "SEM SOUNDFONT", "SEM SOUNDFONT", "SEM SOUNDFONT", "SEM SOUNDFONT"};
        private final String[] presetNames = {"", "", "", "", "", ""};
        private final String[] engineNames = {"VAZIA", "VAZIA", "VAZIA", "VAZIA", "VAZIA", "VAZIA"};

        ClassicPlayerView(Context context) { super(context); paint.setTypeface(android.graphics.Typeface.create("sans", 1)); loadLayerOrder();loadLayerPreferences();loadLiveNames(); }
        private void loadLayerOrder(){
            SharedPreferences prefs=getSharedPreferences("layers",MODE_PRIVATE);boolean[] seen=new boolean[6];
            for(int position=0;position<layerOrder.length;position++){
                int layer=prefs.getInt("layer_order_"+position,position);
                if(layer<0||layer>=layerOrder.length||seen[layer]){
                    for(int i=0;i<layerOrder.length;i++)layerOrder[i]=i;
                    return;
                }
                seen[layer]=true;layerOrder[position]=layer;
            }
        }
        private int orderPosition(int layer){for(int i=0;i<layerOrder.length;i++)if(layerOrder[i]==layer)return i;return -1;}
        boolean canMoveLayer(int layer,int direction){
            int position=orderPosition(layer);
            if(position<0||"VAZIA".equals(engineNames[layer]))return false;
            for(int i=position+direction;i>=0&&i<layerOrder.length;i+=direction)
                if(!"VAZIA".equals(engineNames[layerOrder[i]]))return true;
            return false;
        }
        void moveLayer(int layer,int direction){
            if(!canMoveLayer(layer,direction))return;
            int position=orderPosition(layer),other=position+direction;
            while("VAZIA".equals(engineNames[layerOrder[other]]))other+=direction;
            int moved=layerOrder[position];layerOrder[position]=layerOrder[other];layerOrder[other]=moved;
            SharedPreferences.Editor prefs=getSharedPreferences("layers",MODE_PRIVATE).edit();
            for(int i=0;i<layerOrder.length;i++)prefs.putInt("layer_order_"+i,layerOrder[i]);
            prefs.apply();invalidate();
        }
        private void loadLayerPreferences(){SharedPreferences p=getSharedPreferences("layers",MODE_PRIVATE);
            masterVolume=p.getFloat("master_volume",masterVolume);masterReverb=p.getFloat("master_reverb",masterReverb);masterChorus=p.getFloat("master_chorus",masterChorus);
            for(int i=0;i<6;i++){
            setAnalogPresetValues(i,p.getInt("analog_preset_"+i,0));
            for(int c=0;c<19;c++)analogControls[i][c]=p.getFloat("analog_control_"+i+"_"+c,analogControls[i][c]);
            analogOsc1Tune[i]=p.getFloat("analog_osc1_tune_"+i,analogOsc1Tune[i]);
            for(int osc=0;osc<3;osc++){analogWaves[i][osc]=p.getInt("analog_wave_"+i+"_"+osc,analogWaves[i][osc]);analogOscillatorEnabled[i][osc]=p.getBoolean("analog_enabled_"+i+"_"+osc,true);}
            analogPinkNoise[i]=p.getBoolean("analog_pink_"+i,analogPinkNoise[i]);analogMonophonic[i]=p.getBoolean("analog_mono_"+i,analogMonophonic[i]);
            routeChannel[i]=p.getInt("route_channel_"+i,-1);routeOctave[i]=p.getInt("route_octave_"+i,0);routeVelocity[i]=p.getInt("route_velocity_"+i,0);routeSustain[i]=p.getBoolean("route_sustain_"+i,true);routeMode[i]=p.getInt("route_mode_"+i,0);
            layerAttack[i]=p.getFloat("control_attack_"+i,layerAttack[i]);layerRelease[i]=p.getFloat("control_release_"+i,layerRelease[i]);
            eqLow[i]=p.contains("eq_low_db_"+i)?p.getFloat("eq_low_db_"+i,0):(float)(20*Math.log10(Math.max(.125f,p.getFloat("eq_low_"+i,1))));eqMid[i]=p.contains("eq_mid_db_"+i)?p.getFloat("eq_mid_db_"+i,0):(float)(20*Math.log10(Math.max(.125f,p.getFloat("eq_mid_"+i,1))));eqHigh[i]=p.contains("eq_high_db_"+i)?p.getFloat("eq_high_db_"+i,0):(float)(20*Math.log10(Math.max(.125f,p.getFloat("eq_high_"+i,1))));
            eqLowFreq[i]=p.getFloat("eq_low_freq_"+i,220);eqMidFreq[i]=p.getFloat("eq_mid_freq_"+i,1200);eqHighFreq[i]=p.getFloat("eq_high_freq_"+i,4200);eqLowQ[i]=p.getFloat("eq_low_q_"+i,.707f);eqMidQ[i]=p.getFloat("eq_mid_q_"+i,1);eqHighQ[i]=p.getFloat("eq_high_q_"+i,.707f);eqHighPass[i]=p.getFloat("eq_highpass_"+i,20);eqLowPass[i]=p.getFloat("eq_lowpass_"+i,20000);
            compressorThreshold[i]=p.getFloat("comp_threshold_"+i,compressorThreshold[i]);compressorRatio[i]=p.getFloat("comp_ratio_"+i,compressorRatio[i]);layerVolumes[i]=p.getFloat("control_volume_"+i,layerVolumes[i]);layerPan[i]=p.getFloat("control_pan_"+i,0);muted[i]=p.getBoolean("muted_"+i,false);solo[i]=p.getBoolean("solo_"+i,false);
            layerCutoff[i]=p.getFloat("control_cutoff_"+i,100);layerReverb[i]=p.getFloat("control_reverb_"+i,0);layerCompMix[i]=p.getFloat("control_comp_"+i,0);layerChorus[i]=p.getFloat("control_chorus_"+i,0);reverbSize[i]=p.getFloat("reverb_size_"+i,55);reverbDamping[i]=p.getFloat("reverb_damping_"+i,45);reverbWidth[i]=p.getFloat("reverb_width_"+i,100);compressorAttackMs[i]=p.getFloat("comp_attack_"+i,10);compressorReleaseMs[i]=p.getFloat("comp_release_"+i,120);compressorMakeupDb[i]=p.getFloat("comp_makeup_"+i,0);
            int preset=p.getInt("hammond_preset_"+i,0);for(int b=0;b<9;b++)hammondBars[i][b]=p.getInt("hammond_bar_"+i+"_"+b,-1);hammondLeslie[i]=p.getInt("hammond_leslie_"+i,preset==6?0:preset==7?2:1);hammondPercussion[i]=p.getInt("hammond_percussion_"+i,preset==3?1:0);hammondExtras[i][0]=p.getFloat("hammond_click_"+i,.15f);hammondExtras[i][1]=p.getFloat("hammond_leakage_"+i,.12f);hammondExtras[i][2]=p.getFloat("hammond_drive_"+i,.12f);hammondExtras[i][3]=p.getFloat("hammond_level_"+i,.8f);
        }}
        private void loadLiveNames(){SharedPreferences live=getSharedPreferences("live_set",MODE_PRIVATE);for(int i=0;i<8;i++)names[i]=live.getString("bank_"+liveBank+"_slot_"+i+"_name","VAZIO");postInvalidate();}
        void setMidiStatus(String value) { midiStatus = value; postInvalidate(); }
        void setAudioStatus(String value) { audioStatus = value; postInvalidate(); }
        void setAccount(String value) { account = value == null ? "" : value; postInvalidate(); }
        void setMidiSignal() { midiSignal = true; postInvalidateDelayed(180); }
        void setLastNoteOff(int note) { lastNoteOff = note; postInvalidate(); }
        void setLastMidiControl(int cc,int channel,int value){lastMidiControl="CC "+cc+" · CH "+(channel+1)+" · "+value;postInvalidate();}
        void setLayerName(int layer, String name) { if (layer >= 0 && layer < layerNames.length) { layerNames[layer] = name; postInvalidate(); } }
        void setPresetName(int layer, String name) { if (layer >= 0 && layer < presetNames.length) { presetNames[layer] = name == null ? "" : name; postInvalidate(); } }
        void setEngineName(int layer, String name) { if (layer >= 0 && layer < engineNames.length) { engineNames[layer] = name == null ? "VAZIA" : name; postInvalidate(); } }
        String engineName(int layer) { return layer >= 0 && layer < engineNames.length ? engineNames[layer] : "VAZIA"; }
        void setLiveName(int slot,String name){if(slot>=0&&slot<names.length){names[slot]=name;postInvalidate();}}
        void setLearnedVolume(int target,float value){if(target<6){layerVolumes[target]=value;getSharedPreferences("layers",MODE_PRIVATE).edit().putFloat("control_volume_"+target,value).apply();applyLayerGains();}else{masterVolume=value;getSharedPreferences("layers",MODE_PRIVATE).edit().putFloat("master_volume",value).apply();if(audioEngine!=null)audioEngine.setMaster(faderGain(value));if(padEngine!=null)padEngine.setMaster(faderGain(value));}postInvalidate();}
        void setLayerEnvelope(int layer,float attack,float release){if(layer<0||layer>=6)return;layerAttack[layer]=attack;layerRelease[layer]=release;getSharedPreferences("layers",MODE_PRIVATE).edit().putFloat("control_attack_"+layer,attack).putFloat("control_release_"+layer,release).apply();if(audioEngine!=null)audioEngine.setLayerEnvelope(layer,attack,release);}
        void setLayerPan(int layer,float value){if(layer<0||layer>=6)return;layerPan[layer]=Math.max(-1,Math.min(1,value));getSharedPreferences("layers",MODE_PRIVATE).edit().putFloat("control_pan_"+layer,layerPan[layer]).apply();if(audioEngine!=null)audioEngine.setLayerPan(layer,layerPan[layer]);if(isPadEngine(layer)&&padEngine!=null)padEngine.setPan(layerPan[layer],engineNames[layer].startsWith("CONT"));}
        void setLayerEq(int layer,float low,float mid,float high,float lowFreq,float midFreq,float highFreq,float lowQValue,float midQValue,float highQValue,float highPassValue,float lowPassValue){if(layer<0||layer>=6)return;eqLow[layer]=low;eqMid[layer]=mid;eqHigh[layer]=high;eqLowFreq[layer]=lowFreq;eqMidFreq[layer]=midFreq;eqHighFreq[layer]=highFreq;eqLowQ[layer]=lowQValue;eqMidQ[layer]=midQValue;eqHighQ[layer]=highQValue;eqHighPass[layer]=highPassValue;eqLowPass[layer]=lowPassValue;SharedPreferences.Editor p=getSharedPreferences("layers",MODE_PRIVATE).edit();p.putFloat("eq_low_db_"+layer,low).putFloat("eq_mid_db_"+layer,mid).putFloat("eq_high_db_"+layer,high).putFloat("eq_low_freq_"+layer,lowFreq).putFloat("eq_mid_freq_"+layer,midFreq).putFloat("eq_high_freq_"+layer,highFreq).putFloat("eq_low_q_"+layer,lowQValue).putFloat("eq_mid_q_"+layer,midQValue).putFloat("eq_high_q_"+layer,highQValue).putFloat("eq_highpass_"+layer,highPassValue).putFloat("eq_lowpass_"+layer,lowPassValue).apply();if(audioEngine!=null)audioEngine.setLayerEq(layer,low,mid,high,lowFreq,midFreq,highFreq,lowQValue,midQValue,highQValue,highPassValue,lowPassValue);if(isPadEngine(layer)&&padEngine!=null)padEngine.setEq(low,mid,high,lowFreq,midFreq,highFreq,lowQValue,midQValue,highQValue,highPassValue,lowPassValue,engineNames[layer].startsWith("CONT"));}
        void setLayerCompressor(int layer,float threshold,float ratio,float attack,float release,float makeup){if(layer<0||layer>=6)return;compressorThreshold[layer]=threshold;compressorRatio[layer]=ratio;compressorAttackMs[layer]=attack;compressorReleaseMs[layer]=release;compressorMakeupDb[layer]=makeup;getSharedPreferences("layers",MODE_PRIVATE).edit().putFloat("comp_threshold_"+layer,threshold).putFloat("comp_ratio_"+layer,ratio).putFloat("comp_attack_"+layer,attack).putFloat("comp_release_"+layer,release).putFloat("comp_makeup_"+layer,makeup).apply();if(audioEngine!=null)audioEngine.setLayerCompressor(layer,threshold,ratio,attack,release,makeup);}
        void setLayerTone(int layer,float cutoff,float reverb,float compMix,float chorus){if(layer<0||layer>=6)return;layerCutoff[layer]=cutoff;layerReverb[layer]=reverb;layerCompMix[layer]=compMix;layerChorus[layer]=chorus;getSharedPreferences("layers",MODE_PRIVATE).edit().putFloat("control_cutoff_"+layer,cutoff).putFloat("control_reverb_"+layer,reverb).putFloat("control_comp_"+layer,compMix).putFloat("control_chorus_"+layer,chorus).apply();if(audioEngine!=null)audioEngine.setLayerTone(layer,cutoff,reverb,compMix,chorus);}
        void setLayerReverb(int layer,float size,float damping,float width){if(layer<0||layer>=6)return;reverbSize[layer]=size;reverbDamping[layer]=damping;reverbWidth[layer]=width;getSharedPreferences("layers",MODE_PRIVATE).edit().putFloat("reverb_size_"+layer,size).putFloat("reverb_damping_"+layer,damping).putFloat("reverb_width_"+layer,width).apply();if(audioEngine!=null)audioEngine.setLayerReverb(layer,size,damping,width);}
        void setMasterEffects(float reverb,float chorus){masterReverb=reverb;masterChorus=chorus;getSharedPreferences("layers",MODE_PRIVATE).edit().putFloat("master_reverb",reverb).putFloat("master_chorus",chorus).apply();if(audioEngine!=null)audioEngine.setMasterEffects(reverb,chorus);postInvalidate();}
        void setLayerRouting(int layer,int channel,int octave,int velocity,boolean sustain){setLayerRouting(layer,channel,octave,velocity,sustain,layer>=0&&layer<6?routeMode[layer]:0);}
        void setLayerRouting(int layer,int channel,int octave,int velocity,boolean sustain,int mode){if(layer<0||layer>=6)return;routeChannel[layer]=channel;routeOctave[layer]=octave;routeVelocity[layer]=velocity;routeSustain[layer]=sustain;routeMode[layer]=mode;getSharedPreferences("layers",MODE_PRIVATE).edit().putInt("route_channel_"+layer,channel).putInt("route_octave_"+layer,octave).remove("route_low_"+layer).remove("route_high_"+layer).putInt("route_velocity_"+layer,velocity).putBoolean("route_sustain_"+layer,sustain).putInt("route_mode_"+layer,mode).apply();if(audioEngine!=null)audioEngine.setLayerRouting(layer,channel,octave,0,127,velocity,sustain,mode);}
        void applyLayerRouting(int layer){if(audioEngine!=null)audioEngine.setLayerRouting(layer,routeChannel[layer],routeOctave[layer],0,127,routeVelocity[layer],routeSustain[layer],routeMode[layer]);}
        private void setAnalogPresetValues(int layer,int preset){
            boolean[] pink={false},mono={false};float[] osc1Tune={0};
            AnalogPresetBank.apply(preset,analogControls[layer],analogWaves[layer],analogOscillatorEnabled[layer],pink,mono,osc1Tune);
            analogOsc1Tune[layer]=osc1Tune[0];analogPinkNoise[layer]=pink[0];analogMonophonic[layer]=mono[0];
        }
        void setAnalogPresetDefaults(int layer,int preset){if(layer<0||layer>=6)return;setAnalogPresetValues(layer,preset);SharedPreferences.Editor p=getSharedPreferences("layers",MODE_PRIVATE).edit().putInt("analog_preset_"+layer,preset).putFloat("analog_osc1_tune_"+layer,analogOsc1Tune[layer]).putBoolean("analog_pink_"+layer,analogPinkNoise[layer]).putBoolean("analog_mono_"+layer,analogMonophonic[layer]);for(int c=0;c<19;c++)p.putFloat("analog_control_"+layer+"_"+c,analogControls[layer][c]);for(int osc=0;osc<3;osc++)p.putInt("analog_wave_"+layer+"_"+osc,analogWaves[layer][osc]).putBoolean("analog_enabled_"+layer+"_"+osc,analogOscillatorEnabled[layer][osc]);p.apply();applyAnalog(layer);}
        void setAnalogOsc1Tune(int layer,float value){if(layer<0||layer>=6)return;analogOsc1Tune[layer]=Math.max(-24,Math.min(24,value));getSharedPreferences("layers",MODE_PRIVATE).edit().putFloat("analog_osc1_tune_"+layer,analogOsc1Tune[layer]).apply();applyAnalog(layer);}
        void setAnalogControl(int layer,int control,float value){if(layer<0||layer>=6||control<0||control>=19)return;analogControls[layer][control]=value;getSharedPreferences("layers",MODE_PRIVATE).edit().putFloat("analog_control_"+layer+"_"+control,value).apply();applyAnalog(layer);}
        void setAnalogWave(int layer,int oscillator,int value){if(layer<0||layer>=6||oscillator<0||oscillator>=3)return;analogWaves[layer][oscillator]=value;getSharedPreferences("layers",MODE_PRIVATE).edit().putInt("analog_wave_"+layer+"_"+oscillator,value).apply();applyAnalog(layer);}
        void setAnalogOption(int layer,int option,boolean value){if(layer<0||layer>=6)return;SharedPreferences.Editor p=getSharedPreferences("layers",MODE_PRIVATE).edit();if(option<3){analogOscillatorEnabled[layer][option]=value;p.putBoolean("analog_enabled_"+layer+"_"+option,value);}else if(option==3){analogPinkNoise[layer]=value;p.putBoolean("analog_pink_"+layer,value);}else{analogMonophonic[layer]=value;p.putBoolean("analog_mono_"+layer,value);}p.apply();applyAnalog(layer);}
        private void applyAnalog(int layer){if(audioEngine!=null&&layer>=0&&layer<6){int[] flags=new int[8];for(int i=0;i<3;i++){flags[i]=analogWaves[layer][i];flags[i+3]=analogOscillatorEnabled[layer][i]?1:0;}flags[6]=analogPinkNoise[layer]?1:0;flags[7]=analogMonophonic[layer]?1:0;float[] controls=new float[20];System.arraycopy(analogControls[layer],0,controls,0,19);controls[19]=analogOsc1Tune[layer];audioEngine.setAnalogControls(layer,controls,flags);}}
        void setHammondPresetDefaults(int layer,int preset){final float[][] bars={{.8f,.5f,1f,.8f,.3f,.5f,.2f,.3f,.2f},{.5f,.3f,1f,.6f,.2f,.3f,.1f,.1f,0f},{1f,.8f,1f,.9f,.7f,.8f,.6f,.7f,.6f},{.3f,.2f,1f,.7f,.1f,.2f,0f,0f,0f},{1f,1f,1f,1f,1f,1f,1f,1f,1f},{1f,.7f,1f,.9f,.5f,.7f,.3f,.5f,.4f},{.5f,.4f,1f,.7f,.3f,.4f,.2f,.2f,.1f},{.8f,.6f,1f,.9f,.5f,.7f,.4f,.5f,.3f}};for(int b=0;b<9;b++)hammondBars[layer][b]=Math.round(bars[preset][b]*8);hammondLeslie[layer]=preset==7?2:preset==6?0:1;hammondPercussion[layer]=preset==3?1:0;persistHammond(layer);applyHammond(layer);}
        void setHammondBar(int layer,int bar,int value){hammondBars[layer][bar]=value;persistHammond(layer);applyHammond(layer);}
        void setHammondOption(int layer,boolean leslie,int value){if(leslie)hammondLeslie[layer]=value;else hammondPercussion[layer]=value;persistHammond(layer);applyHammond(layer);}
        void setHammondExtra(int layer,int index,float value){hammondExtras[layer][index]=value;persistHammond(layer);applyHammond(layer);}
        private void persistHammond(int layer){SharedPreferences.Editor p=getSharedPreferences("layers",MODE_PRIVATE).edit();for(int b=0;b<9;b++)p.putInt("hammond_bar_"+layer+"_"+b,hammondBars[layer][b]);p.putInt("hammond_leslie_"+layer,hammondLeslie[layer]).putInt("hammond_percussion_"+layer,hammondPercussion[layer]).putFloat("hammond_click_"+layer,hammondExtras[layer][0]).putFloat("hammond_leakage_"+layer,hammondExtras[layer][1]).putFloat("hammond_drive_"+layer,hammondExtras[layer][2]).putFloat("hammond_level_"+layer,hammondExtras[layer][3]).apply();}
        private void applyHammond(int layer){if(audioEngine!=null){int[] bars=hammondBars[layer].clone();for(int b=0;b<9;b++)if(bars[b]<0)bars[b]=6;audioEngine.setHammondControls(layer,bars,hammondLeslie[layer],hammondPercussion[layer],hammondExtras[layer][0],hammondExtras[layer][1],hammondExtras[layer][2],hammondExtras[layer][3]);}}
        void applyLoadedEditorState(int layer){SharedPreferences p=getSharedPreferences("layers",MODE_PRIVATE);layerVolumes[layer]=p.getFloat("control_volume_"+layer,layerVolumes[layer]);layerPan[layer]=p.getFloat("control_pan_"+layer,0);muted[layer]=p.getBoolean("muted_"+layer,false);solo[layer]=p.getBoolean("solo_"+layer,false);applyLayerGains();if(audioEngine!=null){audioEngine.setLayerGain(layer,faderGain(layerVolumes[layer]));audioEngine.setLayerPan(layer,layerPan[layer]);audioEngine.setLayerEnvelope(layer,layerAttack[layer],layerRelease[layer]);audioEngine.setLayerEq(layer,eqLow[layer],eqMid[layer],eqHigh[layer],eqLowFreq[layer],eqMidFreq[layer],eqHighFreq[layer],eqLowQ[layer],eqMidQ[layer],eqHighQ[layer],eqHighPass[layer],eqLowPass[layer]);audioEngine.setLayerCompressor(layer,compressorThreshold[layer],compressorRatio[layer],compressorAttackMs[layer],compressorReleaseMs[layer],compressorMakeupDb[layer]);audioEngine.setLayerTone(layer,layerCutoff[layer],layerReverb[layer],layerCompMix[layer],layerChorus[layer]);audioEngine.setLayerReverb(layer,reverbSize[layer],reverbDamping[layer],reverbWidth[layer]);applyLayerRouting(layer);if("ANALOG".equals(engineName(layer)))applyAnalog(layer);if("HAMMOND".equals(engineName(layer)))applyHammond(layer);}if(isPadEngine(layer)&&padEngine!=null){boolean continuous=engineName(layer).startsWith("CONT");padEngine.setGain(faderGain(layerVolumes[layer]),continuous);padEngine.setPan(layerPan[layer],continuous);padEngine.setEq(eqLow[layer],eqMid[layer],eqHigh[layer],eqLowFreq[layer],eqMidFreq[layer],eqHighFreq[layer],eqLowQ[layer],eqMidQ[layer],eqHighQ[layer],eqHighPass[layer],eqLowPass[layer],continuous);}}

        private void text(Canvas canvas, String value, float x, float y, float size, int colour) {
            paint.setStyle(Paint.Style.FILL); paint.setColor(colour); paint.setTextSize(size);
            canvas.drawText(value, x, y, paint);
        }
        private void fittedText(Canvas canvas, String value, float left, float right, float y, float size, int colour) {
            if (value == null || value.isEmpty() || right <= left) return;
            paint.setStyle(Paint.Style.FILL); paint.setColor(colour); paint.setTextAlign(Paint.Align.LEFT);
            float fittedSize = size;
            paint.setTextSize(fittedSize);
            float maxWidth = right - left;
            float measured = paint.measureText(value);
            if (measured > maxWidth) {
                fittedSize *= maxWidth / measured;
                fittedSize = Math.max(size * .72f, fittedSize);
                paint.setTextSize(fittedSize);
            }
            String visible = value;
            while (visible.length() > 1 && paint.measureText(visible) > maxWidth) {
                visible = visible.substring(0, visible.length() - 1);
            }
            if (!visible.equals(value) && visible.length() > 1) visible = visible.substring(0, visible.length() - 1) + "…";
            canvas.drawText(visible, left, y, paint);
        }
        private void fittedCenteredText(Canvas canvas,String value,float left,float right,float y,float size,int colour){
            if(value==null||value.isEmpty()||right<=left)return;
            paint.setStyle(Paint.Style.FILL);paint.setColor(colour);paint.setTextSize(size);
            float width=right-left;
            if(paint.measureText(value)>width)paint.setTextSize(Math.max(size*.65f,size*width/paint.measureText(value)));
            String visible=value;
            while(visible.length()>1&&paint.measureText(visible)>width)visible=visible.substring(0,visible.length()-1);
            if(!visible.equals(value)&&visible.length()>1)visible=visible.substring(0,visible.length()-1)+"…";
            paint.setTextAlign(Paint.Align.CENTER);canvas.drawText(visible,(left+right)*.5f,y,paint);paint.setTextAlign(Paint.Align.LEFT);
        }
        private void box(Canvas canvas, float left, float top, float right, float bottom, int colour, boolean outline) {
            paint.setColor(colour); paint.setStyle(outline ? Paint.Style.STROKE : Paint.Style.FILL); paint.setStrokeWidth(2f);
            canvas.drawRoundRect(left, top, right, bottom, 10f, 10f, paint);
        }
        private void button(Canvas canvas, String label, float left, float top, float right, float bottom, boolean active) {
            final int teal = Color.rgb(19, 184, 173);
            box(canvas, left, top, right, bottom, active ? teal : Color.rgb(31, 48, 62), false);
            paint.setTextAlign(Paint.Align.CENTER);
            float fontSize = (bottom - top) * .40f;
            paint.setTextSize(fontSize);
            float available = Math.max(1f, right - left - 18f);
            float measured = paint.measureText(label);
            if (measured > available) fontSize *= available / measured;
            text(canvas, label, (left + right) * .5f, top + (bottom - top) * .66f, fontSize,
                    active ? Color.rgb(7,16,25) : Color.rgb(233,239,240));
            paint.setTextAlign(Paint.Align.LEFT);
        }
        private void drawLogo(Canvas canvas, float cx, float cy, float radius) {
            Drawable logo = getDrawable(R.drawable.classic_keys_samples_logo);
            int r = Math.round(radius);
            logo.setBounds(Math.round(cx) - r, Math.round(cy) - r, Math.round(cx) + r, Math.round(cy) + r);
            logo.draw(canvas);
        }
        private void drawMeter(Canvas canvas, float x, float top, float width, float bottom, float peak) {
            paint.setColor(Color.rgb(5,13,19)); canvas.drawRoundRect(x, top, x + width, bottom, 4, 4, paint);
            float clamped = Math.max(0f, Math.min(1f, peak));
            float meterTop = bottom - (bottom - top) * clamped;
            if (clamped > .001f) {
                paint.setColor(clamped > .85f ? Color.rgb(245,92,72) : clamped > .66f ? Color.rgb(230,196,55) : Color.rgb(50,210,148));
                canvas.drawRoundRect(x + 3, meterTop, x + width - 3, bottom - 3, 3, 3, paint);
            }
        }
        private void drawFader(Canvas canvas, float x, float top, float bottom, float cardW, float value) {
            paint.setColor(Color.rgb(5,13,19)); canvas.drawRoundRect(x - 7, top, x + 7, bottom, 5, 5, paint);
            paint.setColor(Color.rgb(46, 66, 79)); canvas.drawRoundRect(x - 2, top + 4, x + 2, bottom - 4, 2, 2, paint);
            float knobY = bottom - (bottom - top) * value;
            // Desktop fader grip is narrow even on the wider pad strips.
            float thumbHalfWidth=Math.min(cardW*.24f,getHeight()*.016f);
            paint.setColor(Color.rgb(210,219,223)); canvas.drawRoundRect(x-thumbHalfWidth, knobY-12, x+thumbHalfWidth, knobY+12, 4, 4, paint);
            paint.setColor(Color.rgb(95,107,113));
            for (int line = -6; line <= 6; line += 4) canvas.drawRect(x-thumbHalfWidth*.83f, knobY+line, x+thumbHalfWidth*.83f, knobY+line+1.5f, paint);
        }
        // The physical position follows the desktop mixer: unity is at 80%,
        // with extra travel for +3/+6 dB and finer control below 0 dB.
        private float faderDb(float value) {
            final float[] positions = {0f, .05f, .25f, .50f, .67f, .80f, .90f, 1f};
            final float[] decibels = {-60f, -40f, -20f, -10f, -5f, 0f, 3f, 6f};
            float v = Math.max(0f, Math.min(1f, value));
            for (int i = 1; i < positions.length; i++) {
                if (v <= positions[i]) {
                    float amount = (v - positions[i - 1]) / (positions[i] - positions[i - 1]);
                    return decibels[i - 1] + amount * (decibels[i] - decibels[i - 1]);
                }
            }
            return 6f;
        }
        private float faderGain(float value) {
            if (value <= 0f) return 0f;
            return (float) Math.pow(10.0, faderDb(value) / 20.0);
        }
        private String faderLabel(float value) {
            if (value <= 0f) return "−∞ dB";
            int db = Math.round(faderDb(value));
            return (db > 0 ? "+" : "") + db + " dB";
        }
        private boolean hasSolo() { for (boolean value : solo) if (value) return true; return false; }
        private void applyLayerGains() {
            boolean anySolo = hasSolo();
            for (int i = 0; i < 6; i++) {
                float gain=(!muted[i] && (!anySolo || solo[i])) ? faderGain(layerVolumes[i]) : 0f;
                if(audioEngine!=null)audioEngine.setLayerGain(i,gain);
                if(isPadLayer(i)&&padEngine!=null){boolean continuous=engineNames[i].startsWith("CONT");padEngine.setEnabled(gain>0f,continuous);padEngine.setGain(gain,continuous);}
            }
            if(padEngine!=null)padEngine.setMaster(faderGain(masterVolume));
        }

        @Override protected void onDraw(Canvas canvas) {
            final float w = getWidth(), h = getHeight();
            canvas.drawColor(Color.rgb(7, 16, 25));
            final int teal = Color.rgb(19, 184, 173), text = Color.rgb(233, 239, 240), panel = Color.rgb(19, 31, 42);
            box(canvas, 0, 0, w, h * .145f, Color.rgb(9, 20, 30), false);
            drawLogo(canvas, 52, h*.065f, h*.045f);
            text(canvas, "CLASSIC KEYS", 94, h * .05f, h * .023f, teal);
            text(canvas, "CLASSIC PLAYER", 94, h * .095f, h * .047f, text);
            if (!account.isEmpty()) text(canvas, account, 94, h * .125f, h * .015f, Color.rgb(19,184,173));
            fittedText(canvas, liveSet ? "LIVE SET" : settings ? "ÁUDIO / MIDI" : "MIXER",
                    w * .38f, w * .57f, h * .078f, h * .052f, text);
            float statusLeft = w * .61f, statusRight = w * .99f;
            fittedText(canvas, midiStatus, statusLeft, statusRight, h * .05f, h * .018f, Color.rgb(180, 195, 200));
            if (!lastMidiControl.isEmpty()) fittedText(canvas,lastMidiControl,statusLeft,statusRight,h*.069f,h*.013f,Color.rgb(80,190,174));
            else if (lastNoteOff >= 0) fittedText(canvas, "NOTE OFF " + lastNoteOff, statusLeft, statusRight, h * .069f, h * .013f, Color.rgb(80, 190, 174));
            fittedText(canvas, audioStatus, statusLeft, statusRight, h * .087f, h * .015f, Color.rgb(180, 195, 200));
            paint.setColor(midiSignal ? Color.rgb(40, 220, 110) : Color.rgb(70, 90, 95));
            canvas.drawCircle(w * .595f, h * .05f, h * .011f, paint);
            midiSignal = false;
            if (!liveSet && !settings && firstEmptyLayer() >= 0)
                button(canvas, "+ LAYER", w*.65f, h*.101f, w*.75f, h*.139f, false);
            // Keep all navigation inside the header so it never covers Layer 6.
            button(canvas, "MIXER", w*.755f, h*.101f, w*.83f, h*.139f, !liveSet && !settings);
            button(canvas, "LIVE SET", w*.835f, h*.101f, w*.91f, h*.139f, liveSet);
            button(canvas, "ÁUDIO/MIDI", w*.915f, h*.101f, w*.995f, h*.139f, settings);

            if (settings) {
                box(canvas, 28, h * .17f, w - 28, h * .86f, panel, true);
                text(canvas, "ÁUDIO / MIDI", 52, h * .25f, h * .04f, text);
                text(canvas, audioStatus, 52, h * .34f, h * .026f, Color.rgb(180,195,200));
                text(canvas, midiStatus, 52, h * .42f, h * .026f, Color.rgb(180,195,200));
                text(canvas, "Toque nas linhas acima para alternar a saída e o controlador.", 52, h * .54f, h * .022f, Color.rgb(180,195,200));
                float actionRight = Math.min(w - 52, 430);
                button(canvas, "MIDI LEARN · VOLUME", 52, h*.59f, actionRight, h*.66f, pendingLearnTarget>=0);
                button(canvas, "PARAR TODAS AS NOTAS", 52, h*.69f, actionRight, h*.76f, false);
                button(canvas, "VOLTAR AO MIXER", 52, h*.79f, actionRight, h*.86f, false);
                button(canvas, "AJUSTAR BUFFER · "+audioEngine.bufferFrames()+" FRAMES",w*.48f,h*.59f,w-52,h*.66f,false);
                text(canvas,audioEngine.outputStatus(),w*.48f,h*.71f,h*.018f,text);
                text(canvas,audioEngine.outputMode(),w*.48f,h*.75f,h*.016f,text);
                text(canvas,audioEngine.outputLatency(),w*.48f,h*.79f,h*.016f,text);
                button(canvas,"REINICIAR ÁUDIO",w*.48f,h*.81f,w-52,h*.86f,false);
                return;
            }

            if (!liveSet) {
                drawMixer(canvas, w, h, text, teal, panel);
                return;
            }
            final float tabTop = h * .14f, tabs = w / 8f;
            for (int i = 0; i < 8; i++) {
                if (i == liveBank) box(canvas, i * tabs + 4, tabTop, (i + 1) * tabs - 4, tabTop + h * .065f, teal, false);
                text(canvas, "BANCO " + (i + 1), i * tabs + tabs * .18f, tabTop + h * .043f, h * .025f, i == liveBank ? Color.rgb(7,16,25) : text);
            }
            final float margin = 22, top = h * .235f, gap = 14;
            final float cardW = (w - margin * 2 - gap * 3) / 4f, cardH = (h * .69f - top - gap) / 2f;
            for (int i = 0; i < 8; i++) {
                int col = i % 4, row = i / 4;
                float x = margin + col * (cardW + gap), y = top + row * (cardH + gap);
                box(canvas, x, y, x + cardW, y + cardH, i == selected ? teal : Color.rgb(64, 82, 96), true);
                text(canvas, String.format("%02d", i + 1), x + cardW * .40f, y + cardH * .29f, cardH * .28f, teal);
                paint.setColor(teal); canvas.drawRect(x + 18, y + cardH * .40f, x + cardW - 18, y + cardH * .407f, paint);
                text(canvas, names[i], x + 20, y + cardH * .67f, cardH * .15f, text);
                text(canvas, names[i].equals("VAZIO") ? "TOQUE PARA CONFIGURAR" : "PROGRAMA SALVO", x + 20, y + cardH * .84f, cardH * .09f, Color.rgb(180,195,200));
            }
            button(canvas, savingLiveSlot?"CANCELAR SALVAMENTO":"SALVAR SLOT", margin, h*.915f, margin+260, h*.975f, savingLiveSlot);
        }

        private int activeLayerCount() {
            int count = 0;
            for (String engine : engineNames) if (!"VAZIA".equals(engine)) count++;
            return count;
        }
        private int activeLayerAt(int position) {
            for (int i : layerOrder) {
                if (!"VAZIA".equals(engineNames[i]) && position-- == 0) return i;
            }
            return -1;
        }
        private int firstEmptyLayer() {
            for (int i = 0; i < engineNames.length; i++) if ("VAZIA".equals(engineNames[i])) return i;
            return -1;
        }
        private boolean isPadLayer(int layer){return engineNames[layer].contains("PADS");}
        private float mixerCardScale(float w){
            float desired=0;
            int count=activeLayerCount();
            for(int position=0;position<count;position++)desired+=w*(isPadLayer(activeLayerAt(position))?.215f:.115f);
            float available=w-36f-105f-20f-Math.max(0,count-1)*10f;
            return desired>0?Math.min(1f,available/desired):1f;
        }
        private float mixerCardWidth(int layer,float w) {
            return w*(isPadLayer(layer)?.215f:.115f)*mixerCardScale(w);
        }
        private float mixerCardX(int position,float w){
            float x=18;
            for(int p=0;p<position;p++)x+=mixerCardWidth(activeLayerAt(p),w)+10;
            return x;
        }
        private void drawMixer(Canvas canvas, float w, float h, int textColour, int teal, int panel) {
            final float left = 18, top = h * .17f, gap = 10;
            final float cardH = h * .72f;
            int activeCount = activeLayerCount();
            text(canvas, "MIXER · " + activeCount + " / 6 LAYERS", left, h * .16f, h * .024f, Color.rgb(180,195,200));
            if (activeCount == 0)
                text(canvas, "TOQUE EM + LAYER PARA ADICIONAR UM MOTOR", left + 12, h * .36f, h * .024f, Color.rgb(180,195,200));
            for (int position = 0; position < activeCount; position++) {
                int i = activeLayerAt(position);
                float cardW=mixerCardWidth(i,w);
                float x=mixerCardX(position,w);
                box(canvas, x, top, x + cardW, top + cardH, Color.rgb(49, 69, 82), true);
                text(canvas, "LAYER " + (i + 1), x + 12, top + h * .045f, h * .022f, textColour);
                text(canvas, engineNames[i], x+12, top+h*.065f, h*.012f, teal);
                button(canvas, "M", x + cardW*.54f, top+h*.016f, x+cardW*.70f, top+h*.063f, muted[i]);
                button(canvas, "S", x + cardW*.74f, top+h*.016f, x+cardW*.90f, top+h*.063f, solo[i]);
                String source = presetNames[i].isEmpty() ? layerNames[i] : presetNames[i];
                fittedText(canvas, source, x + 12, x + cardW - 12, top + h * .086f, h * .015f, Color.rgb(180,195,200));
                button(canvas, "EDITAR", x+12, top+h*.098f, x+cardW*.72f, top+h*.15f, false);
                button(canvas, "⋯", x+cardW*.74f, top+h*.098f, x+cardW-12, top+h*.15f, false);
                if(isPadLayer(i)){
                    boolean continuous=engineNames[i].startsWith("CONT");
                    int columns=continuous?3:2,count=continuous?12:8;
                    float gridLeft=x+12,gridRight=x+cardW*.78f;
                    float gridTop=top+h*.185f,gridBottom=top+cardH-h*(continuous?.12f:.06f);
                    float cellW=(gridRight-gridLeft-(columns-1)*6)/columns;
                    float cellH=(gridBottom-gridTop-3*8)/4;
                    for(int pad=0;pad<count;pad++){
                        int column=pad%columns,row=pad/columns;
                        float px=gridLeft+column*(cellW+6),py=gridTop+row*(cellH+8);
                        boolean loaded=padEngine.loaded(pad,continuous);
                        boolean flashing=loaded&&flashingPadLayer==i&&flashingPad==pad&&android.os.SystemClock.uptimeMillis()<padFlashExpires;
                        int padFill=flashing?0xffffd84a:loaded?padColour(pad):Color.rgb(49,63,73);
                        paint.setShader(null);paint.setStyle(Paint.Style.FILL);paint.setColor(padFill);
                        canvas.drawRoundRect(px,py,px+cellW,py+cellH,8,8,paint);
                        paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(2f);paint.setColor(Color.rgb(54,68,77));
                        canvas.drawRoundRect(px,py,px+cellW,py+cellH,8,8,paint);paint.setStyle(Paint.Style.FILL);
                        fittedCenteredText(canvas,loaded?padEngine.name(pad,continuous):"PAD "+(pad+1),px+8,px+cellW-8,py+cellH*.55f,h*.015f,loaded?Color.rgb(21,25,29):textColour);
                    }
                    if(continuous)button(canvas,"STOP",gridLeft,top+cardH-h*.085f,gridRight,top+cardH-h*.027f,false);
                    float railTop=top+h*.185f,railBottom=top+cardH-h*.09f;
                    drawMeter(canvas,x+cardW*.81f,railTop,cardW*.025f,railBottom,0f);
                    drawFader(canvas,x+cardW*.91f,railTop,railBottom,cardW,layerVolumes[i]);
                    paint.setTextAlign(Paint.Align.CENTER);
                    text(canvas,faderLabel(layerVolumes[i]),x+cardW*.89f,top+cardH-h*.027f,h*.016f,textColour);
                    paint.setTextAlign(Paint.Align.LEFT);
                    continue;
                }
                float railTop = top + h * .205f, railBottom = top + cardH - h * .09f;
                drawMeter(canvas, x+cardW*.12f, railTop, cardW*.105f, railBottom,
                        audioEngine == null ? 0f : audioEngine.layerPeak(i));
                float railX = x + cardW * .57f;
                drawFader(canvas, railX, railTop, railBottom, cardW, layerVolumes[i]);
                final String[] ticks = {"+6", "+3", "0", "−5", "−10", "−20", "−40"};
                for (int tick = 0; tick < ticks.length; tick++) {
                    float y = railTop + (railBottom-railTop)*tick/(ticks.length-1);
                    text(canvas, ticks[tick], x+cardW*.70f, y+4, h*.012f, Color.rgb(131,151,165));
                }
                paint.setTextAlign(Paint.Align.CENTER);
                text(canvas, faderLabel(layerVolumes[i]), x + cardW*.55f, top + cardH - h*.027f, h*.016f, textColour);
                paint.setTextAlign(Paint.Align.LEFT);
            }
            float masterX = w - left - 105;
            box(canvas, masterX, top, masterX+105, top+cardH, Color.rgb(49,69,82), true);
            paint.setTextAlign(Paint.Align.CENTER); text(canvas, "MASTER", masterX+52, top+h*.05f, h*.019f, textColour); paint.setTextAlign(Paint.Align.LEFT);
            float masterTop = top+h*.12f, masterBottom = top+cardH-h*.09f;
            drawMeter(canvas, masterX+15, masterTop, 13, masterBottom, audioEngine == null ? 0f : audioEngine.masterPeak());
            drawFader(canvas, masterX+60, masterTop, masterBottom, 105, masterVolume);
            paint.setTextAlign(Paint.Align.CENTER); text(canvas, faderLabel(masterVolume), masterX+55, top+cardH-h*.027f, h*.016f, textColour); paint.setTextAlign(Paint.Align.LEFT);
            paint.setColor(Color.rgb(49,69,82)); canvas.drawRect(left, h*.91f, w-left, h*.912f, paint);
            button(canvas, "PANIC", w*.88f, h*.925f, w-left, h*.975f, false);
            postInvalidateDelayed(70);
        }

        @Override public boolean onTouchEvent(MotionEvent event) {
            final int action = event.getActionMasked();
            final boolean tap = action == MotionEvent.ACTION_UP;
            final boolean dragging = action == MotionEvent.ACTION_DOWN
                    || action == MotionEvent.ACTION_MOVE || tap;
            final float w = getWidth(), h = getHeight();
            if (tap && !liveSet && !settings && event.getY() > h*.09f && event.getY() < h*.145f
                    && event.getX() >= w*.65f && event.getX() < w*.75f) {
                int empty = firstEmptyLayer();
                if (empty >= 0) chooseLayerSource(empty);
                return true;
            }
            if (tap && event.getY() > h*.09f && event.getY() < h*.145f && event.getX() > w*.75f) {
                if (event.getX() < w*.832f) { liveSet = false; settings = false; }
                else if (event.getX() < w*.912f) { liveSet = true; settings = false; }
                else { settings = true; liveSet = false; }
                invalidate(); return true;
            }
            if (settings && tap) {
                if(event.getX()>w*.48f&&event.getY()>h*.57f){
                    if(event.getY()<h*.67f)showAudioBufferChooser();
                    else if(event.getY()>h*.81f&&event.getY()<h*.87f){audioEngine.stop();audioEngine.start();invalidate();}
                    return true;
                }
                if (event.getY() > h * .28f && event.getY() < h * .40f && audioOutputManager != null) {
                    showAudioOutputChooser();
                    return true;
                }
                if (event.getY() > h * .40f && event.getY() < h * .52f && midiManager != null) {
                    showMidiDeviceChooser();return true;
                }
                if(event.getY()>h*.57f&&event.getY()<h*.67f){showMidiLearnChooser();return true;}
                if (event.getY() > h*.67f && event.getY() < h*.77f) {
                    if (audioEngine != null) audioEngine.allNotesOff();
                    invalidate(); return true;
                }
                if (event.getY() > h*.77f && event.getY() < h*.88f) { settings=false; invalidate(); }
                return true;
            }
            if (liveSet && tap && event.getY() > h * .235f && event.getY() < h * .90f) {
                float cardW = (w - 44 - 42) / 4f;
                int col = (int) ((event.getX() - 22) / (cardW + 14));
                int row = event.getY() > h * .55f ? 1 : 0;
                if (col >= 0 && col < 4) { selected = row * 4 + col; if(savingLiveSlot){savingLiveSlot=false;saveLiveSlot(liveBank,selected);}else loadLiveSlot(liveBank,selected);invalidate(); }
            }
            if(liveSet&&tap&&event.getY()>h*.14f&&event.getY()<h*.22f){liveBank=Math.max(0,Math.min(7,(int)(event.getX()/(w/8f))));selected=0;loadLiveNames();return true;}
            if(liveSet&&tap&&event.getY()>h*.90f&&event.getX()<300){savingLiveSlot=!savingLiveSlot;invalidate();return true;}
            if (!liveSet && !settings && tap && event.getY() >= h*.925f && event.getY() <= h*.975f) {
                if (event.getX() >= w*.88f && event.getX() <= w-18) { panicMidiState(); invalidate(); return true; }
            }
            if (!liveSet && dragging && event.getY() > h * .17f && event.getY() < h * .87f) {
                float masterX = w - 18 - 105;
                if (event.getX() >= masterX && event.getX() <= masterX+105) {
                    float railTop = h*.17f+h*.12f, railBottom = h*.17f+h*.72f-h*.09f;
                    if (event.getY() >= railTop-h*.035f && event.getY() <= railBottom+h*.035f) {
                        masterVolume = Math.max(0f, Math.min(1f, (railBottom - event.getY()) / (railBottom - railTop)));
                        getSharedPreferences("layers",MODE_PRIVATE).edit().putFloat("master_volume",masterVolume).apply();
                        if (audioEngine != null) audioEngine.setMaster(faderGain(masterVolume));
                        if (padEngine != null) padEngine.setMaster(faderGain(masterVolume));
                        invalidate(); return true;
                    }
                }
                int position=-1,layer=-1;
                for(int p=0;p<activeLayerCount();p++){
                    int candidate=activeLayerAt(p);float cardX=mixerCardX(p,w),cardW=mixerCardWidth(candidate,w);
                    if(event.getX()>=cardX&&event.getX()<=cardX+cardW){position=p;layer=candidate;break;}
                }
                if (layer >= 0) {
                    float cardX=mixerCardX(position,w),cardW=mixerCardWidth(layer,w);
                    if (tap && event.getY() >= h*.17f+h*.016f && event.getY() <= h*.17f+h*.063f) {
                        if (event.getX() >= cardX+cardW*.54f && event.getX() <= cardX+cardW*.70f) { setLayerMuted(layer,!muted[layer]); return true; }
                        else if (event.getX() >= cardX+cardW*.74f && event.getX() <= cardX+cardW*.90f) solo[layer] = !solo[layer];
                        applyLayerGains(); invalidate(); return true;
                    }
                    if (tap && event.getY() >= h*.17f+h*.098f && event.getY() <= h*.17f+h*.16f) {
                        if(event.getX() >= cardX+cardW*.74f)showLayerActions(layer);
                        else openLayerEditor(layer);
                        return true;
                    }
                    if(isPadLayer(layer)){
                        boolean continuous=engineNames[layer].startsWith("CONT");
                        int columns=continuous?3:2;
                        float gridLeft=cardX+12,gridRight=cardX+cardW*.78f;
                        float gridTop=h*.17f+h*.185f,gridBottom=h*.17f+h*.72f-h*(continuous?.12f:.06f);
                        if(tap&&continuous&&event.getX()>=gridLeft&&event.getX()<=gridRight&&event.getY()>=h*.17f+h*.72f-h*.085f&&event.getY()<=h*.17f+h*.72f-h*.027f){padEngine.stopAll(true);invalidate();return true;}
                        if(action==MotionEvent.ACTION_DOWN&&event.getX()>=gridLeft&&event.getX()<=gridRight&&event.getY()>=gridTop&&event.getY()<=gridBottom){
                            float cellW=(gridRight-gridLeft-(columns-1)*6)/columns,cellH=(gridBottom-gridTop-3*8)/4;
                            int column=(int)((event.getX()-gridLeft)/(cellW+6)),row=(int)((event.getY()-gridTop)/(cellH+8));
                            int pad=row*columns+column;
                            if(column<columns&&row<4&&pad<(continuous?12:8)&&event.getX()<=gridLeft+column*(cellW+6)+cellW&&event.getY()<=gridTop+row*(cellH+8)+cellH){
                                if(padEngine.loaded(pad,continuous)){padEngine.trigger(pad,continuous);flashingPadLayer=layer;flashingPad=pad;padFlashExpires=android.os.SystemClock.uptimeMillis()+180;postInvalidateDelayed(190);}else openPadPicker(pad,continuous);
                                invalidate();return true;
                            }
                        }
                        float railTop=h*.17f+h*.185f,railBottom=h*.17f+h*.72f-h*.09f;
                        if(event.getX()>=cardX+cardW*.82f&&event.getY()>=railTop-h*.035f&&event.getY()<=railBottom+h*.035f){
                            layerVolumes[layer]=Math.max(0f,Math.min(1f,(railBottom-event.getY())/(railBottom-railTop)));
                            applyLayerGains();invalidate();return true;
                        }
                        return true;
                    }
                    float railTop = h*.17f+h*.205f, railBottom = h*.17f+h*.72f-h*.09f;
                    // Handle the whole drag gesture, not only ACTION_UP. The
                    // wider hit area makes the fader usable on touchscreens.
                    if (event.getY() >= railTop - h*.035f && event.getY() <= railBottom + h*.035f) {
                        layerVolumes[layer] = Math.max(0f, Math.min(1f, (railBottom - event.getY()) / (railBottom - railTop)));
                        applyLayerGains();
                        invalidate();
                        return true;
                    }
                }
            }
            return true;
        }
    }
}
