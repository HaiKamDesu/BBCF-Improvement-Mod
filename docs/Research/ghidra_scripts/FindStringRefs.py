# -*- coding: utf-8 -*-
# Trigger timing RE (2026-09-27): find ASCII strings and their references; for data refs,
# dump the surrounding pointer table (string pointers resolved). args = <report> <str>...
from java.io import File, PrintWriter
args = getScriptArgs()
out = PrintWriter(File(args[0]), "UTF-8")
rm = currentProgram.getReferenceManager()
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
        addr = currentProgram.getMinAddress()
        found = mem.findBytes(addr, (s + "\0").encode("ascii"), None, True, monitor)
        while found is not None:
            out.printf("=== '%s' at %s%n", s, found)
            for r in rm.getReferencesTo(found):
                fa = r.getFromAddress()
                f = getFunctionContaining(fa)
                out.printf("  from %s type=%s fn=%s%n", fa, r.getReferenceType(), f.getName() if f else "-")
            found = mem.findBytes(found.add(1), (s + "\0").encode("ascii"), None, True, monitor)
finally:
    out.close()
