# -*- coding: utf-8 -*-
# Trigger timing RE (2026-09-27): find raw little-endian dwords anywhere in the image and
# print a window of dwords (with C-string previews) around each hit. args = <report> <hex>...
from java.io import File, PrintWriter
import struct, jarray
args = getScriptArgs()
out = PrintWriter(File(args[0]), "UTF-8")
mem = currentProgram.getMemory()
def cstr(v):
    b = []
    try:
        for i in range(40):
            c = mem.getByte(toAddr(v + i)) & 0xff
            if c == 0: break
            if c < 32 or c > 126: return ""
            b.append(chr(c))
    except:
        return ""
    return "".join(b)
try:
    for s in args[1:]:
        v = int(s, 16)
        pat = jarray.array([((v >> (8*i)) & 0xff) - 256 if ((v >> (8*i)) & 0xff) > 127 else ((v >> (8*i)) & 0xff) for i in range(4)], 'b')
        a = mem.findBytes(currentProgram.getMinAddress(), pat, None, True, monitor)
        n = 0
        while a is not None and n < 20:
            out.printf("=== %08X found at %s%n", v, a)
            for k in range(-6, 7):
                p = a.getOffset() + k * 4
                try:
                    w = mem.getInt(toAddr(p)) & 0xffffffff
                except:
                    continue
                out.printf("   [%08X] %08X %s%n", p, w, cstr(w))
            a = mem.findBytes(a.add(1), pat, None, True, monitor)
            n += 1
finally:
    out.close()
