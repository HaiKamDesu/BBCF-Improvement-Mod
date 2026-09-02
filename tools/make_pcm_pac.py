#!/usr/bin/env python3
"""
Build a BGM replacement .pac whose wave bank is raw PCM instead of WMA.

Why this exists: Wine/Proton exposes no Media Foundation audio encoder at all, so the
converter's WMA path cannot run on Linux. The game's own data shows the XACT runtime
accepts PCM wave banks - 1989 shipped entries use wFormatTag 0, every one of them
satisfying duration * channels * bytesPerSample == PlayRegion length. This tool builds
such a bank by hand so the assumption can be checked in-game before any C++ is written.

It keeps the original track's sound bank (.xsb) byte for byte and swaps only the wave
bank, which is exactly what ConvertAudioToReplacementPac does.

  python3 make_pcm_pac.py <original.pac> <output.pac> [--mono] [--seconds N] [--wav FILE]

With no --wav it synthesises a diagnostic tone sequence: a shared 440 Hz tone, then
880 Hz left-only, then 880 Hz right-only. Wrong sample rate is audible as wrong pitch;
wrong channel count is audible as the separation collapsing.
"""
import argparse, math, os, struct, sys, wave
from array import array

WBND_HEADER   = 52          # 4 + 4 + 4 + 5 * 8
BANKDATA_SIZE = 96
MD_ELEM_SIZE  = 24
FPAC_ALIGN    = 16


def build_tone(rate, channels, seconds):
    """Diagnostic signal: correctness is audible, not just visible in a hex dump."""
    n = int(rate * seconds)
    buf = array('h', bytes(2 * n * channels))
    seg = rate // 2                       # half-second segments
    for i in range(n):
        phase = (i // seg) % 4
        t = i / rate
        left = right = 0.0
        if phase == 0:                    # both channels, 440 Hz
            left = right = math.sin(2 * math.pi * 440 * t)
        elif phase == 1:                  # left only, 880 Hz
            left = math.sin(2 * math.pi * 880 * t)
        elif phase == 2:                  # right only, 880 Hz
            right = math.sin(2 * math.pi * 880 * t)
        # phase 3: silence, so the pattern is easy to follow
        v = 0.35 * 32767
        if channels == 1:
            buf[i] = int(v * (left + right) * 0.5)
        else:
            buf[i * 2]     = int(v * left)
            buf[i * 2 + 1] = int(v * right)
    return buf.tobytes()


def read_wav(path, want_rate, want_channels):
    with wave.open(path, 'rb') as w:
        if w.getsampwidth() != 2:
            sys.exit("wav must be 16-bit")
        if w.getframerate() != want_rate or w.getnchannels() != want_channels:
            sys.exit("wav is %d Hz / %dch; need %d Hz / %dch"
                     % (w.getframerate(), w.getnchannels(), want_rate, want_channels))
        return w.readframes(w.getnframes())


def parse_fpac(blob):
    if blob[:4] != b'FPAC':
        sys.exit("not an FPAC container")
    data_start, total, count, flag, str_size = struct.unpack_from('<IIIII', blob, 4)
    stride = (str_size + 12 + 15) & ~15
    files, pos = [], 0x20
    for _ in range(count):
        name = blob[pos:pos + str_size].split(b'\0')[0].decode('ascii')
        idx, off, size = struct.unpack_from('<III', blob, pos + str_size)
        files.append((name, blob[data_start + off: data_start + off + size]))
        pos += stride
    return files, flag, str_size


def bank_name_of(xwb):
    """The .xsb refers to the bank by name, so a replacement must keep it."""
    if xwb[:4] != b'WBND':
        sys.exit("sub-file is not a WBND wave bank")
    bd_off = struct.unpack_from('<I', xwb, 12)[0]
    return xwb[bd_off + 8: bd_off + 72].split(b'\0')[0].decode('ascii')


def build_pcm_wavebank(bank_name, pcm, channels, rate):
    bits16 = 1
    block_align = channels * 2
    frames = len(pcm) // (channels * 2)

    mini = ((0 & 0x3)                       # wFormatTag 0 = PCM
            | ((channels & 0x7) << 2)
            | ((rate & 0x3FFFF) << 5)
            | ((block_align & 0xFF) << 23)
            | ((bits16 & 0x1) << 31))

    bank = bytearray(BANKDATA_SIZE)
    struct.pack_into('<I', bank, 0, 0x00080000)   # dwFlags, as every shipped bank sets
    struct.pack_into('<I', bank, 4, 1)            # dwEntryCount
    bank[8:8 + len(bank_name)] = bank_name.encode('ascii')[:63]
    struct.pack_into('<IIII', bank, 72, MD_ELEM_SIZE, 64, 4, 0)

    meta = bytearray(MD_ELEM_SIZE)
    struct.pack_into('<I', meta, 0, (frames & 0x0FFFFFFF) << 4)  # flags 0 | duration
    struct.pack_into('<I', meta, 4, mini)
    struct.pack_into('<I', meta, 8, 0)            # PlayRegion offset
    struct.pack_into('<I', meta, 12, len(pcm))    # PlayRegion length
    struct.pack_into('<I', meta, 16, 0)           # LoopRegion start
    struct.pack_into('<I', meta, 20, 0)           # LoopRegion total

    seg0_off, seg0_len = WBND_HEADER, BANKDATA_SIZE
    seg1_off, seg1_len = seg0_off + seg0_len, MD_ELEM_SIZE
    # No SeekTables: 1985 of the game's 1989 PCM entries live in banks without the
    # segment at all, and the remaining four mark it 0xFFFFFFFF. PCM needs no seeking.
    seg2_off = seg2_len = 0
    seg3_off = seg3_len = 0
    seg4_off = (seg1_off + seg1_len + 3) & ~3
    seg4_len = (len(pcm) + 3) & ~3

    xwb = bytearray(seg4_off + seg4_len)
    xwb[0:4] = b'WBND'
    struct.pack_into('<II', xwb, 4, 46, 44)       # tool version, format version
    for i, (o, l) in enumerate([(seg0_off, seg0_len), (seg1_off, seg1_len),
                                (seg2_off, seg2_len), (seg3_off, seg3_len),
                                (seg4_off, seg4_len)]):
        struct.pack_into('<II', xwb, 12 + i * 8, o, l)
    xwb[seg0_off:seg0_off + seg0_len] = bank
    xwb[seg1_off:seg1_off + seg1_len] = meta
    xwb[seg4_off:seg4_off + len(pcm)] = pcm
    return bytes(xwb)


def build_fpac(files, flag, str_size):
    stride = (str_size + 12 + 15) & ~15
    data_start = (0x20 + stride * len(files) + FPAC_ALIGN - 1) & ~(FPAC_ALIGN - 1)
    table, body = bytearray(), bytearray()
    for idx, (name, blob) in enumerate(files):
        off = len(body)
        entry = bytearray(stride)
        entry[0:len(name)] = name.encode('ascii')
        struct.pack_into('<III', entry, str_size, idx, off, len(blob))
        table += entry
        body += blob
        body += bytes((-len(body)) % FPAC_ALIGN)
    out = bytearray(data_start)
    out[0:4] = b'FPAC'
    struct.pack_into('<IIIII', out, 4, data_start, data_start + len(body),
                     len(files), flag, str_size)
    out[0x20:0x20 + len(table)] = table
    return bytes(out + body)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("original"); ap.add_argument("output")
    ap.add_argument("--mono", action="store_true")
    ap.add_argument("--seconds", type=float, default=20.0)
    ap.add_argument("--rate", type=int, default=44100)
    ap.add_argument("--wav")
    a = ap.parse_args()

    channels = 1 if a.mono else 2
    files, flag, str_size = parse_fpac(open(a.original, 'rb').read())

    rebuilt = []
    for name, blob in files:
        if name.lower().endswith('.xwb'):
            bank = bank_name_of(blob)
            pcm = read_wav(a.wav, a.rate, channels) if a.wav \
                  else build_tone(a.rate, channels, a.seconds)
            blob = build_pcm_wavebank(bank, pcm, channels, a.rate)
            print("  wave bank '%s': %d ch, %d Hz, blockAlign %d, %d frames, %d bytes PCM"
                  % (bank, channels, a.rate, channels * 2,
                     len(pcm) // (channels * 2), len(pcm)))
        else:
            print("  kept %s unchanged (%d bytes)" % (name, len(blob)))
        rebuilt.append((name, blob))

    out = build_fpac(rebuilt, flag, str_size)
    open(a.output, 'wb').write(out)
    print("  wrote %s (%d bytes)" % (a.output, len(out)))


if __name__ == '__main__':
    main()
