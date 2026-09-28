# -*- coding: utf-8 -*-
# Reset-direction source RE (2026-09-27): the battle-input writer FUN_00559FE0 and its
# caller chain, the controller wrapper / device held-check the training reset rule uses,
# and the frame ordering against MatchInfo::update_Fight.
from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

FUNCS = [0x00559FE0, 0x0056B1F0, 0x0056AFF0, 0x005613E0, 0x00560C10, 0x00464410, 0x004964E0, 0x0047EBB0, 0x006DD2E0, 0x006DAA00, 0x006D7610, 0x005538F0, 0x0047E9C0]
XREF_ROOTS = [0x0056AFF0, 0x00559FE0, 0x0055F780, 0x005613E0]

def get_fn(a):
    addr = toAddr(a)
    fn = getFunctionAt(addr)
    if fn is None:
        fn = getFunctionContaining(addr)
    return fn

def dec(out, ifc, fn):
    out.printf("----- DECOMPILE %s -----%n", fn.getEntryPoint())
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
        if fn is None:
            out.printf("no function at %08X%n", a); continue
        dec(out, ifc, fn)
        out.printf("  callers of %s:%n", fn.getName())
        for c in fn.getCallingFunctions(ConsoleTaskMonitor()):
            out.printf("    %s %s%n", c.getEntryPoint(), c.getName())
        out.println()
    out.println("===== XREFS =====")
    rm = currentProgram.getReferenceManager()
    for a in XREF_ROOTS:
        fn = get_fn(a)
        out.printf("--- %s (%s) ---%n", fn.getEntryPoint(), fn.getName())
        for r in rm.getReferencesTo(fn.getEntryPoint()):
            f = getFunctionContaining(r.getFromAddress())
            out.printf("  ref from %s type=%s in %s%n", r.getFromAddress(), r.getReferenceType(), f.getName() if f else "?")
        out.println()
finally:
    ifc.dispose(); out.close()
print("done")
