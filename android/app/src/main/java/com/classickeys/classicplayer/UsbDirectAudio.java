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

    private static native boolean nativeStart(int fd, int endpoint, int maxPacket, int packetsPerSecond, int bufferMs, int deviceRate,
                                              int channels, int subframeBytes, int feedbackEndpoint, int feedbackMaxPacket, int unitsPerSecond);
    private static native int nativeSpeed(int fd);
    private static native void nativeStop();
    private static native long[] nativeInfo();
    private static native long[] nativeSelfTest(int milliseconds);
    private static native long[] nativeSelfTestEx(int milliseconds, int rate, int channels, int subframeBytes, double feedbackFramesPerUnit,
                                                  int unitsPerSecond, int packetsPerSecond, boolean feedbackFourBytes);

    private UsbDeviceConnection connection;
    private UsbInterface claimed, claimedControl;
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

    private static int le16(byte[] b, int p) { return (b[p] & 0xff) | ((b[p + 1] & 0xff) << 8); }
    private static long le32(byte[] b, int p) { return (b[p] & 0xffL) | ((b[p + 1] & 0xffL) << 8) | ((b[p + 2] & 0xffL) << 16) | ((b[p + 3] & 0xffL) << 24); }

    /** UAC2: sample rates the clock source supports (GET_RANGE), as the standard rates inside its ranges. */
    private static void queryClockRates(UsbDeviceConnection c, UsbAudioDescriptors.StreamAlt alt) {
        byte[] buffer = new byte[2 + 12 * 20];
        int index = (alt.clockId << 8) | alt.controlInterface;
        int got = c.controlTransfer(0xA1, 0x02, 0x0100, index, buffer, buffer.length, 500);
        if (got < 14) return;
        int count = Math.min(le16(buffer, 0), (got - 2) / 12);
        java.util.ArrayList<Integer> rates = new java.util.ArrayList<>();
        for (int i = 0; i < count; i++) {
            long min = le32(buffer, 2 + 12 * i), max = le32(buffer, 6 + 12 * i), res = le32(buffer, 10 + 12 * i);
            for (int rate : UsbAudioDescriptors.PREFERRED_RATES)
                if (rate >= min && rate <= max && (res == 0 || (rate - min) % res == 0) && !rates.contains(rate)) rates.add(rate);
        }
        alt.rates = new int[rates.size()];
        for (int i = 0; i < alt.rates.length; i++) alt.rates[i] = rates.get(i);
    }

    /** Programs the sample rate; returns the rate the device reports afterwards (or the requested one if it cannot say). */
    private static int programRate(UsbDeviceConnection c, UsbAudioDescriptors.StreamAlt alt, int rate) {
        if (alt.uacVersion == 2) {
            if (alt.clockId < 0) return rate;
            int index = (alt.clockId << 8) | alt.controlInterface;
            byte[] data = {(byte) rate, (byte) (rate >> 8), (byte) (rate >> 16), (byte) (rate >> 24)};
            c.controlTransfer(0x21, 0x01, 0x0100, index, data, 4, 500);
            byte[] now = new byte[4];
            if (c.controlTransfer(0xA1, 0x01, 0x0100, index, now, 4, 500) == 4) {
                long actual = le32(now, 0);
                if (actual >= 4000 && actual <= 400000) return (int) actual;
            }
            return rate;
        }
        // UAC1: the rate belongs to the streaming endpoint. Devices with a single fixed rate may refuse; that is fine.
        byte[] data = {(byte) rate, (byte) (rate >> 8), (byte) (rate >> 16)};
        c.controlTransfer(0x22, 0x01, 0x0100, alt.endpointAddress, data, 3, 500);
        return rate;
    }

    private static UsbInterface findInterface(UsbDevice device, int id, int alternate) {
        for (int i = 0; i < device.getInterfaceCount(); i++) {
            UsbInterface ui = device.getInterface(i);
            if (ui.getId() == id && ui.getAlternateSetting() == alternate) return ui;
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
        UsbAudioDescriptors.ControlInfo control = UsbAudioDescriptors.parseControl(raw);
        if (alts.isEmpty()) { c.close(); return "USB direto: a interface não tem saída de áudio PCM (classe de áudio USB)"; }

        int speed = nativeSpeed(c.getFileDescriptor());
        int bcdUsb = raw != null && raw.length >= 4 ? le16(raw, 2) : 0x0110;
        int units = (speed >= 3 || (speed < 0 && bcdUsb >= 0x0200)) ? 8000 : 1000;

        // UAC2 keeps its sample rates in the clock source: ask the device.
        boolean needsControl = false;
        for (UsbAudioDescriptors.StreamAlt a : alts) if (a.uacVersion == 2) needsControl = true;
        if (needsControl && control.controlInterface >= 0) {
            UsbInterface ac = findInterface(device, control.controlInterface, 0);
            if (ac != null && c.claimInterface(ac, true)) claimedControl = ac;
            for (UsbAudioDescriptors.StreamAlt a : alts) {
                if (a.uacVersion != 2) continue;
                a.controlInterface = control.controlInterface;
                a.clockId = control.clockFor(a.terminalLink);
                if (a.clockId >= 0 && claimedControl != null) queryClockRates(c, a);
            }
        }

        int[] chosenRate = new int[1];
        UsbAudioDescriptors.StreamAlt alt = UsbAudioDescriptors.pick(alts, units, chosenRate);
        if (alt == null) {
            releaseControl(c);
            c.close();
            return "USB direto: nenhum modo de saída compatível (PCM 16/24/32 bits, taxa padrão, endpoint com feedback)";
        }
        UsbInterface target = findInterface(device, alt.interfaceNumber, alt.altSetting);
        if (target == null) { releaseControl(c); c.close(); return "USB direto: interface não encontrada"; }
        if (!c.claimInterface(target, true)) { releaseControl(c); c.close(); return "USB direto: o Android não liberou a interface"; }
        if (!c.setInterface(target)) { c.releaseInterface(target); releaseControl(c); c.close(); return "USB direto: falha ao ativar o modo de saída"; }

        int rate = programRate(c, alt, chosenRate[0]);
        if (rate != chosenRate[0] && !alt.supportsRate(rate)) rate = chosenRate[0];
        int packetsPerSecond = UsbAudioDescriptors.packetsPerSecond(units, alt.interval);
        int packetBytes = alt.maxPacketSize * alt.mult;
        int feedbackPacket = alt.feedbackEndpoint != 0 ? Math.max(3, alt.feedbackMaxPacket) : 0;
        if (!nativeStart(c.getFileDescriptor(), alt.endpointAddress, packetBytes, packetsPerSecond, bufferMs, rate,
                alt.channels, alt.subframeBytes, alt.feedbackEndpoint, feedbackPacket, units)) {
            c.releaseInterface(target); releaseControl(c); c.close();
            return "USB direto: falha ao iniciar a transferência";
        }
        connection = c; claimed = target; this.packetsPerSecond = packetsPerSecond;
        String sync = alt.syncType == 1 ? "assíncrono+feedback" : alt.syncType == 2 ? "adaptativo" : alt.syncType == 3 ? "síncrono" : "sem sync";
        description = "USB direto · " + device.getProductName() + " · UAC" + alt.uacVersion + " · " + alt.channels + "ch " + alt.bitResolution + "bit · "
                + (rate / 1000.0) + " kHz" + (rate == 44100 ? "" : " (resample)") + " · " + sync + " · " + (units == 8000 ? "high speed" : "full speed")
                + " · " + packetsPerSecond + " pacotes/s";
        return null;
    }

    private void releaseControl(UsbDeviceConnection c) {
        if (claimedControl != null) { c.releaseInterface(claimedControl); claimedControl = null; }
    }

    synchronized void close() {
        nativeStop();
        if (connection != null) {
            if (claimed != null) connection.releaseInterface(claimed);
            releaseControl(connection);
            connection.close();
        }
        connection = null; claimed = null; description = "";
    }

    synchronized boolean isOpen() { return connection != null; }

    /** False once the native thread has stopped (device unplugged or a transfer failed). */
    boolean isRunning() { long[] i = nativeInfo(); return i[0] != 0; }

    /** Audio kept queued in the USB transfers, in milliseconds. */
    double queuedMillis() { long[] i = nativeInfo(); return i[5] * i[6] * 1000.0 / Math.max(1, packetsPerSecond); }
    private int packetsPerSecond = 1000;

    /** Status line: transfers sent/completed/errors. */
    synchronized String status() {
        if (connection == null) return "USB direto desligado";
        long[] i = nativeInfo();
        return String.format(java.util.Locale.US, "%s · %d URBs × %d pacotes · enviados %d · concluídos %d · erros %d%s",
                description, i[5], i[6], i[1], i[2], i[3], i[0] == 0 ? " · PARADO (errno " + i[4] + ")" : "");
    }

    /** Runs the driver against a fake device for the given time: {frames, longest packet, URBs with audio, errors, URBs done}. */
    static long[] selfTest(int milliseconds) { return nativeSelfTest(milliseconds); }

    /**
     * Fake-device run with any format: {frames, longest packet bytes, URBs with audio, errors, URBs done, packets submitted,
     * nonzero bytes beyond the stereo pair, feedback updates}. feedbackFramesPerUnit 0 = no feedback endpoint.
     */
    static long[] selfTest(int milliseconds, int rate, int channels, int subframeBytes, double feedbackFramesPerUnit,
                           int unitsPerSecond, int packetsPerSecond, boolean feedbackFourBytes) {
        return nativeSelfTestEx(milliseconds, rate, channels, subframeBytes, feedbackFramesPerUnit, unitsPerSecond, packetsPerSecond, feedbackFourBytes);
    }
}
