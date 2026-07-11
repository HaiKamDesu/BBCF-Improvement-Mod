# Decompile the RTT->delay-bucket converter and candidate accessors for the ranked delay column.
from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

TARGETS = [
    0x004a6620,  # RTT(ms) -> 0..4 delay bucket converter
    0x004a5450,  # candidate accessor (returns candidate object from index)
    0x004a8150,  # search step poll
    0x004ae6d0,  # other RMSR_CheckingRTT ref fn
]

def get_fn(a):
    f = getFunctionAt(toAddr(a))
    return f if f else getFunctionContaining(toAddr(a))

args = getScriptArgs()
report = File(args[0]) if args else File("ranked_delay2.txt")
out = PrintWriter(report, "UTF-8")
ifc = DecompInterface(); ifc.openProgram(currentProgram)
try:
    for a in TARGETS:
        fn = get_fn(a)
        if fn is None:
            out.printf("no fn at %s%n", toAddr(a)); continue
        out.printf("----- DECOMPILE %s %s -----%n", fn.getEntryPoint(), fn.getName())
        r = ifc.decompileFunction(fn, 120, ConsoleTaskMonitor())
        out.println(r.getDecompiledFunction().getC() if r.decompileCompleted() else ("FAIL "+r.getErrorMessage()))
        out.println()
finally:
    ifc.dispose(); out.close()
print("done")
