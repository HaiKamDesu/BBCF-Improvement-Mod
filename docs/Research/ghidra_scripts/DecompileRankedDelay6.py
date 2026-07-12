# Callers of the row-array accessor thunk FUN_004a29f0. One of them populates the
# 0x68-stride ranked rows. Decompile all; we look for writes to +0x5C (RTT), +0x48
# (netcol) and a 64-bit SteamID / 32-bit accountID stored per row.
from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

THUNK = 0x004a29f0

def decomp(out, ifc, fn):
    out.printf("----- %s %s -----%n", fn.getEntryPoint(), fn.getName())
    r = ifc.decompileFunction(fn, 150, ConsoleTaskMonitor())
    out.println(r.getDecompiledFunction().getC() if r.decompileCompleted() else "FAIL")
    out.println()

args = getScriptArgs()
report = File(args[0]) if args else File("ranked_delay6.txt")
out = PrintWriter(report, "UTF-8")
ifc = DecompInterface(); ifc.openProgram(currentProgram)
try:
    out.println("===== CALLERS OF THUNK FUN_004a29f0 =====")
    seen=set(); fns=[]
    for ref in getReferencesTo(toAddr(THUNK)):
        fa=ref.getFromAddress(); fn=getFunctionContaining(fa)
        out.printf("%s %s in %s%n", fa, ref.getReferenceType(), fn.getName() if fn else "<none>")
        if fn:
            k=fn.getEntryPoint().toString()
            if k not in seen: seen.add(k); fns.append(fn)
    out.println("\n===== DECOMPILED =====")
    for fn in fns:
        decomp(out, ifc, fn)
finally:
    ifc.dispose(); out.close()
print("done")
