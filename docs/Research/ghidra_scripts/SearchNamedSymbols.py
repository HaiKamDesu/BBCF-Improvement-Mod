from java.io import File, PrintWriter

# The dead-end row array (DAT_00a5d270) turned out to have a named vtable symbol
# "CNetworkLobbyData::vftable" (found via RankedDelay5GhidraReport.txt) - meaning
# Ghidra's analysis DOES have some recovered C++ class name info (RTTI-derived),
# not just FUN_ generic names. Dump every symbol whose name contains any of a
# curated set of keywords, to find real class/function names for: lobby list
# rows, delay/ping/latency, connection quality, room browser UI, menu/scene ids.

KEYWORDS = [
    "Lobby", "Delay", "Ping", "Latency", "Connection", "Room", "List",
    "Matchmak", "Search", "Menu", "Scene", "Screen", "Panel", "P2P",
    "NetworkLobby", "CNetwork",
]

args = getScriptArgs()
report_file = File(args[0]) if args else File(File(currentProgram.getExecutablePath()).getParentFile(), "report.txt")
out = PrintWriter(report_file, "UTF-8")

symtab = currentProgram.getSymbolTable()
count = 0
for kw in KEYWORDS:
    out.printf("===== keyword: %s =====\n" % kw)
    it = symtab.getSymbolIterator(True)
    matches = []
    while it.hasNext():
        sym = it.next()
        name = sym.getName()
        if kw.lower() in name.lower():
            matches.append((sym.getAddress(), name, sym.getSymbolType().toString()))
    matches.sort(key=lambda t: str(t[0]))
    for addr, name, symtype in matches:
        out.printf("  %s  %-10s %s\n" % (addr, symtype, name))
    out.printf("  (%d matches)\n\n" % len(matches))
    count += len(matches)

out.printf("TOTAL matches across all keywords (with overlap): %d\n" % count)
out.close()
