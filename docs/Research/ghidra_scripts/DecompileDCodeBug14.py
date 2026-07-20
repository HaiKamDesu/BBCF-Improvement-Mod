# -*- coding: utf-8 -*-
# Ghidra headless Jython script, phase 14: AASTEAM_CUMSTask vtable + CCallResult handlers. Natural captures show done=1, errFlags=0x03 or 0x3B, steamEResult=0 (no Steam result ever delivered), recv=-1: the task aborts BEFORE a Steam result. Find the run method, what sets each +0xC0 bit, where +0xB8 EResult is written, and the abort-without-result condition (invalid UGC handle?).
# real failure is transport-level (recvSize=0, every fetch after one moment in
# the session fails persistently; retries useless). The transport is the
# GAMESTEAM_CCUMSTaskTransfer singleton (built by FUN_004B8F70, ctor
# FUN_004717C0 -> base ctor FUN_004B8B90). FUN_004B8CE0 polls via
# obj->vtbl[+8]() then tail-jump inner->vtbl[+0x18](); FUN_004B8EB0 submits via
# obj->vtbl[+0x18](...) then inner->vtbl[+0x10](), naming requests from a string
# table at 0x009DF4BC ("bbdc"...). Goal: dump both vtables, decompile their
# entries, find the error latch that wedges the session, and any reset path.

from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.program.model.symbol import SymbolType
from ghidra.util.task import ConsoleTaskMonitor


STRING_TABLE_ADDR = 0x009df4bc
STRING_TABLE_COUNT = 16
EXTRA_FUNCS = [
    0x004148f0,  # CCUMSTask ctor
    0x004b8b90,  # CCUMSTaskTransfer base ctor
]
VTABLE_MAX_ENTRIES = 24


def get_fn(addr_value):
    addr = toAddr(addr_value)
    fn = getFunctionAt(addr)
    if fn is None:
        fn = getFunctionContaining(addr)
    return fn


def decompile_fn(out, ifc, fn, note=""):
    out.printf("----- DECOMPILE %s %s -----%n", fn.getEntryPoint(), note)
    out.printf("Function: %s%n", fn.getName())
    result = ifc.decompileFunction(fn, 120, ConsoleTaskMonitor())
    if result.decompileCompleted():
        out.println(result.getDecompiledFunction().getC())
    else:
        out.printf("Decompile failed: %s%n", result.getErrorMessage())
    out.println()


def read_ptr(addr):
    return getInt(addr) & 0xffffffff


args = getScriptArgs()
if len(args) > 0:
    report_file = File(args[0])
else:
    report_file = File(File(currentProgram.getExecutablePath()).getParentFile(), "dcode_bug11_decompile.txt")

out = PrintWriter(report_file, "UTF-8")
ifc = DecompInterface()
try:
    if not ifc.openProgram(currentProgram):
        raise Exception("openProgram failed")

    out.printf("Program: %s%n", currentProgram.getName())
    out.printf("Image base: %s%n%n", currentProgram.getImageBase())

    # --- locate vtables by symbol name ---
    out.println("===== SYMBOLS matching CUMSTask =====")
    vtable_addrs = []
    st = currentProgram.getSymbolTable()
    it = st.getSymbolIterator()
    for sym in it:
        name = sym.getName(True)
        if "CUMSTask" in name:
            out.printf("  %s %s (%s)%n", sym.getAddress(), name, sym.getSymbolType())
            if "vftable" in name or "vtable" in name:
                vtable_addrs.append(sym.getAddress())
    out.println()

    decompiled = set()

    # --- dump vtable entries and decompile each ---
    for vt in vtable_addrs:
        out.printf("===== VTABLE at %s =====%n", vt)
        for i in range(VTABLE_MAX_ENTRIES):
            entry_addr = vt.add(i * 4)
            try:
                target = read_ptr(entry_addr)
            except:
                break
            fn = get_fn(target)
            if fn is None or fn.getEntryPoint().getOffset() != target:
                out.printf("  +0x%02X -> %08X (not a function entry, stop)%n", i * 4, target)
                break
            out.printf("  +0x%02X -> %08X %s%n", i * 4, target, fn.getName())
        out.println()
        for i in range(VTABLE_MAX_ENTRIES):
            entry_addr = vt.add(i * 4)
            try:
                target = read_ptr(entry_addr)
            except:
                break
            fn = get_fn(target)
            if fn is None or fn.getEntryPoint().getOffset() != target:
                break
            key = fn.getEntryPoint().toString()
            if key in decompiled:
                continue
            decompiled.add(key)
            decompile_fn(out, ifc, fn, "(vtbl+0x%02X of %s)" % (i * 4, vt))

    # --- extra functions ---
    for addr_value in EXTRA_FUNCS:
        fn = get_fn(addr_value)
        if fn is None:
            out.printf("----- %s -----%nNo function found.%n%n", toAddr(addr_value))
            continue
        key = fn.getEntryPoint().toString()
        if key not in decompiled:
            decompiled.add(key)
            decompile_fn(out, ifc, fn)

    # --- request-name string table ---
    out.printf("===== STRING TABLE at %08X =====%n", STRING_TABLE_ADDR)
    for i in range(STRING_TABLE_COUNT):
        try:
            p = read_ptr(toAddr(STRING_TABLE_ADDR + i * 4))
            chars = []
            a = toAddr(p)
            for j in range(64):
                b = getByte(a.add(j)) & 0xff
                if b == 0:
                    break
                chars.append(chr(b) if 32 <= b < 127 else "?")
            out.printf("  [%d] %08X \"%s\"%n", i, p, "".join(chars))
        except:
            out.printf("  [%d] <unreadable>%n", i)
            break
finally:
    ifc.dispose()
    out.close()

print("Wrote %s" % report_file.getAbsolutePath())
