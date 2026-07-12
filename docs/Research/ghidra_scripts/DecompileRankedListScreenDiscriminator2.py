from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

# Follow-up to DecompileRankedListScreenDiscriminator.py: that pass showed the
# DAT_00cf7958 global (kRankedNetworkStructRva) is accessed as "this" through a
# getter function FUN_004a77e0 (returns &DAT_00cf7958). We need to find every
# CALLER of that getter, decompile them, and specifically look for writes to
# offset +0x4 (state1) or other offsets in range 0x0-0x120 to find whichever
# function actually TRANSITIONS state1 between values (the true screen/menu
# switcher), since that call site's arguments are the best lead for a
# screen/menu-id parameter.

GETTER = 0x004A77E0

args = getScriptArgs()
report_file = File(args[0]) if args else File(File(currentProgram.getExecutablePath()).getParentFile(), "report.txt")
ifc = DecompInterface()
ifc.openProgram(currentProgram)
monitor = ConsoleTaskMonitor()
out = PrintWriter(report_file, "UTF-8")

target = toAddr(GETTER)
seen_funcs = {}
for r in getReferencesTo(target):
    fromAddr = r.getFromAddress()
    fn = getFunctionContaining(fromAddr)
    if fn is not None:
        seen_funcs[fn.getEntryPoint().getOffset()] = fn

out.printf("===== %d unique callers of getter FUN_004a77e0 =====\n" % len(seen_funcs))
for off, fn in seen_funcs.items():
    out.printf("caller: %s @ %s\n" % (fn.getName(), fn.getEntryPoint()))

out.printf("\n===== Full decompiles =====\n")
for off, fn in seen_funcs.items():
    out.printf("===== %s @ %s =====\n" % (fn.getName(), fn.getEntryPoint()))
    res = ifc.decompileFunction(fn, 60, monitor)
    if res and res.decompileCompleted():
        code = res.getDecompiledFunction().getC()
        out.printf("%s\n" % code)
    else:
        out.printf("DECOMPILE FAILED for %s\n" % fn.getName())

out.close()
ifc.dispose()
