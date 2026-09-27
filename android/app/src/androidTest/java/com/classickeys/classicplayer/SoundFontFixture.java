package com.classickeys.classicplayer;

import java.io.*;
import java.nio.charset.StandardCharsets;

/** Original synthetic sine bank: no downloaded or licensed sound library. */
final class SoundFontFixture {
    private static void u16(OutputStream out, int value) throws IOException {
        out.write(value & 255); out.write(value >>> 8 & 255);
    }
    private static void u32(OutputStream out, int value) throws IOException {
        u16(out, value); u16(out, value >>> 16);
    }
    private static void text(OutputStream out, String value) throws IOException {
        out.write(value.getBytes(StandardCharsets.US_ASCII));
    }
    private static void name(OutputStream out, String value) throws IOException {
        byte[] bytes = value.getBytes(StandardCharsets.US_ASCII);
        out.write(bytes, 0, Math.min(20, bytes.length));
        for (int i = Math.min(20, bytes.length); i < 20; i++) out.write(0);
    }
    private static byte[] chunk(String id, byte[] data) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        text(out, id); u32(out, data.length); out.write(data);
        if ((data.length & 1) != 0) out.write(0);
        return out.toByteArray();
    }
    private static byte[] list(String type, byte[]... chunks) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream(); text(out, type);
        for (byte[] bytes : chunks) out.write(bytes);
        return chunk("LIST", out.toByteArray());
    }
    private static void preset(OutputStream out, String label, int bag) throws IOException {
        name(out, label); u16(out, 0); u16(out, 0); u16(out, bag);
        u32(out, 0); u32(out, 0); u32(out, 0);
    }
    private static void sample(OutputStream out, String label, int end) throws IOException {
        name(out, label); u32(out, 0); u32(out, end); u32(out, 0); u32(out, end);
        u32(out, 48000); out.write(60); out.write(0); u16(out, 0); u16(out, 1);
    }
    static void write(File file) throws IOException {
        int length = 2048;
        ByteArrayOutputStream samples = new ByteArrayOutputStream();
        for (int i = 0; i < length; i++) u16(samples, (int)(12000 * Math.sin(2 * Math.PI * 11 * i / length)));
        for (int i = 0; i < 46; i++) u16(samples, 0);
        ByteArrayOutputStream phdr = new ByteArrayOutputStream();
        preset(phdr, "Regression sine", 0); preset(phdr, "EOP", 1);
        ByteArrayOutputStream inst = new ByteArrayOutputStream();
        name(inst, "Sine"); u16(inst, 0); name(inst, "EOI"); u16(inst, 1);
        ByteArrayOutputStream shdr = new ByteArrayOutputStream();
        sample(shdr, "Sine", length); sample(shdr, "EOS", 0);
        ByteArrayOutputStream body = new ByteArrayOutputStream(); text(body, "sfbk");
        body.write(list("INFO", chunk("ifil", new byte[]{2, 0, 1, 0}),
                chunk("isng", "EMU8000\0".getBytes(StandardCharsets.US_ASCII)),
                chunk("INAM", "Regression\0".getBytes(StandardCharsets.US_ASCII))));
        body.write(list("sdta", chunk("smpl", samples.toByteArray())));
        body.write(list("pdta", chunk("phdr", phdr.toByteArray()),
                chunk("pbag", new byte[]{0,0,0,0, 1,0,0,0}), chunk("pmod", new byte[10]),
                chunk("pgen", new byte[]{41,0,0,0, 0,0,0,0}), chunk("inst", inst.toByteArray()),
                chunk("ibag", new byte[]{0,0,0,0, 3,0,0,0}), chunk("imod", new byte[10]),
                chunk("igen", new byte[]{54,0,1,0, 58,0,60,0, 53,0,0,0, 0,0,0,0}),
                chunk("shdr", shdr.toByteArray())));
        try (OutputStream out = new FileOutputStream(file)) { out.write(chunk("RIFF", body.toByteArray())); }
    }
    static void wav(File file, short[]... blocks) throws IOException {
        int count = 0; for (short[] block : blocks) count += block.length;
        try (OutputStream out = new BufferedOutputStream(new FileOutputStream(file))) {
            text(out, "RIFF"); u32(out, 36 + count * 2); text(out, "WAVEfmt ");
            u32(out, 16); u16(out, 1); u16(out, 2); u32(out, 48000); u32(out, 192000);
            u16(out, 4); u16(out, 16); text(out, "data"); u32(out, count * 2);
            for (short[] block : blocks) for (short value : block) u16(out, value);
        }
    }
}
