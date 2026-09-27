# -*- coding: utf-8 -*-
# Exit crash 2026-09-27 00:54:38: the game showed "Thread Delete Time Out"
# (MessageBoxA from 0x007EEE95 <- 0x007BADC4 <- 0x007BA99B on the main
# thread) while a worker thread (proc 0x007BADD0, body around 0x007BAA87)
# crashed in RtlEnterCriticalSection on an already-deleted critical section.
# Decompile the thread-delete path and the worker body to see which thread
# it is and what it waits on.
from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

ADDRS = [0x0041AA87, 0x0041ADD0, 0x0041AD90, 0x0041A99B, 0x0044EE10, 0x0041FB00]  # on-disk VAs (runtime base 0x7A0000 -> 0x400000)

def get_fn(addr):
    fn = getFunctionAt(addr)
    if fn is None:
        fn = getFunctionContaining(addr)
    return fn

def body(ifc, fn):
    r = ifc.decompileFunction(fn, 120, ConsoleTaskMonitor())
    return r.getDecompiledFunction().getC() if r.decompileCompleted() else ""

args = getScriptArgs()
out = PrintWriter(File(args[0]) if len(args) > 0 else File("exit_thread_timeout.txt"), "UTF-8")
ifc = DecompInterface()
try:
    ifc.openProgram(currentProgram)
    mon = ConsoleTaskMonitor()
    done = set()
    for a in ADDRS:
        fn = get_fn(toAddr(a))
        if fn is None:
            out.printf("----- %08X: no function -----%n", a); continue
        if fn.getEntryPoint() in done: continue
        done.add(fn.getEntryPoint())
        out.printf("===== %08X -> %s %s =====%n%s%n", a, fn.getEntryPoint(), fn.getName(), body(ifc, fn))
        out.println("  callers:")
        for up in fn.getCallingFunctions(mon):
            out.printf("      %s %s%n", up.getEntryPoint(), up.getName())
        out.println()
    # thread creation sites: who passes 0x007BADD0 as a thread proc?
    out.println("===== references to 0x007BADD0 (thread proc) =====")
    for ref in getReferencesTo(toAddr(0x007BADD0)):
        f = getFunctionContaining(ref.getFromAddress())
        out.printf("  from %s in %s%n", ref.getFromAddress(), f.getName() if f else "?")
finally:
    ifc.dispose(); out.close()
print("done")
