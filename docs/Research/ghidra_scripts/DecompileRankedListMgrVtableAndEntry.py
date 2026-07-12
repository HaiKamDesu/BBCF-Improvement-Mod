from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

# Goal: identify the ranked-list MGR's concrete class (via the LIVE vtable address logged
# by DiagnosticLogRankedListMgrSlot(): mgr=0x0438E480 vtable=0x00CFC3A4 - a real runtime
# pointer, but since this game's module loads at a fixed 0x00400000 base with no ASLR
# (per bbcf-re-workflow SKILL.md), 0x00CFC3A4 is directly a valid Ghidra static address -
# it points at the module's own .rdata vtable, not a heap allocation), dump its full vtable,
# and try to locate the ENTRY (per-row) class's vtable/constructor by tracing where the
# intrusive linked list (head at listStruct+0xae0, next/prev at node+4/+8) actually gets
# populated - i.e. find whoever calls something like "push/insert node" writes to +0xae0
# or allocates+links nodes, which is likely the concrete class' constructor call site.

MGR_VTABLE_ADDR = 0x00CFC3A4          # live-confirmed vtable pointer value (already a static addr)
MGR_SLOT7_OFFSET = 0x1c               # MGR's own vtable slot 7 (confirmed: returns listStruct, no args)
WALK_ROW_LIST_FN = 0x004a5450          # FUN_004a5450, confirmed: this=listStruct, arg=underlyingIndex -> ENTRY ptr
ROW_ENTRY_ACCESSOR_FN = 0x004a7b40     # FUN_004a7b40, confirmed: singleton -> mgr slot7 -> count/perm array -> walk

args = getScriptArgs()
report_file = File(args[0]) if args else File(File(currentProgram.getExecutablePath()).getParentFile(), "report.txt")
out = PrintWriter(report_file, "UTF-8")

symtab = currentProgram.getSymbolTable()
listing = currentProgram.getListing()
fm = currentProgram.getFunctionManager()
mem = currentProgram.getMemory()

ifc = DecompInterface()
ifc.openProgram(currentProgram)
monitor = ConsoleTaskMonitor()


def qname(sym):
    try:
        return sym.getName(True)
    except Exception:
        return sym.getName()


def decompile(fn, label=None):
    out.printf("\n----- %s %s -----\n" % (fn.getEntryPoint(), label or fn.getName()))
    res = ifc.decompileFunction(fn, 90, monitor)
    if res.decompileCompleted():
        out.print(res.getDecompiledFunction().getC())
    else:
        out.printf("<decompile failed: %s>\n" % res.getErrorMessage())


def dump_vtable(vt_addr, max_slots=48):
    out.printf("\n--- Vtable dump at %s ---\n" % vt_addr)
    addr = vt_addr
    slot_funcs = []
    for i in range(max_slots):
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
            out.printf("  slot %d @ %s: value 0x%08x not a function entry - stopping\n" % (i, addr, ptr))
            break
        out.printf("  slot %2d (off 0x%02x) @ %s -> 0x%08x  %s\n" % (
            i, i * 4, addr, ptr, fn_at_target.getName() if fn_at_target else "<none>"))
        if fn_at_target is not None:
            slot_funcs.append((i, fn_at_target))
        addr = addr.add(4)
    return slot_funcs


# ---- Part 0: what symbol/RTTI, if any, sits at the live vtable address ----
out.printf("===== Symbol/RTTI lookup at live MGR vtable addr %s =====\n" % toAddr(MGR_VTABLE_ADDR))
vt_addr = toAddr(MGR_VTABLE_ADDR)
syms_at = symtab.getSymbols(vt_addr)
found_any = False
for s in syms_at:
    found_any = True
    out.printf("  symbol: %s  type=%s\n" % (qname(s), s.getSymbolType().toString()))
if not found_any:
    out.printf("  <no symbol defined at this exact address>\n")

# RTTI Complete Object Locator is conventionally 4 bytes before the vtable (MSVC layout)
out.printf("\n  Checking for RTTI Complete Object Locator ptr 4 bytes before vtable:\n")
try:
    col_ptr_addr = vt_addr.subtract(4)
    col_ptr = mem.getInt(col_ptr_addr) & 0xFFFFFFFF
    out.printf("    [vtable-4] = 0x%08x\n" % col_ptr)
    if col_ptr != 0 and mem.contains(toAddr(col_ptr)):
        col_addr = toAddr(col_ptr)
        col_syms = symtab.getSymbols(col_addr)
        for s in col_syms:
            out.printf("      symbol at COL: %s\n" % qname(s))
        if mem.contains(col_addr) and mem.contains(col_addr.add(15)):
            sig = mem.getInt(col_addr) & 0xFFFFFFFF
            type_desc_field = mem.getInt(col_addr.add(12)) & 0xFFFFFFFF
            out.printf("      COL.signature=0x%x COL.pTypeDescriptor_field=0x%x\n" % (sig, type_desc_field))
    else:
        out.printf("    <not a valid in-image pointer, skipping COL parse - probably no RTTI here>\n")
except Exception as e:
    out.printf("    <failed: %s>\n" % e)
out.flush()

# ---- Part 1: dump MGR's full vtable ----
out.printf("\n===== MGR vtable slot dump (%s) =====\n" % vt_addr)
mgr_slots = dump_vtable(vt_addr, 48)

out.printf("\n===== Decompiling all resolved MGR vtable slot functions =====\n")
seen = set()
for idx, fn in mgr_slots:
    key = fn.getEntryPoint()
    tag = " <-- SLOT 7, confirmed listStruct getter" if idx == 7 else ""
    if key in seen:
        out.printf("\n  (slot %d reuses already-listed %s%s)\n" % (idx, fn.getName(), tag))
        continue
    seen.add(key)
    decompile(fn, "slot %d: %s%s" % (idx, fn.getName(), tag))

# ---- Part 2: decompile the row-entry accessor and row-list walker again for cross-reference ----
out.printf("\n===== Re-decompiling FUN_004a7b40 (row-entry accessor) and FUN_004a5450 (list walker) =====\n")
fn = getFunctionAt(toAddr(ROW_ENTRY_ACCESSOR_FN))
if fn is not None:
    decompile(fn, "FUN_004a7b40 row-entry-by-index accessor")
fn = getFunctionAt(toAddr(WALK_ROW_LIST_FN))
if fn is not None:
    decompile(fn, "FUN_004a5450 list walker")

# ---- Part 3: search for candidate constructors that populate the intrusive list ----
# Heuristic: find functions containing a store to [reg+4] and [reg+8] near each other
# (next/prev init) combined with a store to some +0xae0-shaped list-head field, OR any
# function that writes an immediate/constant into offset 0 of a freshly allocated block
# (vtable assignment) whose size roughly matches a list node. This is a broad sweep;
# report raw candidates without deep filtering, deep review will be manual.
out.printf("\n===== Searching all functions for 'operator new' followed by a vtable-store pattern (constructor candidates) =====\n")
fm2 = currentProgram.getFunctionManager()
op_new_fn = None
op_new_syms = [s for s in symtab.getSymbolIterator(True) if 'operator.new' in qname(s).lower() or qname(s).lower() == 'operator new']
for s in op_new_syms:
    out.printf("  operator new candidate symbol: %s @ %s\n" % (qname(s), s.getAddress()))

out.flush()
out.close()
ifc.dispose()
