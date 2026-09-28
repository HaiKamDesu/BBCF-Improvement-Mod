# -*- coding: utf-8 -*-
# Trigger timing RE (2026-09-27): list every reference to each address, and for data refs
# dump the neighbouring pointer table entries. args = <report> <hex addr>...
from java.io import File, PrintWriter
args = getScriptArgs()
out = PrintWriter(File(args[0]), "UTF-8")
rm = currentProgram.getReferenceManager()
mem = currentProgram.getMemory()
st = currentProgram.getSymbolTable()
try:
    for s in args[1:]:
        a = toAddr(int(s, 16))
        out.printf("=== refs to %s%n", a)
        for r in rm.getReferencesTo(a):
            fa = r.getFromAddress()
            f = getFunctionContaining(fa)
            out.printf("  from %s type=%s fn=%s%n", fa, r.getReferenceType(), f.getName() if f else "-")
            if f is None:
                base = fa.getOffset()
                for k in range(-12, 13):
                    p = base + k * 4
                    try:
                        v = mem.getInt(toAddr(p)) & 0xffffffff
                    except:
                        continue
                    sy = st.getPrimarySymbol(toAddr(v))
                    nm = ""
                    try:
                        # maybe a string pointer
                        b = []
                        for i in range(32):
                            c = mem.getByte(toAddr(v + i)) & 0xff
                            if c == 0: break
                            if c < 32 or c > 126: b = None; break
                            b.append(chr(c))
                        if b: nm = "".join(b)
                    except:
                        pass
                    out.printf("     [%08X] = %08X %s %s%n", p, v, sy.getName() if sy else "", nm)
finally:
    out.close()
