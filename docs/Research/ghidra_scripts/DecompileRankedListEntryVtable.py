from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

# Goal (RankedListConnectionFilter_Progress.md, 2026-07-12 session): the mod's own diagnostic
# (DiagnosticLogRankedListMgrSlot) now safely logs the ranked-list ENTRY object's own vtable RVA,
# computed entirely from within the mod's own process (entryVtable_address - moduleBase) - zero
# live-debugger risk, unlike the CDB breakpoint attempt that crashed the game twice (see that
# section in the progress doc - CLOSED, do not repeat).
#
# Fresh DEBUG.txt confirms this RVA is STABLE: 0x49cc34 -> Ghidra address 0x0049CC34.
#
# This script:
#   1. Resolves whatever symbol/RTTI sits at 0x0049CC34 (namespace-aware, sym.getName(True) -
#      a flat getName() search missed CNetworkLobbyData::vftable in an earlier session, see
#      "Namespace-aware Ghidra pass" section of the progress doc).
#   2. Dumps ALL vtable slots (not just slot 7), decompiling every non-__purecall slot.
#   3. Finds the constructor(s) that write this vtable address into an object (data xrefs to
#      the vtable address itself), to recover the class's raw field layout.
#   4. Greps every decompiled slot body for Steam API import calls and for suspicious 8-byte /
#      two-dword identity-shaped field accesses, to hunt for a steamId/lobbyId field.

ENTRY_VTABLE_ADDR = 0x0089CC34   # base 0x00400000 + RVA 0x49cc34, per fresh DEBUG.txt
                                  # NOTE: correcting an arithmetic error from the initial task
                                  # hand-off, which wrote this sum as 0x0049CC34 - that's wrong,
                                  # 0x00400000 + 0x0049CC34 = 0x0089CC34 (carry from the 4+8=C
                                  # nibble), confirmed by python3 -c "print(hex(0x00400000+0x0049CC34))".
                                  # 0x0049CC34 itself lands mid-function inside .text
                                  # (FUN_0049cb90) - not a vtable at all. 0x0089CC34 lands in
                                  # .rdata (0x84a000-0x9d2600), a plausible vtable location.

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


def decompile_text(fn, timeout=90):
    res = ifc.decompileFunction(fn, timeout, monitor)
    if res.decompileCompleted():
        return res.getDecompiledFunction().getC()
    return "<decompile failed: %s>\n" % res.getErrorMessage()


def dump_vtable(vt_addr, max_slots=80):
    out.printf("\n--- Vtable dump at %s ---\n" % vt_addr)
    addr = vt_addr
    slots = []  # (idx, offset, fn_or_none, raw_ptr, is_purecall)
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
            out.printf("  slot %d @ %s: value 0x%08x not a function entry - stopping (probable vtable end)\n" % (i, addr, ptr))
            break
        fname = fn_at_target.getName() if fn_at_target else "<none>"
        is_purecall = fn_at_target is not None and "purecall" in fname.lower()
        tag = " [PURECALL]" if is_purecall else ""
        out.printf("  slot %2d (off 0x%02x) @ %s -> 0x%08x  %s%s\n" % (
            i, i * 4, addr, ptr, fname, tag))
        slots.append((i, i * 4, fn_at_target, ptr, is_purecall))
        addr = addr.add(4)
    return slots


# ---- Part 0: symbol / RTTI resolution at the vtable address ----
out.printf("===== ENTRY vtable resolution at %s (RVA 0x49cc34) =====\n" % toAddr(ENTRY_VTABLE_ADDR))
vt_addr = toAddr(ENTRY_VTABLE_ADDR)

syms_at = list(symtab.getSymbols(vt_addr))
if syms_at:
    for s in syms_at:
        out.printf("  symbol at vtable addr: %s  type=%s\n" % (qname(s), s.getSymbolType().toString()))
else:
    out.printf("  <no symbol defined exactly at this address>\n")

# RTTI Complete Object Locator is conventionally 4 bytes before the vtable (MSVC layout)
out.printf("\n  Checking for RTTI Complete Object Locator ptr 4 bytes before vtable:\n")
class_name_guess = None
try:
    col_ptr_addr = vt_addr.subtract(4)
    col_ptr = mem.getInt(col_ptr_addr) & 0xFFFFFFFF
    out.printf("    [vtable-4] = 0x%08x\n" % col_ptr)
    if col_ptr != 0 and mem.contains(toAddr(col_ptr)):
        col_addr = toAddr(col_ptr)
        col_syms = symtab.getSymbols(col_addr)
        for s in col_syms:
            out.printf("      symbol at COL: %s\n" % qname(s))
            class_name_guess = qname(s)
        if mem.contains(col_addr) and mem.contains(col_addr.add(15)):
            sig = mem.getInt(col_addr) & 0xFFFFFFFF
            type_desc_field = mem.getInt(col_addr.add(12)) & 0xFFFFFFFF
            out.printf("      COL.signature=0x%x COL.pTypeDescriptor_field=0x%x\n" % (sig, type_desc_field))
            if mem.contains(toAddr(type_desc_field)):
                td_syms = symtab.getSymbols(toAddr(type_desc_field))
                for s in td_syms:
                    out.printf("      symbol at TypeDescriptor: %s\n" % qname(s))
                    class_name_guess = class_name_guess or qname(s)
    else:
        out.printf("    <not a valid in-image pointer, skipping COL parse - probably no RTTI here>\n")
except Exception as e:
    out.printf("    <failed: %s>\n" % e)
out.flush()

# Also: namespace-aware fully-qualified search for ANY symbol whose address equals or is near
# the vtable (covers cases where Ghidra names the vtable symbol something other than a plain
# "vftable" child, or names it via a differently-cased namespace).
out.printf("\n  Namespace-aware fully-qualified scan for symbols within +/- 0x40 of vtable addr:\n")
it = symtab.getSymbolIterator(True)
nearby = []
while it.hasNext():
    sym = it.next()
    try:
        addr = sym.getAddress()
        if addr is None:
            continue
        if addr.getAddressSpace() != vt_addr.getAddressSpace():
            continue
        diff = addr.subtract(vt_addr)
        if -0x40 <= diff <= 0x40:
            nearby.append((diff, addr, qname(sym), sym.getSymbolType().toString()))
    except Exception:
        continue
nearby.sort(key=lambda t: t[0])
for diff, addr, fq, st in nearby:
    out.printf("    diff=%+d  %s  %-10s %s\n" % (diff, addr, st, fq))
out.flush()

# ---- Part 1: dump ALL vtable slots ----
out.printf("\n===== ENTRY vtable slot dump =====\n")
slots = dump_vtable(vt_addr, 80)
out.printf("\nTotal slots read: %d ; purecall stubs: %d ; real implementations: %d\n" % (
    len(slots), sum(1 for s in slots if s[4]), sum(1 for s in slots if s[2] is not None and not s[4])))

# ---- Part 2: decompile every non-purecall slot ----
out.printf("\n===== Decompiling every non-__purecall ENTRY vtable slot =====\n")
seen = set()
steam_api_hits = []
identity_field_hits = []
STEAM_KEYWORDS = ["Steam", "GetFriend", "GetSteamID", "ISteam", "PersonaName", "SteamAPI"]
for idx, off, fn, ptr, is_purecall in slots:
    if fn is None or is_purecall:
        continue
    key = fn.getEntryPoint()
    tag = " <-- SLOT 7 (offset 0x1c), CONFIRMED tier getter (0-7 byte, zero args, thiscall)" if off == 0x1c else ""
    if key in seen:
        out.printf("\n  (slot %d / off 0x%02x reuses already-decompiled %s%s)\n" % (idx, off, fn.getName(), tag))
        continue
    seen.add(key)
    out.printf("\n----- slot %d (offset 0x%02x) %s @ %s%s -----\n" % (idx, off, fn.getName(), fn.getEntryPoint(), tag))
    body = decompile_text(fn)
    out.print(body)

    for kw in STEAM_KEYWORDS:
        if kw.lower() in body.lower():
            steam_api_hits.append((idx, off, fn.getName(), kw))

    # crude heuristic: look for 8-byte-shaped access patterns - two adjacent dword field refs,
    # or a field offset comment/pattern that looks like it could carry a 64-bit ID (printed
    # separately below via a dedicated offset-frequency pass, this is just an inline keyword net)
    for marker in ["0110", "SteamID", "steamid", "steamId", "lobbyId", "LobbyId", "AccountId"]:
        if marker in body:
            identity_field_hits.append((idx, off, fn.getName(), marker))

out.printf("\n===== Steam-API-keyword hits across ENTRY vtable slots =====\n")
if steam_api_hits:
    for idx, off, fname, kw in steam_api_hits:
        out.printf("  slot %d (off 0x%02x) %s : keyword '%s'\n" % (idx, off, fname, kw))
else:
    out.printf("  <none found>\n")

out.printf("\n===== Identity-shaped keyword hits across ENTRY vtable slots =====\n")
if identity_field_hits:
    for idx, off, fname, marker in identity_field_hits:
        out.printf("  slot %d (off 0x%02x) %s : marker '%s'\n" % (idx, off, fname, marker))
else:
    out.printf("  <none found>\n")
out.flush()

# ---- Part 3: find the constructor(s) - data xrefs to the vtable address itself ----
out.printf("\n===== Data cross-references TO the vtable address itself (constructor candidates) =====\n")
refs = getReferencesTo(vt_addr)
ctor_fns = []
for r in refs:
    from_addr = r.getFromAddress()
    fn = fm.getFunctionContaining(from_addr)
    out.printf("  xref from %s (%s) in function %s\n" % (
        from_addr, r.getReferenceType(), fn.getName() if fn else "<no function>"))
    if fn is not None:
        ctor_fns.append(fn)

out.printf("\n===== Decompiling constructor candidate(s) =====\n")
seen_ctor = set()
for fn in ctor_fns:
    key = fn.getEntryPoint()
    if key in seen_ctor:
        continue
    seen_ctor.add(key)
    out.printf("\n----- constructor candidate %s @ %s -----\n" % (fn.getName(), fn.getEntryPoint()))
    out.print(decompile_text(fn))

out.flush()
out.close()
ifc.dispose()
