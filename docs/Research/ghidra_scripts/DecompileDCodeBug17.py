# -*- coding: utf-8 -*-
# Ghidra headless Jython script, phase 17: inner bbdc transfer-state machine. FUN_00422B00/FUN_00423A50 poll *(FUN_00427CD0()+4): 7/9=download done, 8=share done, 0xB=error, 3x3s timeout. Decompile the state object getter, the issuers, and find what writes states 7/8/9/0xB (Steam CallResult handlers).
# The 2026-07-15 natural capture shows every fetch after one moment fails with
# recvSize=0; the poll FUN_00422E70 maps worker+0x1C=done, worker+0xC0 bit0 =
# ERROR -> returns 100 -> state 6. The worker (0x110 bytes, ctor FUN_00422410)
# is driven by a queue/thread via FUN_0041FB80/FUN_0041FC20/FUN_0041AB10.
# Decompile the ctor (reveals thread proc / Steam callback registration), the
# queue functions, and from there the routine that performs the Steam storage
# call and sets the error bit.
# Usage:
# analyzeHeadless <project_dir> BBCF -process BBCF.exe -noanalysis -scriptPath <script_dir> -postScript DecompileDCodeBug9.py <report_path>
from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor


FUNCTION_ADDRS = [
    0x00427cd0,  # inner transfer-state singleton getter (+4 = state)
    0x00427ef0,  # issue UGC download (bbdc)
    0x00428180,  # issue FileShare (bbdc)
]

# Entry points whose callers we want.
CALLER_TARGETS = [
    0x00427cd0,
    0x00427ef0,
    0x00428180,
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
