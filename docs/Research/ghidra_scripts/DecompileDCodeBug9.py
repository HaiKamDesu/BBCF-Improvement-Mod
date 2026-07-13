# -*- coding: utf-8 -*-
# Ghidra headless Jython script, phase 9: follow-ups from phase 8.
# FUN_004a1dd0 validates via FUN_0040df10(rowBuf, 0x6800) -- decompile that check
# (checksum? magic? version?) since its failure is the concrete trigger for the
# permanent state-6 wedge captured in Debug_DCodeError1.txt. Resolve the transport
# singleton class (ctor FUN_004717c0, 0x1c bytes) so the virtual poll behind
# FUN_004b8ce0 (obj->vtbl[+8], then jmp target->vtbl[+0x18]) can be identified.
# Also FUN_0047c8c0 (globals used to clamp row+0xc0), FUN_0049a940 (third caller
# of the tick FUN_004a25c0), FUN_004a1a00 (gate in the per-frame pump
# FUN_0049d440).
# Usage:
# analyzeHeadless <project_dir> BBCF -process BBCF.exe -noanalysis -scriptPath <script_dir> -postScript DecompileDCodeBug9.py <report_path>
from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor


FUNCTION_ADDRS = [
    0x0040df10,  # payload validity check over the 0x6800 blob (state-6 trigger)
    0x004717c0,  # transport singleton ctor (0x1c bytes) -- reveals vtable
    0x0047c8c0,  # globals getter feeding the +0xc0 clamp in FUN_004a1dd0
    0x0049a940,  # third caller of the tick FUN_004a25c0
    0x004a1a00,  # gate in per-frame pump FUN_0049d440
    0x004a2870,  # called by validator after magic write
]

# Entry points whose callers we want.
CALLER_TARGETS = [
    0x0040df10,  # is the payload check shared with other packet types?
    0x0049d440,  # who drives the per-frame pump
    0x0049a230,  # who drives the trigger variant
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


def dump_callees(out, addrs):
    out.println("===== CALLEES =====")
    seen_targets = set()
    for addr_value in addrs:
        fn = get_fn(addr_value)
        if fn is None:
            continue
        entry = fn.getEntryPoint()
        key = entry.toString()
        if key in seen_targets:
            continue
        seen_targets.add(key)
        out.printf("--- callees of %s (%s) ---%n", entry, fn.getName())
        called = fn.getCalledFunctions(ConsoleTaskMonitor())
        for callee in called:
            out.printf("  %s %s%n", callee.getEntryPoint(), callee.getName())
        out.println()


def dump_callers(out, addrs):
    out.println("===== CALLERS =====")
    for addr_value in addrs:
        fn = get_fn(addr_value)
        if fn is None:
            continue
        entry = fn.getEntryPoint()
        out.printf("--- callers of %s (%s) ---%n", entry, fn.getName())
        callers = fn.getCallingFunctions(ConsoleTaskMonitor())
        for caller in callers:
            out.printf("  %s %s%n", caller.getEntryPoint(), caller.getName())
        out.println()


args = getScriptArgs()
if len(args) > 0:
    report_file = File(args[0])
else:
    report_file = File(File(currentProgram.getExecutablePath()).getParentFile(), "dcode_bug9_decompile.txt")

out = PrintWriter(report_file, "UTF-8")
ifc = DecompInterface()
try:
    if not ifc.openProgram(currentProgram):
        raise Exception("openProgram failed")

    out.printf("Program: %s%n", currentProgram.getName())
    out.printf("Image base: %s%n%n", currentProgram.getImageBase())

    decompile_functions(out, ifc, FUNCTION_ADDRS)
    dump_callees(out, FUNCTION_ADDRS)
    dump_callers(out, CALLER_TARGETS)
finally:
    ifc.dispose()
    out.close()

print("Wrote %s" % report_file.getAbsolutePath())
