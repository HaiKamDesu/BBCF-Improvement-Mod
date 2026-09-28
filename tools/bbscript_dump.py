"""Offline dump of a character's compiled script states from the shipped char_XX_scr.pac.

    python3 tools/bbscript_dump.py <repo root> <tags> <state names>
    python3 tools/bbscript_dump.py . rg,tg CmnActFDown2Stand,CmnActBDown2Stand

Command sizes come from the mod's own src/Game/Scr/CmdList.h and command names from
docs/Research/Tadatys-BBCF-Ghidra/BBCF.h, so it decodes exactly what the in-game parser
does. Written for docs/Research/TriggerTimingInvestigation.md; nothing at runtime uses it.
Set BBCF_DIR if the game is not at the default Steam path.
"""
import zlib, struct, os, re, sys

REPO = sys.argv[1] if len(sys.argv) > 1 else '.'
base = os.path.join(os.environ.get('BBCF_DIR', '/mnt/d/SteamLibrary/steamapps/common/BlazBlue Centralfiction'), 'data', 'Char') + os.sep

# Command sizes from the mod's CmdList.h (total size including the 4-byte id).
src = open(os.path.join(REPO, 'src/Game/Scr/CmdList.h')).read()
src = re.sub(r'//.*', '', src)
src = re.sub(r'/\*.*?\*/', '', src, flags=re.S)
sizes = {}
for m in re.finditer(r'size_(\d+)\s*\{([^}]*)\}', src):
    n = int(m.group(1))
    for t in re.findall(r'\d+', m.group(2)):
        sizes[int(t)] = n
# The ones the parser handles by hand.
for cid, n in {0: 36, 1: 4, 2: 40, 4000: 40, 22007: 8, 22019: 24, 2002: 4, 23027: 4, 9003: 8, 9001: 8,
               9002: 8, 9154: 8, 11000: 8, 9274: 8, 9286: 8, 11036: 8, 11035: 8, 11037: 8, 14068: 36,
               14069: 36, 11088: 8, 12051: 8, 11028: 8}.items():
    sizes.setdefault(cid, n)

names = {}
for m in re.finditer(r'bbcmd_(\d+)_([A-Za-z0-9_]+)\[', open(os.path.join(REPO, 'docs/Research/Tadatys-BBCF-Ghidra/BBCF.h')).read()):
    names.setdefault(int(m.group(1)), m.group(2))


def fpac(d):
    dataStart, _, count = struct.unpack_from('<III', d, 4)
    nameLen = struct.unpack_from('<I', d, 0x14)[0]
    stride = (dataStart - 0x20) // count
    out = {}
    for i in range(count):
        e = 0x20 + i * stride
        name = d[e:e + nameLen].split(b'\0')[0].decode()
        idx, off, size = struct.unpack_from('<III', d, e + nameLen)
        out[name] = d[dataStart + off:dataStart + off + size]
    return out


def load(p):
    d = open(p, 'rb').read()
    if d[:8] == b'DFASFPAC':
        u, c = struct.unpack_from('<II', d, 8)
        d = zlib.decompress(d[16:16 + c])
    return fpac(d)


def states(bin):
    n = struct.unpack_from('<I', bin, 0)[0]
    idx = []
    for i in range(n):
        e = 4 + i * 36
        nm = bin[e:e + 32].split(b'\0')[0].decode(errors='replace')
        off = struct.unpack_from('<I', bin, e + 32)[0]
        idx.append((nm, off))
    body = 4 + n * 36
    return idx, body


def s32(b):
    return b.split(b'\0')[0].decode(errors='replace')


def decode(bin, start, end):
    out = []
    o = start
    while o < end:
        cid = struct.unpack_from('<I', bin, o)[0]
        sz = sizes.get(cid)
        if sz is None:
            out.append((o, cid, 'UNKNOWN', None))
            break
        args = bin[o + 4:o + sz]
        nm = names.get(cid, f'cmd{cid}')
        head = args[:32].split(b'\0')[0]
        if cid == 2:
            rep = (s32(args[:32]), struct.unpack_from('<i', args, 32)[0])
        elif sz - 4 >= 32 and head and all(32 <= c < 127 for c in head):
            rep = (s32(args[:32]),) + struct.unpack_from('<%di' % ((sz - 36) // 4), args, 32)
        else:
            rep = struct.unpack_from('<%di' % ((sz - 4) // 4), args, 0)
        out.append((o, cid, nm, rep))
        o += sz
        if cid == 1:
            break
    return out, o


def dump(tag, want, quiet=False):
    f = load(base + f'char_{tag}_scr.pac')
    bin = f[f'scr_{tag}.bin']
    idx, body = states(bin)
    offs = {nm: body + off for nm, off in idx}
    order = sorted(offs.values())
    res = {}
    for w in want:
        if w not in offs:
            if not quiet:
                print(f'## {tag} {w}: MISSING')
            res[w] = None
            continue
        st = offs[w]
        nxt = [x for x in order if x > st]
        en = nxt[0] if nxt else len(bin)
        cmds, o = decode(bin, st, en)
        res[w] = cmds
        if quiet:
            continue
        print(f'## {tag} {w} @0x{st:x} ({en - st} bytes, ended at 0x{o:x} {"OK" if o == en else "MISMATCH"})')
        depth = 0
        for off, cid, nm, rep in cmds:
            if cid in (5, 6, 16):
                depth = max(0, depth - 1)
            print('  ' * depth + f'{nm}({",".join(map(str, rep)) if rep else ""})')
            if cid in (4, 15, 6):
                depth += 1
    return res


if __name__ == '__main__':
    tags = sys.argv[2].split(',') if len(sys.argv) > 2 else ['rg']
    want = sys.argv[3].split(',') if len(sys.argv) > 3 else ['CmnActFDown2Stand']
    for tag in tags:
        dump(tag, want)
