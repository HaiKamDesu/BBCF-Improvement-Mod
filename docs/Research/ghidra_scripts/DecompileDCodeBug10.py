# -*- coding: utf-8 -*-
# Ghidra headless Jython script, phase 10: the 2026-07-13 rollback happened with
# ZERO fetch-state anomalies (DCodeIncidents.log), so the wedge is not the only
# rollback path -- the save side is now primary. Live logging showed the "auto
# save trigger global" DAT_00EA97C8 is just CSaveDataManager+0x1B11F0 (manager is
# statically allocated at VA 0xC986D8), and disasm shows small request helpers:
#   004BB2C0 -> nextAction=7 (the pulse observed after matches/at exit)
#   004BB300 -> 2, 004BB350 -> 3, 004BB3A0 -> 5, 004BB3D0 -> 6,
#   004BB410 -> 1, 004BB4C0 -> 0, 004BB4F0 -> 8, 004BAC00 init
# Decompile the action-7/1/2 requesters and ALL their callers (the actual game
# events that ask for a save), plus the save task pump FUN_004B9F70 and the
# completion poll FUN_004CACA0, to find (a) when saves fire during network play
# and (b) any gate that could silently skip them for a whole session.
# Usage:
# analyzeHeadless <project_dir> BBCF -process BBCF.exe -noanalysis -scriptPath <script_dir> -postScript DecompileDCodeBug10.py <report_path>

from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor


FUNCTION_ADDRS = [
    0x004bb2c0,  # request nextAction=7
    0x004bb410,  # request nextAction=1
    0x004bb300,  # request nextAction=2
    0x004b9f70,  # GAME_CSaveTask::update_task (save pump)
    0x004caca0,  # completion poll used by pump state 2
]

# Decompile every caller of these (the save request sites).
CALLER_DECOMPILE_TARGETS = [
    0x004bb2c0,
    0x004bb410,
    0x004bb300,
]


def get_fn(addr_value):
    addr = toAddr(addr_value)
    fn = getFunctionAt(addr)
    if fn is None:
        fn = getFunctionContaining(addr)
    return fn


def decompile_fn(out, ifc, fn, note=""):
    out.printf("----- DECOMPILE %s %s -----%n", fn.getEntryPoint(), note)
    out.printf("Function: %s%n", fn.getName())
    result = ifc.decompileFunction(fn, 120, ConsoleTaskMonitor())
    if result.decompileCompleted():
        out.println(result.getDecompiledFunction().getC())
    else:
        out.printf("Decompile failed: %s%n", result.getErrorMessage())
    out.println()


args = getScriptArgs()
if len(args) > 0:
    report_file = File(args[0])
else:
    report_file = File(File(currentProgram.getExecutablePath()).getParentFile(), "dcode_bug10_decompile.txt")

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
            out.printf("----- %s -----%nNo function found.%n%n", toAddr(addr_value))
            continue
        key = fn.getEntryPoint().toString()
        if key not in seen:
            seen.add(key)
            decompile_fn(out, ifc, fn)

    out.println("===== CALLERS OF SAVE REQUESTERS (each decompiled) =====")
    for addr_value in CALLER_DECOMPILE_TARGETS:
        fn = get_fn(addr_value)
        if fn is None:
            continue
        out.printf("--- callers of %s (%s) ---%n", fn.getEntryPoint(), fn.getName())
        callers = fn.getCallingFunctions(ConsoleTaskMonitor())
        for caller in callers:
            out.printf("  %s %s%n", caller.getEntryPoint(), caller.getName())
        out.println()
        for caller in callers:
            key = caller.getEntryPoint().toString()
            if key in seen:
                continue
            seen.add(key)
            decompile_fn(out, ifc, caller, "(caller of %s)" % fn.getName())
            grandcallers = caller.getCallingFunctions(ConsoleTaskMonitor())
            out.printf("  callers of %s:%n", caller.getName())
            for gc in grandcallers:
                out.printf("    %s %s%n", gc.getEntryPoint(), gc.getName())
            out.println()
finally:
    ifc.dispose()
    out.close()

print("Wrote %s" % report_file.getAbsolutePath())
