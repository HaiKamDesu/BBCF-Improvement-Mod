#!/usr/bin/env python3
"""Pack HIKARI's palette reference sheets into the blob the mod embeds.

The sheets are the per-character images from HIKARI's CFPL-PNG-CONVERTER
(`cfpl2basepalconverter/images/00.bmp` .. `23.bmp`), which are 8-bit indexed BMPs
laid out so that every palette entry a character actually uses is visible on a
sprite, plus a swatch block for the ones that are not. They are numbered by
CharIndex, in the same order as src/Game/characters.h.

Only the pixel indices are packed. The BMP's own palette is thrown away, because
the mod substitutes the palette being exported - that substitution is the whole
feature. Indices deflate extremely well (43 MB of sheets becomes ~2.4 MB), which
is what makes embedding them in the DLL practical at all.

What is stored is not the raw indices but the finished PNG image stream: the
scanlines already carry their PNG filter byte and are already deflated. Because a
sheet's pixels never change - an export only swaps the palette - the mod can copy
this straight into the IDAT chunk of the file it writes. That means no inflating
and no recompressing at export time, and the PNG that lands on disk is properly
compressed rather than the stored-block stream the mod could otherwise produce
(it has a zlib decompressor available to it, but no compressor).

Usage:
    python tools/build_palette_templates.py <images-dir> [-o resource/palette_templates.bin]

Re-run this only if the sheets themselves change; the output is checked in.
"""

import argparse
import os
import struct
import sys
import zlib

CHARACTER_COUNT = 36
MAGIC = b"BBPT"
VERSION = 2  # 1 stored raw indices; 2 stores a ready-made PNG image stream


def read_indexed_bmp(path):
    """Return (width, height, top-down index bytes) for an 8-bit BMP."""
    with open(path, "rb") as handle:
        data = handle.read()

    if data[:2] != b"BM":
        raise ValueError("%s is not a BMP" % path)

    pixel_offset = struct.unpack_from("<I", data, 0x0A)[0]
    width, height = struct.unpack_from("<ii", data, 0x12)
    bits_per_pixel = struct.unpack_from("<H", data, 0x1C)[0]
    compression = struct.unpack_from("<I", data, 0x1E)[0]

    if bits_per_pixel != 8:
        raise ValueError("%s is %d bpp; only 8-bit indexed is supported" % (path, bits_per_pixel))
    if compression != 0:
        raise ValueError("%s is compressed; only uncompressed BI_RGB is supported" % path)

    bottom_up = height > 0
    height = abs(height)
    stride = ((width * 8 + 31) // 32) * 4  # BMP rows are padded to 4 bytes

    rows = []
    for y in range(height):
        source_row = (height - 1 - y) if bottom_up else y
        start = pixel_offset + source_row * stride
        row = data[start:start + width]
        if len(row) != width:
            raise ValueError("%s is truncated at row %d" % (path, y))
        rows.append(row)

    return width, height, b"".join(rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("images_dir", help="folder holding 00.bmp .. 23.bmp")
    parser.add_argument("-o", "--output", default="resource/palette_templates.bin")
    args = parser.parse_args()

    entries = []
    width = height = None

    for char_index in range(CHARACTER_COUNT):
        path = os.path.join(args.images_dir, "%02X.bmp" % char_index)
        if not os.path.isfile(path):
            sys.exit("missing sheet for character %d (%s)" % (char_index, path))

        sheet_width, sheet_height, pixels = read_indexed_bmp(path)
        if width is None:
            width, height = sheet_width, sheet_height
        elif (sheet_width, sheet_height) != (width, height):
            # One size for all of them keeps the runtime side free of per-entry
            # dimensions and bounds juggling.
            sys.exit("%s is %dx%d, expected %dx%d" %
                     (path, sheet_width, sheet_height, width, height))

        # PNG scanlines: one filter byte (0 = none) then the row's indices. Filtering
        # buys little on flat indexed art and would cost the mod a decoder, so leave it.
        scanlines = bytearray()
        for y in range(sheet_height):
            scanlines.append(0)
            scanlines += pixels[y * sheet_width:(y + 1) * sheet_width]

        entries.append(zlib.compress(bytes(scanlines), 9))
        print("  %02X  %-7d ->%8d bytes" % (char_index, len(pixels), len(entries[-1])))

    header_size = 4 + 4 * 4 + CHARACTER_COUNT * 8
    blob = bytearray()
    blob += MAGIC
    blob += struct.pack("<IIII", VERSION, CHARACTER_COUNT, width, height)

    offset = header_size
    for payload in entries:
        blob += struct.pack("<II", offset, len(payload))
        offset += len(payload)
    for payload in entries:
        blob += payload

    assert len(blob) == offset, "table and payload disagree"

    output_dir = os.path.dirname(args.output)
    if output_dir:
        os.makedirs(output_dir, exist_ok=True)
    with open(args.output, "wb") as handle:
        handle.write(blob)

    print("\n%s: %d sheets, %dx%d, %.2f MB" %
          (args.output, CHARACTER_COUNT, width, height, len(blob) / 1e6))


if __name__ == "__main__":
    main()
