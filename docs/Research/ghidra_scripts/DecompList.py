# -*- coding: utf-8 -*-
# Decompiles a list of functions, with their callers and (optionally) their disassembly.
# args: <report path> <hex address>[,<hex address>...] [asm]
from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

args = getScriptArgs()
out = PrintWriter(File(args[0]), "UTF-8")
addrs = [int(a, 16) for a in args[1].split(",") if a]
with_asm = len(args) > 2 and args[2] == "asm"
ifc = DecompInterface()
ifc.openProgram(currentProgram)
mon = ConsoleTaskMonitor()
listing = currentProgram.getListing()
for a in addrs:
    fn = getFunctionAt(toAddr(a)) or getFunctionContaining(toAddr(a))
    if fn is None:
        out.printf("===== %08X: no function =====%n%n", a)
        continue
    out.printf("===== %s %s =====%n", fn.getEntryPoint(), fn.getName())
    out.printf("convention %s, size %d%n", fn.getCallingConventionName(), fn.getBody().getNumAddresses())
    out.printf("callers: %s%n", ", ".join(sorted(set("%s %s" % (c.getEntryPoint(), c.getName()) for c in fn.getCallingFunctions(mon)))))
    r = ifc.decompileFunction(fn, 180, mon)
    out.println(r.getDecompiledFunction().getC() if r.decompileCompleted() else "decompile failed")
    if with_asm:
        out.println("--- asm ---")
        for ins in listing.getInstructions(fn.getBody(), True):
            out.printf("%s  %s%n", ins.getAddress(), ins)
    out.println()
out.close()
