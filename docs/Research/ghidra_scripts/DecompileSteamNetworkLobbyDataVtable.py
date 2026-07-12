from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

# CNetworkLobbyData::vftable (0089c7cc) turned out to be mostly __purecall stubs - a base/
# abstract interface. A sibling class CSTEAMNetworkLobbyData::vftable exists at 0089c88c and
# shares several DATA-reference targets with the base (004a2a00 "const 10", 004a2a50
# "aggregate", 004a2970 "row accessor", 0046a9a0 "const 32") per
# NetworkLobbyDataCallersGhidraReport.txt. That means CSTEAMNetworkLobbyData very likely
# *overrides* the slots that were pure-virtual in the base (the real per-lobby-row data
# reads), while inheriting the shared helper slots. Dump CSTEAMNetworkLobbyData::vftable's
# full slot table and decompile every override to find the real implementations.

VTABLE_ADDR = 0x0089c88c

args = getScriptArgs()
report_file = File(args[0]) if args else File(File(currentProgram.getExecutablePath()).getParentFile(), "report.txt")
out = PrintWriter(report_file, "UTF-8")

mem = currentProgram.getMemory()
fm = currentProgram.getFunctionManager()
ifc = DecompInterface()
ifc.openProgram(currentProgram)
monitor = ConsoleTaskMonitor()

addr = toAddr(VTABLE_ADDR)
slot_funcs = []
out.printf("--- Vtable at %s (CSTEAMNetworkLobbyData) ---\n" % addr)
for i in range(64):
    try:
        ptr = mem.getInt(addr) & 0xFFFFFFFF
    except Exception:
        out.printf("  slot %d @ %s: read failed, stopping\n" % (i, addr))
        break
    target = None
    try:
        target = toAddr(ptr)
    except Exception:
        pass
    fn_at_target = fm.getFunctionAt(target) if target is not None else None
    if fn_at_target is None and i > 0:
        out.printf("  slot %d @ %s: value 0x%08x is not a function entry - stopping (probable vtable end)\n" % (i, addr, ptr))
        break
    out.printf("  slot %d @ %s -> 0x%08x  %s\n" % (i, addr, ptr, fn_at_target.getName() if fn_at_target else "<none>"))
    if fn_at_target is not None:
        slot_funcs.append((i, fn_at_target))
    addr = addr.add(4)

out.printf("\n  ---- Decompiling %d vtable slot functions ----\n" % len(slot_funcs))
for i, fn_at_target in slot_funcs:
    out.printf("\n  ----- slot %d: %s %s -----\n" % (i, fn_at_target.getEntryPoint(), fn_at_target.getName()))
    res = ifc.decompileFunction(fn_at_target, 60, monitor)
    if res.decompileCompleted():
        out.print(res.getDecompiledFunction().getC())
    else:
        out.printf("  <decompile failed: %s>\n" % res.getErrorMessage())

out.close()
ifc.dispose()
