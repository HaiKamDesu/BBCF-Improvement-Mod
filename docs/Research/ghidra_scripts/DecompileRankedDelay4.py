# Find the row-array accessor (FUN_0046a820) and the functions that populate the
# 0x68-stride ranked search rows (writers of row+0x5C RTT and the SteamID field).
from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

ACCESSOR = 0x0046a820   # thunk_FUN_0046a820 target: returns row-array container base
# candidate/list helpers already known
EXTRA = [0x004a5450, 0x00486070, 0x0049a920, 0x004a40a0, 0x004a41d0, 0x004a4110, 0x004a4440]

def get_fn(a):
    f = getFunctionAt(toAddr(a))
    return f if f else getFunctionContaining(toAddr(a))

def decomp(out, ifc, fn, tag=""):
    out.printf("----- %s %s %s -----%n", fn.getEntryPoint(), fn.getName(), tag)
    r = ifc.decompileFunction(fn, 120, ConsoleTaskMonitor())
    out.println(r.getDecompiledFunction().getC() if r.decompileCompleted() else "FAIL")
    out.println()

args = getScriptArgs()
report = File(args[0]) if args else File("ranked_delay4.txt")
out = PrintWriter(report, "UTF-8")
ifc = DecompInterface(); ifc.openProgram(currentProgram)
try:
    acc = get_fn(ACCESSOR)
    out.println("===== ROW-ARRAY ACCESSOR =====")
    if acc: decomp(out, ifc, acc, "(row array container)")

    out.println("===== CALLERS OF ACCESSOR (candidates for row populate/render) =====")
    seen=set(); callers=[]
    for ref in getReferencesTo(toAddr(ACCESSOR)):
        fn = getFunctionContaining(ref.getFromAddress())
        if fn:
            k=fn.getEntryPoint().toString()
            out.printf("%s in %s%n", ref.getFromAddress(), fn.getName())
            if k not in seen:
                seen.add(k); callers.append(fn)
    out.println()
    out.println("===== DECOMPILED CALLERS =====")
    for fn in callers:
        decomp(out, ifc, fn)

    out.println("===== HELPER FUNCTIONS =====")
    for a in EXTRA:
        fn=get_fn(a)
        if fn: decomp(out, ifc, fn)
finally:
    ifc.dispose(); out.close()
print("done")
