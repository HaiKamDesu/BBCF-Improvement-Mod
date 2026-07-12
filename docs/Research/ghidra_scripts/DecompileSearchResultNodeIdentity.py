from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

# Follow-up to DecompileRankedListEntryVtable.py: GAMESTEAM_SearchResultNode confirmed as the
# ENTRY class (RTTI-resolved). Slot 4 (offset 0x10 in the vtable, function FUN_0046e860) copies
# a 2-dword pair out of a sub-object at entry+0x114 (fields +0xc/+0x10 of that sub-object) - this
# exact shape (entry+0x114 -> [+0xc],[+0x10] as a 2-dword identity key) was independently found in
# an earlier session feeding GAMESTEAM_CNetworkServer's own per-peer RTT-sample lookup
# (FUN_0046e9e0), which is strong evidence +0x114 is a shared "peer identity" pointer used for
# connection-quality correlation across subsystems - the most promising identity-field lead so far.
#
# This script:
#   1. Decompiles FUN_0046dc00 (called right after vtable assignment in the ENTRY constructor
#      FUN_0046f680 - the likely field-initializer).
#   2. Decompiles the functions containing raw-disasm hits near 0x0046DB74/0x0046DD18/0x0046DFE4/
#      0x0046E281/0x0046E863 (all "+0x114" accesses clustered right around ENTRY's own vtable
#      slot functions - i.e. likely ENTRY's own methods, not unrelated code).
#   3. Decompiles FUN_0046e9e0 again (the confirmed identity-keyed RTT lookup from the earlier
#      session) for side-by-side comparison.
#   4. Dumps whatever object address 0046e860 uses to see if the +0x114 sub-object has more
#      identity-shaped fields than just +0xc/+0x10, by inspecting all functions that reference
#      that sub-object's other offsets.

CANDIDATE_ADDRS = [
    0x0046DB74, 0x0046DD18, 0x0046DFE4, 0x0046E281, 0x0046E863,
    0x0046dc00, 0x0046e9e0, 0x0046e860,
]

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
        out.printf("\n(%s already decompiled above, containing function for %s)\n" % (fn.getName(), addr))
        continue
    seen.add(key)
    decompile(fn, "%s (contains queried addr %s)" % (fn.getName(), addr))

out.close()
ifc.dispose()
