from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

# GAME_CNetworkServer::vftable (0089c9f4, 57 slots) and GAMESTEAM_CNetworkServer::vftable
# (0089cb4c, 57 slots) surfaced as the only ~57-slot classes with "NetworkServer" in the
# name (structural scan in RankedListEntryClassSearchGhidraReport.txt). ENTRY (the per-row
# ranked-list object returned by FUN_004a5450) is known to have vtable slots in active use
# at offsets 0x04, 0x10, 0x1c (tier getter, confirmed), 0x20, 0x24, 0x30, 0x34, 0x50, 0x58,
# 0x5c, 0xac, 0xb0 - far beyond CNetworkLobbyData/CSTEAMNetworkLobbyData's 36-slot vtables
# (already ruled out in an earlier session), and a 57-slot class is large enough to
# plausibly be it. This script dumps both vtables fully and decompiles the slots ENTRY is
# known to use, to check whether the *implementations* match ENTRY's known observed
# behavior (slot 7/+0x1c returns a small 0-7 byte with no args; slot 8/9 (+0x20/+0x24)
# return/format a member count; etc).

VTABLES = [
    (0x0089c9f4, "GAME_CNetworkServer"),
    (0x0089cb4c, "GAMESTEAM_CNetworkServer"),
]

args = getScriptArgs()
report_file = File(args[0]) if args else File(File(currentProgram.getExecutablePath()).getParentFile(), "report.txt")
out = PrintWriter(report_file, "UTF-8")

fm = currentProgram.getFunctionManager()
mem = currentProgram.getMemory()
ifc = DecompInterface()
ifc.openProgram(currentProgram)
monitor = ConsoleTaskMonitor()

INTERESTING_OFFSETS = [0x04, 0x10, 0x1c, 0x20, 0x24, 0x30, 0x34, 0x50, 0x58, 0x5c, 0xac, 0xb0]


def decompile(fn, label):
    out.printf("\n----- %s %s -----\n" % (fn.getEntryPoint(), label))
    res = ifc.decompileFunction(fn, 60, monitor)
    if res.decompileCompleted():
        out.print(res.getDecompiledFunction().getC())
    else:
        out.printf("<decompile failed: %s>\n" % res.getErrorMessage())
    out.flush()


for vt_addr_int, name in VTABLES:
    vt_addr = toAddr(vt_addr_int)
    out.printf("\n===== %s vftable at %s =====\n" % (name, vt_addr))
    addr = vt_addr
    slot_funcs = {}
    for i in range(64):
        try:
            ptr = mem.getInt(addr) & 0xFFFFFFFF
        except Exception:
            break
        try:
            target = toAddr(ptr)
        except Exception:
            break
        fn = fm.getFunctionAt(target)
        if fn is None:
            break
        slot_funcs[i] = fn
        marker = " <-- ENTRY uses this offset" if (i * 4) in INTERESTING_OFFSETS else ""
        out.printf("  slot %2d (off 0x%02x) -> %s %s%s\n" % (i, i * 4, fn.getEntryPoint(), fn.getName(), marker))
        addr = addr.add(4)
    out.flush()

    out.printf("\n  ---- Decompiling slots at ENTRY's known-used offsets for %s ----\n" % name)
    for off in INTERESTING_OFFSETS:
        idx = off // 4
        fn = slot_funcs.get(idx)
        if fn is None:
            out.printf("  offset 0x%02x: <no slot / out of range>\n" % off)
            continue
        decompile(fn, "%s slot %d (off 0x%02x): %s" % (name, idx, off, fn.getName()))

out.flush()
out.close()
ifc.dispose()
