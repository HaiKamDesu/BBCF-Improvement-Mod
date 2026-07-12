# Find every function referencing the ranked row-array container global DAT_00a5d270
# and decompile them, so we can spot the row populate function (writes row+0x5C RTT,
# row+0x48 netcol) and locate the SteamID / lobby / accountID field in the 0x68 row.
from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

GLOBAL = 0x00a5d270   # &DAT_00a5d270 -> +0x1510 = row array, stride 0x68

def decomp(out, ifc, fn):
    out.printf("----- %s %s -----%n", fn.getEntryPoint(), fn.getName())
    r = ifc.decompileFunction(fn, 150, ConsoleTaskMonitor())
    out.println(r.getDecompiledFunction().getC() if r.decompileCompleted() else "FAIL")
    out.println()

args = getScriptArgs()
report = File(args[0]) if args else File("ranked_delay5.txt")
out = PrintWriter(report, "UTF-8")
ifc = DecompInterface(); ifc.openProgram(currentProgram)
try:
    out.println("===== REFERENCES TO DAT_00a5d270 =====")
    seen=set(); fns=[]
    for ref in getReferencesTo(toAddr(GLOBAL)):
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
