package com.classickeys.classicplayer;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.Dialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.content.pm.ActivityInfo;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.drawable.Drawable;
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
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;

/**
 * First Android surface for Classic Player. The audio monitor runs alongside
 * the UI so MIDI/SoundFont integration can be tested without blocking it.
 */
public final class MainActivity extends Activity {
    private ClassicPlayerView screen;
    private MidiManager midiManager;
    private PolySynthEngine audioEngine;
    private MidiDevice midiDevice;
    private MidiOutputPort midiInput;
    private int pendingLayer = -1;
    private int pendingEngine = 1;
    private final String[] sf2Uris = new String[6];
    private SoundFontLayer[] soundFontLayers;
    private LicenseManager licenseManager;
    private AudioOutputManager audioOutputManager;
    private final AudioDeviceCallback audioDeviceCallback = new AudioDeviceCallback() {
        @Override public void onAudioDevicesAdded(AudioDeviceInfo[] added) { refreshMidiDevices(); restorePreferredAudioDevice(); }
        @Override public void onAudioDevicesRemoved(AudioDeviceInfo[] removed) { refreshMidiDevices(); }
    };
    private PadEngine padEngine;
    private int pendingPad = -1;
    private boolean pendingContinuous;
    private boolean padLayerActive;
    private boolean continuousPadActive;
    private int pendingLearnTarget = -1;
    private int midiIndex;
    private int midiRunningStatus;
    private int midiFirstData = -1;
    // Keep the same per-channel key state used by the desktop engines. It is
    // essential for distinguishing a real release from a sustain-pedal change.
    private final boolean[][] midiHeld = new boolean[16][128];
    private final boolean[] midiSustain = new boolean[16];
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
            // Learn is handled before the reserved MIDI CC filters so STOP
            // can intentionally use CC 120/123 when a controller provides it.
            if (pendingLearnTarget >= 0) {
                getSharedPreferences("midi_learn",MODE_PRIVATE).edit().putInt("cc_"+pendingLearnTarget,first).apply();
                screen.setMidiStatus("MIDI: CC "+first+" aprendido"); pendingLearnTarget=-1; return;
            }
            if(first!=64&&first!=120&&first!=123){
                SharedPreferences learn=getSharedPreferences("midi_learn",MODE_PRIVATE);for(int target=0;target<8;target++)if(learn.getInt("cc_"+target,-1)==first){if(target==7){padEngine.stopAll();screen.setMidiStatus("MIDI: pads parados");}else screen.setLearnedVolume(target,second/127f);return;}
            } else if (getSharedPreferences("midi_learn",MODE_PRIVATE).getInt("cc_7",-1)==first) {
                padEngine.stopAll(); screen.setMidiStatus("MIDI: pads parados"); return;
            }
            if (first == 64) {
                boolean wasDown = midiSustain[channel];
                midiSustain[channel] = second >= 64;
                audioEngine.setSustain(midiSustain[channel]);
                // Releasing the pedal must release every key that is no
                // longer physically held. The native engines are channel
                // agnostic, so send the complete pending note set.
                if (wasDown && !midiSustain[channel])
                    for (int note = 0; note < 128; note++)
                        if (!midiHeld[channel][note]) audioEngine.noteOff(note);
            } else if (first == 120 || first == 123) {
                panicMidiState();
            }
            return;
        }
        if (type != 0x80 && type != 0x90) return;
        screen.setMidiSignal();
        if (type == 0x90 && second > 0) {
            // Some controllers resend Note On before a missing release. Kill
            // the previous instance first so the voice can never accumulate.
            if (midiHeld[channel][first]) audioEngine.noteOff(first);
            midiHeld[channel][first] = true;
            if(padLayerActive&&first>=36&&first<48){padEngine.setContinuous(continuousPadActive);padEngine.trigger(first-36);}
            audioEngine.noteOn(first, second);
        }
        else {
            // Note On with velocity zero is the MIDI-standard equivalent of
            // Note Off. Both paths must reach every internal engine.
            midiHeld[channel][first] = false;
            audioEngine.noteOff(first);
            screen.setLastNoteOff(first);
        }
    }

    private void panicMidiState() {
        for (int channel = 0; channel < 16; channel++)
            java.util.Arrays.fill(midiHeld[channel], false);
        java.util.Arrays.fill(midiSustain, false);
        if (audioEngine != null) audioEngine.allNotesOff();
    }
    private final MidiManager.DeviceCallback midiCallback = new MidiManager.DeviceCallback() {
        @Override public void onDeviceAdded(MidiDeviceInfo device) { refreshMidiDevices(); }
        @Override public void onDeviceRemoved(MidiDeviceInfo device) { refreshMidiDevices(); }
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
        audioEngine = new PolySynthEngine();
        licenseManager = new LicenseManager(this);
        audioOutputManager = new AudioOutputManager(this);
        padEngine = new PadEngine(this);
        SharedPreferences padPrefs=getSharedPreferences("pads",MODE_PRIVATE);
        for(int p=0;p<12;p++){String path=padPrefs.getString("pad_"+p,null);if(path!=null)padEngine.load(p,path);}
        soundFontLayers = new SoundFontLayer[6];
        for (int i = 0; i < soundFontLayers.length; i++) soundFontLayers[i] = new SoundFontLayer(this);
        android.content.SharedPreferences prefs = getSharedPreferences("layers", MODE_PRIVATE);
        for (int i = 0; i < sf2Uris.length; i++) {
            int engine = prefs.getInt("engine_" + i, 1);
            if (engine == 2) {
                String dx7Path=prefs.getString("dx7_"+i,null);
                String dx7Name=prefs.getString("name_"+i,"Banco DX7");
                if(dx7Path!=null&&audioEngine.loadDx7(i,dx7Path)){
                    int patch=prefs.getInt("dx7_patch_"+i,0); audioEngine.setDx7Patch(i,patch);
                    screen.setLayerName(i,dx7Name); screen.setEngineName(i,"DX7"); screen.setPresetName(i,audioEngine.dx7PatchName(i,patch));
                }
                continue;
            }
            if(engine==3){
                int preset=prefs.getInt("analog_preset_"+i,0); audioEngine.activateAnalog(i); audioEngine.setAnalogPreset(i,preset);
                screen.setLayerName(i,"Classic Keys Analog"); screen.setEngineName(i,"ANALOG"); screen.setPresetName(i,audioEngine.analogPresetName(preset));
                continue;
            }
            if(engine==4){
                int preset=prefs.getInt("hammond_preset_"+i,0); audioEngine.activateHammond(i); audioEngine.setHammondPreset(i,preset);
                screen.setLayerName(i,"Classic Keys Hammond"); screen.setEngineName(i,"HAMMOND"); screen.setPresetName(i,audioEngine.hammondPresetName(preset));
                continue;
            }
            if(engine==5||engine==6){
                padLayerActive=true;continuousPadActive=engine==6;audioEngine.clearLayer(i);screen.setLayerName(i,engine==6?"Pads contínuos":"Drum Pads");screen.setEngineName(i,engine==6?"CONT. PADS":"DRUM PADS");screen.setPresetName(i,"12 pads · notas MIDI 36–47");continue;
            }
            sf2Uris[i] = prefs.getString("sf2_" + i, null);
            String name = prefs.getString("name_" + i, null);
            if (sf2Uris[i] != null) {
                String cachedPath = restoreSoundFont(sf2Uris[i], i);
                if (cachedPath != null && audioEngine.loadLayer(i, cachedPath)) {
                    Uri saved = Uri.parse(sf2Uris[i]);
                    soundFontLayers[i].load(saved, name);
                    screen.setLayerName(i, soundFontLayers[i].displayName());
                    screen.setEngineName(i, "SF2");
                    int preset = prefs.getInt("preset_" + i, 0);
                    if (audioEngine.setPreset(i, preset)) {
                        soundFontLayers[i].setPreset(preset);
                        screen.setPresetName(i, audioEngine.presetName(i, preset));
                    }
                }
            } else if (name != null) screen.setLayerName(i, name);
        }
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
        closeMidi();
        super.onPause();
    }

    @Override protected void onDestroy() {
        if (padEngine != null) padEngine.stopAll();
        if (audioEngine != null) audioEngine.close();
        super.onDestroy();
    }

    private void hideSystemBars() {
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
    }

    private void refreshMidiDevices() {
        if (screen == null) return;
        int count = midiManager == null ? 0 : midiManager.getDevices().length;
        screen.setMidiStatus(count == 0 ? "MIDI USB: nenhum dispositivo" :
                "MIDI USB: " + count + (count == 1 ? " dispositivo" : " dispositivos"));
        if (audioOutputManager != null) screen.setAudioStatus(audioOutputManager.outputs().isEmpty()
                ? "ÁUDIO: saída do sistema" : "ÁUDIO: " + audioOutputManager.outputs().get(0));
        if (count > 0 && midiDevice == null) openMidi(midiManager.getDevices()[Math.min(midiIndex, count - 1)]);
    }

    private void restorePreferredAudioDevice() {
        if (audioOutputManager == null || audioEngine == null) return;
        int id = getSharedPreferences("audio", MODE_PRIVATE).getInt("output_id", -1);
        AudioDeviceInfo device = audioOutputManager.deviceById(id);
        if (device != null && audioEngine.setPreferredDevice(device))
            screen.setAudioStatus("ÁUDIO: " + device.getProductName() + " (ID " + device.getId() + ")");
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
                        getSharedPreferences("audio", MODE_PRIVATE).edit().putInt("output_id", device.getId()).apply();
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
            midiDevice = device;
            MidiDeviceInfo.PortInfo[] ports = info.getPorts();
            for (MidiDeviceInfo.PortInfo port : ports) {
                if (port.getType() == MidiDeviceInfo.PortInfo.TYPE_OUTPUT) {
                    midiInput = device.openOutputPort(port.getPortNumber());
                    if (midiInput != null) midiInput.connect(midiReceiver);
                    break;
                }
            }
        }, mainHandler);
    }
    private void showMidiDeviceChooser(){
        if(midiManager==null)return;MidiDeviceInfo[] devices=midiManager.getDevices();
        if(devices.length==0){new AlertDialog.Builder(this).setTitle("CONTROLADOR MIDI").setMessage("Nenhum controlador MIDI USB foi encontrado.").setPositiveButton("OK",null).show();return;}
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
            if (which == 0) activateBundledDx7(layer, R.raw.dx7_bank_1, "Divine Masquerade 1");
            else if (which == 1) activateBundledDx7(layer, R.raw.dx7_bank_2, "Divine Masquerade 2");
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
            screen.setAudioStatus("ÁUDIO: banco DX7 interno inválido"); return;
        }
        screen.setLayerName(layer, displayName); screen.setEngineName(layer, "DX7");
        screen.setPresetName(layer, audioEngine.dx7PatchName(layer, 0));
        getSharedPreferences("layers", MODE_PRIVATE).edit().putInt("engine_" + layer, 2)
                .putString("dx7_" + layer, target.getAbsolutePath()).putString("name_" + layer, displayName)
                .putInt("dx7_patch_" + layer, 0).apply();
        screen.setAudioStatus("ÁUDIO: " + displayName + " carregado");
    }

    private void chooseLayerSource(int layer) {
        new AlertDialog.Builder(this).setTitle("TIPO DA LAYER " + (layer + 1))
                .setItems(new String[]{"SOUNDFONT 2 (.sf2)", "DX7 SYSEX (.syx)", "CLASSIC KEYS ANALOG", "HAMMOND / LESLIE", "DRUM PADS", "PADS CONTÍNUOS"}, (dialog, which) -> {
                    if (which == 0) openSf2Picker(layer); else if(which==1) openDx7Picker(layer); else if(which==2) activateAnalog(layer); else if(which==3) activateHammond(layer); else openPadEditor(layer,which==5);
                }).setNegativeButton("CANCELAR", null).show();
    }

    private void showLayerActions(final int layer) {
        final String engine = screen.engineName(layer);
        final Dialog dialog=new Dialog(this); LinearLayout root=new LinearLayout(this); root.setOrientation(LinearLayout.VERTICAL); root.setPadding(30,26,30,20); root.setBackgroundColor(Color.rgb(7,16,25));
        TextView title=new TextView(this); title.setText("LAYER "+(layer+1)+" · "+engine); title.setTextColor(Color.rgb(233,239,240)); title.setTextSize(22); root.addView(title,new LinearLayout.LayoutParams(-1,-2));
        String[] actions={"EDITAR MOTOR ATUAL","TROCAR MOTOR","LIMPAR LAYER"}; for(int i=0;i<actions.length;i++){final int action=i;Button b=new Button(this);b.setText(actions[i]);b.setTextColor(Color.WHITE);b.setBackgroundColor(i==2?Color.rgb(90,45,48):Color.rgb(31,70,86));b.setOnClickListener(v->{dialog.dismiss();if(action==1){panicAndChooseLayerSource(layer);return;}if(action==2){clearLayer(layer);return;}if(engine.equals("DX7"))openDx7Editor(layer);else if(engine.equals("ANALOG"))openAnalogEditor(layer);else if(engine.equals("HAMMOND"))openHammondEditor(layer);else if(engine.contains("PADS"))openPadEditor(layer,engine.startsWith("CONT"));else openSoundFontEditor(layer);});root.addView(b,new LinearLayout.LayoutParams(-1,-2));}
        Button cancel=new Button(this);cancel.setText("CANCELAR");cancel.setTextColor(Color.rgb(19,184,173));cancel.setBackgroundColor(Color.TRANSPARENT);cancel.setOnClickListener(v->dialog.dismiss());root.addView(cancel,new LinearLayout.LayoutParams(-1,-2));dialog.setContentView(root);dialog.show();if(dialog.getWindow()!=null){dialog.getWindow().setBackgroundDrawableResource(android.R.color.transparent);dialog.getWindow().setLayout(-1,-2);}
    }

    private void panicAndChooseLayerSource(int layer) {
        if (audioEngine != null) audioEngine.allNotesOff();
        if (padEngine != null) padEngine.stopAll();
        chooseLayerSource(layer);
    }

    private void clearLayer(int layer) {
        if (audioEngine != null) { audioEngine.allNotesOff(); audioEngine.clearLayer(layer); }
        if (padEngine != null) padEngine.stopAll();
        getSharedPreferences("layers", MODE_PRIVATE).edit()
                .remove("engine_"+layer).remove("sf2_"+layer).remove("dx7_"+layer)
                .remove("name_"+layer).remove("preset_"+layer).remove("dx7_patch_"+layer)
                .remove("analog_preset_"+layer).remove("hammond_preset_"+layer).apply();
        screen.setLayerName(layer,"Sem SoundFont"); screen.setEngineName(layer,"VAZIA"); screen.setPresetName(layer,"");
    }

    private void activateAnalog(int layer) {
        audioEngine.activateAnalog(layer); audioEngine.setAnalogPreset(layer,0);
        screen.setLayerName(layer,"Classic Keys Analog"); screen.setEngineName(layer,"ANALOG"); screen.setPresetName(layer,audioEngine.analogPresetName(0));
        getSharedPreferences("layers",MODE_PRIVATE).edit().putInt("engine_"+layer,3).putInt("analog_preset_"+layer,0).putString("name_"+layer,"Classic Keys Analog").apply();
    }

    private void openAnalogEditor(final int layer) {
        int count=audioEngine.analogPresetCount(); String[] presets=new String[count];
        for(int i=0;i<count;++i)presets[i]=audioEngine.analogPresetName(i);
        int selectedPreset=getSharedPreferences("layers",MODE_PRIVATE).getInt("analog_preset_"+layer,0);
        showPresetEditor(layer, "LAYER "+(layer+1)+" · CLASSIC KEYS ANALOG", "Selecione o banco e o timbre desta layer", presets, selectedPreset,
                which -> { if(audioEngine.setAnalogPreset(layer,which)){screen.setPresetName(layer,audioEngine.analogPresetName(which));getSharedPreferences("layers",MODE_PRIVATE).edit().putInt("analog_preset_"+layer,which).apply();} }, "TROCAR MOTOR", () -> panicAndChooseLayerSource(layer));
    }

    private void activateHammond(int layer) {
        audioEngine.activateHammond(layer); audioEngine.setHammondPreset(layer,0);
        screen.setLayerName(layer,"Classic Keys Hammond"); screen.setEngineName(layer,"HAMMOND"); screen.setPresetName(layer,audioEngine.hammondPresetName(0));
        getSharedPreferences("layers",MODE_PRIVATE).edit().putInt("engine_"+layer,4).putInt("hammond_preset_"+layer,0).putString("name_"+layer,"Classic Keys Hammond").apply();
    }
    private void openHammondEditor(final int layer) {
        int count=audioEngine.hammondPresetCount(); String[] presets=new String[count];
        for(int i=0;i<count;++i)presets[i]=audioEngine.hammondPresetName(i);
        int selected=getSharedPreferences("layers",MODE_PRIVATE).getInt("hammond_preset_"+layer,0);
        showPresetEditor(layer, "LAYER "+(layer+1)+" · HAMMOND / LESLIE", "Selecione o banco e o registro desta layer", presets, selected,
                which -> {audioEngine.setHammondPreset(layer,which);screen.setPresetName(layer,audioEngine.hammondPresetName(which));getSharedPreferences("layers",MODE_PRIVATE).edit().putInt("hammond_preset_"+layer,which).apply();}, "TROCAR MOTOR", () -> panicAndChooseLayerSource(layer));
    }

    private void openPadEditor(final int layer, final boolean continuous) {
        audioEngine.clearLayer(layer);padLayerActive=true;continuousPadActive=continuous;padEngine.setContinuous(continuous);
        screen.setLayerName(layer,continuous?"Pads contínuos":"Drum Pads");
        screen.setEngineName(layer,continuous?"CONT. PADS":"DRUM PADS"); screen.setPresetName(layer,"12 pads · notas MIDI 36–47");
        getSharedPreferences("layers",MODE_PRIVATE).edit().putInt("engine_"+layer,continuous?6:5).putString("name_"+layer,continuous?"Pads contínuos":"Drum Pads").apply();
        showPadEditorScreen(layer, continuous);
    }

    private void showPadEditorScreen(final int layer, final boolean continuous) {
        final Dialog dialog = new Dialog(this);
        LinearLayout root = new LinearLayout(this); root.setOrientation(LinearLayout.VERTICAL); root.setPadding(28,22,28,18); root.setBackgroundColor(Color.rgb(7,16,25));
        TextView heading = new TextView(this); heading.setText((continuous?"PADS CONTÍNUOS":"DRUM PADS")+" · LAYER "+(layer+1)); heading.setTextColor(Color.rgb(233,239,240)); heading.setTextSize(22); root.addView(heading,new LinearLayout.LayoutParams(-1,-2));
        TextView help = new TextView(this); help.setText("Toque em um pad para disparar. Carregue arquivos individuais quando necessário."); help.setTextColor(Color.rgb(145,170,180)); help.setPadding(0,8,0,16); root.addView(help,new LinearLayout.LayoutParams(-1,-2));
        LinearLayout grid = new LinearLayout(this); grid.setOrientation(LinearLayout.VERTICAL); root.addView(grid,new LinearLayout.LayoutParams(-1,0,1f));
        for(int row=0;row<4;row++){LinearLayout line=new LinearLayout(this);line.setGravity(Gravity.CENTER);grid.addView(line,new LinearLayout.LayoutParams(-1,0,1f));for(int col=0;col<3;col++){final int pad=row*3+col;Button b=new Button(this);b.setText("PAD "+(pad+1)+"\n"+padEngine.name(pad));b.setOnClickListener(v->{if(padEngine.loaded(pad))padEngine.trigger(pad);else openPadPicker(pad,continuous);});line.addView(b,new LinearLayout.LayoutParams(0,-1,1f));}}
        LinearLayout actions=new LinearLayout(this);actions.setGravity(Gravity.RIGHT|Gravity.CENTER_VERTICAL);actions.setPadding(0,14,0,0);Button load=new Button(this);load.setText("CARREGAR PAD");load.setOnClickListener(v->choosePadToLoad(continuous));actions.addView(load);Button stop=new Button(this);stop.setText("PARAR");stop.setOnClickListener(v->padEngine.stopAll());actions.addView(stop);Button close=new Button(this);close.setText("FECHAR");close.setOnClickListener(v->dialog.dismiss());actions.addView(close);root.addView(actions,new LinearLayout.LayoutParams(-1,-2));
        dialog.setContentView(root);dialog.show();if(dialog.getWindow()!=null){dialog.getWindow().setBackgroundDrawableResource(android.R.color.transparent);dialog.getWindow().setLayout(-1,-1);}
    }
    private void choosePadToLoad(boolean continuous){
        String[] pads=new String[12];for(int i=0;i<12;i++)pads[i]="PAD "+(i+1);
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
                    String root="bank_"+bank+"_slot_"+slot;e.putInt(root+"_version",2).putFloat(root+"_master",screen.masterVolume);for(int layer=0;layer<6;layer++){String p=root+"_"+layer+"_";e.putInt(p+"engine",layers.getInt("engine_"+layer,0));e.putString(p+"sf2",layers.getString("sf2_"+layer,null));e.putString(p+"dx7",layers.getString("dx7_"+layer,null));e.putString(p+"name",layers.getString("name_"+layer,null));e.putInt(p+"preset",layers.getInt("preset_"+layer,0));e.putInt(p+"dx7_patch",layers.getInt("dx7_patch_"+layer,0));e.putInt(p+"analog",layers.getInt("analog_preset_"+layer,0));e.putInt(p+"hammond",layers.getInt("hammond_preset_"+layer,0));e.putFloat(p+"volume",screen.layerVolumes[layer]).putBoolean(p+"muted",screen.muted[layer]).putBoolean(p+"solo",screen.solo[layer]);}
                    SharedPreferences pads=getSharedPreferences("pads",MODE_PRIVATE);for(int pad=0;pad<12;pad++)e.putString(root+"_pad_"+pad,pads.getString("pad_"+pad,null));
                    e.putInt(root+"_version",3).putFloat(root+"_reverb",screen.masterReverb).putFloat(root+"_chorus",screen.masterChorus);for(int layer=0;layer<6;layer++){String p=root+"_"+layer+"_";e.putFloat(p+"attack",screen.layerAttack[layer]).putFloat(p+"release",screen.layerRelease[layer]).putFloat(p+"eqLow",screen.eqLow[layer]).putFloat(p+"eqMid",screen.eqMid[layer]).putFloat(p+"eqHigh",screen.eqHigh[layer]).putFloat(p+"compThreshold",screen.compressorThreshold[layer]).putFloat(p+"compRatio",screen.compressorRatio[layer]);}
                    String name=input.getText().toString().trim();if(name.isEmpty())name="PROGRAMA "+(slot+1);e.putBoolean(root+"_valid",true).putString(root+"_name",name).apply();screen.setLiveName(slot,name);
                }).setNegativeButton("CANCELAR",null).show();
    }
    private void loadLiveSlot(int bank,int slot){
        SharedPreferences live=getSharedPreferences("live_set",MODE_PRIVATE);String root="bank_"+bank+"_slot_"+slot;if(!live.getBoolean(root+"_valid",false)){new AlertDialog.Builder(this).setMessage("Este slot está vazio. Ative SALVAR SLOT e toque nele para guardar o programa atual.").setPositiveButton("OK",null).show();return;}
        SharedPreferences.Editor e=getSharedPreferences("layers",MODE_PRIVATE).edit(); screen.masterReverb=live.getFloat(root+"_reverb",0f); screen.masterChorus=live.getFloat(root+"_chorus",0f); if(audioEngine!=null) audioEngine.setMasterEffects(screen.masterReverb,screen.masterChorus); for(int layer=0;layer<6;layer++){String p=root+"_"+layer+"_";screen.layerAttack[layer]=live.getFloat(p+"attack",screen.layerAttack[layer]);screen.layerRelease[layer]=live.getFloat(p+"release",screen.layerRelease[layer]);screen.eqLow[layer]=live.getFloat(p+"eqLow",screen.eqLow[layer]);screen.eqMid[layer]=live.getFloat(p+"eqMid",screen.eqMid[layer]);screen.eqHigh[layer]=live.getFloat(p+"eqHigh",screen.eqHigh[layer]);screen.compressorThreshold[layer]=live.getFloat(p+"compThreshold",screen.compressorThreshold[layer]);screen.compressorRatio[layer]=live.getFloat(p+"compRatio",screen.compressorRatio[layer]);if(audioEngine!=null){audioEngine.setLayerEnvelope(layer,screen.layerAttack[layer],screen.layerRelease[layer]);audioEngine.setLayerEq(layer,screen.eqLow[layer],screen.eqMid[layer],screen.eqHigh[layer]);audioEngine.setLayerCompressor(layer,screen.compressorThreshold[layer],screen.compressorRatio[layer]);}}
        for(int layer=0;layer<6;layer++){String p=root+"_"+layer+"_";e.putInt("engine_"+layer,live.getInt(p+"engine",0));e.putString("sf2_"+layer,live.getString(p+"sf2",null));e.putString("dx7_"+layer,live.getString(p+"dx7",null));e.putString("name_"+layer,live.getString(p+"name",null));e.putInt("preset_"+layer,live.getInt(p+"preset",0));e.putInt("dx7_patch_"+layer,live.getInt(p+"dx7_patch",0));e.putInt("analog_preset_"+layer,live.getInt(p+"analog",0));e.putInt("hammond_preset_"+layer,live.getInt(p+"hammond",0));screen.layerVolumes[layer]=live.getFloat(p+"volume",screen.layerVolumes[layer]);screen.muted[layer]=live.getBoolean(p+"muted",false);screen.solo[layer]=live.getBoolean(p+"solo",false);}e.apply();screen.masterVolume=live.getFloat(root+"_master",screen.masterVolume);screen.applyLayerGains();SharedPreferences.Editor pads=getSharedPreferences("pads",MODE_PRIVATE).edit();for(int pad=0;pad<12;pad++){String path=live.getString(root+"_pad_"+pad,null);if(path!=null)pads.putString("pad_"+pad,path);}pads.apply();recreate();
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
        if(requestCode==710&&resultCode==RESULT_OK&&data!=null&&data.getData()!=null&&pendingPad>=0){
            String path=cachePad(data.getData(),pendingPad);if(path!=null){padEngine.setContinuous(pendingContinuous);padEngine.load(pendingPad,path);getSharedPreferences("pads",MODE_PRIVATE).edit().putString("pad_"+pendingPad,path).apply();screen.setAudioStatus("ÁUDIO: PAD "+(pendingPad+1)+" carregado");}pendingPad=-1;return;
        }
        if (requestCode == 701 && resultCode == RESULT_OK && data != null && data.getData() != null && pendingLayer >= 0) {
            Uri uri=data.getData(); final int layer=pendingLayer;
            String cachedPath=cacheDocument(uri,layer,"syx");
            if(cachedPath==null||audioEngine==null||!audioEngine.loadDx7(layer,cachedPath)){
                screen.setAudioStatus("ÁUDIO: banco DX7 inválido"); pendingLayer=-1; return;
            }
            String name=uri.getLastPathSegment()==null?"Banco DX7":uri.getLastPathSegment();
            screen.setLayerName(layer,name); screen.setEngineName(layer,"DX7");
            screen.setPresetName(layer,audioEngine.dx7PatchName(layer,0));
            getSharedPreferences("layers",MODE_PRIVATE).edit().putInt("engine_"+layer,2)
                    .putString("dx7_"+layer,cachedPath).putString("name_"+layer,name).putInt("dx7_patch_"+layer,0).apply();
            screen.setAudioStatus("ÁUDIO: banco DX7 carregado"); pendingLayer=-1; return;
        }
        if (requestCode == 700 && resultCode == RESULT_OK && data != null && data.getData() != null && pendingLayer >= 0) {
            Uri uri = data.getData();
            try { getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION); }
            catch (SecurityException ignored) { }
            final int layer = pendingLayer;
            String cachedPath = cacheSoundFont(uri, layer);
            if (cachedPath == null || audioEngine == null || !audioEngine.loadLayer(layer, cachedPath)) {
                screen.setAudioStatus("ÁUDIO: falha ao abrir o arquivo SF2");
                pendingLayer = -1;
                return;
            }
            String name = uri.getLastPathSegment() == null ? "SF2 carregado" : uri.getLastPathSegment();
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
            screen.setAudioStatus("ÁUDIO: SF2 carregado");
            pendingLayer = -1;
        }
    }

    /** Android document URIs are not file paths; make a private copy for the native SF2 renderer. */
    private String restoreSoundFont(String stored, int layer) {
        File existing = new File(stored);
        if (existing.isFile()) return existing.getAbsolutePath();
        try { return cacheSoundFont(Uri.parse(stored), layer); }
        catch (Exception ignored) { return null; }
    }

    private String cacheSoundFont(Uri source, int layer) {
        return cacheDocument(source, layer, "sf2");
    }

    private String cachePad(Uri source,int pad){
        File directory=new File(getFilesDir(),"pads");if(!directory.exists()&&!directory.mkdirs())return null;
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
        final int count = audioEngine.dx7PatchCount(layer);
        if (count <= 0) { openDx7Picker(layer); return; }
        // DX7 gets an explicit bank + timbre selector, matching the desktop
        // workflow.  The old generic list hid the bank choice and made it
        // look as if only one patch existed.
        final Dialog dialog = new Dialog(this);
        LinearLayout root = new LinearLayout(this); root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(28,22,28,18); root.setBackgroundColor(Color.rgb(7,16,25));
        TextView heading = new TextView(this); heading.setText("DX7 · LAYER "+(layer+1)); heading.setTextColor(Color.rgb(233,239,240)); heading.setTextSize(22); root.addView(heading);
        TextView help = new TextView(this); help.setText("Selecione o banco e o timbre desta layer."); help.setTextColor(Color.rgb(145,170,180)); help.setPadding(0,6,0,16); root.addView(help);
        TextView bankLabel = new TextView(this); bankLabel.setText("BANCO DX7"); bankLabel.setTextColor(Color.rgb(19,184,173)); root.addView(bankLabel);
        Spinner bank = new Spinner(this); String[] banks={"DIVINE MASQUERADE 1","DIVINE MASQUERADE 2","IMPORTAR BANCO .SYX"}; bank.setAdapter(new ArrayAdapter<String>(this, android.R.layout.simple_spinner_dropdown_item, banks)); root.addView(bank,new LinearLayout.LayoutParams(-1,-2));
        TextView patchLabel = new TextView(this); patchLabel.setText("TIMBRE DX7"); patchLabel.setTextColor(Color.rgb(19,184,173)); patchLabel.setPadding(0,14,0,0); root.addView(patchLabel);
        String[] patches=new String[count]; for(int patch=0;patch<count;++patch) patches[patch]=String.format("%02d  %s",patch+1,audioEngine.dx7PatchName(layer,patch));
        Spinner patchSpinner=new Spinner(this); patchSpinner.setAdapter(new ArrayAdapter<String>(this, android.R.layout.simple_spinner_dropdown_item, patches));
        int selectedPatch=getSharedPreferences("layers",MODE_PRIVATE).getInt("dx7_patch_"+layer,0); patchSpinner.setSelection(Math.max(0,Math.min(selectedPatch,count-1))); root.addView(patchSpinner,new LinearLayout.LayoutParams(-1,-2));
        patchSpinner.setOnItemSelectedListener(new android.widget.AdapterView.OnItemSelectedListener(){public void onNothingSelected(android.widget.AdapterView<?> p){} public void onItemSelected(android.widget.AdapterView<?> p,android.view.View v,int which,long id){if(audioEngine.setDx7Patch(layer,which)){screen.setPresetName(layer,audioEngine.dx7PatchName(layer,which));getSharedPreferences("layers",MODE_PRIVATE).edit().putInt("dx7_patch_"+layer,which).apply();}}});
        LinearLayout actions=new LinearLayout(this); actions.setGravity(Gravity.RIGHT|Gravity.CENTER_VERTICAL); actions.setPadding(0,18,0,0);
        Button change=new Button(this); change.setText("TROCAR BANCO"); change.setTextColor(Color.WHITE); change.setBackgroundColor(Color.rgb(31,70,86)); change.setOnClickListener(v->{dialog.dismiss();openDx7Picker(layer);}); actions.addView(change);
        Button close=new Button(this); close.setText("FECHAR"); close.setTextColor(Color.WHITE); close.setBackgroundColor(Color.rgb(19,120,116)); close.setOnClickListener(v->dialog.dismiss()); actions.addView(close); root.addView(actions,new LinearLayout.LayoutParams(-1,-2));
        final boolean[] bankReady={false};
        bank.setOnItemSelectedListener(new android.widget.AdapterView.OnItemSelectedListener(){public void onNothingSelected(android.widget.AdapterView<?> p){} public void onItemSelected(android.widget.AdapterView<?> p,android.view.View v,int which,long id){if(!bankReady[0]){bankReady[0]=true;return;} if(which<2){dialog.dismiss();activateBundledDx7(layer,which==0?R.raw.dx7_bank_1:R.raw.dx7_bank_2,which==0?"Divine Masquerade 1":"Divine Masquerade 2");openDx7Editor(layer);}}});
        dialog.setContentView(root); dialog.show(); if(dialog.getWindow()!=null){dialog.getWindow().setBackgroundDrawableResource(android.R.color.transparent);dialog.getWindow().setLayout(-1,-2);}
    }

    private interface PresetSelection { void apply(int index); }

    /** Full-height editor shared by the melodic engines, matching the desktop workflow. */
    private void showPresetEditor(final int layer, String title, String subtitle, String[] items, int selected,
                                  PresetSelection selection, String secondaryLabel, Runnable secondaryAction) {
        final Dialog dialog = new Dialog(this);
        LinearLayout root = new LinearLayout(this); root.setOrientation(LinearLayout.VERTICAL); root.setPadding(28, 22, 28, 18); root.setBackgroundColor(Color.rgb(7,16,25));
        TextView heading = new TextView(this); heading.setText(title); heading.setTextColor(Color.rgb(233,239,240)); heading.setTextSize(22); heading.setPadding(0,0,0,8); root.addView(heading, new LinearLayout.LayoutParams(-1,-2));
        TextView help = new TextView(this); help.setText(subtitle); help.setTextColor(Color.rgb(145,170,180)); help.setTextSize(14); help.setPadding(0,0,0,14); root.addView(help, new LinearLayout.LayoutParams(-1,-2));
        TextView attackLabel = new TextView(this); attackLabel.setText("ATTACK"); attackLabel.setTextColor(Color.rgb(180,195,200)); root.addView(attackLabel,new LinearLayout.LayoutParams(-1,-2));
        SeekBar attack = new SeekBar(this); attack.setMax(199); attack.setProgress((int)((screen.layerAttack[layer]-0.001f)/1.999f*199f)); root.addView(attack,new LinearLayout.LayoutParams(-1,-2));
        TextView releaseLabel = new TextView(this); releaseLabel.setText("RELEASE"); releaseLabel.setTextColor(Color.rgb(180,195,200)); root.addView(releaseLabel,new LinearLayout.LayoutParams(-1,-2));
        SeekBar release = new SeekBar(this); release.setMax(199); release.setProgress((int)((screen.layerRelease[layer]-0.02f)/3.98f*199f)); root.addView(release,new LinearLayout.LayoutParams(-1,-2));
        SeekBar.OnSeekBarChangeListener envelope = new SeekBar.OnSeekBarChangeListener(){public void onProgressChanged(SeekBar b,int p,boolean from){if(!from)return;float a=0.001f+attack.getProgress()/199f*1.999f;float r=0.02f+release.getProgress()/199f*3.98f;screen.setLayerEnvelope(layer,a,r);}public void onStartTrackingTouch(SeekBar b){}public void onStopTrackingTouch(SeekBar b){}};
        attack.setOnSeekBarChangeListener(envelope); release.setOnSeekBarChangeListener(envelope);
        SeekBar low = new SeekBar(this); SeekBar mid = new SeekBar(this); SeekBar high = new SeekBar(this); low.setMax(200); mid.setMax(200); high.setMax(200); low.setProgress((int)(screen.eqLow[layer]*100)); mid.setProgress((int)(screen.eqMid[layer]*100)); high.setProgress((int)(screen.eqHigh[layer]*100));
        TextView eqLabel = new TextView(this); eqLabel.setText("EQ"); eqLabel.setTextColor(Color.rgb(19,184,173)); eqLabel.setTextSize(16); root.addView(eqLabel,new LinearLayout.LayoutParams(-1,-2));
        TextView lowLabel = new TextView(this); lowLabel.setText("GRAVES"); lowLabel.setTextColor(Color.rgb(180,195,200)); root.addView(lowLabel,new LinearLayout.LayoutParams(-1,-2)); root.addView(low,new LinearLayout.LayoutParams(-1,-2));
        TextView midLabel = new TextView(this); midLabel.setText("MÉDIOS"); midLabel.setTextColor(Color.rgb(180,195,200)); root.addView(midLabel,new LinearLayout.LayoutParams(-1,-2)); root.addView(mid,new LinearLayout.LayoutParams(-1,-2));
        TextView highLabel = new TextView(this); highLabel.setText("AGUDOS"); highLabel.setTextColor(Color.rgb(180,195,200)); root.addView(highLabel,new LinearLayout.LayoutParams(-1,-2)); root.addView(high,new LinearLayout.LayoutParams(-1,-2));
        SeekBar.OnSeekBarChangeListener eq = new SeekBar.OnSeekBarChangeListener(){public void onProgressChanged(SeekBar b,int p,boolean from){if(from)screen.setLayerEq(layer,low.getProgress()/100f,mid.getProgress()/100f,high.getProgress()/100f);}public void onStartTrackingTouch(SeekBar b){}public void onStopTrackingTouch(SeekBar b){}}; low.setOnSeekBarChangeListener(eq);mid.setOnSeekBarChangeListener(eq);high.setOnSeekBarChangeListener(eq);
        TextView compLabel = new TextView(this); compLabel.setText("COMPRESSOR"); compLabel.setTextColor(Color.rgb(19,184,173)); compLabel.setTextSize(16); root.addView(compLabel,new LinearLayout.LayoutParams(-1,-2));
        TextView thresholdLabel = new TextView(this); thresholdLabel.setText("THRESHOLD"); thresholdLabel.setTextColor(Color.rgb(180,195,200)); root.addView(thresholdLabel,new LinearLayout.LayoutParams(-1,-2)); SeekBar threshold = new SeekBar(this); threshold.setMax(100); threshold.setProgress((int)(screen.compressorThreshold[layer]*100)); root.addView(threshold,new LinearLayout.LayoutParams(-1,-2));
        TextView ratioLabel = new TextView(this); ratioLabel.setText("RATIO"); ratioLabel.setTextColor(Color.rgb(180,195,200)); root.addView(ratioLabel,new LinearLayout.LayoutParams(-1,-2)); SeekBar ratio = new SeekBar(this); ratio.setMax(190); ratio.setProgress((int)((screen.compressorRatio[layer]-1f)/19f*190f)); root.addView(ratio,new LinearLayout.LayoutParams(-1,-2));
        SeekBar.OnSeekBarChangeListener comp = new SeekBar.OnSeekBarChangeListener(){public void onProgressChanged(SeekBar b,int p,boolean from){if(from)screen.setLayerCompressor(layer,threshold.getProgress()/100f,1f+ratio.getProgress()/190f*19f);}public void onStartTrackingTouch(SeekBar b){}public void onStopTrackingTouch(SeekBar b){}}; threshold.setOnSeekBarChangeListener(comp); ratio.setOnSeekBarChangeListener(comp);
        TextView fxLabel = new TextView(this); fxLabel.setText("MASTER FX"); fxLabel.setTextColor(Color.rgb(19,184,173)); fxLabel.setTextSize(16); root.addView(fxLabel,new LinearLayout.LayoutParams(-1,-2));
        TextView reverbLabel = new TextView(this); reverbLabel.setText("REVERB"); reverbLabel.setTextColor(Color.rgb(180,195,200)); root.addView(reverbLabel,new LinearLayout.LayoutParams(-1,-2)); SeekBar reverb = new SeekBar(this); reverb.setMax(100); reverb.setProgress((int)(screen.masterReverb*100)); root.addView(reverb,new LinearLayout.LayoutParams(-1,-2));
        TextView chorusLabel = new TextView(this); chorusLabel.setText("CHORUS"); chorusLabel.setTextColor(Color.rgb(180,195,200)); root.addView(chorusLabel,new LinearLayout.LayoutParams(-1,-2)); SeekBar chorus = new SeekBar(this); chorus.setMax(100); chorus.setProgress((int)(screen.masterChorus*100)); root.addView(chorus,new LinearLayout.LayoutParams(-1,-2));
        SeekBar.OnSeekBarChangeListener fx = new SeekBar.OnSeekBarChangeListener(){public void onProgressChanged(SeekBar b,int p,boolean from){if(from)screen.setMasterEffects(reverb.getProgress()/100f,chorus.getProgress()/100f);}public void onStartTrackingTouch(SeekBar b){}public void onStopTrackingTouch(SeekBar b){}}; reverb.setOnSeekBarChangeListener(fx); chorus.setOnSeekBarChangeListener(fx);
        ListView list = new ListView(this); list.setChoiceMode(ListView.CHOICE_MODE_SINGLE); list.setDivider(new android.graphics.drawable.ColorDrawable(Color.rgb(35,62,74))); list.setDividerHeight(1); list.setAdapter(new ArrayAdapter<String>(this, android.R.layout.simple_list_item_single_choice, items){@Override public android.view.View getView(int position,android.view.View convert,android.view.ViewGroup parent){android.view.View v=super.getView(position,convert,parent);if(v instanceof android.widget.TextView){android.widget.TextView t=(android.widget.TextView)v;t.setTextColor(Color.rgb(233,239,240));t.setTextSize(17);t.setPadding(12,16,12,16);}return v;}}); list.setItemChecked(Math.max(0, Math.min(selected, items.length-1)), true);
        list.setOnItemClickListener((parent, view, position, id) -> { selection.apply(position); list.setItemChecked(position, true); });
        root.addView(list, new LinearLayout.LayoutParams(-1, 0, 1f));
        LinearLayout actions = new LinearLayout(this); actions.setGravity(Gravity.RIGHT|Gravity.CENTER_VERTICAL); actions.setPadding(0,14,0,0);
        if (secondaryLabel != null) { Button secondary = new Button(this); secondary.setText(secondaryLabel); secondary.setTextColor(Color.WHITE); secondary.setBackgroundColor(Color.rgb(31,70,86)); secondary.setOnClickListener(v -> { dialog.dismiss(); secondaryAction.run(); }); actions.addView(secondary, new LinearLayout.LayoutParams(-2,-2)); }
        Button close = new Button(this); close.setText("FECHAR"); close.setTextColor(Color.WHITE); close.setBackgroundColor(Color.rgb(19,120,116)); close.setOnClickListener(v -> dialog.dismiss()); actions.addView(close, new LinearLayout.LayoutParams(-2,-2)); root.addView(actions, new LinearLayout.LayoutParams(-1,-2));
        dialog.setContentView(root); Window window = dialog.getWindow(); if (window != null) { window.setBackgroundDrawableResource(android.R.color.transparent); window.setLayout(-1,-1); }
        dialog.show(); if (dialog.getWindow() != null) dialog.getWindow().setLayout(-1,-1);
    }

    private final class ClassicPlayerView extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final String[] names = new String[8];
        private String midiStatus = "MIDI USB: procurando...";
        private String audioStatus = "ÁUDIO: procurando...";
        private String account = "";
        private boolean midiSignal;
        private int lastNoteOff = -1;
        // Desktop builds open directly on the mixer; keep the same workflow on Android.
        private boolean liveSet = false;
        private boolean settings = false;
        private int outputIndex = 0;
        private int selected = 0;
        private boolean savingLiveSlot;
        private int liveBank;
        private final float[] layerVolumes = {0.8f, 0.8f, 0.8f, 0.8f, 0.8f, 0.8f};
        private final float[] layerAttack = {0.01f,0.01f,0.01f,0.01f,0.01f,0.01f};
        private final float[] layerRelease = {0.25f,0.25f,0.25f,0.25f,0.25f,0.25f};
        private final float[] eqLow = {1f,1f,1f,1f,1f,1f}, eqMid = {1f,1f,1f,1f,1f,1f}, eqHigh = {1f,1f,1f,1f,1f,1f};
        private final float[] compressorThreshold = {0.85f,0.85f,0.85f,0.85f,0.85f,0.85f}, compressorRatio = {1f,1f,1f,1f,1f,1f};
        private float masterReverb, masterChorus;
        private final boolean[] muted = new boolean[6];
        private final boolean[] solo = new boolean[6];
        private float masterVolume = 0.8f;
        private final String[] layerNames = {"SEM SOUNDFONT", "SEM SOUNDFONT", "SEM SOUNDFONT", "SEM SOUNDFONT", "SEM SOUNDFONT", "SEM SOUNDFONT"};
        private final String[] presetNames = {"", "", "", "", "", ""};
        private final String[] engineNames = {"VAZIA", "VAZIA", "VAZIA", "VAZIA", "VAZIA", "VAZIA"};

        ClassicPlayerView(Context context) { super(context); paint.setTypeface(android.graphics.Typeface.create("sans", 1)); loadLiveNames(); }
        private void loadLiveNames(){SharedPreferences live=getSharedPreferences("live_set",MODE_PRIVATE);for(int i=0;i<8;i++)names[i]=live.getString("bank_"+liveBank+"_slot_"+i+"_name","VAZIO");postInvalidate();}
        void setMidiStatus(String value) { midiStatus = value; postInvalidate(); }
        void setAudioStatus(String value) { audioStatus = value; postInvalidate(); }
        void setAccount(String value) { account = value == null ? "" : value; postInvalidate(); }
        void setMidiSignal() { midiSignal = true; postInvalidateDelayed(180); }
        void setLastNoteOff(int note) { lastNoteOff = note; postInvalidate(); }
        void setLayerName(int layer, String name) { if (layer >= 0 && layer < layerNames.length) { layerNames[layer] = name; postInvalidate(); } }
        void setPresetName(int layer, String name) { if (layer >= 0 && layer < presetNames.length) { presetNames[layer] = name == null ? "" : name; postInvalidate(); } }
        void setEngineName(int layer, String name) { if (layer >= 0 && layer < engineNames.length) { engineNames[layer] = name == null ? "VAZIA" : name; postInvalidate(); } }
        String engineName(int layer) { return layer >= 0 && layer < engineNames.length ? engineNames[layer] : "VAZIA"; }
        void setLiveName(int slot,String name){if(slot>=0&&slot<names.length){names[slot]=name;postInvalidate();}}
        void setLearnedVolume(int target,float value){if(target<6){layerVolumes[target]=value;applyLayerGains();}else{masterVolume=value;if(audioEngine!=null)audioEngine.setMaster(faderGain(value));}postInvalidate();}
        void setLayerEnvelope(int layer,float attack,float release){if(layer<0||layer>=6)return;layerAttack[layer]=attack;layerRelease[layer]=release;if(audioEngine!=null)audioEngine.setLayerEnvelope(layer,attack,release);}
        void setLayerEq(int layer,float low,float mid,float high){if(layer<0||layer>=6)return;eqLow[layer]=low;eqMid[layer]=mid;eqHigh[layer]=high;if(audioEngine!=null)audioEngine.setLayerEq(layer,low,mid,high);}
        void setLayerCompressor(int layer,float threshold,float ratio){if(layer<0||layer>=6)return;compressorThreshold[layer]=threshold;compressorRatio[layer]=ratio;if(audioEngine!=null)audioEngine.setLayerCompressor(layer,threshold,ratio);}
        void setMasterEffects(float reverb,float chorus){masterReverb=reverb;masterChorus=chorus;if(audioEngine!=null)audioEngine.setMasterEffects(reverb,chorus);postInvalidate();}

        private void text(Canvas canvas, String value, float x, float y, float size, int colour) {
            paint.setStyle(Paint.Style.FILL); paint.setColor(colour); paint.setTextSize(size);
            canvas.drawText(value, x, y, paint);
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
            Drawable logo = getDrawable(R.drawable.ic_launcher);
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
            paint.setColor(Color.rgb(210,219,223)); canvas.drawRoundRect(x-cardW*.24f, knobY-12, x+cardW*.24f, knobY+12, 4, 4, paint);
            paint.setColor(Color.rgb(95,107,113));
            for (int line = -6; line <= 6; line += 4) canvas.drawRect(x-cardW*.20f, knobY+line, x+cardW*.20f, knobY+line+1.5f, paint);
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
            if (audioEngine == null) return;
            boolean anySolo = hasSolo();
            for (int i = 0; i < 6; i++) audioEngine.setLayerGain(i,
                    (!muted[i] && (!anySolo || solo[i])) ? faderGain(layerVolumes[i]) : 0f);
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
            text(canvas, liveSet ? "LIVE SET" : settings ? "ÁUDIO / MIDI" : "MIXER", w * .43f, h * .078f, h * .052f, text);
            text(canvas, midiStatus, w * .76f, h * .055f, h * .022f, Color.rgb(180, 195, 200));
            if (lastNoteOff >= 0) text(canvas, "NOTE OFF " + lastNoteOff, w * .76f, h * .078f, h * .014f, Color.rgb(80, 190, 174));
            text(canvas, audioStatus, w * .76f, h * .085f, h * .018f, Color.rgb(180, 195, 200));
            paint.setColor(midiSignal ? Color.rgb(40, 220, 110) : Color.rgb(70, 90, 95));
            canvas.drawCircle(w * .735f, h * .055f, h * .012f, paint);
            midiSignal = false;
            // Keep all navigation inside the header so it never covers Layer 6.
            button(canvas, "MIXER", w*.755f, h*.096f, w*.83f, h*.137f, !liveSet && !settings);
            button(canvas, "LIVE SET", w*.835f, h*.096f, w*.91f, h*.137f, liveSet);
            button(canvas, "ÁUDIO/MIDI", w*.915f, h*.096f, w*.995f, h*.137f, settings);

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

        private void drawMixer(Canvas canvas, float w, float h, int textColour, int teal, int panel) {
            final float left = 18, top = h * .17f, gap = 10;
            final float cardW = (w - left * 2 - gap * 6 - 105) / 6f;
            final float cardH = h * .72f;
            text(canvas, "6 LAYERS · ADICIONE UM MOTOR EM CADA SLOT", left, h * .16f, h * .024f, Color.rgb(180,195,200));
            for (int i = 0; i < 6; i++) {
                float x = left + i * (cardW + gap);
                box(canvas, x, top, x + cardW, top + cardH, Color.rgb(49, 69, 82), true);
                text(canvas, "LAYER " + (i + 1), x + 12, top + h * .045f, h * .022f, textColour);
                text(canvas, engineNames[i], x+12, top+h*.065f, h*.012f, teal);
                button(canvas, "M", x + cardW*.54f, top+h*.016f, x+cardW*.70f, top+h*.063f, muted[i]);
                button(canvas, "S", x + cardW*.74f, top+h*.016f, x+cardW*.90f, top+h*.063f, solo[i]);
                String source = presetNames[i].isEmpty() ? layerNames[i] : presetNames[i];
                text(canvas, source, x + 12, top + h * .086f, h * .015f, Color.rgb(180,195,200));
                button(canvas, engineNames[i].equals("VAZIA") ? "ADICIONAR MOTOR" : "EDITAR " + engineNames[i], x+12, top+h*.098f, x+cardW-12, top+h*.15f, false);
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
            float masterX = left + 6*(cardW+gap);
            box(canvas, masterX, top, masterX+105, top+cardH, Color.rgb(49,69,82), true);
            paint.setTextAlign(Paint.Align.CENTER); text(canvas, "MASTER", masterX+52, top+h*.05f, h*.019f, textColour); paint.setTextAlign(Paint.Align.LEFT);
            float masterTop = top+h*.12f, masterBottom = top+cardH-h*.09f;
            drawMeter(canvas, masterX+15, masterTop, 13, masterBottom, audioEngine == null ? 0f : audioEngine.masterPeak());
            drawFader(canvas, masterX+60, masterTop, masterBottom, 105, masterVolume);
            paint.setTextAlign(Paint.Align.CENTER); text(canvas, faderLabel(masterVolume), masterX+55, top+cardH-h*.027f, h*.016f, textColour); paint.setTextAlign(Paint.Align.LEFT);
            postInvalidateDelayed(70);
        }

        @Override public boolean onTouchEvent(MotionEvent event) {
            final int action = event.getActionMasked();
            final boolean tap = action == MotionEvent.ACTION_UP;
            final boolean dragging = action == MotionEvent.ACTION_DOWN
                    || action == MotionEvent.ACTION_MOVE || tap;
            final float w = getWidth(), h = getHeight();
            if (tap && event.getY() > h*.09f && event.getY() < h*.145f && event.getX() > w*.75f) {
                if (event.getX() < w*.832f) { liveSet = false; settings = false; }
                else if (event.getX() < w*.912f) { liveSet = true; settings = false; }
                else { settings = true; liveSet = false; }
                invalidate(); return true;
            }
            if (settings && tap) {
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
            if (!liveSet && dragging && event.getY() > h * .17f && event.getY() < h * .87f) {
                float cardW = (w - 36 - 60 - 105) / 6f;
                float masterX = 18 + 6*(cardW+10);
                if (event.getX() >= masterX) {
                    float railTop = h*.17f+h*.12f, railBottom = h*.17f+h*.72f-h*.09f;
                    masterVolume = Math.max(0f, Math.min(1f, (railBottom - event.getY()) / (railBottom - railTop)));
                    if (audioEngine != null) audioEngine.setMaster(faderGain(masterVolume));
                    invalidate(); return true;
                }
                int layer = (int) ((event.getX() - 18) / (cardW + 10));
                if (layer >= 0 && layer < 6) {
                    float cardX = 18 + layer*(cardW+10);
                    if (tap && event.getY() >= h*.17f+h*.016f && event.getY() <= h*.17f+h*.063f) {
                        if (event.getX() >= cardX+cardW*.54f && event.getX() <= cardX+cardW*.70f) muted[layer] = !muted[layer];
                        else if (event.getX() >= cardX+cardW*.74f && event.getX() <= cardX+cardW*.90f) solo[layer] = !solo[layer];
                        applyLayerGains(); invalidate(); return true;
                    }
                    if (tap && event.getY() >= h*.17f+h*.098f && event.getY() <= h*.17f+h*.16f) {
                        if (engineNames[layer].equals("VAZIA")) chooseLayerSource(layer);
                        else showLayerActions(layer);
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
