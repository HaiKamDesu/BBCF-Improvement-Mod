from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

# Working backward from the ranked-search-list row render function (FUN_00657150, found via
# xrefs to the net_col_A..G.hip tier-icon draw helper FUN_00655260). Manual disasm tracing of the
# call site at 006579CF (call 00655260) showed the decompiler's own 4-arg rendering of that call
# is INCOMPLETE - raw stack math ("add esp,18h" after the call = 24 bytes = 6 dwords) proves 6
# args are really passed, matching FUN_00655260's own recovered signature (param_5 = net-col
# tier index 0-7, used to select "net_col_A.hip".."net_col_G.hip"/def). Manually walking the
# pushes backward from the call site in program order and mapping cdecl right-to-left push order
# to parameter position identified param_5 (index) as coming from `uVar2`, the (zero-extended
# byte) return value of a vtable dispatch:
#     (**(code **)(*piVar7 + 0x1c))(local_1120)
# where piVar7 = FUN_004a7b40(iVar6) (iVar6 being the per-row loop index/room handle), i.e. THIS
# is the row's own object, and vtable slot (0x1c/4 = slot 7) is the tier getter for THIS specific
# row - not the already-examined-and-dead CNetworkLobbyData/DAT_00a5d270 container.
#
# This script:
#   1. Decompiles FUN_004a7b40 (0x004a7b40) to see what it returns / how it indexes.
#   2. Finds references TO 0x004a7b40 more broadly (other callers, for corroboration).
#   3. Attempts to resolve the runtime type of its return value by finding the vtable-assignment
#      constructor: searches data at typical vtable-pointer-store patterns is out of scope for a
#      single pass, so as a concrete/cheap next step this script instead decompiles the raw
#      instruction bytes' surrounding function (FUN_00657150 in full) so the loop's iVar6/iVar4
#      indexing (room handle vs row index) is fully visible, and separately decompiles
#      FUN_004a7b40 and any small helper it calls, to look for a struct-offset read that would
#      reveal the tier value's real backing storage (if FUN_004a7b40 does more than vtable
#      dispatch itself).

TARGETS = {
    0x004a7b40: "RowObjectAccessor_by_index",
    0x00657150: "RankedListRowRender_FUN_00657150_FULL",
}

args = getScriptArgs()
report_file = File(args[0]) if args else File(File(currentProgram.getExecutablePath()).getParentFile(), "report.txt")
out = PrintWriter(report_file, "UTF-8")

ifc = DecompInterface()
ifc.openProgram(currentProgram)
monitor = ConsoleTaskMonitor()
fm = currentProgram.getFunctionManager()


def decompile_addr(addr_val, label):
    addr = toAddr(addr_val)
    fn = fm.getFunctionAt(addr)
    out.printf("===== %s: %s %s =====\n" % (label, addr, fn.getName(True) if fn else "<no function>"))
    if fn is None:
        out.printf("  <no function at this address>\n")
        return
    res = ifc.decompileFunction(fn, 90, monitor)
    if res.decompileCompleted():
        out.println(res.getDecompiledFunction().getC())
    else:
        out.printf("  <decompile failed: %s>\n" % res.getErrorMessage())


for tv, label in TARGETS.items():
    decompile_addr(tv, label)
    out.printf("\n\n")

# Also list callers of FUN_004a7b40 for corroboration of what object type it deals in.
target = toAddr(0x004a7b40)
out.printf("===== Callers of 0x004a7b40 =====\n")
seen = set()
for r in getReferencesTo(target):
    caller_fn = getFunctionContaining(r.getFromAddress())
    cname = caller_fn.getName(True) if caller_fn else "<no function>"
    out.printf("  %s  %-16s caller=%s\n" % (r.getFromAddress(), r.getReferenceType(), cname))
    if caller_fn is not None:
        seen.add(caller_fn.getEntryPoint())
out.printf("\n  ---- Decompiling %d unique callers ----\n" % len(seen))
for caddr in seen:
    cf = fm.getFunctionAt(caddr)
    out.printf("\n  ----- caller %s %s -----\n" % (caddr, cf.getName(True)))
    res = ifc.decompileFunction(cf, 90, monitor)
    if res.decompileCompleted():
        out.println(res.getDecompiledFunction().getC())
    else:
        out.printf("  <decompile failed: %s>\n" % res.getErrorMessage())

out.close()
ifc.dispose()
