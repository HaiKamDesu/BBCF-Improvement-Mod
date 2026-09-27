# -*- coding: utf-8 -*-
# Why is the opponent's D-Code invisible after a rejected fetch? Decompile the
# display/pump side that consumes the per-row fetch state (FUN_004A1AB0 busy,
# FUN_004A0B80 poll result, FUN_004A25C0 tick) and the transport submit path.
from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

DIRECT = [0x0049D560, 0x0049D440, 0x004B8EB0, 0x00422A10, 0x004A2870, 0x004A0FE0]
CALLERS_OF = [0x004A1AB0, 0x004A0B80, 0x004A25C0, 0x004A1DD0]

def get_fn(addr):
    fn = getFunctionAt(addr)
    return fn if fn is not None else getFunctionContaining(addr)

def body(ifc, fn):
    r = ifc.decompileFunction(fn, 120, ConsoleTaskMonitor())
    return r.getDecompiledFunction().getC() if r.decompileCompleted() else ""

args = getScriptArgs()
out = PrintWriter(File(args[0]) if len(args) > 0 else File("dcode_display.txt"), "UTF-8")
ifc = DecompInterface()
try:
    ifc.openProgram(currentProgram)
    mon = ConsoleTaskMonitor()
    done = set()
    def emit(fn, why):
        if fn is None or fn.getEntryPoint() in done: return
        done.add(fn.getEntryPoint())
        out.printf("===== %s %s (%s) =====%n%s%n", fn.getEntryPoint(), fn.getName(), why, body(ifc, fn))
    for a in DIRECT:
        emit(get_fn(toAddr(a)), "direct")
    for a in CALLERS_OF:
        fn = get_fn(toAddr(a))
        if fn is None: continue
        out.printf("##### callers of %s %s:%n", fn.getEntryPoint(), fn.getName())
        for c in fn.getCallingFunctions(mon):
            out.printf("      %s %s%n", c.getEntryPoint(), c.getName())
        for c in fn.getCallingFunctions(mon):
            emit(c, "caller of " + fn.getName())
finally:
    ifc.dispose(); out.close()
print("done")
