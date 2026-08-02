# -*- coding: utf-8 -*-
# Ghidra headless Jython script, phase 23: the profile-upload trigger + its gate.
# FUN_004A96D0 = "upload my network profile": builds a 0x6800 blob and submits it
# via FUN_004B9210 with file index 0 (L"bbdc.dat"). It early-returns whenever the
# global DAT_00CF77A8 is nonzero. If TUS uploads stop, ranked/netcolor progress is
# never made durable and a restart reverts to the last successful upload -- which
# is exactly the reported "progress reset". Trace: who calls the upload, and every
# reader/writer of the gate global.
from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

UPLOAD_FN = 0x004a96d0
GATE_DATA = [0x00cf77a8]
LEVELS = 3

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
report_file = File(args[0]) if len(args) > 0 else File("dcode_bug23.txt")
out = PrintWriter(report_file, "UTF-8")
ifc = DecompInterface()
try:
    if not ifc.openProgram(currentProgram):
        raise Exception("openProgram failed")
    out.printf("Program: %s%nImage base: %s%n%n", currentProgram.getName(), currentProgram.getImageBase())
    seen = set()

    out.println("===== UPLOAD TRIGGER CALLER TREE =====")
    fn = get_fn(UPLOAD_FN)
    frontier = [fn]
    seen.add(fn.getEntryPoint().toString())
    for level in range(1, LEVELS + 1):
        out.printf("--- level %d ---%n", level)
        nxt = []
        for f in frontier:
            callers = list(f.getCallingFunctions(ConsoleTaskMonitor()))
            out.printf("  callers of %s (%s): %d%n", f.getEntryPoint(), f.getName(), len(callers))
            for c in callers:
                out.printf("    %s %s%n", c.getEntryPoint(), c.getName())
                k = c.getEntryPoint().toString()
                if k not in seen:
                    seen.add(k); nxt.append(c)
        out.println()
        for c in nxt:
            dec(out, ifc, c, "(level %d caller of upload)" % level)
        frontier = nxt
        if not frontier:
            break

    out.println("===== GATE GLOBAL XREFS =====")
    for d in GATE_DATA:
        addr = toAddr(d)
        out.printf("--- refs to %s ---%n", addr)
        gate_fns = set()
        for ref in getReferencesTo(addr):
            f = getFunctionContaining(ref.getFromAddress())
            nm = f.getName() if f else "<none>"
            out.printf("  %s in %s (%s)%n", ref.getFromAddress(), nm, ref.getReferenceType())
            if f:
                gate_fns.add(f)
        out.println()
        for f in gate_fns:
            k = f.getEntryPoint().toString()
            if k in seen:
                continue
            seen.add(k)
            dec(out, ifc, f, "(touches gate global)")
finally:
    ifc.dispose(); out.close()
print("done")
