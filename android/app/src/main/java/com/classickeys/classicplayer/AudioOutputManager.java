package com.classickeys.classicplayer;

import android.content.Context;
import android.media.AudioDeviceInfo;
import android.media.AudioManager;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/** Enumerates Android output routes and identifies USB audio endpoints. */
final class AudioOutputManager {
    private final AudioManager audio;
    AudioOutputManager(Context context) { audio = (AudioManager) context.getSystemService(Context.AUDIO_SERVICE); }
    List<String> outputs() {
        List<String> result = new ArrayList<>();
        if (audio == null) return result;
        for (AudioDeviceInfo d : audio.getDevices(AudioManager.GET_DEVICES_OUTPUTS)) {
            String name = d.getProductName() == null ? "Saída de áudio" : d.getProductName().toString();
            result.add(typeName(d.getType()) + " · " + name + " (ID " + d.getId() + ")");
        }
        return result;
    }
    AudioDeviceInfo deviceAt(int index) {
        if (audio == null) return null;
        AudioDeviceInfo[] devices = audio.getDevices(AudioManager.GET_DEVICES_OUTPUTS);
        return index >= 0 && index < devices.length ? devices[index] : null;
    }
    AudioDeviceInfo deviceById(int id) {
        if (audio == null) return null;
        for (AudioDeviceInfo device : audio.getDevices(AudioManager.GET_DEVICES_OUTPUTS))
            if (device.getId() == id) return device;
        return null;
    }
    AudioDeviceInfo deviceByIdentity(int type, String name) {
        if (audio == null || name.isEmpty()) return null;
        for (AudioDeviceInfo device : audio.getDevices(AudioManager.GET_DEVICES_OUTPUTS))
            if (device.getType() == type && device.getProductName() != null
                    && name.contentEquals(device.getProductName())) return device;
        return null;
    }
    String formatCapabilities(AudioDeviceInfo device) {
        if (device == null) return "FORMATOS (Android): rota de saída ainda não confirmada";
        return "FORMATOS (Android): " + sampleRates(device.getSampleRates())
                + " · " + channelCounts(device.getChannelCounts())
                + " · " + encodings(device.getEncodings());
    }
    private static String sampleRates(int[] rates) {
        if (rates == null) return "taxas indisponíveis";
        if (rates.length == 0) return "qualquer taxa (sem lista restritiva)";
        StringBuilder text = new StringBuilder();
        int shown = Math.min(rates.length, 5);
        for (int i = 0; i < shown; i++) {
            if (i > 0) text.append('/');
            text.append(String.format(Locale.forLanguageTag("pt-BR"), "%.1f", rates[i] / 1000.0));
        }
        if (rates.length > shown) text.append("/…");
        return text.append(" kHz").toString();
    }
    private static String channelCounts(int[] counts) {
        if (counts == null) return "canais indisponíveis";
        if (counts.length == 0) return "qualquer nº de canais (sem lista restritiva)";
        StringBuilder text = new StringBuilder();
        int shown = Math.min(counts.length, 4);
        for (int i = 0; i < shown; i++) {
            if (i > 0) text.append('/');
            text.append(counts[i]);
        }
        if (counts.length > shown) text.append("/…");
        return text.insert(0, "canais ").toString();
    }
    private static String encodings(int[] formats) {
        if (formats == null) return "formatos indisponíveis";
        if (formats.length == 0) return "qualquer formato (sem lista restritiva)";
        StringBuilder text = new StringBuilder();
        int shown = Math.min(formats.length, 4);
        for (int i = 0; i < shown; i++) {
            if (i > 0) text.append('/');
            switch (formats[i]) {
                case android.media.AudioFormat.ENCODING_PCM_8BIT: text.append("PCM 8"); break;
                case android.media.AudioFormat.ENCODING_PCM_16BIT: text.append("PCM 16"); break;
                case android.media.AudioFormat.ENCODING_PCM_FLOAT: text.append("PCM float/alta precisão"); break;
                default: text.append("formato ").append(formats[i]); break;
            }
        }
        if (formats.length > shown) text.append("/…");
        return text.toString();
    }
    AudioDeviceInfo automaticUsbOutput() {
        if (audio == null) return null;
        AudioDeviceInfo firstUsbOutput = null;
        AudioDeviceInfo yamahaOutput = null;
        for (AudioDeviceInfo device : audio.getDevices(AudioManager.GET_DEVICES_OUTPUTS)) {
            if (!isUsbAudioOutput(device)) continue;
            if (firstUsbOutput == null) firstUsbOutput = device;
            String name = device.getProductName() == null ? ""
                    : device.getProductName().toString().toLowerCase(Locale.ROOT);
            if (name.contains("yamaha") || name.contains("ck series") || name.contains("ck61"))
                yamahaOutput = device;
        }
        return yamahaOutput != null ? yamahaOutput : firstUsbOutput;
    }
    private static boolean isUsbAudioOutput(AudioDeviceInfo device) {
        return device.getType() == AudioDeviceInfo.TYPE_USB_DEVICE
                || device.getType() == AudioDeviceInfo.TYPE_USB_HEADSET
                || device.getType() == AudioDeviceInfo.TYPE_USB_ACCESSORY;
    }
    private String typeName(int type) {
        switch (type) {
            case AudioDeviceInfo.TYPE_USB_DEVICE: return "USB";
            case AudioDeviceInfo.TYPE_USB_HEADSET: return "USB headset";
            case AudioDeviceInfo.TYPE_USB_ACCESSORY: return "USB accessory";
            case AudioDeviceInfo.TYPE_BLUETOOTH_A2DP: return "Bluetooth";
            case AudioDeviceInfo.TYPE_WIRED_HEADPHONES: return "Fones";
            case AudioDeviceInfo.TYPE_WIRED_HEADSET: return "Headset";
            case AudioDeviceInfo.TYPE_HDMI: return "HDMI";
            case AudioDeviceInfo.TYPE_BUILTIN_SPEAKER: return "Tablet";
            default: return "Áudio";
        }
    }
}
