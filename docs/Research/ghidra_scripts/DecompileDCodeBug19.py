# -*- coding: utf-8 -*-
# Ghidra headless Jython script, phase 19: what does the bbdc.dat transfer
# actually use, and WHO uploads our own profile?
# The 2026-07-30 third-party report (v8.1) refuted the "results stop being
# applied" theory (netcolor counter moved 53->52 DURING the wedge). New theory:
# the authoritative network profile is the 0x6800 blob stored as "bbdc.dat" in
# Steam Cloud; the UPLOAD (share) path is what makes progress durable and is
# completely uninstrumented. Here we: (a) decompile the type-7/8 strategy
# classes that perform the transfer, (b) list every reference to
# ISteamRemoteStorage-ish symbols to prove/disprove Steam Cloud, (c) find the
# callers of the share path FUN_004237B0 / FUN_00423A50 = who asks for the
# profile upload and when.
from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

FUNCTION_ADDRS = [
    0x00428d70,  # type 7 strategy ctor (download)
    0x00428fb0,  # type 8 strategy ctor (share)
]

# Walk callers upward this many levels for the share entry points.
SHARE_ENTRIES = [
    0x004237b0,  # share dispatcher (name check)
    0x00423a50,  # bbdc share impl
]

SYMBOL_SUBSTRINGS = ["RemoteStorage", "UGC", "FileShare", "FileWrite", "Cloud"]


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
report_file = File(args[0]) if len(args) > 0 else File("dcode_bug19.txt")
out = PrintWriter(report_file, "UTF-8")
ifc = DecompInterface()
try:
    if not ifc.openProgram(currentProgram):
        raise Exception("openProgram failed")
    out.printf("Program: %s%nImage base: %s%n%n", currentProgram.getName(), currentProgram.getImageBase())

    seen = set()
    for a in FUNCTION_ADDRS:
        fn = get_fn(a)
        if fn is None:
            out.printf("no fn at %s%n", toAddr(a)); continue
        k = fn.getEntryPoint().toString()
        if k in seen: continue
        seen.add(k); dec(out, ifc, fn)
        # also decompile its vtable-installed methods by following callees
        for callee in fn.getCalledFunctions(ConsoleTaskMonitor()):
            ck = callee.getEntryPoint().toString()
            if ck in seen: continue
            seen.add(ck); dec(out, ifc, callee, "(callee of %s)" % fn.getName())

    out.println("===== SHARE PATH CALLER CHAINS (2 levels, decompiled) =====")
    for a in SHARE_ENTRIES:
        fn = get_fn(a)
        if fn is None: continue
        out.printf("--- level1 callers of %s (%s) ---%n", fn.getEntryPoint(), fn.getName())
        lvl1 = list(fn.getCallingFunctions(ConsoleTaskMonitor()))
        for c in lvl1:
            out.printf("  %s %s%n", c.getEntryPoint(), c.getName())
        out.println()
        for c in lvl1:
            ck = c.getEntryPoint().toString()
            if ck not in seen:
                seen.add(ck); dec(out, ifc, c, "(caller of %s)" % fn.getName())
            out.printf("  --- level2 callers of %s ---%n", c.getName())
            for g in c.getCallingFunctions(ConsoleTaskMonitor()):
                out.printf("    %s %s%n", g.getEntryPoint(), g.getName())
            out.println()

    out.println("===== SYMBOLS matching Steam storage APIs =====")
    st = currentProgram.getSymbolTable()
    for sym in st.getSymbolIterator():
        n = sym.getName(True)
        for sub in SYMBOL_SUBSTRINGS:
            if sub in n:
                out.printf("  %s %s%n", sym.getAddress(), n)
                break
finally:
    ifc.dispose(); out.close()
print("done")
