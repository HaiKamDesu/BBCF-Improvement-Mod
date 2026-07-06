# Ghidra headless Jython script. Dumps raw hex bytes at a fixed address for verifying a
# hand-authored hook signature against ground truth (not just the disassembled mnemonic text).
# Usage:
# analyzeHeadless <project_dir> BBCF -process BBCF.exe -noanalysis -scriptPath <script_dir> -postScript DumpPreTickBytes.py <report_path>

from java.io import File, PrintWriter

TARGET = 0x0056B1F0
LENGTH = 40

args = getScriptArgs()
report_file = File(args[0]) if len(args) > 0 else File(File(currentProgram.getExecutablePath()).getParentFile(), "pretick_bytes.txt")

out = PrintWriter(report_file, "UTF-8")
try:
    addr = toAddr(TARGET)
    data = getBytes(addr, LENGTH)
    hexstr = " ".join("%02X" % (b & 0xFF) for b in data)
    out.printf("Address: %s%n", addr)
    out.printf("Bytes (%d): %s%n", LENGTH, hexstr)

    # Also print per-instruction breakdown using the listing
    listing = currentProgram.getListing()
    cur = addr
    total = 0
    out.println()
    while total < LENGTH:
        instr = listing.getInstructionAt(cur)
        if instr is None:
            out.printf("%s: <no instruction>%n", cur)
            break
        ilen = instr.getLength()
        ibytes = getBytes(cur, ilen)
        ihex = " ".join("%02X" % (b & 0xFF) for b in ibytes)
        out.printf("%s: %-30s bytes=%s len=%d%n", cur, instr.toString(), ihex, ilen)
        cur = cur.add(ilen)
        total += ilen
finally:
    out.close()

print("Wrote %s" % report_file.getAbsolutePath())
