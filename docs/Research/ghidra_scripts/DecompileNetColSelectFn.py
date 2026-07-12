from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

# Goal: fully decompile FUN_00655260 (the tiered net_col_def/A-G.hip icon-selection+draw
# helper used by the confirmed ranked-list row renderer FUN_00661060) to determine whether
# it uses the raw 0-7 tier byte (param_5, per prior session's manual stack-arg reconstruction)
# directly as an array index into the 8 sprite path strings, or applies some transform
# (clamp/modulo/lookup) before indexing. Also decompile its sibling FUN_00533d10 (the
# post-match-screen variant) for comparison, and dump the raw net_col_* string table bytes
# in the vicinity of 0x0094d088-0x0094d0e8 (per RankedDelayGhidraReport.txt) to confirm the
# 8 literal path strings are what we expect, in order.

TARGETS = [0x00655260, 0x00533d10]

args = getScriptArgs()
report_file = File(args[0]) if args else File(File(currentProgram.getExecutablePath()).getParentFile(), "report.txt")
out = PrintWriter(report_file, "UTF-8")

fm = currentProgram.getFunctionManager()
listing = currentProgram.getListing()

ifc = DecompInterface()
ifc.openProgram(currentProgram)
monitor = ConsoleTaskMonitor()

for target in TARGETS:
    fn = fm.getFunctionAt(toAddr(target))
    if fn is None:
        out.printf("!!! No function at 0x%08x\n" % target)
        continue
    out.printf("===== Decompile of %s (%s) =====\n" % (fn.getName(True), fn.getEntryPoint()))
    res = ifc.decompileFunction(fn, 60, monitor)
    if res.decompileCompleted():
        out.printf(res.getDecompiledFunction().getC())
    else:
        out.printf("!!! decompile failed: %s\n" % res.getErrorMessage())
    out.printf("\n\n")

    out.printf("===== Full raw disassembly of %s =====\n" % fn.getName(True))
    body = fn.getBody()
    ii = listing.getInstructions(body, True)
    for instr in ii:
        out.printf("%s  %s\n" % (instr.getAddress(), instr.toString()))
    out.printf("\n\n")

out.close()
