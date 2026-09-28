# -*- coding: utf-8 -*-
# Trigger timing RE (2026-09-27): print a vtable's slots. args = <report> <vtable hex> <slot hex>...
from java.io import File, PrintWriter
args = getScriptArgs()
out = PrintWriter(File(args[0]), "UTF-8")
try:
    sym = [s for s in currentProgram.getSymbolTable().getSymbols("vftable")]
    for s in sym:
        out.printf("vftable symbol %s at %s%n", s.getParentNamespace().getName(), s.getAddress())
    vt = int(args[1], 16)
    mem = currentProgram.getMemory()
    for s in args[2:]:
        off = int(s, 16)
        v = mem.getInt(toAddr(vt + off)) & 0xffffffff
        f = getFunctionAt(toAddr(v))
        out.printf("slot +0x%x -> %08X %s%n", off, v, f.getName() if f else "?")
finally:
    out.close()
