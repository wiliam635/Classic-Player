package com.classickeys.classicplayer;

import android.content.Context;
import android.media.AudioDeviceInfo;
import android.media.AudioManager;

import java.util.ArrayList;
import java.util.List;

/** Enumerates Android output routes so the future Audio/MIDI page can expose a chooser. */
final class AudioOutputManager {
    private final AudioManager audio;
    AudioOutputManager(Context context) { audio = (AudioManager) context.getSystemService(Context.AUDIO_SERVICE); }
    List<String> outputs() {
        List<String> result = new ArrayList<>();
        if (audio == null) return result;
        for (AudioDeviceInfo d : audio.getDevices(AudioManager.GET_DEVICES_OUTPUTS)) {
            String name = d.getProductName() == null ? "Saída de áudio" : d.getProductName().toString();
            result.add(name + " (" + d.getId() + ")");
        }
        return result;
    }
    AudioDeviceInfo deviceAt(int index) {
        if (audio == null) return null;
        AudioDeviceInfo[] devices = audio.getDevices(AudioManager.GET_DEVICES_OUTPUTS);
        return index >= 0 && index < devices.length ? devices[index] : null;
    }
}
