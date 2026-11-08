#!/usr/bin/env python3
"""Exports the game struct layouts the mod uses, exactly as the compiler lays them out.

The offset comments in our headers (`//0x0170`) are hand-written and some are wrong
(CharData::actionTimeNoHitstop says 0x170, it is at 0x168). The layout the mod actually reads
memory with is the compiler's, so this asks MSVC for it (/d1reportAllClassLayout) and takes
only the field types from the headers.

Output: docs/Decompilation/types/structs.json, which ApplyRepoTypes.py turns into Ghidra
structures (tools/decomp/sync_archive.sh runs both). Commit the JSON: collaborators without
MSVC (Linux, Steam Deck) use the committed copy.

Needs Windows MSVC (VS 2022) reachable from WSL through cmd.exe.

Usage: python3 tools/decomp/gen_struct_layouts.py [--check]
  --check   exit 1 if the committed JSON is out of date, without writing it
"""

import json
import os
import re
import subprocess
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
OUT = os.path.join(REPO, "docs", "Decompilation", "types", "structs.json")
VCVARS = r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"

# Headers that describe game memory. Every class/struct/union defined in them is exported.
HEADERS = [
    "src/Game/CharData.h",
    "src/Game/Room/RoomMemberEntry.h",
    "src/Game/Room/Room.h",
    "src/Game/ReplayFiles/ReplayFile.h",
    "src/Game/GhidraDefs.h",
]
# Mod-side views of game objects that duplicate another struct (EntityData is an older copy of
# CharData) or are not game memory.
SKIP = {"EntityData"}

DEF = re.compile(r"\b(class|struct|union)\s+(\w+)\s*(?::[^{;]*)?\{")


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", lambda m: " " * len(m.group(0)), text, flags=re.S)
    return re.sub(r"//[^\n]*", lambda m: " " * len(m.group(0)), text)


ENUM = re.compile(r"\benum\s+(?:class\s+|struct\s+)?(\w+)\s*(?::\s*([\w\s]+?))?\s*\{([^}]*)\}")


def header_enums(path):
    """{enum name: {"base": type, "values": [[name, value]]}}; values follow C rules (previous + 1)."""
    code = strip_comments(open(path, encoding="utf-8", errors="replace").read())
    found = {}
    for match in ENUM.finditer(code):
        values, current = [], -1
        for item in match.group(3).split(","):
            item = item.strip()
            if not item:
                continue
            if "=" in item:
                name, expr = [x.strip() for x in item.split("=", 1)]
                try:
                    current = int(eval(expr, {}, {n: v for n, v in values}))
                except Exception:
                    current += 1
            else:
                name = item
                current += 1
            values.append([name, current])
        found[match.group(1)] = {"base": (match.group(2) or "int").strip(), "values": values,
                                 "header": os.path.relpath(path, REPO).replace("\\", "/")}
    return found


def header_members(path):
    """{type name: {"kind", "members": {field: (type, pointer depth, comment)}}} from one header."""
    raw = open(path, encoding="utf-8", errors="replace").read()
    code = strip_comments(raw)
    raw_lines = raw.splitlines()
    found = {}
    for match in DEF.finditer(code):
        kind, name = match.group(1), match.group(2)
        start = match.end()
        depth, i = 1, start
        while i < len(code) and depth:
            depth += {"{": 1, "}": -1}.get(code[i], 0)
            i += 1
        body = code[start:i - 1]
        members = {}
        # Only top-level statements of the body (skip inline method bodies).
        flat, depth = [], 0
        for ch in body:
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    flat.append(";")
                continue
            if depth == 0:
                flat.append(ch)
        for statement in "".join(flat).split(";"):
            statement = " ".join(statement.split())
            statement = re.sub(r"^(public|private|protected)\s*:\s*", "", statement)
            if not statement or statement.startswith(("static ", "typedef ", "using ", "friend ", "enum ")):
                continue
            fnptr = re.search(r"\(\s*(?:__\w+\s*)?\*\s*(\w+)\s*\)\s*\(", statement)
            if fnptr:
                members[fnptr.group(1)] = ("code", 1, "function pointer: " + statement)
                continue
            if "(" in statement:
                continue
            m = re.match(r"^(?:class |struct |union |enum )?(?P<type>[\w:]+(?:\s+[\w:]+)*?)\s*(?P<ptr>\*+)?\s*(?P<name>\w+)\s*(?P<dims>(?:\[[^\]]*\])*)\s*(?::\s*\d+)?$", statement.replace(" *", "*").replace("* ", "*"))
            if not m:
                m = re.match(r"^(?:class |struct |union |enum )?(?P<type>[\w:]+(?:\s+[\w:]+)*?)(?P<ptr>\*+)(?P<name>\w+)(?P<dims>(?:\[[^\]]*\])*)$", statement.replace(" ", " "))
            if not m:
                continue
            field = m.group("name")
            comment = ""
            for line in raw_lines:
                if re.search(r"\b%s\b\s*(\[[^\]]*\])*\s*(:\s*\d+\s*)?;" % re.escape(field), line):
                    tail = re.split(r"//+|/\*", line, maxsplit=1)
                    if len(tail) > 1:
                        comment = tail[1].replace("*/", "").strip()
                        comment = re.sub(r"^(0x[0-9A-Fa-f]+\s*)+", "", comment).strip(" -:")
                    break
            members[field] = (m.group("type").strip(), len(m.group("ptr") or ""), comment)
        found[name] = {"kind": kind, "members": members, "header": os.path.relpath(path, REPO).replace("\\", "/")}
    return found


def msvc_layouts():
    build = os.path.join(REPO, "build", "layout")
    os.makedirs(build, exist_ok=True)
    with open(os.path.join(build, "tu.cpp"), "w") as tu:
        for header in HEADERS:
            tu.write('#include "../../%s"\n' % header)
    with open(os.path.join(build, "run.bat"), "w", newline="\r\n") as bat:
        bat.write('@echo off\ncall "%s" >nul\ncd /d "%%~dp0"\ncl /nologo /c /std:c++17 /EHsc /d1reportAllClassLayout tu.cpp /Fonul.obj\n' % VCVARS)
    win_bat = subprocess.check_output(["wslpath", "-w", os.path.join(build, "run.bat")]).decode().strip()
    result = subprocess.run(["cmd.exe", "/c", win_bat], capture_output=True)
    text = result.stdout.decode("utf-8", "replace").replace("\r", "")
    if result.returncode != 0:
        sys.exit("cl failed:\n" + "\n".join(l for l in text.splitlines() if "error" in l)[:4000])
    layouts, current = {}, None
    for line in text.splitlines():
        head = re.match(r"^(class|struct|union)\s+(\S+)\s+size\((\d+)\):", line)
        if head:
            current = {"kind": head.group(1), "size": int(head.group(3)), "fields": []}
            layouts[head.group(2)] = current
            continue
        if current is None:
            continue
        field = re.match(r"^\s*(\d+)\.?\s*\|\s*(?:(?:class |struct |union |enum )?[\w:]+\s+)?(\w+)(?:\s*\(bitstart=(\d+),nbits=(\d+)\))?\s*$", line)
        if field:
            current["fields"].append((int(field.group(1)), field.group(2), field.group(3), field.group(4)))
        elif not line.strip():
            current = None
    return layouts


def main():
    check = "--check" in sys.argv
    declared = {}
    for header in HEADERS:
        declared.update(header_members(os.path.join(REPO, header)))
    layouts = msvc_layouts()
    structs = []
    for name in sorted(declared):
        if name in SKIP or name not in layouts:
            continue
        layout, info = layouts[name], declared[name]
        fields = []
        raw = layout["fields"]
        for index, (offset, field, bitstart, nbits) in enumerate(raw):
            if bitstart is not None:
                # Bitfields share one storage unit: record them on the unit, not as fields.
                if fields and fields[-1]["offset"] == offset:
                    fields[-1]["comment"] = (fields[-1]["comment"] + "; " if fields[-1]["comment"] else "") + "%s: bits %s..%d" % (field, bitstart, int(bitstart) + int(nbits) - 1)
                    continue
            if layout["kind"] == "union":
                end = layout["size"]
            else:
                later = [o for o, _, _, _ in raw[index + 1:] if o > offset]
                end = later[0] if later else layout["size"]
            ctype, pointers, comment = info["members"].get(field, ("", 0, ""))
            if bitstart is not None:
                comment = (comment + "; " if comment else "") + "%s: bits %s..%d" % (field, bitstart, int(bitstart) + int(nbits) - 1)
            fields.append({"offset": offset, "name": field, "type": ctype, "pointers": pointers,
                           "span": end - offset, "comment": comment})
        structs.append({"name": name, "kind": info["kind"] if info["kind"] != "class" else "struct",
                        "size": layout["size"], "header": info["header"], "fields": fields})
    enums = {}
    for header in HEADERS:
        enums.update(header_enums(os.path.join(REPO, header)))
    data = {"generated_by": "tools/decomp/gen_struct_layouts.py (MSVC /d1reportAllClassLayout, Win32)",
            "enums": [dict(name=k, **v) for k, v in sorted(enums.items())],
            "structs": structs}
    text = json.dumps(data, indent=1, ensure_ascii=False) + "\n"
    old = open(OUT, encoding="utf-8").read() if os.path.exists(OUT) else ""
    if check:
        if old != text:
            print("structs.json is out of date: run python3 tools/decomp/gen_struct_layouts.py")
            sys.exit(1)
        print("structs.json is current (%d structs)" % len(structs))
        return
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    missing = sorted(set(declared) - set(layouts) - SKIP)
    print("%d structs -> %s%s" % (len(structs), os.path.relpath(OUT, REPO),
                                  ("; not laid out by MSVC (unused templates/forward decls): " + ", ".join(missing)) if missing else ""))
    untyped = [(s["name"], f["name"]) for s in structs for f in s["fields"] if not f["type"]]
    if untyped:
        print("fields with no type parsed from the header: %s" % ", ".join("%s.%s" % u for u in untyped[:30]))


if __name__ == "__main__":
    main()
