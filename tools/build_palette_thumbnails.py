#!/usr/bin/env python3
"""Pack one idle sprite per character into the thumbnail blob the mod embeds.

These back the palette grid: every palette of a character shares the same sprite
and differs only in the 256 colours applied to it, so one set of indices per
character is all the grid ever needs.

The sprite is `<prefix>000_00.hip` from data/Char/char_<tag>_img.pac - animation
0, frame 0, which is the neutral standing pose for all 36 characters. It is
cropped to its bounding box and scaled down in INDEX space, which is why the
downscale takes the most common index in each source block rather than averaging:
averaging palette indices is meaningless, and the whole point is to keep the
image recolourable.

Requires a BBCF install to read the sprites from. Output is checked in, so this
only needs re-running if the thumbnail geometry changes.

Usage:
    python tools/build_palette_thumbnails.py "<BBCF install dir>" [-o resource/palette_thumbnails.bin]
"""

import argparse
import os
import re
import struct
import sys
import zlib
from collections import Counter

CHARACTER_COUNT = 36
MAGIC = b"BBPB"
VERSION = 1
# Every thumbnail is this exact size, with the character scaled to the same height and
# stood on the same baseline. A grid is for comparing colours, so consistent framing
# beats faithful relative scale: fitting each sprite to its own bounding box instead
# renders Mai at a third the size of everyone else, because her idle pose holds a spear
# out horizontally and the box is mostly weapon.
#
# Stored a little larger than they are drawn so the GPU downsamples smoothly. This also
# caps what one cached thumbnail costs as RGBA at runtime: 160*200*4 = 125 KB.
BOX_WIDTH = 160
BOX_HEIGHT = 200
# Height the character itself is scaled to, leaving a little air top and bottom.
FIGURE_HEIGHT = 188

# Asset tag per CharIndex. Derived by byte-matching the mod's own default palette
# templates against each shipped char_XX_pal.pac; see src/Palette/PaletteSheet.cpp.
CHAR_TAGS = [
    "rg", "jn", "no", "rc", "tk", "tg", "lc", "ar", "bn", "ca", "ha", "ny",
    "tb", "hz", "mu", "mk", "vh", "pt", "rl", "iz", "am", "bl", "az", "kg",
    "kk", "tm", "ce", "rm", "hb", "ph", "nt", "mi", "su", "es", "ma", "jb",
]


def unwrap(data):
    """Shipped .pac files are a DFASFPAC header wrapping a zlib stream."""
    if data[:8] == b"DFASFPAC":
        compressed_size = struct.unpack_from("<I", data, 12)[0]
        return zlib.decompress(data[16:16 + compressed_size])
    return data


def fpac_entries(data):
    data_start = struct.unpack_from("<I", data, 4)[0]
    count = struct.unpack_from("<I", data, 0x0C)[0]
    name_len = struct.unpack_from("<I", data, 0x14)[0]
    stride = (data_start - 0x20) // count
    for i in range(count):
        record = 0x20 + i * stride
        name = data[record:record + name_len].split(b"\0")[0].decode("ascii", "replace")
        _, offset, size = struct.unpack_from("<III", data, record + name_len)
        yield name, bytes(data[data_start + offset:data_start + offset + size])


def decode_hip(blob):
    """Indexed HIP -> (width, height, index bytes)."""
    palette_count = struct.unpack_from("<I", blob, 0x0C)[0]
    extra_size = struct.unpack_from("<I", blob, 0x1C)[0]
    width, height = struct.unpack_from("<II", blob, 0x10)
    offset = 0x20
    if extra_size >= 0x10:
        width, height = struct.unpack_from("<II", blob, 0x20)
        offset = 0x20 + extra_size
    offset += palette_count * 4

    pixels = bytearray()
    total = width * height
    while len(pixels) < total and offset + 1 < len(blob):
        index, run = blob[offset], blob[offset + 1]
        offset += 2
        if run:  # zero-length runs do occur in shipped sprites
            pixels += bytes([index]) * run
    if len(pixels) != total:
        raise ValueError("sprite decoded to %d of %d pixels" % (len(pixels), total))
    return width, height, bytes(pixels)


def crop_to_content(width, height, pixels):
    """Bounding box of everything that is not index 0 (the transparency slot)."""
    rows = [y for y in range(height) if any(pixels[y * width:(y + 1) * width])]
    if not rows:
        raise ValueError("sprite is entirely transparent")
    top, bottom = rows[0], rows[-1]
    columns = [x for x in range(width)
               if any(pixels[y * width + x] for y in range(top, bottom + 1))]
    left, right = columns[0], columns[-1]
    return left, top, right - left + 1, bottom - top + 1


def body_centre_x(pixels, width, left, top, crop_w, crop_h):
    """Horizontal centre of mass of the opaque pixels.

    Not the bounding box centre: a held weapon is thin but wide and drags the box
    centre off the character, which then frames the figure off to one side. The body
    is far denser than any weapon, so the centroid sits on it.
    """
    total = 0
    weighted = 0
    for y in range(top, top + crop_h):
        row = y * width
        for x in range(left, left + crop_w):
            if pixels[row + x]:
                total += 1
                weighted += x
    return (weighted // total) if total else (left + crop_w // 2)


def render_thumbnail(pixels, width, left, top, crop_w, crop_h):
    """Scale the figure to FIGURE_HEIGHT and stamp it into a fixed canvas.

    Sampling is modal - the most common index in each source block wins, and index 0
    never wins - because these are palette indices, not colours: averaging them is
    meaningless, and keeping them is what lets one sprite serve every palette.
    Anything that overflows the canvas horizontally (a held weapon) is clipped, which
    keeps every character the same size and standing on the same line.
    """
    scale = FIGURE_HEIGHT / float(crop_h)
    out = bytearray(BOX_WIDTH * BOX_HEIGHT)

    baseline = BOX_HEIGHT - (BOX_HEIGHT - FIGURE_HEIGHT) // 2  # feet sit here
    centre = body_centre_x(pixels, width, left, top, crop_w, crop_h)

    for ty in range(BOX_HEIGHT):
        # Canvas row -> source row, measured up from the baseline.
        src_y = top + crop_h + int((ty - baseline) / scale)
        sy0 = src_y
        sy1 = max(sy0 + 1, top + crop_h + int((ty + 1 - baseline) / scale))
        if sy1 <= top or sy0 >= top + crop_h:
            continue
        sy0 = max(sy0, top)
        sy1 = min(sy1, top + crop_h)

        for tx in range(BOX_WIDTH):
            src_x = centre + int((tx - BOX_WIDTH / 2.0) / scale)
            sx0 = src_x
            sx1 = max(sx0 + 1, centre + int((tx + 1 - BOX_WIDTH / 2.0) / scale))
            if sx1 <= left or sx0 >= left + crop_w:
                continue
            sx0 = max(sx0, left)
            sx1 = min(sx1, left + crop_w)

            counts = Counter()
            for y in range(sy0, sy1):
                row = y * width
                for x in range(sx0, sx1):
                    value = pixels[row + x]
                    if value:
                        counts[value] += 1
            if counts:
                out[ty * BOX_WIDTH + tx] = counts.most_common(1)[0][0]

    return bytes(out)


def build_thumbnail(char_dir, tag):
    archive = os.path.join(char_dir, "char_%s_img.pac" % tag)
    with open(archive, "rb") as handle:
        data = unwrap(handle.read())

    sprites = dict(fpac_entries(data))

    # Lambda's archive is tagged "rm" but holds ny* sprites (she shares Nu's body), so
    # take the archive's dominant prefix rather than assuming it matches the tag.
    prefixes = Counter()
    for name in sprites:
        match = re.match(r"^([a-z]+)\d", name)
        if match:
            prefixes[match.group(1)] += 1
    prefix = prefixes.most_common(1)[0][0]

    idle_name = "%s000_00.hip" % prefix
    if idle_name not in sprites:
        raise ValueError("%s has no %s" % (archive, idle_name))

    width, height, pixels = decode_hip(sprites[idle_name])
    left, top, crop_w, crop_h = crop_to_content(width, height, pixels)

    thumb = render_thumbnail(pixels, width, left, top, crop_w, crop_h)
    return idle_name, BOX_WIDTH, BOX_HEIGHT, thumb


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("install_dir", help="BBCF install folder (the one holding BBCF.exe)")
    parser.add_argument("-o", "--output", default="resource/palette_thumbnails.bin")
    args = parser.parse_args()

    char_dir = os.path.join(args.install_dir, "data", "Char")
    if not os.path.isdir(char_dir):
        sys.exit("no data/Char under %s" % args.install_dir)

    entries = []
    for char_index, tag in enumerate(CHAR_TAGS):
        name, width, height, thumb = build_thumbnail(char_dir, tag)
        entries.append((width, height, zlib.compress(thumb, 9)))
        print("  %02d %-3s %-14s %3dx%-3d ->%6d bytes" %
              (char_index, tag, name, width, height, len(entries[-1][2])))

    header_size = 4 + 4 * 2 + CHARACTER_COUNT * 16
    blob = bytearray()
    blob += MAGIC
    blob += struct.pack("<II", VERSION, CHARACTER_COUNT)

    offset = header_size
    for width, height, payload in entries:
        blob += struct.pack("<IIII", width, height, offset, len(payload))
        offset += len(payload)
    for _, _, payload in entries:
        blob += payload

    assert len(blob) == offset, "table and payload disagree"

    output_dir = os.path.dirname(args.output)
    if output_dir:
        os.makedirs(output_dir, exist_ok=True)
    with open(args.output, "wb") as handle:
        handle.write(blob)

    print("\n%s: %d thumbnails, %.0f KB" %
          (args.output, CHARACTER_COUNT, len(blob) / 1024.0))


if __name__ == "__main__":
    main()
