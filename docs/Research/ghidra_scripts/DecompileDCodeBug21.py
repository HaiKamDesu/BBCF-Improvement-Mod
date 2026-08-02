# -*- coding: utf-8 -*-
# Ghidra headless Jython script, phase 21: locate the profile-UPLOAD builder.
# FUN_00423950 (TUS upload submit) is only reachable through the
# CUserManagedStorage vtable (+0x0C), so Ghidra shows no callers. Find it via
# xrefs to the wide-string filename table PTR_u_bbdc_dat_009df4bc that every
# request builder indexes, plus xrefs to the COnlineStorageTransfer facade
# singleton getter FUN_004B8F70, then decompile each and walk callers upward.
from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

DATA_ADDRS = [0x009df4bc]
FACADE_FNS = [0x004b8f70, 0x004b8eb0, 0x004b8ce0, 0x004b8c50, 0x004b8c40, 0x004b8b90]
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
report_file = File(args[0]) if len(args) > 0 else File("dcode_bug21.txt")
out = PrintWriter(report_file, "UTF-8")
ifc = DecompInterface()
try:
    if not ifc.openProgram(currentProgram):
        raise Exception("openProgram failed")
    out.printf("Program: %s%nImage base: %s%n%n", currentProgram.getName(), currentProgram.getImageBase())
    seen = set()

    out.println("===== FILENAME TABLE CONTENTS (wide strings) =====")
    for i in range(8):
        try:
            p = getInt(toAddr(0x009df4bc + i * 4)) & 0xffffffff
            a = toAddr(p)
            chars = []
            for j in range(0, 128, 2):
                lo = getByte(a.add(j)) & 0xff
                hi = getByte(a.add(j + 1)) & 0xff
                if lo == 0 and hi == 0:
                    break
                chars.append(chr(lo))
            out.printf("  [%d] %08X L\"%s\"%n", i, p, "".join(chars))
        except:
            out.printf("  [%d] <unreadable>%n", i)
            break
    out.println()

    frontier = []
    out.println("===== XREFS TO FILENAME TABLE =====")
    for d in DATA_ADDRS:
        for ref in getReferencesTo(toAddr(d)):
            fn = getFunctionContaining(ref.getFromAddress())
            if fn is None:
                continue
            k = fn.getEntryPoint().toString()
            out.printf("  from %s in %s%n", ref.getFromAddress(), fn.getName())
            if k not in seen:
                seen.add(k)
                frontier.append(fn)
    out.println()
    for fn in list(frontier):
        dec(out, ifc, fn, "(references filename table)")

    out.println("===== FACADE FUNCTION CALLERS =====")
    for a in FACADE_FNS:
        fn = get_fn(a)
        if fn is None:
            continue
        out.printf("--- callers of %s (%s) ---%n", fn.getEntryPoint(), fn.getName())
        for c in fn.getCallingFunctions(ConsoleTaskMonitor()):
            out.printf("  %s %s%n", c.getEntryPoint(), c.getName())
            k = c.getEntryPoint().toString()
            if k not in seen:
                seen.add(k)
                frontier.append(c)
        out.println()

    for level in range(1, LEVELS + 1):
        out.printf("===== UPWARD LEVEL %d =====%n", level)
        nxt = []
        for fn in frontier:
            callers = list(fn.getCallingFunctions(ConsoleTaskMonitor()))
            out.printf("--- callers of %s ---%n", fn.getName())
            for c in callers:
                out.printf("  %s %s%n", c.getEntryPoint(), c.getName())
                k = c.getEntryPoint().toString()
                if k not in seen:
                    seen.add(k)
                    nxt.append(c)
            out.println()
        frontier = nxt
        if not frontier:
            break
finally:
    ifc.dispose(); out.close()
print("done")
