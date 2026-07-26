# -*- coding: utf-8 -*-
# Ghidra headless Jython script: Platinum item-state custom-palette bug, phase 3.
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
DATA_ADDRS = []

FUNCTION_ADDRS = [
    0x005a7150,  # reads char+0x35C (linked palette id) -- draw-time consumer
    0x00591b80,  # writes +0x358 (palette id assignment)
    0x0057f7e0,  # resets +0x358 / +0x35C to -1 (init)
    0x00580820,  # releases +0x358
    0x0057e0e0,  # reads +0x358
    0x005787c0,  # reads +0x358
    0x0055df60,  # object accessor used by PT_LinkColor
    0x0055c540,  # find-object-by-name used by PT_LinkColor
    0x004dcb60,  # writes +0x358
]


def get_fn(addr_value):
    addr = toAddr(addr_value)
    fn = getFunctionAt(addr)
    if fn is None:
        fn = getFunctionContaining(addr)
    return fn


args = getScriptArgs()
report_file = File(args[0]) if len(args) > 0 else File("palette_link.txt")
out = PrintWriter(report_file, "UTF-8")
ifc = DecompInterface()

try:
    if not ifc.openProgram(currentProgram):
        raise Exception("openProgram failed")

    out.printf("Program: %s%n", currentProgram.getName())
    out.printf("Image base: %s%n%n", currentProgram.getImageBase())

    seen = set()
    for addr_value in FUNCTION_ADDRS:
        fn = get_fn(addr_value)
        if fn is None:
            out.printf("----- %s: no function -----%n", toAddr(addr_value))
            continue
        key = fn.getEntryPoint().toString()
        if key in seen:
            continue
        seen.add(key)
        out.printf("----- %s (%s) containing %s -----%n", fn.getEntryPoint(), fn.getName(), toAddr(addr_value))
        result = ifc.decompileFunction(fn, 180, ConsoleTaskMonitor())
        if result.decompileCompleted():
            out.println(result.getDecompiledFunction().getC())
        else:
            out.printf("Decompile failed: %s%n", result.getErrorMessage())
        out.println()

    out.println("===== CALLERS =====")
    for addr_value in FUNCTION_ADDRS:
        fn = get_fn(addr_value)
        if fn is None:
            continue
        out.printf("--- callers of %s (%s) ---%n", fn.getEntryPoint(), fn.getName())
        for caller in fn.getCallingFunctions(ConsoleTaskMonitor()):
            out.printf("  %s %s%n", caller.getEntryPoint(), caller.getName())
        out.println()
finally:
    ifc.dispose()
    out.close()

print("Wrote %s" % report_file.getAbsolutePath())
