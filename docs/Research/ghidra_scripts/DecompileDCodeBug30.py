# -*- coding: utf-8 -*-
# Phase 30: can a fresh user/login be forced from the mod, safely?
# FUN_00428050 re-arms a type-1 (Login) strategy at workMgr+0xE0, but only when
# DAT_00A5A070 == 0, and it FREES the current strategy first (FUN_00429390).
# Two blockers to settle before any repair can be written:
#   1. what is DAT_00A5A070, and who writes it?
#   2. which thread pumps the strategies at +0xE0 / +0xE4? If the CUMSTask
#      worker thread touches them, freeing one from the game thread is a
#      use-after-free.
from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

DATA = [(0x00A5A070, "login latch?"), (0x00A5A050, "work manager base")]
FUNCS = [
    (0x00428050, "re-arm Login strategy (the candidate repair entry point)"),
    (0x00427c40, "manager shutdown/reset?"),
    (0x004279a0, "manager dtor?"),
    (0x004282c0, "manager init tail"),
    (0x00427e00, "?"),
    (0x00427e40, "request: ?"),
    (0x00427ec0, "request: ?"),
    (0x00427f60, "request: ?"),
    (0x00427fd0, "request: ?"),
    (0x00428020, "request: ?"),
    (0x00428110, "request: ?"),
    (0x00428180, "request: ?"),
    (0x004281e0, "request: ?"),
    (0x00428ef0, "type-1 strategy ctor (Login)"),
]

def get_fn(addr):
    fn = getFunctionAt(addr)
    if fn is None:
        fn = getFunctionContaining(addr)
    return fn

def dec(out, ifc, fn, note=""):
    out.printf("----- DECOMPILE %s %s -----%n", fn.getEntryPoint(), note)
    r = ifc.decompileFunction(fn, 180, ConsoleTaskMonitor())
    if r.decompileCompleted():
        out.println(r.getDecompiledFunction().getC())
    else:
        out.printf("Decompile failed: %s%n", r.getErrorMessage())
    out.println()

args = getScriptArgs()
out = PrintWriter(File(args[0]) if len(args) > 0 else File("dcode_bug30.txt"), "UTF-8")
ifc = DecompInterface()
try:
    ifc.openProgram(currentProgram)
    out.printf("Program: %s%nImage base: %s%n%n", currentProgram.getName(), currentProgram.getImageBase())
    rm = currentProgram.getReferenceManager()
    extra = []
    for addr, note in DATA:
        out.printf("===== XREFS TO %08X (%s) =====%n", addr, note)
        for ref in rm.getReferencesTo(toAddr(addr)):
            fa = ref.getFromAddress()
            fn = get_fn(fa)
            name = fn.getName() if fn else "?"
            out.printf("  %s  from %s in %s%n", ref.getReferenceType(), fa, name)
            if fn is not None:
                extra.append((fn.getEntryPoint().getOffset(), "touches %08X" % addr))
        out.println()

    seen = set()
    for addr, note in FUNCS + extra:
        if addr in seen:
            continue
        seen.add(addr)
        fn = get_fn(toAddr(addr))
        if fn is None:
            out.printf("----- %08X: no function -----%n%n", addr)
            continue
        dec(out, ifc, fn, "(%s)" % note)
        out.printf("--- callers of %08X ---%n", addr)
        for c in fn.getCallingFunctions(ConsoleTaskMonitor()):
            out.printf("  %s %s%n", c.getEntryPoint(), c.getName())
        out.println()
finally:
    ifc.dispose(); out.close()
print("done")
