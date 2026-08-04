# -*- coding: utf-8 -*-
# Ghidra headless Jython script, phase 25: dump the FULL vtables of
# uei::ThinkLogicStrategyDownloadTUS and uei::ThinkLogicStrategyUploadTUS
# (only their trivial ctors were decompiled in phase 18; their real execute/
# callback methods -- where the shared work-manager state field at
# DAT_00A5A050+4 actually gets written -- were never examined). Also grab
# ThinkLogicStrategyBase and ThinkLogicStrategyIdle for the common interface.
from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

VTABLE_NAMES = [
    "uei::ThinkLogicStrategyBase::vftable",
    "uei::ThinkLogicStrategyDownloadTUS::vftable",
    "uei::ThinkLogicStrategyUploadTUS::vftable",
    "uei::ThinkLogicStrategyIdle::vftable",
]
MAX_ENTRIES = 16

def get_fn(addr):
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

def read_ptr(addr):
    return getInt(addr) & 0xffffffff

args = getScriptArgs()
report_file = File(args[0]) if len(args) > 0 else File("dcode_bug25.txt")
out = PrintWriter(report_file, "UTF-8")
ifc = DecompInterface()
try:
    if not ifc.openProgram(currentProgram):
        raise Exception("openProgram failed")
    out.printf("Program: %s%nImage base: %s%n%n", currentProgram.getName(), currentProgram.getImageBase())

    st = currentProgram.getSymbolTable()
    seen = set()
    for want in VTABLE_NAMES:
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
            entry_addr = found.add(i * 4)
            try:
                target = read_ptr(entry_addr)
            except:
                break
            fn = get_fn(toAddr(target))
            if fn is None or fn.getEntryPoint().getOffset() != target:
                out.printf("  +0x%02X -> %08X (not a function entry, stop)%n", i * 4, target)
                break
            out.printf("  +0x%02X -> %08X %s%n", i * 4, target, fn.getName())
        out.println()
        for i in range(MAX_ENTRIES):
            entry_addr = found.add(i * 4)
            try:
                target = read_ptr(entry_addr)
            except:
                break
            fn = get_fn(toAddr(target))
            if fn is None or fn.getEntryPoint().getOffset() != target:
                break
            k = fn.getEntryPoint().toString()
            if k in seen:
                continue
            seen.add(k)
            dec(out, ifc, fn, "(vtbl+0x%02X of %s)" % (i * 4, want))
finally:
    ifc.dispose(); out.close()
print("done")
