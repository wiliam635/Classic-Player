package com.classickeys.classicplayer;

import android.app.Activity;
import android.content.Context;
import android.content.pm.ActivityInfo;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.media.midi.MidiDeviceInfo;
import android.media.midi.MidiDevice;
import android.media.midi.MidiInputPort;
import android.media.midi.MidiManager;
import android.media.midi.MidiReceiver;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.content.Intent;
import android.net.Uri;
import android.view.MotionEvent;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.view.Gravity;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;

/**
 * First Android surface for Classic Player. The audio monitor runs alongside
 * the UI so MIDI/SoundFont integration can be tested without blocking it.
 */
public final class MainActivity extends Activity {
    private ClassicPlayerView screen;
    private MidiManager midiManager;
    private PolySynthEngine audioEngine;
    private MidiDevice midiDevice;
    private MidiInputPort midiInput;
    private int pendingLayer = -1;
    private final String[] sf2Uris = new String[6];
    private SoundFontLayer[] soundFontLayers;
    private LicenseManager licenseManager;
    private AudioOutputManager audioOutputManager;
    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private final MidiReceiver midiReceiver = new MidiReceiver() {
        @Override public void onSend(byte[] data, int offset, int count, long timestamp) {
            for (int i = offset; i + 2 < offset + count; i++) {
                int status = data[i] & 0xff;
                if ((status & 0x80) == 0) continue;
                int type = status & 0xf0;
                if (type == 0xb0) {
                    int cc = data[i + 1] & 0x7f, value = data[i + 2] & 0x7f;
                    if (audioEngine != null) {
                        if (cc == 64) audioEngine.setSustain(value >= 64);
                        else if (cc == 120 || cc == 123) audioEngine.allNotesOff();
                    }
                    i += 2; continue;
                }
                if (type != 0x80 && type != 0x90) continue;
                int note = data[i + 1] & 0x7f;
                int velocity = data[i + 2] & 0x7f;
                if (audioEngine == null) continue;
                if (type == 0x90 && velocity > 0) audioEngine.noteOn(note, velocity);
                else audioEngine.noteOff(note);
                i += 2;
            }
        }
    };
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
        soundFontLayers = new SoundFontLayer[6];
        for (int i = 0; i < soundFontLayers.length; i++) soundFontLayers[i] = new SoundFontLayer(this);
        android.content.SharedPreferences prefs = getSharedPreferences("layers", MODE_PRIVATE);
        for (int i = 0; i < sf2Uris.length; i++) {
            sf2Uris[i] = prefs.getString("sf2_" + i, null);
            String name = prefs.getString("name_" + i, null);
            if (sf2Uris[i] != null) {
                Uri saved = Uri.parse(sf2Uris[i]);
                soundFontLayers[i].load(saved, name);
                screen.setLayerName(i, soundFontLayers[i].displayName());
            } else if (name != null) screen.setLayerName(i, name);
        }
        if (!licenseManager.isUsableOffline()) showLoginScreen();
    }

    @Override public void onResume() {
        super.onResume();
        hideSystemBars();
        if (audioEngine != null) audioEngine.start();
        if (midiManager != null) midiManager.registerDeviceCallback(midiCallback, null);
        refreshMidiDevices();
        if (licenseManager != null && licenseManager.isActivated()) revalidateLicenseAsync();
    }

    @Override public void onPause() {
        if (midiManager != null) midiManager.unregisterDeviceCallback(midiCallback);
        if (audioEngine != null) audioEngine.stop();
        closeMidi();
        super.onPause();
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
        if (count > 0 && midiDevice == null) openMidi(midiManager.getDevices()[0]);
    }

    private void openMidi(MidiDeviceInfo info) {
        if (midiManager == null) return;
        midiManager.openDevice(info, device -> {
            midiDevice = device;
            MidiDeviceInfo.PortInfo[] ports = info.getPorts();
            for (MidiDeviceInfo.PortInfo port : ports) {
                if (port.getType() == MidiDeviceInfo.PortInfo.TYPE_OUTPUT) {
                    midiInput = device.openInputPort(port.getPortNumber());
                    if (midiInput != null) {
                        try { midiInput.connect(midiReceiver); }
                        catch (IOException ignored) { midiInput.close(); midiInput = null; }
                    }
                    break;
                }
            }
        }, mainHandler);
    }

    private void closeMidi() {
        if (midiInput != null) { midiInput.close(); midiInput = null; }
        if (midiDevice != null) { midiDevice.close(); midiDevice = null; }
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
            c.getOutputStream().write("{\"platform\":\"Android\"}".getBytes(StandardCharsets.UTF_8));
            int code = c.getResponseCode();
            if (code == 401 || code == 403) { licenseManager.clear(); return false; }
            if (code < 200 || code >= 300) return null;
            InputStream in = c.getInputStream(); byte[] bytes = new byte[4096]; int n = in.read(bytes);
            String body = n < 0 ? "" : new String(bytes, 0, n, StandardCharsets.UTF_8);
            if (body.contains("\"valid\":false")) { licenseManager.clear(); return false; }
            licenseManager.refreshOfflineWindow();
            return true;
        } catch (Exception ignored) { return null; }
    }

    private void openSf2Picker(int layer) {
        pendingLayer = layer;
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("audio/x-soundfont");
        startActivityForResult(i, 700);
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
            String body = "{\"email\":\"" + jsonEscape(email) + "\",\"password\":\"" + jsonEscape(password) + "\",\"platform\":\"Android\"}";
            c.getOutputStream().write(body.getBytes(StandardCharsets.UTF_8));
            InputStream in = c.getResponseCode() >= 400 ? c.getErrorStream() : c.getInputStream(); if (in == null || c.getResponseCode() >= 400) return null;
            byte[] bytes = new byte[8192]; int n = in.read(bytes); String json = n < 0 ? "" : new String(bytes, 0, n, StandardCharsets.UTF_8);
            String token = jsonValue(json, "access_token"); if (token == null) return null;
            licenseManager.storeSession(token, jsonValue(json, "display_name"), jsonValue(json, "email")); return token;
        } catch (Exception ignored) { return null; }
    }
    private static String jsonEscape(String s) { return s.replace("\\", "\\\\").replace("\"", "\\\""); }
    private static String jsonValue(String json, String key) { java.util.regex.Matcher m = java.util.regex.Pattern.compile("\\\"" + key + "\\\"\\s*:\\s*\\\"([^\\\"]*)").matcher(json); return m.find() ? m.group(1) : null; }
    private void showMixerAfterLogin(String ignored) { setContentView(screen); }

    @Override protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == 700 && resultCode == RESULT_OK && data != null && data.getData() != null && pendingLayer >= 0) {
            Uri uri = data.getData();
            try { getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION); }
            catch (SecurityException ignored) { }
            screen.setLayerName(pendingLayer, uri.getLastPathSegment() == null ? "SF2 carregado" : uri.getLastPathSegment());
            soundFontLayers[pendingLayer].load(uri, uri.getLastPathSegment());
            getSharedPreferences("layers", MODE_PRIVATE).edit()
                    .putString("sf2_" + pendingLayer, uri.toString())
                    .putString("name_" + pendingLayer, uri.getLastPathSegment() == null ? "SF2 carregado" : uri.getLastPathSegment())
                    .apply();
            sf2Uris[pendingLayer] = uri.toString();
            pendingLayer = -1;
        }
    }

    private final class ClassicPlayerView extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final String[] names = { "Piano + Pad", "Worship Atmosphere", "EP + Strings", "Organ Leslie",
                "Piano Solo", "Brass Layer", "Synth Lead", "Guitar + Pad" };
        private String midiStatus = "MIDI USB: procurando...";
        private String audioStatus = "ÁUDIO: procurando...";
        // Desktop builds open directly on the mixer; keep the same workflow on Android.
        private boolean liveSet = false;
        private boolean settings = false;
        private int outputIndex = 0;
        private int selected = 0;
        private final String[] layerNames = {"SEM SOUNDFONT", "SEM SOUNDFONT", "SEM SOUNDFONT", "SEM SOUNDFONT", "SEM SOUNDFONT", "SEM SOUNDFONT"};

        ClassicPlayerView(Context context) { super(context); paint.setTypeface(android.graphics.Typeface.create("sans", 1)); }
        void setMidiStatus(String value) { midiStatus = value; postInvalidate(); }
        void setAudioStatus(String value) { audioStatus = value; postInvalidate(); }
        void setLayerName(int layer, String name) { if (layer >= 0 && layer < layerNames.length) { layerNames[layer] = name; postInvalidate(); } }

        private void text(Canvas canvas, String value, float x, float y, float size, int colour) {
            paint.setStyle(Paint.Style.FILL); paint.setColor(colour); paint.setTextSize(size);
            canvas.drawText(value, x, y, paint);
        }
        private void box(Canvas canvas, float left, float top, float right, float bottom, int colour, boolean outline) {
            paint.setColor(colour); paint.setStyle(outline ? Paint.Style.STROKE : Paint.Style.FILL); paint.setStrokeWidth(2f);
            canvas.drawRoundRect(left, top, right, bottom, 10f, 10f, paint);
        }

        @Override protected void onDraw(Canvas canvas) {
            final float w = getWidth(), h = getHeight();
            canvas.drawColor(Color.rgb(7, 16, 25));
            final int teal = Color.rgb(19, 184, 173), text = Color.rgb(233, 239, 240), panel = Color.rgb(19, 31, 42);
            box(canvas, 0, 0, w, h * .12f, Color.rgb(9, 20, 30), false);
            text(canvas, "CLASSIC KEYS", 28, h * .05f, h * .023f, teal);
            text(canvas, "CLASSIC PLAYER", 28, h * .095f, h * .047f, text);
            text(canvas, liveSet ? "LIVE SET" : "MIXER", w * .44f, h * .078f, h * .06f, text);
            text(canvas, midiStatus, w * .76f, h * .055f, h * .022f, Color.rgb(180, 195, 200));
            text(canvas, audioStatus, w * .76f, h * .085f, h * .018f, Color.rgb(180, 195, 200));
            text(canvas, "MIXER", w * .88f, h * .097f, h * .025f, teal);

            if (settings) {
                box(canvas, 28, h * .17f, w - 28, h * .86f, panel, true);
                text(canvas, "ÁUDIO / MIDI", 52, h * .25f, h * .04f, text);
                text(canvas, audioStatus, 52, h * .34f, h * .026f, Color.rgb(180,195,200));
                text(canvas, midiStatus, 52, h * .42f, h * .026f, Color.rgb(180,195,200));
                text(canvas, "A saída e o controlador serão selecionáveis nesta tela.", 52, h * .54f, h * .022f, Color.rgb(180,195,200));
                text(canvas, "VOLTAR AO MIXER", 52, h * .76f, h * .026f, teal);
                return;
            }

            if (!liveSet) {
                drawMixer(canvas, w, h, text, teal, panel);
                return;
            }
            final float tabTop = h * .14f, tabs = w / 8f;
            for (int i = 0; i < 8; i++) {
                if (i == 0) box(canvas, i * tabs + 4, tabTop, (i + 1) * tabs - 4, tabTop + h * .065f, teal, false);
                text(canvas, "BANCO " + (i + 1), i * tabs + tabs * .18f, tabTop + h * .043f, h * .025f, i == 0 ? Color.rgb(7,16,25) : text);
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
                text(canvas, (i == 1 ? "3" : i == 4 || i == 6 ? "1" : "2") + " CAMADAS", x + 20, y + cardH * .84f, cardH * .09f, Color.rgb(180,195,200));
            }
            text(canvas, "ANTERIOR", margin + 28, h * .955f, h * .027f, text);
            text(canvas, "PRÓXIMO", w - 125, h * .955f, h * .027f, text);
        }

        private void drawMixer(Canvas canvas, float w, float h, int textColour, int teal, int panel) {
            final float left = 18, top = h * .17f, gap = 10;
            final float cardW = (w - left * 2 - gap * 5) / 6f;
            final float cardH = h * .70f;
            text(canvas, "MIXER", left, h * .16f, h * .03f, textColour);
            text(canvas, "6 LAYERS", w * .46f, h * .16f, h * .022f, Color.rgb(180,195,200));
            for (int i = 0; i < 6; i++) {
                float x = left + i * (cardW + gap);
                box(canvas, x, top, x + cardW, top + cardH, Color.rgb(49, 69, 82), true);
                text(canvas, "LAYER " + (i + 1), x + 12, top + h * .045f, h * .022f, textColour);
                text(canvas, layerNames[i], x + 12, top + h * .082f, h * .016f, Color.rgb(180,195,200));
                float railX = x + cardW * .48f;
                float railTop = top + h * .14f, railBottom = top + cardH - h * .10f;
                paint.setColor(Color.rgb(5, 13, 19)); paint.setStyle(Paint.Style.FILL);
                canvas.drawRoundRect(railX - 7, railTop, railX + 7, railBottom, 5, 5, paint);
                float knobY = railBottom - (railBottom - railTop) * .72f;
                paint.setColor(teal); canvas.drawRoundRect(railX - cardW * .22f, knobY - 10, railX + cardW * .22f, knobY + 10, 8, 8, paint);
                text(canvas, "0 dB", x + cardW * .36f, top + cardH - h * .04f, h * .018f, textColour);
            }
            text(canvas, "MASTER", w - 120, h * .16f, h * .022f, textColour);
        }

        @Override public boolean onTouchEvent(MotionEvent event) {
            if (event.getAction() != MotionEvent.ACTION_UP) return true;
            final float w = getWidth(), h = getHeight();
            if (event.getY() < h * .12f && event.getX() > w * .74f) { settings = !settings; invalidate(); return true; }
            if (settings) {
                if (event.getY() > h * .28f && event.getY() < h * .40f && audioOutputManager != null) {
                    java.util.List<String> outputs = audioOutputManager.outputs();
                    if (!outputs.isEmpty()) {
                        outputIndex = (outputIndex + 1) % outputs.size();
                        if (audioEngine != null) audioEngine.setPreferredDevice(audioOutputManager.deviceAt(outputIndex));
                        setAudioStatus("ÁUDIO: " + outputs.get(outputIndex));
                    }
                }
                return true;
            }
            if (liveSet && event.getY() > h * .235f && event.getY() < h * .90f) {
                float cardW = (w - 44 - 42) / 4f;
                int col = (int) ((event.getX() - 22) / (cardW + 14));
                int row = event.getY() > h * .55f ? 1 : 0;
                if (col >= 0 && col < 4) { selected = row * 4 + col; invalidate(); }
            }
            if (!liveSet && event.getY() > h * .17f && event.getY() < h * .87f) {
                float cardW = (w - 36 - 50) / 6f;
                int layer = (int) ((event.getX() - 18) / (cardW + 10));
                if (layer >= 0 && layer < 6) openSf2Picker(layer);
            }
            return true;
        }
    }
}
