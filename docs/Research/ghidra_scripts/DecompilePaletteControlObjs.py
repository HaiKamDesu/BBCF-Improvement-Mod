# -*- coding: utf-8 -*-
# Ghidra headless Jython script: Platinum item-state custom-palette bug, phase 2.
#
# The runtime scan found 8 stale palette mirrors at a 0x1108 stride that never pick up a mid-match
# palette write, and the binary has strings PaletteControlObj1..8 -- almost certainly the same 8.
# Decompile everything that references those names, plus the AA_CPalette_* / AA_CPaletteFactory_*
# vtable users, to find who fills a PaletteControlObj's palette data and what would refresh it.
#
# Usage:
# analyzeHeadless <project_dir> BBCF -process BBCF.exe -noanalysis -scriptPath <script_dir> -postScript DecompilePaletteControlObjs.py <report_path>
from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

# PaletteControlObj1..8 string addresses, and the palette class vtables.
DATA_ADDRS = [
    0x00954d50, 0x00954d64, 0x00954d78, 0x00954d8c,
    0x00954da0, 0x00954db4, 0x00954dc8, 0x00954ddc,
    0x00899384,  # AA_CPalette_Custom::vftable
    0x008993b8,  # AA_CPalette_HIP::vftable
    0x0089c318,  # AA_CPaletteFactory_HIP::vftable
    0x0094e140,  # AA_CPaletteFactory_Custom::vftable
    0x0084efe8,  # "PALETTETEXTURE"
]


def referrers(addr_value):
    """Functions containing a reference to this address."""
    out = []
    seen = set()
    refs = getReferencesTo(toAddr(addr_value))
    for ref in refs:
        fn = getFunctionContaining(ref.getFromAddress())
        if fn is None:
            continue
        key = fn.getEntryPoint().toString()
        if key in seen:
            continue
        seen.add(key)
        out.append((fn, ref.getFromAddress()))
    return out


args = getScriptArgs()
report_file = File(args[0]) if len(args) > 0 else File("palette_control_objs.txt")
out = PrintWriter(report_file, "UTF-8")
ifc = DecompInterface()

try:
    if not ifc.openProgram(currentProgram):
        raise Exception("openProgram failed")

    out.printf("Program: %s%n", currentProgram.getName())
    out.printf("Image base: %s%n%n", currentProgram.getImageBase())

    out.println("===== REFERRERS =====")
    allFns = {}
    for addr_value in DATA_ADDRS:
        out.printf("--- refs to %s ---%n", toAddr(addr_value))
        found = referrers(addr_value)
        if not found:
            out.println("  (none)")
        for fn, site in found:
            out.printf("  %s  %s   (ref at %s)%n", fn.getEntryPoint(), fn.getName(), site)
            allFns[fn.getEntryPoint().toString()] = fn
    out.println()

    out.println("===== DECOMPILED REFERRERS =====")
    for key in sorted(allFns.keys()):
        fn = allFns[key]
        out.printf("----- %s (%s) -----%n", fn.getEntryPoint(), fn.getName())
        result = ifc.decompileFunction(fn, 180, ConsoleTaskMonitor())
        if result.decompileCompleted():
            out.println(result.getDecompiledFunction().getC())
        else:
            out.printf("Decompile failed: %s%n", result.getErrorMessage())
        out.println()

    out.println("===== CALLERS OF REFERRERS =====")
    for key in sorted(allFns.keys()):
        fn = allFns[key]
        out.printf("--- callers of %s (%s) ---%n", fn.getEntryPoint(), fn.getName())
        for caller in fn.getCallingFunctions(ConsoleTaskMonitor()):
            out.printf("  %s %s%n", caller.getEntryPoint(), caller.getName())
        out.println()
finally:
    ifc.dispose()
    out.close()

print("Wrote %s" % report_file.getAbsolutePath())
