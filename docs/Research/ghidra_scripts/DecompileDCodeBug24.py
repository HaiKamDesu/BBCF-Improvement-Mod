# -*- coding: utf-8 -*-
# Ghidra headless Jython script, phase 24: a NEW natural capture (2026-08-03,
# v8.2, third-party) shows a DIFFERENT failure shape than the DAT_00CF77A8
# latch: the opponent DOWNLOAD failed/exhausted, and ~2.5 min later the LOCAL
# PLAYER's own UPLOAD (bbdc.dat share) started failing EVERY attempt for the
# rest of the 52-minute session (7503 failures), each failing in ~3.5s -- yet
# the DAT_00CF77A8 "TUS disabled" gate NEVER latched (stayed 0 the whole time).
# So the existing auto-clear fix cannot even engage here.
# Both download and upload read/poll the SAME shared singleton's state field
# at DAT_00A5A050+4 (getter FUN_00427CD0). Find every WRITER of that field --
# not just its initializer -- to determine whether it can get stuck at a
# terminal value that blocks all future transfers regardless of the gate.
from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

STATE_FIELD_ADDR = 0x00a5a054   # DAT_00A5A050 + 4
MGR_BASE_ADDR = 0x00a5a050
WORKITEM_STRATEGY_CTORS = [0x00428d70, 0x00428fb0]  # DownloadTUS, UploadTUS

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
report_file = File(args[0]) if len(args) > 0 else File("dcode_bug24.txt")
out = PrintWriter(report_file, "UTF-8")
ifc = DecompInterface()
try:
    if not ifc.openProgram(currentProgram):
        raise Exception("openProgram failed")
    out.printf("Program: %s%nImage base: %s%n%n", currentProgram.getName(), currentProgram.getImageBase())
    seen = set()

    out.println("===== ALL XREFS TO DAT_00A5A050 (manager base) =====")
    seen_fns = {}
    for ref in getReferencesTo(toAddr(MGR_BASE_ADDR)):
        f = getFunctionContaining(ref.getFromAddress())
        nm = f.getName() if f else "<none>"
        out.printf("  %s in %s (%s)%n", ref.getFromAddress(), nm, ref.getReferenceType())
        if f:
            seen_fns[f.getEntryPoint().toString()] = f
    out.println()

    out.println("===== ALL XREFS TO DAT_00A5A054 (state field, base+4) =====")
    for ref in getReferencesTo(toAddr(STATE_FIELD_ADDR)):
        f = getFunctionContaining(ref.getFromAddress())
        nm = f.getName() if f else "<none>"
        out.printf("  %s in %s (%s)%n", ref.getFromAddress(), nm, ref.getReferenceType())
        if f:
            seen_fns[f.getEntryPoint().toString()] = f
    out.println()

    for k, f in seen_fns.items():
        if k in seen:
            continue
        seen.add(k)
        dec(out, ifc, f, "(touches work manager)")

    out.println("===== STRATEGY CLASS FULL VTABLES =====")
    for ctor_addr in WORKITEM_STRATEGY_CTORS:
        fn = get_fn(ctor_addr)
        if fn is None:
            continue
        # find the vtable install instruction's operand via decompile already done;
        # instead scan data refs FROM this function to any *vftable* symbol
        out.printf("--- refs from %s (%s) ---%n", fn.getEntryPoint(), fn.getName())
        insn = getInstructionAt(fn.getEntryPoint())
        addr = fn.getEntryPoint()
        end = fn.getBody().getMaxAddress()
        while addr is not None and addr <= end:
            for ref in getReferencesFrom(addr):
                toA = ref.getToAddress()
                sym = getSymbolAt(toA)
                if sym is not None and "vftable" in sym.getName():
                    out.printf("  %s -> %s%n", addr, sym.getName(True))
            nxt = getInstructionAt(addr)
            if nxt is None:
                break
            addr = nxt.getNext().getAddress() if nxt.getNext() else None
        out.println()
finally:
    ifc.dispose(); out.close()
print("done")
