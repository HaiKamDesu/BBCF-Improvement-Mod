# -*- coding: utf-8 -*-
# Ghidra headless Jython script: training "reset positions" direction handling.
# Decompiles _get_training_input_TRI_Posing_ (FUN_006CB490), its callers, and
# GameVals::reset_for_Training (FUN_0072CA20), to find where the held direction
# picks the reset position (left corner / middle / right corner).
from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

FUNCS = [0x006CB490, 0x0072CA20, 0x0055A0CC, 0x0055A4CC]

def get_fn(a):
    addr = toAddr(a)
    fn = getFunctionAt(addr)
    if fn is None:
        fn = getFunctionContaining(addr)
    return fn

def dec(out, ifc, fn, note=""):
    out.printf("----- DECOMPILE %s %s -----%n", fn.getEntryPoint(), note)
    out.printf("Function: %s%n", fn.getName())
    r = ifc.decompileFunction(fn, 180, ConsoleTaskMonitor())
    if r.decompileCompleted():
        out.println(r.getDecompiledFunction().getC())
    else:
        out.printf("Decompile failed: %s%n", r.getErrorMessage())
    out.println()

args = getScriptArgs()
report_file = File(args[0]) if len(args) > 0 else File("training_reset.txt")
out = PrintWriter(report_file, "UTF-8")
ifc = DecompInterface()
try:
    if not ifc.openProgram(currentProgram):
        raise Exception("openProgram failed")
    out.printf("Program: %s%nImage base: %s%n%n", currentProgram.getName(), currentProgram.getImageBase())
    seen = set()
    for a in FUNCS:
        fn = get_fn(a)
        if fn is None:
            out.printf("no function at %08x%n", a); continue
        k = fn.getEntryPoint().toString()
        if k in seen: continue
        seen.add(k)
        dec(out, ifc, fn)
        out.printf("  callers of %s:%n", fn.getName())
        for c in fn.getCallingFunctions(ConsoleTaskMonitor()):
            out.printf("    %s %s%n", c.getEntryPoint(), c.getName())
        out.println()
finally:
    ifc.dispose(); out.close()
print("done")
