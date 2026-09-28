# -*- coding: utf-8 -*-
# Trigger timing RE (2026-09-27): decompile the move gate FUN_005D0690 and whatever the
# caller passes; args = <report path> <hex addr> [<hex addr> ...]. Prints callers too.
from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

def get_fn(a):
    addr = toAddr(a)
    fn = getFunctionAt(addr)
    if fn is None:
        fn = getFunctionContaining(addr)
    return fn

args = getScriptArgs()
out = PrintWriter(File(args[0]), "UTF-8")
ifc = DecompInterface()
try:
    ifc.openProgram(currentProgram)
    for s in args[1:]:
        a = int(s, 16)
        fn = get_fn(a)
        if fn is None:
            out.printf("no function at %08X%n", a); continue
        out.printf("----- DECOMPILE %s -----%n", fn.getEntryPoint())
        out.printf("Function: %s%n", fn.getName())
        r = ifc.decompileFunction(fn, 240, ConsoleTaskMonitor())
        if r.decompileCompleted():
            out.println(r.getDecompiledFunction().getC())
        else:
            out.printf("Decompile failed: %s%n", r.getErrorMessage())
        out.printf("  callers of %s:%n", fn.getName())
        for c in fn.getCallingFunctions(ConsoleTaskMonitor()):
            out.printf("    %s %s%n", c.getEntryPoint(), c.getName())
        out.println()
finally:
    ifc.dispose(); out.close()
print("done")
