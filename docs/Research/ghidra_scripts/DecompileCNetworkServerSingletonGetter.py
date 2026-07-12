from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

TARGETS = [0x008485a5, 0x0046cf20]

args = getScriptArgs()
report_file = File(args[0]) if args else File(File(currentProgram.getExecutablePath()).getParentFile(), "report.txt")
out = PrintWriter(report_file, "UTF-8")
fm = currentProgram.getFunctionManager()
ifc = DecompInterface()
ifc.openProgram(currentProgram)
monitor = ConsoleTaskMonitor()

for a in TARGETS:
    addr = toAddr(a)
    fn = getFunctionAt(addr)
    if fn is None:
        fn = fm.getFunctionContaining(addr)
    if fn is None:
        out.printf("----- %08x: no function (checking instruction) -----\n" % a)
        instr = getInstructionAt(addr)
        out.printf("  instr: %s\n\n" % instr)
        continue
    out.printf("----- %s %s -----\n" % (fn.getEntryPoint(), fn.getName()))
    res = ifc.decompileFunction(fn, 60, monitor)
    if res.decompileCompleted():
        out.print(res.getDecompiledFunction().getC())
    else:
        out.printf("<failed: %s>\n" % res.getErrorMessage())
    out.printf("\n\n")
    out.printf("  callers of %s:\n" % fn.getName())
    for r in getReferencesTo(fn.getEntryPoint()):
        cfn = fm.getFunctionContaining(r.getFromAddress())
        out.printf("    %s  from %s\n" % (r.getFromAddress(), cfn.getName() if cfn else "?"))
    out.printf("\n\n")

out.flush()
out.close()
ifc.dispose()
