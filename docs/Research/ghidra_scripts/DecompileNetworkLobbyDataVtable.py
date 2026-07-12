from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor
from ghidra.program.model.symbol import SymbolType

# Follow-up to SearchNamedSymbols.py, which searched sym.getName() (LOCAL name only)
# and found zero hits for "CNetwork"/"Lobby" even though RankedDelay5GhidraReport.txt's
# decompile literally shows `_DAT_00a5d270 = CNetworkLobbyData::vftable;`. Root cause:
# Ghidra's demangler places recovered C++ symbols (vftables, methods) inside a
# GhidraClass / Namespace named "CNetworkLobbyData" - Symbol.getName() only returns the
# child name ("vftable"), not the qualified path. This script uses getName(True) (fully
# qualified) so namespace-scoped symbols are actually matched, then:
#   1. Locates the CNetworkLobbyData namespace/class and every symbol inside it.
#   2. Resolves the vftable's address and dumps its function-pointer slots.
#   3. Decompiles every distinct vtable slot function.
#   4. Also fully-qualified-searches the same topic keywords as before, in case other
#      namespaced classes (e.g. a lobby *entry*/per-row class, distinct from the list
#      container) exist too.

KEYWORDS = [
    "Lobby", "Delay", "Ping", "Latency", "Connection", "Room", "List",
    "Matchmak", "Search", "Menu", "Scene", "Screen", "Panel", "P2P",
    "NetworkLobby", "CNetwork", "Network",
]

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


# ---- Part 1: fully-qualified keyword search ----
out.printf("===== FULLY-QUALIFIED keyword search (getName(True)) =====\n")
count = 0
all_syms = []
it = symtab.getSymbolIterator(True)
while it.hasNext():
    sym = it.next()
    all_syms.append(sym)

for kw in KEYWORDS:
    out.printf("----- keyword: %s -----\n" % kw)
    matches = []
    for sym in all_syms:
        fq = qname(sym)
        if kw.lower() in fq.lower():
            matches.append((sym.getAddress(), fq, sym.getSymbolType().toString()))
    matches.sort(key=lambda t: str(t[0]))
    for addr, fq, symtype in matches:
        out.printf("  %s  %-10s %s\n" % (addr, symtype, fq))
    out.printf("  (%d matches)\n\n" % len(matches))
    count += len(matches)
out.printf("TOTAL fully-qualified matches: %d\n\n" % count)

# ---- Part 2: enumerate all namespaces/classes whose name matches, list their children ----
out.printf("===== Namespaces/classes matching keywords =====\n")
symtab2 = currentProgram.getSymbolTable()
target_namespaces = []
for sym in all_syms:
    st = sym.getSymbolType()
    if st == SymbolType.CLASS or st == SymbolType.NAMESPACE:
        nm = sym.getName()
        for kw in KEYWORDS:
            if kw.lower() in nm.lower():
                target_namespaces.append(sym)
                break

for nssym in target_namespaces:
    ns = nssym.getObject()
    out.printf("Namespace: %s (id=%s)\n" % (qname(nssym), ns.getID()))
    children = symtab2.getSymbols(ns)
    for child in children:
        out.printf("    %s  %-10s %s\n" % (child.getAddress(), child.getSymbolType().toString(), qname(child)))
out.printf("\n")

# ---- Part 3: resolve CNetworkLobbyData::vftable specifically ----
out.printf("===== CNetworkLobbyData vftable resolution =====\n")
vftable_syms = [s for s in all_syms if "vftable" in qname(s).lower() and "cnetworklobbydata" in qname(s).lower()]
for s in vftable_syms:
    out.printf("Found vftable symbol: %s at %s\n" % (qname(s), s.getAddress()))

vt_addrs = [s.getAddress() for s in vftable_syms]

# If not found via symbol table, try to locate via the known static DAT_00a5d270 write site
# (FUN_00848550 in RankedDelay5GhidraReport.txt) by re-decompiling it and inspecting the
# high-level pcode / reference at that instruction for the actual vtable data address.
if not vt_addrs:
    out.printf("No direct vftable symbol found by name; falling back to xref-based resolution.\n")
    fn = getFunctionAt(toAddr(0x00848550))
    if fn is not None:
        refs = getReferencesFrom(fn.getEntryPoint())
        for r in refs:
            out.printf("  ref from entry: -> %s (%s)\n" % (r.getToAddress(), r.getReferenceType()))
        # Walk instructions in the function body for any data references
        instr = getInstructionAt(fn.getEntryPoint())
        while instr is not None and fn.getBody().contains(instr.getAddress()):
            for r in instr.getReferencesFrom():
                out.printf("  insn %s -> %s (%s)\n" % (instr.getAddress(), r.getToAddress(), r.getReferenceType()))
            instr = instr.getNext()

for vt_addr in vt_addrs:
    out.printf("\n--- Vtable at %s ---\n" % vt_addr)
    # Walk forward reading pointer-sized slots until we hit a non-function-pointer or a
    # data break (another defined data symbol / end of block), cap at 64 slots for safety.
    addr = vt_addr
    slot_funcs = []
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
        # Stop if this doesn't look like a function pointer into the executable image and
        # we already collected at least one slot (heuristic vtable-end detection).
        if fn_at_target is None and i > 0:
            out.printf("  slot %d @ %s: value 0x%08x is not a function entry - stopping (probable vtable end)\n" % (i, addr, ptr))
            break
        out.printf("  slot %d @ %s -> 0x%08x  %s\n" % (i, addr, ptr, fn_at_target.getName() if fn_at_target else "<none>"))
        if fn_at_target is not None:
            slot_funcs.append(fn_at_target)
        addr = addr.add(4)

    out.printf("\n  ---- Decompiling %d vtable slot functions ----\n" % len(slot_funcs))
    for fn_at_target in slot_funcs:
        out.printf("\n  ----- %s %s -----\n" % (fn_at_target.getEntryPoint(), fn_at_target.getName()))
        res = ifc.decompileFunction(fn_at_target, 60, monitor)
        if res.decompileCompleted():
            out.printf(res.getDecompiledFunction().getC())
        else:
            out.printf("  <decompile failed: %s>\n" % res.getErrorMessage())

out.close()
ifc.dispose()
