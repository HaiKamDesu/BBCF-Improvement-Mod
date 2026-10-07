#!/usr/bin/env python3
"""How far the BBCF decompilation has got. See docs/Decompilation/README.md.

  overall    every function in the binary (from the private decompile export), how many the
             repo references with meaning (src/ and docs/, not raw Ghidra report dumps)
  subsystems each docs/Decompilation/<Subsystem>.md table: functions per level L0-L3, and
             how much of the subsystem's code that is

Usage: python3 tools/decomp_progress.py [--export C:/BBCF_Archive/build_11073075/decomp]
"""

import argparse
import glob
import os
import re

DEFAULT_EXPORT = "/mnt/c/BBCF_Archive/build_11073075/decomp"
IMAGE_BASE = 0x400000


def load_functions(export_dir):
    path = os.path.join(export_dir, "functions.tsv")
    functions = {}
    with open(path, encoding="utf-8") as handle:
        next(handle)
        for line in handle:
            cols = line.rstrip("\n").split("\t")
            functions[int(cols[0], 16)] = (cols[1], int(cols[2]))
    return functions


def referenced_addresses(repo):
    found = set()
    files = (glob.glob(os.path.join(repo, "src/**/*.h"), recursive=True)
             + glob.glob(os.path.join(repo, "src/**/*.cpp"), recursive=True)
             + glob.glob(os.path.join(repo, "docs/**/*.md"), recursive=True)
             + [os.path.join(repo, "notes.h")])
    for path in files:
        try:
            text = open(path, encoding="utf-8", errors="ignore").read()
        except OSError:
            continue
        for value in re.findall(r"(?:FUN|LAB)_(00[0-9A-Fa-f]{6})", text):
            found.add(int(value, 16))
        for value in re.findall(r"\b0x(00[4-8][0-9A-Fa-f]{5})\b", text):
            found.add(int(value, 16))
        for value in re.findall(r"uintptr_t\s+ADDR_\w+\s*=\s*0x([0-9A-Fa-f]+)", text):
            number = int(value, 16)
            found.add(number if number >= IMAGE_BASE else number + IMAGE_BASE)
    return found


ROW = re.compile(r"^\|\s*0x([0-9A-Fa-f]{6,8})\s*\|[^|]*\|[^|]*\|\s*(L[0-3])\s*\|")


def subsystem_tables(repo):
    for path in sorted(glob.glob(os.path.join(repo, "docs/Decompilation/*.md"))):
        if os.path.basename(path) == "README.md":
            continue
        rows = []
        for line in open(path, encoding="utf-8"):
            match = ROW.match(line)
            if match:
                rows.append((int(match.group(1), 16), match.group(2)))
        yield os.path.splitext(os.path.basename(path))[0], rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--export", default=DEFAULT_EXPORT)
    args = parser.parse_args()
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    functions = load_functions(args.export)
    total_bytes = sum(size for _, size in functions.values())
    named = sum(1 for name, _ in functions.values() if not name.startswith("FUN_"))
    refs = [a for a in referenced_addresses(repo) if a in functions]
    ref_bytes = sum(functions[a][1] for a in refs)

    print("overall")
    print("  functions          %6d  (%.2f MB)" % (len(functions), total_bytes / 1e6))
    print("  named in Ghidra    %6d  (mostly library/RTTI auto-names)" % named)
    print("  referenced in repo %6d  %5.1f%% of code" % (len(refs), 100.0 * ref_bytes / total_bytes))

    print("\nsubsystems (docs/Decompilation)")
    for name, rows in subsystem_tables(repo):
        counts = {level: 0 for level in ("L0", "L1", "L2", "L3")}
        size = {level: 0 for level in counts}
        for address, level in rows:
            counts[level] += 1
            size[level] += functions.get(address, ("", 0))[1]
        all_bytes = sum(size.values()) or 1
        print("  %-24s %4d fn   L3 %3d  L2 %3d  L1 %3d  L0 %3d   L2+ %5.1f%% of its code" % (
            name, len(rows), counts["L3"], counts["L2"], counts["L1"], counts["L0"],
            100.0 * (size["L2"] + size["L3"]) / all_bytes))


if __name__ == "__main__":
    main()
