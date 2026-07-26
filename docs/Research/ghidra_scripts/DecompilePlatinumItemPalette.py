# -*- coding: utf-8 -*-
# Ghidra headless Jython script: Platinum item-state custom-palette bug.
#
# Established at runtime: the mod patches the palette container (obj+0x830) and the game draws
# Platinum's normal states from it correctly, but while she holds a drive item she renders from a
# snapshot taken at round init -- no byte-exact copy of the original palette is left in system
# memory at that point, and neither an index toggle nor moving to an unused palette index
# refreshes it. A round reset does.
#
# Goal: find the code that fills that per-object palette snapshot at round init, and whatever
# invalidates/refreshes it, so a mid-match palette switch can trigger the same refresh.
#
# Anchors:
#   0x005B6310  loads a palette container and stores it at owner+0x830 (the mod's
#               GetPalBaseAddresses hook site is the store at 0x005B6372)
#   0x005B6440  container consumer (reads +0x830, calls entry getter 0x00411F90)
#   0x005B6840  container consumer (reads +0x830)
#   0x0047D910  round-init copy of char-select data into the live per-player struct at +0x24D8
#               (the mod's GetPaletteIndexPointers hook site is 0x0047D92D)
#   0x00411F90  palette entry getter used by the consumers above
#
# Usage:
# analyzeHeadless <project_dir> BBCF -process BBCF.exe -noanalysis -scriptPath <script_dir> -postScript DecompilePlatinumItemPalette.py <report_path>
from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor


FUNCTION_ADDRS = [
    0x005b6310,
    0x005b6440,
    0x005b6840,
    0x0047d910,
    0x00411f90,
]

CALLER_TARGETS = [
    0x005b6310,
    0x005b6440,
    0x005b6840,
    0x0047d910,
]


def get_fn(addr_value):
    addr = toAddr(addr_value)
    fn = getFunctionAt(addr)
    if fn is None:
        fn = getFunctionContaining(addr)
    return fn


def decompile_functions(out, ifc, addrs):
    seen = set()
    for addr_value in addrs:
        fn = get_fn(addr_value)
        if fn is None:
            out.printf("----- %s -----%nNo function found.%n%n", toAddr(addr_value))
            continue
        key = fn.getEntryPoint().toString()
        if key in seen:
            continue
        seen.add(key)

        out.printf("----- DECOMPILE %s containing %s -----%n", fn.getEntryPoint(), toAddr(addr_value))
        out.printf("Function: %s%n", fn.getName())
        result = ifc.decompileFunction(fn, 120, ConsoleTaskMonitor())
        if result.decompileCompleted():
            out.println(result.getDecompiledFunction().getC())
        else:
            out.printf("Decompile failed: %s%n", result.getErrorMessage())
        out.println()


def dump_callers(out, addrs, depth=2):
    out.println("===== CALLERS =====")
    for addr_value in addrs:
        fn = get_fn(addr_value)
        if fn is None:
            continue
        out.printf("--- callers of %s (%s) ---%n", fn.getEntryPoint(), fn.getName())

        level = set([fn])
        seen = set([fn.getEntryPoint().toString()])
        for d in range(depth):
            nxt = set()
            for f in level:
                for caller in f.getCallingFunctions(ConsoleTaskMonitor()):
                    key = caller.getEntryPoint().toString()
                    if key in seen:
                        continue
                    seen.add(key)
                    nxt.add(caller)
                    out.printf("  [depth %d] %s %s%n", d + 1, caller.getEntryPoint(), caller.getName())
            level = nxt
            if not level:
                break
        out.println()


def decompile_callers(out, ifc, addr_value, limit=6):
    """Decompile the immediate callers of one function -- that is where the round-init fill lives."""
    fn = get_fn(addr_value)
    if fn is None:
        return
    out.printf("===== DECOMPILED CALLERS of %s (%s) =====%n", fn.getEntryPoint(), fn.getName())
    count = 0
    for caller in fn.getCallingFunctions(ConsoleTaskMonitor()):
        if count >= limit:
            out.printf("(caller list truncated at %d)%n", limit)
            break
        count += 1
        out.printf("----- CALLER %s (%s) -----%n", caller.getEntryPoint(), caller.getName())
        result = ifc.decompileFunction(caller, 120, ConsoleTaskMonitor())
        if result.decompileCompleted():
            out.println(result.getDecompiledFunction().getC())
        else:
            out.printf("Decompile failed: %s%n", result.getErrorMessage())
        out.println()


args = getScriptArgs()
if len(args) > 0:
    report_file = File(args[0])
else:
    report_file = File(File(currentProgram.getExecutablePath()).getParentFile(), "platinum_item_palette.txt")

out = PrintWriter(report_file, "UTF-8")
ifc = DecompInterface()
try:
    if not ifc.openProgram(currentProgram):
        raise Exception("openProgram failed")

    out.printf("Program: %s%n", currentProgram.getName())
    out.printf("Image base: %s%n%n", currentProgram.getImageBase())

    decompile_functions(out, ifc, FUNCTION_ADDRS)
    dump_callers(out, CALLER_TARGETS)
    decompile_callers(out, ifc, 0x005b6310)
finally:
    ifc.dispose()
    out.close()

print("Wrote %s" % report_file.getAbsolutePath())
