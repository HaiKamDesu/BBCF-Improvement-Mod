#!/usr/bin/env python3
"""Walk every character script (data/Char/char_XX_scr.pac) the way BBCF's interpreter does and
list the commands you ask for, per state, with the frame each one runs on.

It reads only files from your own install. The command sizes come from the game's own table in
BBCF.exe (0x951D88: 1050 sorted (opcode, total size) pairs, looked up by FUN_0058B250), so the walk
never desyncs on a command the mod does not know. Verified against Steam build 11073075: all 26,774
states of the 36 playable characters walk without an error (the char_boss_* files are an older
format and are skipped).

Examples
    # every invul command, every character
    python3 tools/bbcf_scr_scan.py "D:/SteamLibrary/steamapps/common/BlazBlue Centralfiction" --ops 22006-22037
    # what Nu's backdash does
    python3 tools/bbcf_scr_scan.py <install> --chars ny --states CmnActBDash --all-ops
    # which states use the invul timer (22008)
    python3 tools/bbcf_scr_scan.py <install> --ops 22008

Character tags: rg jn no rc tk tg lc ar bn ca ha ny tb hz mu mk vh pt rl iz am bl az kg kk tm ce
rm hb ph nt mi su es ma jb (Nu=ny, Rachel=rc, Relius=rl, Lambda=rm, Nine=ph, Izanami=mi).
Opcode meanings for the invul family: docs/Decompilation/Invulnerability.md.
"""
import argparse
import glob
import os
import struct
import sys
import zlib

SIZE_TABLE_VA = 0x951D88
SIZE_TABLE_COUNT = 0x41A
EXPECTED_SHA256 = "9a94ac4003cc8dc77226cadc7496a0eb112f69ccc92e50ded27262ed80551f25"

INVUL_NAMES = {
    22006: "invulBit01", 22007: "setInvincible(0x02)", 22008: "invulTimer(0x994)", 22009: "invulOn(0x04)",
    22019: "invulAttributes(head,body,foot,projectile,throw)", 22020: "guardPoint(0x99C)",
    22021: "set0x9A0", 22022: "invulBit80", 22023: "invulBit100", 22024: "set0x9A8",
    22025: "guardPointValues(0x9AC..)", 22026: "invulBurst(0x200)", 22027: "invulBit400",
    22028: "invulBit800", 22029: "invulBit1000", 22030: "invulBit2000", 22031: "set0x9B8/0x9BC",
    22032: "invulBit4000", 22033: "invulBit8000", 22034: "invulBit10000", 22035: "set0x9C0",
    22036: "invulBit20000", 22037: "invulBit40000", 2: "sprite", 1: "endState",
}


def read_size_table(exe_path):
    exe = open(exe_path, "rb").read()
    pe = struct.unpack_from("<I", exe, 0x3C)[0]
    nsec = struct.unpack_from("<H", exe, pe + 6)[0]
    opt = struct.unpack_from("<H", exe, pe + 20)[0]
    base = struct.unpack_from("<I", exe, pe + 24 + 28)[0]
    rva = SIZE_TABLE_VA - base
    for i in range(nsec):
        o = pe + 24 + opt + 40 * i
        _vsize, va, rsize, raw = struct.unpack_from("<IIII", exe, o + 8)
        if va <= rva < va + rsize:
            off = raw + rva - va
            return {op: size for op, size in (struct.unpack_from("<II", exe, off + 8 * k) for k in range(SIZE_TABLE_COUNT))}
    sys.exit("size table not found: is this BBCF.exe build 11073075?")


def unpac(data):
    if data[:8] == b"DFASFPAC":
        comp = struct.unpack_from("<I", data, 0x0C)[0]
        data = zlib.decompress(data[16:16 + comp])
    if data[:4] != b"FPAC":
        return {}
    data_start, = struct.unpack_from("<I", data, 4)
    count, = struct.unpack_from("<I", data, 0x0C)
    name_len, = struct.unpack_from("<I", data, 0x14)
    stride = (data_start - 0x20) // count
    out = {}
    for i in range(count):
        e = 0x20 + i * stride
        name = data[e:e + name_len].split(b"\0")[0].decode("latin1")
        _idx, off, size = struct.unpack_from("<III", data, e + name_len)
        out[name] = data[data_start + off:data_start + off + size]
    return out


def walk_states(blob, sizes):
    """Yields (state name, [(frame, opcode, args)]) for one scr_*.bin."""
    count, = struct.unpack_from("<I", blob, 0)
    body0 = 4 + count * 36
    for i in range(count):
        name = blob[4 + i * 36:4 + i * 36 + 32].split(b"\0")[0].decode("latin1")
        off, = struct.unpack_from("<I", blob, 4 + i * 36 + 32)
        p = body0 + off
        frame = 0        # frames played before the current sprite started
        next_frame = 0   # frame the next sprite will start on
        cmds = []
        ok = True
        while p + 4 <= len(blob):
            op, = struct.unpack_from("<I", blob, p)
            size = sizes.get(op)
            if not size:
                ok = False
                break
            raw = blob[p + 4:p + size]
            if op == 2:
                # sprite(name[32], frames). The interpreter keeps running the commands after a sprite
                # on that sprite's first frame and only waits when it reaches the next sprite, so
                # everything up to the next sprite is reported at this sprite's start frame.
                frame = next_frame
                dur, = struct.unpack_from("<i", raw, 32)
                next_frame += 1 if dur >= 32767 or dur < 0 else dur
            cmds.append((frame, op, raw))
            if op == 1:
                break
            p += size
        yield name, cmds, ok


def fmt_args(op, raw):
    if op == 2:
        return "%s %df" % (raw[:32].split(b"\0")[0].decode("latin1"), struct.unpack_from("<i", raw, 32)[0])
    if len(raw) % 4 == 0 and len(raw) <= 64:
        return ",".join(str(v) for v in struct.unpack_from("<%di" % (len(raw) // 4), raw))
    return raw.hex()


def parse_ops(text):
    ops = set()
    for part in text.split(","):
        if "-" in part:
            a, b = part.split("-")
            ops.update(range(int(a), int(b) + 1))
        elif part:
            ops.add(int(part))
    return ops


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("install", help="BBCF install folder (the one with BBCF.exe and data/)")
    ap.add_argument("--ops", default="22006-22037", help="opcodes to list, e.g. 22008 or 22006-22037")
    ap.add_argument("--all-ops", action="store_true", help="list every command (use with --states)")
    ap.add_argument("--chars", default="", help="comma-separated tags, e.g. ny,rc (default: all)")
    ap.add_argument("--states", default="", help="only states whose name contains one of these (comma-separated)")
    args = ap.parse_args()

    exe = os.path.join(args.install, "BBCF.exe")
    import hashlib
    if hashlib.sha256(open(exe, "rb").read()).hexdigest() != EXPECTED_SHA256:
        print("warning: BBCF.exe is not Steam build 11073075; the size table address may be wrong", file=sys.stderr)
    sizes = read_size_table(exe)
    ops = parse_ops(args.ops)
    chars = [c for c in args.chars.split(",") if c]
    states = [s.lower() for s in args.states.split(",") if s]
    errors = 0
    for path in sorted(glob.glob(os.path.join(args.install, "data", "Char", "char_*_scr.pac"))):
        tag = os.path.basename(path)[5:-8]
        if tag.startswith("boss_") or (chars and tag not in chars):
            continue
        for fname, blob in unpac(open(path, "rb").read()).items():
            if not fname.endswith(".bin"):
                continue
            for name, cmds, ok in walk_states(blob, sizes):
                errors += not ok
                if states and not any(s in name.lower() for s in states):
                    continue
                for frame, op, raw in cmds:
                    if args.all_ops or op in ops:
                        print("%-4s %-14s %-32s f%-4d %5d %-24s %s" % (
                            tag, fname, name, frame, op, INVUL_NAMES.get(op, ""), fmt_args(op, raw)))
    if errors:
        print("warning: %d states stopped on an opcode missing from the size table" % errors, file=sys.stderr)


if __name__ == "__main__":
    main()
