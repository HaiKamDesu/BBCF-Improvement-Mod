from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

# FUN_004a1110 is the function whose return value is written directly into the
# RoomMemberEntry.netcolor byte (local member, offset +0x5a) inside FUN_0049f990:
#   uVar1 = FUN_004a1110();
#   *(undefined1 *)(iVar6 + 0x5a) = uVar1;
# This is the actual RTT/ping -> discrete-tier bucketing function. Also grab its
# neighbors that write the adjacent fields (+0x58, +0x59, +0x5d, +0x5e) for
# context, since they're written in the same block and may be related counters.
TARGETS = [
    0x004a1110,  # writes netcolor byte (+0x5a)
    0x004a1480,  # writes +0x58
    0x004a11c0,  # writes +0x59
    0x004be030,  # gates +0x5d
    0x0049f990,  # the calling function itself (for full context, already partially seen)
]

args = getScriptArgs()
report_file = File(args[0]) if args else File(File(currentProgram.getExecutablePath()).getParentFile(), "NetColorComputeFnReport.txt")

ifc = DecompInterface()
ifc.openProgram(currentProgram)
monitor = ConsoleTaskMonitor()

out = PrintWriter(report_file, "UTF-8")
try:
    for addr_val in TARGETS:
        fn = getFunctionAt(toAddr(addr_val))
        out.printf("----- 0x%x: %s -----%n", addr_val, fn.getName() if fn else "<no function found at this address>")
        if not fn:
            continue
        res = ifc.decompileFunction(fn, 60, monitor)
        if res and res.decompileCompleted():
            out.printf("%s%n", res.getDecompiledFunction().getC())
        else:
            out.printf("  (decompile failed)%n")

    # Also find and decompile every caller of FUN_004a1110 (the netcolor compute fn)
    # to see if it's called from more than one place (e.g. also for opponents/remote
    # room members, not just the local one via FUN_0049f990).
    target = toAddr(0x004a1110)
    out.printf("%n===== Callers of FUN_004a1110 =====%n")
    caller_funcs = set()
    for r in getReferencesTo(target):
        from_addr = r.getFromAddress()
        fn = getFunctionContaining(from_addr)
        fn_name = fn.getName() if fn else "<no function>"
        out.printf("  %s  %s%n", from_addr, fn_name)
        if fn:
            caller_funcs.add(fn.getEntryPoint())
    for entry in caller_funcs:
        fn = getFunctionAt(entry)
        if not fn:
            continue
        out.printf("----- caller %s (%s) -----%n", fn.getName(), entry)
        res = ifc.decompileFunction(fn, 60, monitor)
        if res and res.decompileCompleted():
            out.printf("%s%n", res.getDecompiledFunction().getC())
finally:
    out.close()
    ifc.dispose()
