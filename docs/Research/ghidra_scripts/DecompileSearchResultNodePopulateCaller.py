from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

# Goal: confirm FUN_0046d890 (the sole caller of FUN_0046fcc0, the HOST_NETCOLOR/tier-byte
# populate function) is genuinely "create/populate a ranked search-result row from a fresh
# Steam lobby-list fetch", not some unrelated path, by decompiling it and its own callers.

TARGETS = [0x0046d890]

args = getScriptArgs()
report_file = File(args[0]) if args else File(File(currentProgram.getExecutablePath()).getParentFile(), "report.txt")
out = PrintWriter(report_file, "UTF-8")

fm = currentProgram.getFunctionManager()
ifc = DecompInterface()
ifc.openProgram(currentProgram)
monitor = ConsoleTaskMonitor()

for addr in TARGETS:
    fn = fm.getFunctionAt(toAddr(addr))
    if fn is None:
        out.printf("!!! No function at 0x%08x\n" % addr)
        continue
    out.printf("===== Decompile of %s (%s) =====\n" % (fn.getName(True), fn.getEntryPoint()))
    res = ifc.decompileFunction(fn, 60, monitor)
    if res.decompileCompleted():
        out.print(res.getDecompiledFunction().getC())
    else:
        out.printf("!!! decompile failed: %s\n" % res.getErrorMessage())
    out.printf("\n\n===== Callers of %s =====\n" % fn.getName(True))
    refs = getReferencesTo(fn.getEntryPoint())
    for ref in refs:
        callerFn = fm.getFunctionContaining(ref.getFromAddress())
        if callerFn is not None:
            out.printf("  called from %s at %s\n" % (callerFn.getName(True), ref.getFromAddress()))
        else:
            out.printf("  called from (no function) at %s\n" % ref.getFromAddress())
    out.printf("\n\n")

out.close()
