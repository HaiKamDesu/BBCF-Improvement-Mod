from java.io import File, PrintWriter

TARGETS = {
    "known-good CNetworkLobbyData::vftable": 0x0089c7cc,
    "known-good GAMESTEAM_CNetworkServer::vftable": 0x0089cb4c,
    "confirmed-live MGR vtable (stale, old ASLR-session value, may be garbage)": 0x00CFC3A4,
    "candidate ENTRY vtable RVA 0x49cc34": 0x0049CC34,
}

args = getScriptArgs()
report_file = File(args[0]) if args else File(File(currentProgram.getExecutablePath()).getParentFile(), "report.txt")
out = PrintWriter(report_file, "UTF-8")

mem = currentProgram.getMemory()
fm = currentProgram.getFunctionManager()
listing = currentProgram.getListing()

for label, av in TARGETS.items():
    addr = toAddr(av)
    block = mem.getBlock(addr)
    fn = fm.getFunctionContaining(addr)
    instr = listing.getInstructionAt(addr)
    data = listing.getDataAt(addr)
    try:
        dw = mem.getInt(addr) & 0xFFFFFFFF
    except Exception:
        dw = None
    out.printf("%s @ %s\n" % (label, addr))
    out.printf("  block=%s r=%s w=%s x=%s\n" % (
        block.getName() if block else "<none>",
        block.isRead() if block else None, block.isWrite() if block else None, block.isExecute() if block else None))
    out.printf("  function containing=%s\n" % (fn.getName() if fn else "<none>"))
    out.printf("  instruction at addr=%s\n" % (instr if instr else "<none>"))
    out.printf("  data at addr=%s\n" % (data if data else "<none>"))
    out.printf("  dword at addr=0x%08x\n\n" % (dw if dw is not None else -1))

out.printf("\nAll memory blocks:\n")
for b in mem.getBlocks():
    out.printf("  %-20s %s - %s  r=%s w=%s x=%s size=0x%x\n" % (
        b.getName(), b.getStart(), b.getEnd(), b.isRead(), b.isWrite(), b.isExecute(), b.getSize()))

out.close()
