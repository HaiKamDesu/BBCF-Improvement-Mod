# -*- coding: utf-8 -*-
# Ghidra headless Jython script, phase 8: the captured log Debug_DCodeError1.txt
# shows the wedge is state 6 (response received but rejected), not a silent state-2
# stall. FUN_004a25c0's state-2 branch: FUN_004b8ce0() != -100 means "done or error";
# then it requires recvSize(subobj+0xd0) == 0x6800 AND FUN_004a1dd0() != 0, else
# FUN_004a0d50() + state=6 (permanent -- nothing resets 6). Decompile the transport
# poll, send path, validator, and reset so we know (a) what error codes 004b8ce0 can
# return, (b) what 004a1dd0 actually validates, (c) what buffer/fields to dump when
# live logging catches the next occurrence. Also decompile the per-frame poll-site
# candidates (FUN_0049a230 / FUN_0049d440) for a future watchdog hook point, and
# list callers of the state machine entry points.
# Usage:
# analyzeHeadless <project_dir> BBCF -process BBCF.exe -noanalysis -scriptPath <script_dir> -postScript DecompileDCodeBug8.py <report_path>

from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor


FUNCTION_ADDRS = [
    0x004b8ce0,  # transport poll -- return codes decide done(-> validate) vs -100 pending
    0x004a1dd0,  # payload validator -- its failure is one path to state 6
    0x004b8eb0,  # request send (returns nonzero -> -100 without state change)
    0x004b8f70,  # transport pump, called with the 0x6800 buffer and again per poll
    0x004a0d50,  # reset called right before state=6
    0x0047e860,  # context getter whose +0x25f0 field feeds the send
    0x0049a230,  # per-frame poll-site candidate ("own" variant)
    0x0049d440,  # per-frame poll-site candidate ("opponent" variant)
]

# Entry points whose callers we want, to map the per-frame drive path.
CALLER_TARGETS = [
    0x004a25c0,  # state machine tick
    0x004a26a0,  # trigger (sets state=1)
    0x004b8ce0,  # transport poll (shared with other systems?)
    0x004a1dd0,  # validator
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
    report_file = File(File(currentProgram.getExecutablePath()).getParentFile(), "dcode_bug8_decompile.txt")

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
