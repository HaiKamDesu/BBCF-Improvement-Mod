from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

# DecompileLobbyPingWritePath.py found a brand-new static singleton, DAT_00a25788 (RVA
# 0x625788, lazy-init guard DAT_00a291d8 bit 0, getter FUN_0041c900), distinct from the
# already-confirmed-dead DAT_00a5d270/RVA 0x65D270 CNetworkLobbyData container. A per-index
# array at container+0x129c, stride 0x68, gets a live elapsed-ticks-derived value written into
# it by CSTEAMNetworkLobbyData::vftable slot 32 (FUN_0046b9c0) - looks exactly like a
# per-lobby-row ping/RTT sample cache. This script finds every other reference to DAT_00a25788
# (not just through the FUN_0041c900 getter) to find any READ-side accessor (the UI code that
# displays the delay dots), and decompiles each.

TARGET = 0x00a25788

args = getScriptArgs()
report_file = File(args[0]) if args else File(File(currentProgram.getExecutablePath()).getParentFile(), "report.txt")
out = PrintWriter(report_file, "UTF-8")

ifc = DecompInterface()
ifc.openProgram(currentProgram)
monitor = ConsoleTaskMonitor()
fm = currentProgram.getFunctionManager()

target = toAddr(TARGET)
out.printf("===== All references to DAT_00a25788 =====\n")
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

out.printf("\n  ---- Decompiling %d unique referencing functions ----\n" % len(seen_callers))
for caddr in seen_callers:
    caller_fn = fm.getFunctionAt(caddr)
    out.printf("\n----- %s %s -----\n" % (caddr, caller_fn.getName()))
    res = ifc.decompileFunction(caller_fn, 60, monitor)
    if res.decompileCompleted():
        out.print(res.getDecompiledFunction().getC())
    else:
        out.printf("<decompile failed: %s>\n" % res.getErrorMessage())

out.close()
ifc.dispose()
