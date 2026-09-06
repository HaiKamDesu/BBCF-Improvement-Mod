# -*- coding: utf-8 -*-
# Phase 27 (2026-09-06 capture): the D-Code fetch does NOT go through Steam UGC.
# uei::ThinkLogicStrategyDownloadTUS::Tick (FUN_00428AC0) issues a
# uei::tl::ReadTusRequestParam and writes the shared work-manager state field
# (DAT_00A5A050+4): 7 = ok, 9 = wrong length, 0xB = request refused / TUS error.
# The live capture shows state 9 (single fast attempt, 0 bytes). This phase
# decompiles the TUS request layer itself.
from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

FUNCS = [
    (0x00434750, "issue TUS request (returns 0 -> mgr+4=0xB)"),
    (0x00433fe0, "poll TUS result (out buf/len, +0x108 err)"),
    (0x00432e10, "request param helper / ctor"),
    (0x004284a0, "called each poll tick of DownloadTUS"),
    (0x00428780, "error reporter (takes a string ptr)"),
    (0x00428950, "success-path notify(steamId)"),
    (0x0040df10, "profile checksum16 validator"),
    (0x00427ef0, "workmgr: start bbdc download"),
    (0x0042edd0, "UploadTUS tick"),
    (0x00434d30, "sibling of 00432e10 (from atexit)"),
]
STRING_PTRS = [0x008503dc, 0x00850400, 0x008503c0, 0x00850420]
VTABLES = ["uei::tl::ReadTusRequestParam::vftable"]
MAX_ENTRIES = 16

def get_fn(addr):
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

def dump_bytes(out, addr, n, label):
    try:
        vals = []
        for i in range(n):
            vals.append("%02X" % (getByte(addr.add(i)) & 0xff))
        out.printf("%s @%s: %s%n", label, addr, " ".join(vals))
        chars = []
        for i in range(n):
            b = getByte(addr.add(i)) & 0xff
            chars.append(chr(b) if 32 <= b < 127 else ".")
        out.printf("%s ascii: %s%n", label, "".join(chars))
    except:
        out.printf("%s @%s: unreadable%n", label, addr)

args = getScriptArgs()
out = PrintWriter(File(args[0]) if len(args) > 0 else File("dcode_bug27.txt"), "UTF-8")
ifc = DecompInterface()
try:
    if not ifc.openProgram(currentProgram):
        raise Exception("openProgram failed")
    out.printf("Program: %s%nImage base: %s%n%n", currentProgram.getName(), currentProgram.getImageBase())

    out.println("===== ERROR / TAG STRINGS =====")
    for p in STRING_PTRS:
        dump_bytes(out, toAddr(p), 48, "%08X" % p)
    out.println()

    st = currentProgram.getSymbolTable()
    for want in VTABLES:
        found = None
        for sym in st.getAllSymbols(True):
            if sym.getName(True) == want:
                found = sym.getAddress()
                break
        if found is None:
            out.printf("SYMBOL NOT FOUND: %s%n%n", want)
            continue
        out.printf("===== %s at %s =====%n", want, found)
        for i in range(MAX_ENTRIES):
            target = getInt(found.add(i * 4)) & 0xffffffff
            fn = get_fn(toAddr(target))
            if fn is None or fn.getEntryPoint().getOffset() != target:
                out.printf("  +0x%02X -> %08X (not a function entry, stop)%n", i * 4, target)
                break
            out.printf("  +0x%02X -> %08X %s%n", i * 4, target, fn.getName())
        out.println()

    for addr, note in FUNCS:
        fn = get_fn(toAddr(addr))
        if fn is None:
            out.printf("----- %08X: no function -----%n%n", addr)
            continue
        dec(out, ifc, fn, "(%s)" % note)
        out.printf("--- callees of %08X ---%n", addr)
        for c in fn.getCalledFunctions(ConsoleTaskMonitor()):
            out.printf("  %s %s%n", c.getEntryPoint(), c.getName())
        out.printf("--- callers of %08X ---%n", addr)
        for c in fn.getCallingFunctions(ConsoleTaskMonitor()):
            out.printf("  %s %s%n", c.getEntryPoint(), c.getName())
        out.println()
finally:
    ifc.dispose(); out.close()
print("done")
