# -*- coding: utf-8 -*-
# Phase 2: the training per-frame update (FUN_006cc520), the restart variants'
# callers, and anything referencing the reset-positions action bit.
from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor
from ghidra.program.model.symbol import RefType

FUNCS = [0x0055F780, 0x0056B1F0, 0x00510120, 0x00593B60, 0x00593E40]
CALLER_ROOTS = []

def get_fn(a):
    addr = toAddr(a)
    fn = getFunctionAt(addr)
    if fn is None:
        fn = getFunctionContaining(addr)
    return fn

def dec(out, ifc, fn, note=""):
    out.printf("----- DECOMPILE %s %s -----%n", fn.getEntryPoint(), note)
    out.printf("Function: %s%n", fn.getName())
    r = ifc.decompileFunction(fn, 240, ConsoleTaskMonitor())
    if r.decompileCompleted():
        out.println(r.getDecompiledFunction().getC())
    else:
        out.printf("Decompile failed: %s%n", r.getErrorMessage())
    out.println()

args = getScriptArgs()
out = PrintWriter(File(args[0]), "UTF-8")
ifc = DecompInterface()
try:
    ifc.openProgram(currentProgram)
    for a in FUNCS:
        fn = get_fn(a)
        dec(out, ifc, fn)
        out.printf("  callers of %s:%n", fn.getName())
        for c in fn.getCallingFunctions(ConsoleTaskMonitor()):
            out.printf("    %s %s%n", c.getEntryPoint(), c.getName())
        out.println()
    out.println("===== CALLERS / XREFS OF RESTART VARIANTS =====")
    rm = currentProgram.getReferenceManager()
    for a in CALLER_ROOTS:
        fn = get_fn(a)
        out.printf("--- %s (%s) ---%n", fn.getEntryPoint(), fn.getName())
        for r in rm.getReferencesTo(fn.getEntryPoint()):
            f = getFunctionContaining(r.getFromAddress())
            out.printf("  ref from %s type=%s in %s%n", r.getFromAddress(), r.getReferenceType(), f.getName() if f else "?")
        out.println()
finally:
    ifc.dispose(); out.close()
print("done")
