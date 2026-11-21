package com.classickeys.classicplayer;

import android.content.Context;
import android.hardware.usb.UsbConstants;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbInterface;
import android.hardware.usb.UsbManager;
import java.util.List;

/**
 * Direct USB audio output (experimental): talks to a class-compliant USB audio interface without
 * Android's audio mixer. Opening needs the USB permission to have been granted already (the UI
 * asks for it). Only UAC1 16-bit stereo at 44.1 kHz on adaptive/synchronous endpoints is
 * supported by this first driver; everything else returns an explanatory message so the caller
 * can keep using Oboe.
 */
final class UsbDirectAudio {
    static { System.loadLibrary("classic_player_native"); }

    private static native boolean nativeStart(int fd, int endpoint, int maxPacket, int packetsPerSecond, int bufferMs);
    private static native void nativeStop();
    private static native long[] nativeInfo();
    private static native long[] nativeSelfTest(int milliseconds);

    private UsbDeviceConnection connection;
    private UsbInterface claimed;
    private String description = "";

    /** First attached device that exposes an AudioStreaming interface, or null. */
    static UsbDevice findCandidate(UsbManager manager) {
        if (manager == null) return null;
        for (UsbDevice device : manager.getDeviceList().values())
            for (int i = 0; i < device.getInterfaceCount(); i++) {
                UsbInterface ui = device.getInterface(i);
                if (ui.getInterfaceClass() == UsbConstants.USB_CLASS_AUDIO && ui.getInterfaceSubclass() == 2) return device;
            }
        return null;
    }

    /** @return null on success, otherwise a message in Portuguese for the status line. */
    synchronized String open(Context context, UsbDevice device, int bufferMs) {
        close();
        UsbManager manager = (UsbManager) context.getSystemService(Context.USB_SERVICE);
        if (manager == null || device == null) return "USB direto: nenhum dispositivo";
        if (!manager.hasPermission(device)) return "USB direto: permissão USB não concedida";
        UsbDeviceConnection c = manager.openDevice(device);
        if (c == null) return "USB direto: não foi possível abrir o dispositivo";
        byte[] raw = c.getRawDescriptors();
        List<UsbAudioDescriptors.StreamAlt> alts = UsbAudioDescriptors.parse(raw);
        UsbAudioDescriptors.StreamAlt alt = UsbAudioDescriptors.choose(alts, new int[]{44100});
        if (alt == null) {
            c.close();
            return UsbAudioDescriptors.choose(alts, new int[]{48000}) != null
                    ? "USB direto: esta interface só aceita 48 kHz (ainda não suportado)"
                    : "USB direto: sem modo estéreo de 16 bits (UAC1)";
        }
        if (alt.syncType == 1) { c.close(); return "USB direto: endpoint assíncrono ainda não suportado"; }
        UsbInterface target = null;
        for (int i = 0; i < device.getInterfaceCount(); i++) {
            UsbInterface ui = device.getInterface(i);
            if (ui.getId() == alt.interfaceNumber && ui.getAlternateSetting() == alt.altSetting) target = ui;
        }
        if (target == null) { c.close(); return "USB direto: interface não encontrada"; }
        if (!c.claimInterface(target, true)) { c.close(); return "USB direto: o Android não liberou a interface"; }
        if (!c.setInterface(target)) { c.releaseInterface(target); c.close(); return "USB direto: falha ao ativar o modo de saída"; }
        int bcdUsb = raw != null && raw.length >= 4 ? (raw[2] & 0xff) | ((raw[3] & 0xff) << 8) : 0x0110;
        int base = bcdUsb >= 0x0200 ? 8000 : 1000;
        int packetsPerSecond = Math.max(1, base >> Math.max(0, alt.interval - 1));
        int packetBytes = alt.maxPacketSize & 0x7ff;
        if (!nativeStart(c.getFileDescriptor(), alt.endpointAddress, packetBytes, packetsPerSecond, bufferMs)) {
            c.releaseInterface(target); c.close();
            return "USB direto: falha ao iniciar a transferência";
        }
        connection = c; claimed = target;
        description = "USB direto · " + device.getProductName() + " · " + alt + " · " + packetsPerSecond + " pacotes/s";
        return null;
    }

    synchronized void close() {
        nativeStop();
        if (connection != null) {
            if (claimed != null) connection.releaseInterface(claimed);
            connection.close();
        }
        connection = null; claimed = null; description = "";
    }

    synchronized boolean isOpen() { return connection != null; }

    /** Status line: transfers sent/completed/errors. */
    synchronized String status() {
        if (connection == null) return "USB direto desligado";
        long[] i = nativeInfo();
        return String.format(java.util.Locale.US, "%s · %d URBs × %d pacotes · enviados %d · concluídos %d · erros %d%s",
                description, i[5], i[6], i[1], i[2], i[3], i[0] == 0 ? " · PARADO (errno " + i[4] + ")" : "");
    }

    /** Runs the driver against a fake device for the given time: {frames, longest packet, URBs with audio, errors, URBs done}. */
    static long[] selfTest(int milliseconds) { return nativeSelfTest(milliseconds); }
}
