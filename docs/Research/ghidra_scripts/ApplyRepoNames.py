# -*- coding: utf-8 -*-
# Carries the names the mod already uses into a Ghidra project, so the decompile
# export reads "DCodeFetchTick" instead of FUN_004A25C0.
#
# Source: every `static constexpr uintptr_t ADDR_<Name> = 0x<RVA>;` in
# src/Game/GhidraDefs.h (base-relative RVAs, image base 0x00400000). A function entry
# that still has its default FUN_ name is renamed; anything else gets a label. Names
# Ghidra or a person already gave are never overwritten.
#
# args: <path to GhidraDefs.h> [report path]
import re
from java.io import File, PrintWriter
from ghidra.program.model.symbol import SourceType

IMAGE_BASE = 0x00400000
PATTERN = re.compile(r"static\s+constexpr\s+uintptr_t\s+ADDR_(\w+)\s*=\s*0x([0-9A-Fa-f]+)\s*;")

args = getScriptArgs()
defs_path = args[0]
out = PrintWriter(File(args[1]) if len(args) > 1 else File("apply_repo_names.txt"), "UTF-8")

renamed = labelled = kept = missing = 0
seen = set()
with open(defs_path) as handle:
    text = handle.read()
for name, value in PATTERN.findall(text):
    if name in seen:
        continue
    seen.add(name)
    va = int(value, 16)
    if va < IMAGE_BASE:
        va += IMAGE_BASE
    addr = toAddr(va)
    if currentProgram.getMemory().getBlock(addr) is None:
        out.printf("  outside image  %08X %s%n", va, name)
        missing += 1
        continue
    fn = getFunctionAt(addr)
    if fn is not None:
        if fn.getName().startswith("FUN_"):
            out.printf("  rename   %08X %s -> %s%n", va, fn.getName(), name)
            fn.setName(name, SourceType.USER_DEFINED)
            renamed += 1
        else:
            out.printf("  kept     %08X %s (repo: %s)%n", va, fn.getName(), name)
            kept += 1
        continue
    createLabel(addr, name, False, SourceType.USER_DEFINED)
    out.printf("  label    %08X %s%n", va, name)
    labelled += 1

total = currentProgram.getFunctionManager().getFunctionCount()
named = 0
for fn in currentProgram.getFunctionManager().getFunctions(True):
    if not fn.getName().startswith("FUN_"):
        named += 1
out.printf("%nrenamed %d, labelled %d, kept %d, outside image %d%n", renamed, labelled, kept, missing)
out.printf("functions: %d total, %d with a non-default name%n", total, named)
out.close()
print("renamed %d labelled %d kept %d missing %d; %d/%d functions named" % (renamed, labelled, kept, missing, named, total))
