package com.classickeys.classicplayer;

import android.content.ContentResolver;
import android.content.Context;
import android.net.Uri;

import java.io.IOException;
import java.io.InputStream;

/** Persistent description of one SF2 layer. The native renderer consumes this metadata. */
final class SoundFontLayer {
    private final Context context;
    private Uri uri;
    private String displayName = "SEM SOUNDFONT";
    private int preset;
    private float gain = 1.0f;

    SoundFontLayer(Context context) { this.context = context.getApplicationContext(); }
    void load(Uri value, String name) { uri = value; displayName = name == null ? "SF2 carregado" : name; }
    Uri uri() { return uri; }
    String displayName() { return displayName; }
    int preset() { return preset; }
    void setPreset(int value) { preset = Math.max(0, value); }
    float gain() { return gain; }
    void setGain(float value) { gain = Math.max(0f, Math.min(1f, value)); }

    /** Opens a fresh stream; the SF2 engine must not retain a transient picker stream. */
    InputStream open() throws IOException {
        if (uri == null) throw new IOException("Nenhum SoundFont selecionado");
        ContentResolver resolver = context.getContentResolver();
        InputStream stream = resolver.openInputStream(uri);
        if (stream == null) throw new IOException("Não foi possível abrir o SoundFont");
        return stream;
    }
}
