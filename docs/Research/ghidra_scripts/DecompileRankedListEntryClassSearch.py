from java.io import File, PrintWriter
from ghidra.program.model.symbol import SymbolType

# Namespace-aware keyword search (getName(True), per the lesson learned in
# DecompileNetworkLobbyDataVtable.py) for the ENTRY (per-row) class's RTTI-recovered name.
# We know from RankedListRowEntryClassGhidraReport.txt / RankedListPermArrayOtherReadersGhidraReport.txt
# that ENTRY's vtable has AT LEAST 45 slots (offsets seen in use: 0x04,0x10,0x1c,0x20,0x24,
# 0x30,0x34,0x50,0x58,0x5c,0xac,0xb0), ruling it out from being CNetworkLobbyData/
# CSTEAMNetworkLobbyData (max 36 slots, mostly purecall) - this is a distinct, much larger,
# concrete class. Strings seen near its use: "RMSR_CheckingRTT", "SkillRank_%02d",
# "NTER_RankMatch_NotMatching", "NM_ResultInfo" - strongly suggests a
# "RankMatchSearchResult"/"NetworkRoomEntry"-style name.

KEYWORDS = [
    "RankMatch", "SearchResult", "RoomEntry", "NetworkRoom", "RMSR", "Entry",
    "RoomList", "SearchEntry", "MatchEntry", "RoomInfo", "RoomData",
]

args = getScriptArgs()
report_file = File(args[0]) if args else File(File(currentProgram.getExecutablePath()).getParentFile(), "report.txt")
out = PrintWriter(report_file, "UTF-8")

symtab = currentProgram.getSymbolTable()


def qname(sym):
    try:
        return sym.getName(True)
    except Exception:
        return sym.getName()


all_syms = []
it = symtab.getSymbolIterator(True)
while it.hasNext():
    all_syms.append(it.next())

out.printf("Total symbols: %d\n\n" % len(all_syms))

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
    out.flush()

# Also list every distinct GhidraClass/Namespace whose vtable has 40+ resolvable function
# pointer slots, regardless of name - a purely structural search independent of naming luck.
out.printf("===== Structural scan: every 'vftable' symbol, slot count (looking for 40+ slot classes) =====\n")
fm = currentProgram.getFunctionManager()
mem = currentProgram.getMemory()
vft_syms = [s for s in all_syms if qname(s).lower().endswith("::vftable")]
out.printf("Found %d vftable symbols total\n" % len(vft_syms))
for s in vft_syms:
    addr = s.getAddress()
    count = 0
    a = addr
    for i in range(80):
        try:
            ptr = mem.getInt(a) & 0xFFFFFFFF
        except Exception:
            break
        try:
            target = toAddr(ptr)
        except Exception:
            break
        fn = fm.getFunctionAt(target)
        if fn is None and i > 0:
            break
        if fn is None and i == 0:
            break
        count += 1
        a = a.add(4)
    if count >= 38:
        out.printf("  %s  slots=%d  %s\n" % (addr, count, qname(s)))
out.flush()

out.close()
