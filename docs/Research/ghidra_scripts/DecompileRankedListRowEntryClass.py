from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

# Follow-up to DecompileRankedListRowTierAccessor.py. That pass found:
#   FUN_004a7b40(param_1=row index):
#     mgr = FUN_00486070()-vtable-slot7()   # some manager singleton
#     if (uint)*(mgr+0xae8) <= param_1: return 0     # count field
#     entry_ptr = *(mgr + 0xaf4 + param_1*4)          # pointer array of row entries
#     return FUN_004a5450(entry_ptr)                   # wraps/validates -> "piVar7" in FUN_00657150
#
# And in FUN_00657150 (the ranked-list row renderer), for the row object returned above
# ("piVar7"), the connection-tier value fed into the net_col_A..G.hip icon selector comes from:
#   uVar2 = (**(code **)(*piVar7 + 0x1c))(local_1120)     // vtable slot (0x1c/4 = slot 7)
#   ... zero-extended byte, used as net-col tier index (0-7) for FUN_00655260
#
# This script:
#   1. Decompiles FUN_00486070 (the manager singleton accessor) to find its concrete address/RVA.
#   2. Decompiles FUN_004a5450 (the entry-pointer wrapper/validator).
#   3. Searches for a vtable near the row-entry class: since we don't have a `new`/constructor
#      address handy, this script instead lists callers of FUN_004a7b40 (the row-entry accessor)
#      more broadly (in case a caller constructs/registers entries, revealing the class name via
#      RTTI in a decompile) AND decompiles a few surrounding functions likely to be the
#      entry-registration/parsing path (any function that WRITES to the mgr+0xaf4 array or the
#      mgr+0xae8 count, found via direct xrefs to FUN_00486070 filtered to writers).

TARGETS = {
    0x00486070: "ManagerSingletonAccessor",
    0x004a5450: "EntryPtrWrapperValidator",
}

args = getScriptArgs()
report_file = File(args[0]) if args else File(File(currentProgram.getExecutablePath()).getParentFile(), "report.txt")
out = PrintWriter(report_file, "UTF-8")

ifc = DecompInterface()
ifc.openProgram(currentProgram)
monitor = ConsoleTaskMonitor()
fm = currentProgram.getFunctionManager()


def decompile_fn(fn):
    if fn is None:
        out.printf("  <no function>\n")
        return
    res = ifc.decompileFunction(fn, 90, monitor)
    if res.decompileCompleted():
        out.println(res.getDecompiledFunction().getC())
    else:
        out.printf("  <decompile failed: %s>\n" % res.getErrorMessage())


for tv, label in TARGETS.items():
    addr = toAddr(tv)
    fn = fm.getFunctionAt(addr)
    out.printf("===== %s: %s %s =====\n" % (label, addr, fn.getName(True) if fn else "<no function>"))
    decompile_fn(fn)
    out.printf("\n\n")

# List and decompile ALL callers of FUN_00486070 (the manager singleton) - the class it returns
# is used across many subsystems, but seeing what OTHER code does with its vtable slots 0-40 or so
# (via the offsets used at each call site) helps map the class layout beyond slot 7 alone.
target = toAddr(0x00486070)
out.printf("===== All callers of 0x00486070 (manager singleton accessor) =====\n")
refs = list(getReferencesTo(target))
out.printf("  (%d references)\n" % len(refs))
seen = set()
offset_uses = {}
for r in refs:
    caller_fn = getFunctionContaining(r.getFromAddress())
    if caller_fn is not None:
        seen.add(caller_fn.getEntryPoint())
out.printf("  -> %d unique caller functions (too many to decompile all; listing addresses only)\n" % len(seen))
for a in sorted(seen):
    out.printf("     %s\n" % a)

out.printf("\n===== Callers of FUN_004a7b40 (row-entry-by-index accessor) - already partly seen via FUN_00657150; listing ALL =====\n")
target2 = toAddr(0x004a7b40)
refs2 = list(getReferencesTo(target2))
out.printf("  (%d references)\n" % len(refs2))
seen2 = set()
for r in refs2:
    caller_fn = getFunctionContaining(r.getFromAddress())
    cname = caller_fn.getName(True) if caller_fn else "<no function>"
    out.printf("  %s  caller=%s\n" % (r.getFromAddress(), cname))
    if caller_fn is not None:
        seen2.add(caller_fn.getEntryPoint())
out.printf("\n  ---- Decompiling %d unique callers (excluding FUN_00657150 already seen) ----\n" % len(seen2))
for caddr in seen2:
    if caddr == toAddr(0x00657150):
        continue
    cf = fm.getFunctionAt(caddr)
    out.printf("\n  ----- caller %s %s -----\n" % (caddr, cf.getName(True)))
    decompile_fn(cf)

out.close()
ifc.dispose()
