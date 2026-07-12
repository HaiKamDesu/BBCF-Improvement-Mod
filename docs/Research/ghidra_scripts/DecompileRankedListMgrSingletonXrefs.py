from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

# Follow-up: FUN_00486070 (the master network-list singleton accessor) returns &DAT_00c97e3c.
# FUN_004a7b40 (row-by-index accessor for the ranked-list) does:
#     iVar2 = (**(code **)(*(int *)*puVar1 + 0x1c))();   // vtable slot 7 (0x1c/4) of the
#                                                          // pointer STORED AT DAT_00c97e3c[0]
# i.e. DAT_00c97e3c's first 4 bytes hold a pointer to a polymorphic "network room-list manager"
# object, and slot 7 of ITS vtable returns the struct with the +0xae0/+0xae8/+0xaec/+0xaf0/+0xaf4
# fields (list head / count / cursor cache / sorted-index array) used to fetch individual row
# entries. The row entries themselves are intrusive-list nodes whose OWN vtable slot 7 (same
# offset, different class) is what returns the net-col tier byte (0-7) feeding the ranked list's
# delay-dot icon (see DecompileNetColIconCallers.py / RankedListRowTierAccessorGhidraReport.txt).
#
# Since DAT_00c97e3c is a runtime-populated global (zero at image time), Ghidra's static bytes
# there are useless - what we need is the CONSTRUCTOR/writer of DAT_00c97e3c[0] (find via data
# xrefs, not just the accessor's own call xrefs) to identify the concrete submanager class by
# name/vtable address, and separately the atexit teardown (LAB_008486d0 per the accessor) which
# conventionally resets these pointers and often literally assigns `= SomeClass::vftable`-shaped
# constants making the class identifiable even without a constructor.

DAT_ADDR = 0x00c97e3c
GUARD_ADDR = 0x00c97e40
TEARDOWN_LAB = 0x008486d0

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


def dump_xrefs_and_callers(addr_val, label):
    target = toAddr(addr_val)
    out.printf("===== Data xrefs to %s (%s) =====\n" % (target, label))
    refs = list(getReferencesTo(target))
    out.printf("  (%d references)\n" % len(refs))
    seen = set()
    for r in refs:
        caller_fn = getFunctionContaining(r.getFromAddress())
        cname = caller_fn.getName(True) if caller_fn else "<no function>"
        out.printf("  %s  %-16s caller=%s\n" % (r.getFromAddress(), r.getReferenceType(), cname))
        if caller_fn is not None:
            seen.add(caller_fn.getEntryPoint())
    out.printf("\n  ---- Decompiling %d unique referencing functions ----\n" % len(seen))
    for caddr in seen:
        cf = fm.getFunctionAt(caddr)
        out.printf("\n  ----- %s %s -----\n" % (caddr, cf.getName(True)))
        decompile_fn(cf)
    out.printf("\n\n")


dump_xrefs_and_callers(DAT_ADDR, "DAT_00c97e3c (master netlist singleton storage)")
dump_xrefs_and_callers(GUARD_ADDR, "DAT_00c97e40 (lazy-init guard byte)")

out.printf("===== Teardown function at %s =====\n" % toAddr(TEARDOWN_LAB))
tfn = getFunctionContaining(toAddr(TEARDOWN_LAB))
out.printf("  containing function: %s\n" % (tfn.getName(True) if tfn else "<none>"))
if tfn is not None:
    decompile_fn(tfn)

out.close()
ifc.dispose()
