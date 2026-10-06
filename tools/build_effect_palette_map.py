#!/usr/bin/env python3
"""Work out which palette file every effect sprite is drawn with, and pack that map.

A BBCF palette is eight files of 256 colours: file 0 is the character, 1-7 are what the
game draws that character's effects with. The palette editor previews effect files on
the effect sprites themselves, which needs to know which sprite uses which file. The
game decides that at runtime, but from data that is all on disk:

  * bbscript 4061 setPalette(n) sets the object's palette file (entity+0x354) - BBCF.exe
    0x0059B030, reached through OBJ_CBase vtable+0x5AC. Objects spawn with file 0
    (0x0057F816 resets it) and never inherit it from their parent.
  * bbscript 30012 sets the same field through 0x0059B050 but also the +0x356 flag, which
    makes the renderer use file 7 of the colour n slots further on (0x0057E157). That is
    not the palette being edited, so those objects are left out.
  * The renderer (0x005A6C80) then asks each .jonbin chunk for an override
    (AA_CCollision_JON vtable+0x54 -> chunk dword +0x48): non-zero means file = value-1
    for that one image. That is how player sprites carry effect images.

So: walk every state of the character's scripts, track the palette file an object holds
when it shows each sprite, open that sprite's .jonbin, and record the file for each
image it draws. setPalette inside if/ifNot/else/upon blocks adds an alternative rather
than replacing, which is why a sprite can map to more than one file. Cross-checked on
the images whose embedded palette exactly equals one of Color 01's files: 1451 agree,
61 disagree (mostly afterimage states, which rewrite palettes with opcodes this does
not model).

Only the map is embedded; the mod reads the sprites from the player's own game files.

Needs the bbscript command database for command sizes - bbcf.ron from
https://github.com/super-continent/bbscript (static_db/bbcf.ron).

Usage:
    python tools/build_effect_palette_map.py "<BBCF install dir>" <bbcf.ron> [-o resource/effect_palette_map.bin]
"""

import argparse
import os
import re
import struct
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from build_palette_thumbnails import CHAR_TAGS, unwrap, fpac_entries  # noqa: E402

MAGIC = b"BBEM"
VERSION = 2  # 1 had no render state
PACK_IMG = 0
PACK_VRI = 1

# bbscript control flow the walk needs by id.
CMD_STATE = 0
CMD_SPRITE = 2
CMD_IF = 4
CMD_END_IF = 5
CMD_SUBROUTINE = 8
CMD_END_SUBROUTINE = 9
CMD_CALL_SUBROUTINE = 10
CMD_UPON = 15
CMD_END_UPON = 16
CMD_IF_NOT = 54
CMD_END_IF_NOT = 55
CMD_ELSE = 56
CMD_END_ELSE = 57
CMD_SET_PALETTE = 4061
CMD_SET_PALETTE_ALT = 30012
# How the object is drawn (BBCF.exe, OBJ_CBase vtable 0x9509E4; renderer 0x005A6C80):
#   3031-3035 set the blend mode, bits 16-18 of +0x150: 0 opaque, 1 alpha, 2 additive,
#             3 premultiplied, 4 subtractive (switch at 0x447AC7).
#   3040      +0x1258, palette recipe: 1 gradient dissolve, 2 three-colour fade. Either way
#             the drawn colours are rebuilt from palette entries 3041-3043 over the sprite's
#             index (0x452DE0), so what the sprite's own entries hold does not matter.
#   3041-3048 the recipe's parameters (+0x125C..+0x1278).
CMD_BLEND_FIRST = 3031
CMD_BLEND_LAST = 3035
CMD_RECIPE = 3040
CMD_RECIPE_PARAM_FIRST = 3041
CMD_RECIPE_PARAM_LAST = 3048
# Hand-picked previews for files the walk gets technically right but unhelpfully: the
# file IS drawn on those sprites, yet a player editing it recognises it by one object.
# (tag, file) -> regex an image must match to stay on that file's sheet.
#   mi 6: the robe from Izanami's intro. Her body sprites are drawn with file 6 too, but
#         a sheet of thirty copies of her buried the robe the file is actually for.
PREVIEW_ONLY = {
    ("mi", 6): r"mi600_\d+y$",
}
OPENERS = {CMD_IF, CMD_IF_NOT, CMD_ELSE, CMD_UPON}
CLOSERS = {CMD_END_IF, CMD_END_IF_NOT, CMD_END_ELSE, CMD_END_UPON}


def load_command_sizes(ron_path):
    with open(ron_path, "r", encoding="utf-8") as handle:
        ron = handle.read()
    sizes = {}
    for match in re.finditer(r"\n\s+(\d+): \(\s*\n\s+size: (\d+),", ron):
        sizes[int(match.group(1))] = int(match.group(2))
    if not sizes:
        sys.exit("%s does not look like the bbscript command database" % ron_path)
    return sizes


def parse_script(blob, sizes):
    """bbscript -> [(command id, argument bytes)]."""
    count = struct.unpack_from("<I", blob, 0)[0]
    pos = 4 + count * 36
    commands = []
    while pos + 4 <= len(blob):
        cid = struct.unpack_from("<I", blob, pos)[0]
        if cid not in sizes:
            raise ValueError("unknown bbscript command %d at 0x%x" % (cid, pos))
        size = sizes[cid]
        commands.append((cid, blob[pos + 4:pos + size]))
        pos += size
    return commands


def c_string(data):
    return data.split(b"\0")[0].decode("ascii", "replace")


def first_int(data):
    return struct.unpack_from("<i", data, 0)[0]


def parse_jonbin(blob):
    """-> (image names, [(image index, palette override)]) per chunk."""
    if blob[:4] != b"JONB":
        return [], []
    count = struct.unpack_from("<H", blob, 4)[0]
    pos = 6
    images = [c_string(blob[pos + i * 32:pos + i * 32 + 32]) for i in range(count)]
    pos += count * 32
    chunk_count = struct.unpack_from("<I", blob, pos + 3)[0]
    pos += 93
    chunks = []
    for i in range(chunk_count):
        fields = struct.unpack_from("<8f12I", blob, pos + i * 0x50)
        chunks.append((fields[15], fields[18]))
    return images, chunks


def collect_subroutines(scripts):
    subs = {}
    for commands in scripts:
        current = None
        for cid, args in commands:
            if cid == CMD_SUBROUTINE:
                current = c_string(args[:32])
                subs[current] = []
            elif cid == CMD_END_SUBROUTINE:
                current = None
            elif current is not None:
                subs[current].append((cid, args))
    return subs


def new_render_state():
    return {"blend": 0, "recipe": 0, "params": [0] * 8}


def walk(commands, subs, files, render, depth=0):
    """Yield (sprite name, set of palette files, render state) through a state's commands.

    `files` is the set the object may hold, updated in place. An unconditional setPalette
    replaces it; a conditional one (inside if/ifNot/else, or upon other than IMMEDIATE,
    which runs on state entry) adds to it. `render` is how the object is drawn; for it the
    last write wins, conditional or not - a preview can only show one way. Subroutines
    are followed."""
    stack = []
    for cid, args in commands:
        if cid in OPENERS:
            stack.append(cid == CMD_UPON and first_int(args) == 0)
        elif cid in CLOSERS:
            if stack:
                stack.pop()
        elif cid in (CMD_SET_PALETTE, CMD_SET_PALETTE_ALT):
            # 30012 is not "file n": it sets the flag at +0x356, and the renderer then asks for
            # file n*8+7 of the owner's colour - file 7 of a colour n slots further on
            # (0x0057E157). That slot is not the palette being edited (the mod writes the
            # player's colour and one neighbour, and flips between them), so such an object
            # does not follow the palette at all. It is recorded as -1, which maps nowhere.
            value = first_int(args) if cid == CMD_SET_PALETTE else -1
            if all(stack):
                files.clear()
            files.add(value)
        elif CMD_BLEND_FIRST <= cid <= CMD_BLEND_LAST:
            render["blend"] = cid - CMD_BLEND_FIRST
        elif cid == CMD_RECIPE:
            render["recipe"] = first_int(args)
        elif CMD_RECIPE_PARAM_FIRST <= cid <= CMD_RECIPE_PARAM_LAST:
            render["params"][cid - CMD_RECIPE_PARAM_FIRST] = first_int(args)
        elif cid == CMD_CALL_SUBROUTINE and depth < 8:
            body = subs.get(c_string(args[:32]), [])
            if all(stack):
                yield from walk(body, subs, files, render, depth + 1)
            else:
                branch = set(files)
                for item in walk(body, subs, branch, render, depth + 1):
                    yield item
                files |= branch
        elif cid == CMD_SPRITE:
            yield c_string(args[:32]), set(files), {"blend": render["blend"], "recipe": render["recipe"],
                                                    "params": list(render["params"])}


def map_character(char_dir, tag, sizes):
    def load(kind):
        with open(os.path.join(char_dir, "char_%s_%s.pac" % (tag, kind)), "rb") as handle:
            return list(fpac_entries(unwrap(handle.read())))

    jonbins = {name[:-7]: blob for name, blob in load("col") if name.endswith(".jonbin")}
    packs = {}
    for kind, pack in (("img", PACK_IMG), ("vri", PACK_VRI)):
        for name, _ in load(kind):
            if name.endswith(".hip"):
                packs.setdefault(name[:-4], pack)

    scripts = [parse_script(blob, sizes) for _, blob in load("scr")]
    subs = collect_subroutines(scripts)

    usage = defaultdict(set)
    render_of = {}
    for commands in scripts:
        body = None
        states = []
        for cid, args in commands:
            if cid == CMD_STATE:
                body = []
                states.append(body)
            elif cid == CMD_SUBROUTINE:
                body = None
            elif body is not None:
                body.append((cid, args))
        for body in states:
            for sprite, files, render in walk(body, subs, {0}, new_render_state()):
                jonbin = jonbins.get(sprite)
                if jonbin is None:
                    continue
                images, chunks = parse_jonbin(jonbin)
                for image_index, override in chunks:
                    if not images:
                        continue
                    image = images[image_index] if image_index < len(images) else images[0]
                    image = image.rsplit(".", 1)[0]
                    drawn_with = {override - 1} if override else files
                    usage[image] |= drawn_with
                    # The first time it is drawn with an effect file is how the preview shows it.
                    if image not in render_of and any(1 <= f <= 7 for f in drawn_with):
                        render_of[image] = render

    entries = []
    for image in sorted(usage):
        if image not in packs:
            continue
        mask = 0
        for f in usage[image]:
            if 1 <= f <= 7:  # file 0 is the character's own colours, already on the sheet
                keep = PREVIEW_ONLY.get((tag, f))
                if keep is None or re.match(keep, image):
                    mask |= 1 << f
        if mask:
            entries.append((packs[image], mask, image, render_of.get(image, new_render_state())))
    return entries


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("install_dir", help="BBCF install folder (the one holding BBCF.exe)")
    parser.add_argument("ron", help="bbcf.ron from super-continent/bbscript")
    parser.add_argument("-o", "--output", default="resource/effect_palette_map.bin")
    args = parser.parse_args()

    char_dir = os.path.join(args.install_dir, "data", "Char")
    sizes = load_command_sizes(args.ron)

    tables = []
    for char_index, tag in enumerate(CHAR_TAGS):
        entries = map_character(char_dir, tag, sizes)
        per_file = [sum(1 for e in entries if e[1] & (1 << f)) for f in range(1, 8)]
        print("  %02d %-3s %4d effect images  per file 1-7: %s" % (char_index, tag, len(entries), per_file))
        tables.append(entries)

    # "BBEM", u32 version, u32 count, count * { u32 offset, u32 entries },
    # then per entry: u8 pack (0 img, 1 vri), u8 file mask (bit n = file n), u8 name length, name,
    # u8 blend mode, u8 palette recipe, and when the recipe is not 0: 8 * i32 for 3041-3048
    header_size = 12 + len(tables) * 8
    body = bytearray()
    table = bytearray()
    for entries in tables:
        table += struct.pack("<II", header_size + len(body), len(entries))
        for pack, mask, name, render in entries:
            encoded = name.encode("ascii")
            body += struct.pack("<BBB", pack, mask, len(encoded)) + encoded
            recipe = render["recipe"] if render["recipe"] in (1, 2) else 0
            body += struct.pack("<BB", render["blend"], recipe)
            if recipe:
                body += struct.pack("<8i", *render["params"])
    blob = MAGIC + struct.pack("<II", VERSION, len(tables)) + table + body

    with open(args.output, "wb") as handle:
        handle.write(blob)
    print("\n%s: %d characters, %.0f KB" % (args.output, len(tables), len(blob) / 1024.0))


if __name__ == "__main__":
    main()
