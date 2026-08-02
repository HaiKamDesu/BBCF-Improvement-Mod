# -*- coding: utf-8 -*-
# Ghidra headless Jython script, phase 22: the PROFILE-UPLOAD state machine.
# Builders resolved (phase 21): FUN_004B9210 = generic TUS UPLOAD submit
# (UMS vtbl+0x0C, file index = param_6; 0 = L"bbdc.dat"), FUN_004B8EB0 =
# download submit (already hooked path). FUN_004A8190 is a facade user that the
# mod's existing [RANK] tracing already touches (RankUploadA8190Virtual0C/10).
# Decompile the upload submit's callers and the surrounding state machines so we
# can find a pollable "upload state" field -- the thing whose failure loses
# ranked/netcolor progress when the TUS layer wedges.
from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

FUNCS = [0x004a8190, 0x004a79f0, 0x004a7a20, 0x004a9af0, 0x004aaad0, 0x0049f590, 0x004af1a0]
CALLER_ROOTS = [0x004b9210, 0x004b9180, 0x004b8e00]
LEVELS = 2

def get_fn(a):
    addr = toAddr(a)
    fn = getFunctionAt(addr)
    if fn is None:
        fn = getFunctionContaining(addr)
    return fn

def dec(out, ifc, fn, note=""):
    out.printf("----- DECOMPILE %s %s -----%n", fn.getEntryPoint(), note)
    out.printf("Function: %s%n", fn.getName())
    r = ifc.decompileFunction(fn, 120, ConsoleTaskMonitor())
    if r.decompileCompleted():
        out.println(r.getDecompiledFunction().getC())
    else:
        out.printf("Decompile failed: %s%n", r.getErrorMessage())
    out.println()

args = getScriptArgs()
report_file = File(args[0]) if len(args) > 0 else File("dcode_bug22.txt")
out = PrintWriter(report_file, "UTF-8")
ifc = DecompInterface()
try:
    if not ifc.openProgram(currentProgram):
        raise Exception("openProgram failed")
    out.printf("Program: %s%nImage base: %s%n%n", currentProgram.getName(), currentProgram.getImageBase())
    seen = set()
    for a in FUNCS:
        fn = get_fn(a)
        if fn is None: continue
        k = fn.getEntryPoint().toString()
        if k in seen: continue
        seen.add(k)
        dec(out, ifc, fn, "(facade user)")
        out.printf("  callers of %s:%n", fn.getName())
        for c in fn.getCallingFunctions(ConsoleTaskMonitor()):
            out.printf("    %s %s%n", c.getEntryPoint(), c.getName())
        out.println()

    out.println("===== CALLERS OF REQUEST BUILDERS =====")
    frontier = []
    for a in CALLER_ROOTS:
        fn = get_fn(a)
        if fn is None: continue
        out.printf("--- callers of %s (%s) ---%n", fn.getEntryPoint(), fn.getName())
        for c in fn.getCallingFunctions(ConsoleTaskMonitor()):
            out.printf("  %s %s%n", c.getEntryPoint(), c.getName())
            k = c.getEntryPoint().toString()
            if k not in seen:
                seen.add(k); frontier.append(c)
        out.println()
    for fn in frontier:
        dec(out, ifc, fn, "(builder caller)")
finally:
    ifc.dispose(); out.close()
print("done")
