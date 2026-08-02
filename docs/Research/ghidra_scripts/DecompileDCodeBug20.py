# -*- coding: utf-8 -*-
# Ghidra headless Jython script, phase 20: find the PROFILE UPLOAD trigger.
# Established: bbdc.dat is the network profile in TUS (Title User Storage) --
# strategies uei::ThinkLogicStrategyDownloadTUS (type 7) / UploadTUS (type 8).
# CUserManagedStorage vtbl+0x10 = FUN_00422A10 = download submit (used by the
# row fetch machine we already hook). vtbl+0x0C = FUN_00423950 = UPLOAD submit
# (+0x90 = buffer, +0x98 = size). The upload is what makes ranked/netcolor
# progress durable, and it is completely uninstrumented. Trace its callers up
# to the game logic that requests it (post-match commit?), so we can hook and
# log it -- and so we know whether a wedge silently drops profile uploads.
from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

ROOTS = [0x00423950]   # upload submit
MAX_LEVELS = 4

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
report_file = File(args[0]) if len(args) > 0 else File("dcode_bug20.txt")
out = PrintWriter(report_file, "UTF-8")
ifc = DecompInterface()
try:
    if not ifc.openProgram(currentProgram):
        raise Exception("openProgram failed")
    out.printf("Program: %s%nImage base: %s%n%n", currentProgram.getName(), currentProgram.getImageBase())

    seen = set()
    frontier = []
    for a in ROOTS:
        fn = get_fn(a)
        if fn:
            frontier.append(fn)
            seen.add(fn.getEntryPoint().toString())
            dec(out, ifc, fn, "(ROOT upload submit)")

    for level in range(1, MAX_LEVELS + 1):
        out.printf("===== CALLER LEVEL %d =====%n", level)
        nxt = []
        for fn in frontier:
            callers = list(fn.getCallingFunctions(ConsoleTaskMonitor()))
            out.printf("--- callers of %s (%s): %d ---%n", fn.getEntryPoint(), fn.getName(), len(callers))
            for c in callers:
                out.printf("  %s %s%n", c.getEntryPoint(), c.getName())
            out.println()
            for c in callers:
                k = c.getEntryPoint().toString()
                if k in seen:
                    continue
                seen.add(k)
                nxt.append(c)
                dec(out, ifc, c, "(level %d caller)" % level)
        if not nxt:
            out.println("(no further callers)")
            break
        frontier = nxt
finally:
    ifc.dispose(); out.close()
print("done")
