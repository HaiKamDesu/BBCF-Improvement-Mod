from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

# Follow-up: FUN_0046e790 (GAMESTEAM_CNetworkServer method) reads its own +0xc78/+0xc7c as a
# 2-dword key fed into FUN_0046e9e0 (the same identity-keyed lookup ENTRY's +0x114 substruct
# also feeds). Raw-disasm grep for literal "0xc78"/"0xc7c" operands found two call sites that
# PUSH both fields as arguments (0046D2D4/0046D2E0 and 0046D5A6/0046D5B2) - decompiling their
# containing functions to see what's called with this pair, which may reveal the real semantic
# meaning (e.g. a CSteamID passed from a Steam P2P callback).

CANDIDATE_ADDRS = [0x0046D2D4, 0x0046D5A6, 0x0046f720]

args = getScriptArgs()
report_file = File(args[0]) if args else File(File(currentProgram.getExecutablePath()).getParentFile(), "report.txt")
out = PrintWriter(report_file, "UTF-8")

fm = currentProgram.getFunctionManager()
ifc = DecompInterface()
ifc.openProgram(currentProgram)
monitor = ConsoleTaskMonitor()


def decompile(fn, label=None):
    out.printf("\n----- %s %s -----\n" % (fn.getEntryPoint(), label or fn.getName()))
    res = ifc.decompileFunction(fn, 90, monitor)
    if res.decompileCompleted():
        out.print(res.getDecompiledFunction().getC())
    else:
        out.printf("<decompile failed: %s>\n" % res.getErrorMessage())


seen = set()
for av in CANDIDATE_ADDRS:
    addr = toAddr(av)
    fn = fm.getFunctionContaining(addr)
    if fn is None:
        out.printf("\n(no function found containing %s)\n" % addr)
        continue
    key = fn.getEntryPoint()
    if key in seen:
        continue
    seen.add(key)
    decompile(fn, "%s (contains queried addr %s)" % (fn.getName(), addr))

    # also show callers of this function for more context
    out.printf("\n  callers of %s:\n" % fn.getName())
    for r in getReferencesTo(fn.getEntryPoint()):
        caller = fm.getFunctionContaining(r.getFromAddress())
        out.printf("    %s from %s\n" % (r.getFromAddress(), caller.getName() if caller else "<none>"))

out.close()
ifc.dispose()
