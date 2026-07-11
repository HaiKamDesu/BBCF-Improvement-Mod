# Find callers of the RTT->bucket converter FUN_004a6620 and trace the RTT source.
from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

CONVERTER = 0x004a6620

def get_fn(a):
    f = getFunctionAt(toAddr(a))
    return f if f else getFunctionContaining(toAddr(a))

args = getScriptArgs()
report = File(args[0]) if args else File("ranked_delay3.txt")
out = PrintWriter(report, "UTF-8")
ifc = DecompInterface(); ifc.openProgram(currentProgram)
try:
    out.println("===== CALLERS OF FUN_004a6620 (RTT->bucket) =====")
    seen = set()
    for ref in getReferencesTo(toAddr(CONVERTER)):
        fa = ref.getFromAddress()
        fn = getFunctionContaining(fa)
        name = fn.getName() if fn else "<none>"
        out.printf("%s in %s%n", fa, name)
        if fn: seen.add(fn.getEntryPoint().toString())

    # decompile each unique caller
    out.println("\n===== DECOMPILED CALLERS =====")
    done=set()
    for ref in getReferencesTo(toAddr(CONVERTER)):
        fn = getFunctionContaining(ref.getFromAddress())
        if not fn: continue
        k=fn.getEntryPoint().toString()
        if k in done: continue
        done.add(k)
        out.printf("----- %s %s -----%n", fn.getEntryPoint(), fn.getName())
        r = ifc.decompileFunction(fn, 120, ConsoleTaskMonitor())
        out.println(r.getDecompiledFunction().getC() if r.decompileCompleted() else "FAIL")
        out.println()
finally:
    ifc.dispose(); out.close()
print("done")
