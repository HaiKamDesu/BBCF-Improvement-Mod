# -*- coding: utf-8 -*-
# Ghidra headless Jython script: enumerate every palette-related symbol, RTTI class and string in
# BBCF.exe, to locate the class that owns the per-object palette snapshot the item state renders
# from. FUN_005b6420 references AA_CPaletteFactory_Custom::vftable, so RTTI names survived.
#
# Usage:
# analyzeHeadless <project_dir> BBCF -process BBCF.exe -noanalysis -scriptPath <script_dir> -postScript DumpPaletteSymbols.py <report_path>
from java.io import File, PrintWriter
from ghidra.util.task import ConsoleTaskMonitor

KEYWORDS = ["palette", "Palette", "PALETTE", "pallet", "Pallet"]


def matches(name):
    for kw in KEYWORDS:
        if kw in name:
            return True
    return False


args = getScriptArgs()
report_file = File(args[0]) if len(args) > 0 else File("palette_symbols.txt")
out = PrintWriter(report_file, "UTF-8")

try:
    out.printf("Program: %s%n", currentProgram.getName())
    out.printf("Image base: %s%n%n", currentProgram.getImageBase())

    out.println("===== SYMBOLS MATCHING PALETTE =====")
    symTable = currentProgram.getSymbolTable()
    count = 0
    for sym in symTable.getAllSymbols(True):
        name = sym.getName()
        if not matches(name):
            continue
        count += 1
        out.printf("%s  %s  (%s)%n", sym.getAddress(), name, sym.getSymbolType())
        if count > 4000:
            out.println("(truncated)")
            break
    out.printf("total: %d%n%n", count)

    out.println("===== FUNCTIONS MATCHING PALETTE =====")
    fm = currentProgram.getFunctionManager()
    for fn in fm.getFunctions(True):
        if matches(fn.getName()):
            out.printf("%s  %s%n", fn.getEntryPoint(), fn.getName())
    out.println()

    out.println("===== STRINGS MATCHING PALETTE / pal / vri =====")
    listing = currentProgram.getListing()
    dataIter = listing.getDefinedData(True)
    shown = 0
    while dataIter.hasNext():
        data = dataIter.next()
        try:
            val = data.getValue()
        except:
            continue
        if val is None:
            continue
        s = str(val)
        low = s.lower()
        if ("palette" in low or "pallet" in low or "_pal" in low or "vri" in low or
                low.startswith("vr")):
            out.printf("%s  %s%n", data.getAddress(), s)
            shown += 1
            if shown > 1500:
                out.println("(truncated)")
                break
    out.println()

    out.println("===== CLASS NAMESPACES =====")
    for sym in symTable.getAllSymbols(True):
        ns = sym.getParentNamespace()
        if ns is None:
            continue
        nsName = ns.getName()
        if matches(nsName):
            out.printf("%s  %s::%s%n", sym.getAddress(), nsName, sym.getName())
finally:
    out.close()

print("Wrote %s" % report_file.getAbsolutePath())
