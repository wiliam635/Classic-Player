#!/usr/bin/env python3
"""Convert supported Nord CBIN/CNSP piano files to a test SoundFont 2 bank.

Only attack strokes are exported. The input is never modified. This is a
clean-room test converter based on the Classic Player NordPianoDecoder model.
"""

import argparse
import math
import struct
import zlib
from pathlib import Path

import numpy as np


RATE = 35002
BLOCK_WORDS = 511
OVERLAP = 64


def u16(data, at):
    return struct.unpack_from(">H", data, at)[0]


def u32(data, at):
    return struct.unpack_from(">I", data, at)[0]


def chunk(out, name, payload):
    out.write(name)
    out.write(struct.pack("<I", len(payload)))
    out.write(payload)
    if len(payload) & 1:
        out.write(b"\0")


def begin_chunk(out, name):
    out.write(name)
    length_at = out.tell()
    out.write(b"\0\0\0\0")
    return length_at


def end_chunk(out, length_at):
    end = out.tell()
    length = end - length_at - 4
    if length > 0xFFFFFFFF:
        raise ValueError("O SF2 ultrapassa o limite RIFF de 4 GiB")
    out.seek(length_at)
    out.write(struct.pack("<I", length))
    out.seek(end)
    if length & 1:
        out.write(b"\0")


def name20(value):
    return value.encode("ascii", "replace")[:20].ljust(20, b"\0")


def info_string(value):
    data = value.encode("ascii", "replace") + b"\0"
    return data + (b"\0" if len(data) & 1 else b"")


def parse(source):
    raw = source.read_bytes()
    if len(raw) < 44 + 0x732 or raw[:4] != b"CBIN" or raw[8:12] != b"npno":
        raise ValueError("Não é um arquivo Nord CBIN/npno reconhecido")
    if struct.unpack_from("<I", raw, 4)[0] != 1:
        raise ValueError("Geração CBIN não suportada")
    body = memoryview(raw)[44:]
    if zlib.crc32(body) != struct.unpack_from("<I", raw, 24)[0]:
        raise ValueError("CRC do arquivo não confere")
    if body[:4] != b"CNSP" or u16(body, 4) not in (0x450, 0x464):
        raise ValueError("Versão CNSP não suportada")
    if u16(body, 0x61C) != u16(body, 4):
        raise ValueError("Versões CNSP inconsistentes")
    channels = u16(body, 0x61E)
    if channels not in (1, 2):
        raise ValueError("Contagem de canais não suportada")
    count = u16(body, 0x620)
    root_counts = [u16(body, 0x622 + 2 * i) for i in range(128)]
    if sum(root_counts) != count:
        raise ValueError("Contagem de gravações inconsistente")
    block_bytes = BLOCK_WORDS * 2 * channels
    directory_end = 0x732 + count * 118
    cursor = ((directory_end + 192 + block_bytes - 1) // block_bytes) * block_bytes - 192
    if cursor > len(body) or any(body[directory_end:cursor]):
        raise ValueError("Diretório ou alinhamento inválido")
    key_map = list(body[0x8C:0x10C])
    fine_tune = list(struct.unpack_from("128b", body, 0x18C))
    gain_db = struct.unpack_from("b", body, 0x40C)[0] / 10
    title = bytes(body[0x1C:0x3C]).split(b"\0", 1)[0].split(b"#", 1)[0].decode("ascii", "replace")
    if u16(body, 4) == 0x464:
        long_title = bytes(body[0x3C:0x5C]).split(b"\0", 1)[0].decode("ascii", "replace")
        title = long_title or title
    strokes = []
    index = 0
    for root, root_count in enumerate(root_counts):
        for _ in range(root_count):
            record = body[0x732 + index * 118:0x732 + (index + 1) * 118]
            start, bank, layer = u32(record, 0), record[4], record[5]
            frames, blocks = u32(record, 6), u16(record, 10)
            if start != cursor or blocks == 0 or frames == 0 or cursor + blocks * block_bytes > len(body):
                raise ValueError(f"Gravação {index} inválida")
            loop_start, loop_length = u32(record, 0x1C), u32(record, 0x20)
            loop_end = loop_start + loop_length
            if not (loop_start > 0 and loop_length > 0 and loop_end <= frames):
                loop_start = loop_end = 0
            strokes.append(dict(root=root, bank=bank, layer=layer, frames=frames,
                                blocks=blocks, start=cursor, record=bytes(record),
                                trim=struct.unpack_from(">h", record, 0x34)[0],
                                loop_start=loop_start, loop_end=loop_end))
            cursor += blocks * block_bytes
            index += 1
    if cursor != len(body):
        raise ValueError("Dados extras ou áudio incompleto")
    attacks = [s for s in strokes if s["bank"] == 0]
    if not attacks:
        raise ValueError("Sem amostras de ataque")
    roots = {s["root"] for s in attacks}
    if any(root != 255 and root not in roots for root in key_map):
        raise ValueError("Mapa de teclas aponta para nota ausente")
    velocity_scale = max(31, (max(s["layer"] for s in attacks) * 31 + 26) // 27)
    return body, channels, key_map, fine_tune, gain_db, title, attacks, velocity_scale


def decode_stroke(body, channels, stroke):
    frames = stroke["frames"]
    record = stroke["record"]
    history = np.zeros((channels, 4), dtype=np.int64)
    for channel in range(channels):
        for seed in range(4):
            history[channel, 3 - seed] = struct.unpack_from(">h", record, 12 + 2 * (channel * 4 + seed))[0]
    result = np.empty((frames, channels), dtype=np.int16)
    cursor = 0
    previous_tail = None
    block_bytes = BLOCK_WORDS * 2 * channels
    powers = [1 << n for n in range(17)]
    for block in range(stroke["blocks"]):
        at = stroke["start"] + block * block_bytes
        raw = body[at:at + block_bytes]
        header = u16(raw, 0)
        width, order = header & 31, (header >> 5) & 7
        if not 1 <= width <= 16 or order > 4:
            raise ValueError("Bloco de áudio não suportado")
        block_frames = (8 * (block_bytes - 2)) // (width * channels)
        if block_frames < OVERLAP + 4:
            raise ValueError("Bloco de áudio curto demais")
        # Each big-endian 16-bit word stores fields least-significant bit first.
        words = np.frombuffer(raw[2:], dtype=">u2").astype("<u2")
        bits = np.unpackbits(words.view(np.uint8), bitorder="little")
        fields = bits[:block_frames * channels * width].reshape(-1, width)
        residual = fields.astype(np.int64) @ np.asarray(powers[:width], dtype=np.int64)
        residual -= (residual >= (1 << (width - 1))).astype(np.int64) * (1 << width)
        residual = residual.reshape(block_frames, channels)
        decoded = np.empty_like(residual)
        for channel in range(channels):
            values = residual[:, channel]
            h = history[channel]
            if order:
                # Integrate the backward difference of order k using the prior
                # four samples as initial conditions. Equivalent to the C++
                # recurrence in NordPianoDecoder::predict.
                differences = [sum(((-1) ** j) * math.comb(k, j) * int(h[j])
                                   for j in range(k + 1)) for k in range(order)]
                for k in range(order - 1, -1, -1):
                    values = np.cumsum(values, dtype=np.int64) + differences[k]
            decoded[:, channel] = values
        owned = block_frames - OVERLAP
        if previous_tail is not None and not np.array_equal(decoded[:OVERLAP], previous_tail):
            raise ValueError("Sobreposição entre blocos não confere")
        if cursor + owned > frames:
            raise ValueError("Áudio excede duração declarada")
        result[cursor:cursor + owned] = np.clip(decoded[:owned], -32768, 32767).astype(np.int16)
        cursor += owned
        previous_tail = decoded[owned:].copy()
        history = decoded[owned - 4:owned][::-1].T.copy()
    if cursor != frames:
        raise ValueError("Duração decodificada não confere")
    return result


def gen(operator, amount):
    return struct.pack("<HH", operator, amount & 0xFFFF)


def make_zones(attacks, key_map, fine_tune, velocity_scale):
    by_root = {}
    for i, stroke in enumerate(attacks):
        by_root.setdefault(stroke["root"], []).append(i)
    zones = []
    for note, root in enumerate(key_map):
        if root == 255:
            continue
        candidates = sorted(by_root[root], key=lambda i: attacks[i]["layer"])
        previous = None
        low = 1
        for velocity in range(1, 129):
            chosen = None
            if velocity < 128:
                limit = ((127 - velocity) * velocity_scale) // 127
                for i in candidates:
                    if attacks[i]["layer"] <= limit:
                        chosen = i
            if chosen != previous:
                if previous is not None:
                    zones.append((note, low, velocity - 1, previous, fine_tune[note]))
                previous = chosen
                low = velocity
    return zones


def convert(source, output):
    body, channels, key_map, fine_tune, gain_db, title, attacks, velocity_scale = parse(source)
    zones = make_zones(attacks, key_map, fine_tune, velocity_scale)
    if not zones:
        raise ValueError("Nenhuma zona de teclado exportável")
    # All relative gains are expressed as nonnegative SF2 attenuations; a
    # common pre-gain recovers positive gain values within the PCM16 range.
    gains = [gain_db - s["trim"] / 10 for s in attacks]
    common_boost = max(0.0, max(gains))
    sample_headers = []
    clipped = 0
    with output.open("wb") as out:
        riff = begin_chunk(out, b"RIFF")
        out.write(b"sfbk")
        info = begin_chunk(out, b"LIST")
        out.write(b"INFO")
        chunk(out, b"ifil", struct.pack("<HH", 2, 1))
        chunk(out, b"isng", b"EMU8000\0")
        chunk(out, b"INAM", info_string(title))
        chunk(out, b"ICMT", info_string("Personal test conversion from user-supplied npno; attack samples only."))
        end_chunk(out, info)
        sdta = begin_chunk(out, b"LIST")
        out.write(b"sdta")
        smpl = begin_chunk(out, b"smpl")
        cursor = 0
        boost_factor = 10 ** (common_boost / 20)
        for index, stroke in enumerate(attacks):
            pcm = decode_stroke(body, channels, stroke)
            if channels == 1:
                pcm = np.repeat(pcm, 2, axis=1)
            amplified = np.rint(pcm.astype(np.float64) * boost_factor)
            clipped += int(np.count_nonzero((amplified < -32768) | (amplified > 32767)))
            pcm = np.clip(amplified, -32768, 32767).astype("<i2")
            frames = stroke["frames"]
            for channel in range(2):
                start = cursor
                out.write(pcm[:, channel].tobytes())
                out.write(b"\0" * (46 * 2))
                cursor += frames + 46
                loop_start = start + stroke["loop_start"] if stroke["loop_end"] else start
                loop_end = start + stroke["loop_end"] if stroke["loop_end"] else start
                sample_headers.append(struct.pack(
                    "<20sIIIIIBbHH", name20(f"N{stroke['root']:03d}L{stroke['layer']:03d}{'L' if channel == 0 else 'R'}"),
                    start, start + frames, loop_start, loop_end, RATE, stroke["root"], 0,
                    index * 2 + (1 - channel), 4 if channel == 0 else 2))
            if (index + 1) % 50 == 0:
                print(f"Decodificadas {index + 1}/{len(attacks)} amostras de ataque", flush=True)
        end_chunk(out, smpl)
        end_chunk(out, sdta)
        pdta = begin_chunk(out, b"LIST")
        out.write(b"pdta")
        chunk(out, b"phdr", struct.pack("<20sHHHIII", name20(title), 0, 0, 0, 0, 0, 0)
              + struct.pack("<20sHHHIII", name20("EOP"), 0, 0, 1, 0, 0, 0))
        chunk(out, b"pbag", struct.pack("<HHHH", 0, 0, 1, 0))
        chunk(out, b"pmod", b"\0" * 10)
        chunk(out, b"pgen", gen(41, 0) + b"\0" * 4)
        chunk(out, b"inst", struct.pack("<20sH", name20(title), 0)
              + struct.pack("<20sH", name20("EOI"), len(zones)))
        ibags = bytearray()
        igens = bytearray()
        for note, lo, hi, sample_index, fine in zones:
            stroke = attacks[sample_index]
            ibags += struct.pack("<HH", len(igens) // 4, 0)
            igens += gen(43, note | (note << 8))
            igens += gen(44, lo | (hi << 8))
            igens += gen(48, round((common_boost - gains[sample_index]) * 10))
            igens += gen(38, -3600)  # ~125 ms release envelope
            if fine:
                igens += gen(52, round(fine * 0.7))
            if stroke["loop_end"]:
                igens += gen(54, 3)  # loop until key release
            igens += gen(53, sample_index * 2)  # stereo left links to right
        ibags += struct.pack("<HH", len(igens) // 4, 0)
        igens += b"\0" * 4
        chunk(out, b"ibag", ibags)
        chunk(out, b"imod", b"\0" * 10)
        chunk(out, b"igen", igens)
        chunk(out, b"shdr", b"".join(sample_headers)
              + struct.pack("<20sIIIIIBbHH", name20("EOS"), cursor, cursor, cursor,
                            cursor, RATE, 0, 0, 0, 1))
        end_chunk(out, pdta)
        end_chunk(out, riff)
    print(f"SF2: {output}")
    print(f"Tamanho: {output.stat().st_size / (1024 * 1024):.1f} MiB; "
          f"amostras: {len(attacks)} estereo; zonas: {len(zones)}; "
          f"amostras PCM limitadas: {clipped}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.input.resolve() == args.output.resolve():
        parser.error("A saída deve ser diferente da entrada")
    convert(args.input, args.output)
