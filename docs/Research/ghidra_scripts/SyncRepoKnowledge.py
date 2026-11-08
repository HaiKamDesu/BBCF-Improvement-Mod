# -*- coding: utf-8 -*-
# Pushes everything the repo knows about BBCF.exe into a Ghidra project, so the project (and
# the C export made from it) carries our names and our notes, not just FUN_ addresses.
#
# Sources, in priority order:
#   1. src/Game/GhidraDefs.h: every `static constexpr uintptr_t ADDR_<Name> = 0x<RVA>;`
#      names the address, and the `//` comment lines right above it become its note.
#   2. docs/Decompilation/*.md: every ledger row `| 0x00XXXXXX | Name | Size | Level | Notes |`
#      becomes a note ("[Invulnerability.md, L2] ..."), and names the function when the Name
#      column is a plain identifier used by exactly one row and GhidraDefs.h does not name it.
#
# Notes live in the plate comment, between "[repo]" and "[/repo]" lines. Each run replaces that
# block and leaves anything outside it alone, so a note someone types in the Ghidra GUI survives.
#
# Names: the repo is canonical. A GhidraDefs.h name replaces a default (FUN_) or user-defined
# name. A ledger name replaces only a default name or one a previous sync set. Names Ghidra
# imported or derived (library functions, thunks) are never touched.
#
# Safe to re-run any number of times. Run it on the archive project with tools/decomp/sync_archive.sh.
#
# args: <repo root> [report path]
import io
import os
import re
from java.io import File, PrintWriter
from ghidra.program.model.symbol import SourceType

IMAGE_BASE = 0x00400000
PLATE = 3  # CodeUnit.PLATE_COMMENT
BEGIN = "[repo]"
END = "[/repo]"
IDENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
DEFS = re.compile(r"static\s+constexpr\s+uintptr_t\s+ADDR_(\w+)\s*=\s*0x([0-9A-Fa-f]+)\s*;")
FULL_VA = re.compile(r"0x([0-9A-Fa-f]{6,8})")

args = getScriptArgs()
repo = args[0]
out = PrintWriter(File(args[1]) if len(args) > 1 else File("sync_repo_knowledge.txt"), "UTF-8")
listing = currentProgram.getListing()
memory = currentProgram.getMemory()
symbols = currentProgram.getSymbolTable()


def read(path):
    with io.open(path, encoding="utf-8", errors="replace") as handle:
        return handle.read()


def to_va(value):
    va = int(value, 16)
    return va + IMAGE_BASE if va < IMAGE_BASE else va


# address -> {"name": str or None, "name_from": "defs"/"doc", "notes": [str]}
knowledge = {}


def entry(va):
    return knowledge.setdefault(va, {"name": None, "name_from": None, "notes": []})


# 1. GhidraDefs.h
defs_text = read(os.path.join(repo, "src", "Game", "GhidraDefs.h"))
lines = defs_text.splitlines()
defs_count = 0
for index, line in enumerate(lines):
    match = DEFS.search(line)
    if not match:
        continue
    name, value = match.group(1), match.group(2)
    va = to_va(value)
    comment = []
    back = index - 1
    while back >= 0:
        stripped = lines[back].strip()
        if not stripped.startswith("//"):
            break
        body = stripped.lstrip("/").strip()
        if body and not re.match(r"^[-=]{4,}$", body):
            comment.insert(0, body)
        back -= 1
    item = entry(va)
    if item["name_from"] != "defs":
        item["name"], item["name_from"] = name, "defs"
    note = "[GhidraDefs.h] ADDR_%s" % name
    if comment:
        note += "\n" + "\n".join(comment)
    item["notes"].append(note)
    defs_count += 1


# 2. docs/Decompilation ledgers
def row_addresses(cell):
    """'0x00572C90 / CE0 / D30', '0x00597010 / 0x5973B0', '0x004830D0, 0x00483370'."""
    found = []
    first = None
    for token in re.split(r"[/,]", cell):
        token = token.strip()
        full = FULL_VA.search(token)
        if full:
            text = full.group(1)
            found.append(to_va(text))
            if first is None:
                first = "%08X" % to_va(text)
        elif first and re.match(r"^[0-9A-Fa-f]{2,5}$", token):
            found.append(int(first[:8 - len(token)] + token, 16))
    return found


doc_rows = 0
doc_names = {}
docs_dir = os.path.join(repo, "docs", "Decompilation")
for doc in sorted(os.listdir(docs_dir)):
    if not doc.endswith(".md") or doc in ("README.md", "LocalSetup.md"):
        continue
    section = ""
    for line in read(os.path.join(docs_dir, doc)).splitlines():
        if line.startswith("#"):
            section = line.lstrip("#").strip()
            continue
        if not re.match(r"^\|\s*0x", line):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        addresses = row_addresses(cells[0])
        if not addresses:
            continue
        name = cells[1] if len(cells) > 1 else ""
        level = ""
        for cell in cells[2:]:
            if re.match(r"^L[0-3]$", cell):
                level = cell
        rest = [c for c in cells[2:] if c and c != level and not re.match(r"^\d+(\s*-\s*\d+)?$", c)]
        header = "[%s%s%s]" % (doc, ", " + level if level else "", ", " + section if section else "")
        text = header + " " + name
        if rest:
            text += "\n" + "\n".join(rest)
        for va in addresses:
            entry(va)["notes"].append(text)
        doc_rows += 1
        if len(addresses) == 1 and IDENT.match(name):
            doc_names.setdefault(name, set()).add(addresses[0])

for name, vas in doc_names.items():
    if len(vas) != 1:
        continue  # shared helper names like libcurl_internal
    item = entry(list(vas)[0])
    if item["name"] is None:
        item["name"], item["name_from"] = name, "doc"


# Apply
def previous_repo_name(plate):
    if plate and BEGIN in plate:
        match = re.search(r"\[repo\] name=(\S+)", plate)
        if match:
            return match.group(1)
    return None


def strip_block(plate):
    if not plate or BEGIN not in plate:
        return plate or ""
    start = plate.index(BEGIN)
    end = plate.find(END, start)
    end = len(plate) if end < 0 else end + len(END)
    return (plate[:start] + plate[end:]).strip("\n")


renamed = labelled = kept = notes = missing = 0
for va in sorted(knowledge):
    item = knowledge[va]
    addr = toAddr(va)
    if memory.getBlock(addr) is None:
        out.printf("  outside image  %08X %s%n", va, item["name"] or "")
        missing += 1
        continue
    fn = getFunctionAt(addr)
    old_plate = fn.getComment() if fn is not None else listing.getComment(PLATE, addr)
    owned_name = previous_repo_name(old_plate)
    name = item["name"]
    if name:
        if fn is not None:
            source = fn.getSymbol().getSource()
            current = fn.getName()
            allowed = (source == SourceType.DEFAULT
                       or (item["name_from"] == "defs" and source == SourceType.USER_DEFINED)
                       or (owned_name is not None and current == owned_name))
            if current == name:
                pass
            elif allowed:
                out.printf("  rename   %08X %s -> %s (%s)%n", va, current, name, item["name_from"])
                fn.setName(name, SourceType.USER_DEFINED)
                renamed += 1
            else:
                out.printf("  kept     %08X %s (repo: %s)%n", va, current, name)
                kept += 1
        else:
            existing = [s.getName() for s in symbols.getSymbols(addr)]
            if name not in existing:
                createLabel(addr, name, True, SourceType.USER_DEFINED)
                labelled += 1
    block = BEGIN + (" name=%s" % name if name else "") + "\n" + "\n\n".join(item["notes"]) + "\n" + END
    rest = strip_block(old_plate)
    plate = block + ("\n" + rest if rest else "")
    if fn is not None:
        fn.setComment(plate)
    else:
        listing.setComment(addr, PLATE, plate)
    notes += 1

total = currentProgram.getFunctionManager().getFunctionCount()
named = 0
for fn in currentProgram.getFunctionManager().getFunctions(True):
    if not fn.getName().startswith("FUN_"):
        named += 1
summary = ("GhidraDefs.h names %d, ledger rows %d; renamed %d, labelled %d, kept %d, notes %d, outside image %d; "
           "%d/%d functions named" % (defs_count, doc_rows, renamed, labelled, kept, notes, missing, named, total))
out.printf("%n%s%n", summary)
out.close()
print(summary)
