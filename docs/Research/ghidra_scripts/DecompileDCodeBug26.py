# -*- coding: utf-8 -*-
# Ghidra headless Jython script, phase 26: FUN_0042EDD0 (UploadTUS tick, item
# state 0) checksums *(workMgr+0xD8) BEFORE any Steam call, and fails
# immediately if invalid -- this is the actual mechanism behind the
# 2026-08-03 report's 7503 consecutive upload failures (TUS gate never
# latched). The buffer pointer traces back to FUN_0049D5C0(), called once at
# the top of FUN_004A96D0 (the upload trigger). Determine whether that
# pointer is the SAME persistent netUserData-adjacent memory read everywhere
# else in this investigation (meaning a standing corruption of the live
# profile) or something else (e.g. a dangling stack pointer).
from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

FUNCS = [0x0049d5c0, 0x0049a470, 0x0049a400]

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
report_file = File(args[0]) if len(args) > 0 else File("dcode_bug26.txt")
out = PrintWriter(report_file, "UTF-8")
ifc = DecompInterface()
try:
    if not ifc.openProgram(currentProgram):
        raise Exception("openProgram failed")
    out.printf("Program: %s%nImage base: %s%n%n", currentProgram.getName(), currentProgram.getImageBase())
    for a in FUNCS:
        fn = get_fn(a)
        if fn is None:
            out.printf("no fn at %s%n%n", toAddr(a))
            continue
        dec(out, ifc, fn)
finally:
    ifc.dispose(); out.close()
print("done")
