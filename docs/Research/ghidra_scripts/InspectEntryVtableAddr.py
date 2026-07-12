from java.io import File, PrintWriter

TARGET = 0x0049CC34

args = getScriptArgs()
report_file = File(args[0]) if args else File(File(currentProgram.getExecutablePath()).getParentFile(), "report.txt")
out = PrintWriter(report_file, "UTF-8")

mem = currentProgram.getMemory()
fm = currentProgram.getFunctionManager()
listing = currentProgram.getListing()

addr = toAddr(TARGET)
block = mem.getBlock(addr)
out.printf("Address: %s\n" % addr)
out.printf("Block: %s\n" % (block.getName() if block else "<none>"))
if block:
    out.printf("  start=%s end=%s perms r=%s w=%s x=%s\n" % (
        block.getStart(), block.getEnd(), block.isRead(), block.isWrite(), block.isExecute()))

fn = fm.getFunctionContaining(addr)
out.printf("Function containing addr: %s\n" % (fn.getName() if fn else "<none>"))

instr = listing.getInstructionAt(addr)
out.printf("Instruction at addr: %s\n" % (instr if instr else "<none>"))

data = listing.getDataAt(addr)
out.printf("Data at addr: %s\n" % (data if data else "<none>"))

out.printf("\nRaw bytes from %s-0x20 to +0x40:\n" % addr)
base = addr.subtract(0x20)
for i in range(0x60):
    a = base.add(i)
    try:
        b = mem.getByte(a) & 0xFF
    except Exception:
        b = None
    marker = " <-- TARGET" if a.equals(addr) else ""
    out.printf("  %s: 0x%02x%s\n" % (a, b if b is not None else -1, marker))

out.printf("\nDwords around target (+/- 0x20):\n")
base2 = addr.subtract(0x20)
for i in range(0x10):
    a = base2.add(i * 4)
    try:
        v = mem.getInt(a) & 0xFFFFFFFF
    except Exception:
        v = None
    marker = " <-- TARGET" if a.equals(addr) else ""
    out.printf("  %s: 0x%08x%s\n" % (a, v if v is not None else -1, marker))

out.close()
