from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

# Follow-up to DecompileNetworkLobbyDataVtable.py: the CNetworkLobbyData vtable at
# Ghidra addr 0x0089c7cc resolved these non-purecall slots:
#   slot0 0x0046a1a0  scalar-deleting destructor
#   slot1 0x004a2a50  aggregator: loops idx=0..9 calling vtable-slot2 (this,a,b,idx), sums
#   slot2 0x004a2970  real accessor: row = f(a,b) clamped <10; validity check at
#                      this+8+row*0xa8; returns DWORD at this+0x18+row*0xa8+col*0x10
#   slot6 0x0046a9a0  returns constant 0x20 (32)
#   slot7 0x004a2a00  returns constant 10
#   slot10 0x0046be80 returns constant 100
#   slot21 0x006a9da0 returns constant 0
# Also 0x0046a820 is the lazy-singleton accessor returning &DAT_00a5d270 (the CNetworkLobbyData
# instance itself, previously confirmed to read back all-zero at its OLD hand-guessed offset
# +0x1510/stride 0x68 - but slot2's real offsets are +8/stride 0xa8, untested).
#
# This script finds and decompiles every caller of: the singleton accessor (0046a820), the
# aggregator (004a2a50), and the real accessor (004a2970) directly (i.e. not just via vtable
# dispatch) - direct calls are far more informative than vtable calls since we can see literal
# argument values/semantics at the call site.

TARGETS = {
    0x0046a820: "CNetworkLobbyData_GetSingleton",
    0x004a2a50: "CNetworkLobbyData_slot1_Aggregate",
    0x004a2970: "CNetworkLobbyData_slot2_RowAccessor",
    0x0046a9a0: "CNetworkLobbyData_slot6_Const32",
    0x004a2a00: "CNetworkLobbyData_slot7_Const10",
}

args = getScriptArgs()
report_file = File(args[0]) if args else File(File(currentProgram.getExecutablePath()).getParentFile(), "report.txt")
out = PrintWriter(report_file, "UTF-8")

ifc = DecompInterface()
ifc.openProgram(currentProgram)
monitor = ConsoleTaskMonitor()
fm = currentProgram.getFunctionManager()

for tv, label in TARGETS.items():
    target = toAddr(tv)
    out.printf("===== Callers of %s (%s) =====\n" % (target, label))
    fn = fm.getFunctionAt(target)
    refs = list(getReferencesTo(target))
    out.printf("  (%d references)\n" % len(refs))
    seen_callers = set()
    for r in refs:
        from_addr = r.getFromAddress()
        caller_fn = getFunctionContaining(from_addr)
        cname = caller_fn.getName() if caller_fn else "<no function>"
        out.printf("  %s  %-12s caller=%s\n" % (from_addr, r.getReferenceType(), cname))
        if caller_fn is not None:
            seen_callers.add(caller_fn.getEntryPoint())

    out.printf("\n  ---- Decompiling %d unique caller functions ----\n" % len(seen_callers))
    for caddr in seen_callers:
        caller_fn = fm.getFunctionAt(caddr)
        out.printf("\n  ----- caller %s %s -----\n" % (caddr, caller_fn.getName()))
        res = ifc.decompileFunction(caller_fn, 60, monitor)
        if res.decompileCompleted():
            out.printf(res.getDecompiledFunction().getC())
        else:
            out.printf("  <decompile failed: %s>\n" % res.getErrorMessage())
    out.printf("\n\n")

out.close()
ifc.dispose()
