from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

# CSTEAMNetworkLobbyData::vftable slot 32 (FUN_0046b9c0, Ghidra addr 0x0046b9c0) computes an
# elapsed-time-in-ms value from __Xtime_get_ticks() minus a timestamp read out of its param_2
# struct (+8/+0xc), then WRITES that computed value into
#   FUN_0041c900()_container + 0x129c + (param_2+4)*0x68
# - a completely different array (different base pointer, different stride 0x68 vs the
# this+8/stride-0xa8 array in slot 2) from anything tried before. This looks exactly like a
# live per-index round-trip-time/ping write path. This script:
#   1. Decompiles FUN_0041c900 to find what static/global pointer it actually returns (so we
#      can compute its RVA for a live read).
#   2. Finds and decompiles every caller of FUN_0046b9c0 (both directly and via the vtable
#      slot-32 offset 0x80) to learn what "index" and "timestamp" actually mean (e.g. is this
#      called once per received network packet from a specific opponent/row).
#   3. Also finds callers of FUN_004a2970 (slot 2, the this+8/stride 0xa8 accessor) beyond what
#      the previous callers pass found (that search only found DATA xrefs from vtables - this
#      searches indirect-call sites that dereference through a vtable pointer, which the
#      previous script's straight getReferencesTo(target) approach cannot find, so this widens
#      the net to plain textual disassembly matches for "0x46b9c0"/"0x4a2970" as call targets,
#      including thunks).

TARGET_FUN_0041c900 = 0x0041c900
TARGET_SLOT32 = 0x0046b9c0

args = getScriptArgs()
report_file = File(args[0]) if args else File(File(currentProgram.getExecutablePath()).getParentFile(), "report.txt")
out = PrintWriter(report_file, "UTF-8")

ifc = DecompInterface()
ifc.openProgram(currentProgram)
monitor = ConsoleTaskMonitor()
fm = currentProgram.getFunctionManager()

def decompile_and_print(addr_val, label):
    fn = fm.getFunctionAt(toAddr(addr_val))
    if fn is None:
        out.printf("No function at %s (%s)\n" % (hex(addr_val), label))
        return
    out.printf("\n----- %s %s -----\n" % (fn.getEntryPoint(), fn.getName()))
    res = ifc.decompileFunction(fn, 60, monitor)
    if res.decompileCompleted():
        out.print(res.getDecompiledFunction().getC())
    else:
        out.printf("<decompile failed: %s>\n" % res.getErrorMessage())

out.printf("===== FUN_0041c900 (container getter used by slot32 write path) =====\n")
decompile_and_print(TARGET_FUN_0041c900, "container getter")

out.printf("\n\n===== Callers of FUN_0046b9c0 (slot32 ping/delay write) =====\n")
target = toAddr(TARGET_SLOT32)
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
    out.printf("\n----- caller %s %s -----\n" % (caddr, caller_fn.getName()))
    res = ifc.decompileFunction(caller_fn, 60, monitor)
    if res.decompileCompleted():
        out.print(res.getDecompiledFunction().getC())
    else:
        out.printf("<decompile failed: %s>\n" % res.getErrorMessage())

out.close()
ifc.dispose()
