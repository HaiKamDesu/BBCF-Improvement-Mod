from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

# Investigation: find a code-level signal distinguishing "raw ranked search list
# screen visible" from other ranked-adjacent screens sharing the same
# state==4 / state1 in [30,36,38,39,42] band.
#
# kRankedNetworkStructRva = 0x008F7958 -> Ghidra addr 0x00400000+0x8F7958 = 0x00CF7958
# Known-touched nearby statics from raw grep: 0x00CF7944, 0x00CF7948, 0x00CF794C,
# 0x00CF7950 (adjacent, possibly unrelated globals in same .data blob), 0x00CF7958
# itself (compared to 7 - a guard/ready sentinel, called via ctor at 0x004A5860).
#
# Also decompile FUN_004A7FB0 (fallback readiness check touching 0x00CF7BC8/0x00CF7BD0)
# and the four caller stubs at 0x004A5EF3, 0x004A5FDF-ish (0x004A5FDC block),
# 0x004AA762, 0x004B1182 to see what class/member they belong to.

TARGET_GLOBAL = 0x00CF7958
CTOR = 0x004A5860
FALLBACK_CHECK = 0x004A7FB0
CALLER_FUNCS = [0x004A5EF3, 0x004A5FDC, 0x004AA762, 0x004B1182]

args = getScriptArgs()
report_file = File(args[0]) if args else File(File(currentProgram.getExecutablePath()).getParentFile(), "report.txt")
ifc = DecompInterface()
ifc.openProgram(currentProgram)
monitor = ConsoleTaskMonitor()
out = PrintWriter(report_file, "UTF-8")

def decompile_and_print(addr_val, label):
    addr = toAddr(addr_val)
    fn = getFunctionContaining(addr)
    if fn is None:
        out.printf("===== %s @ %s: no function found =====\n" % (label, addr))
        return
    out.printf("===== %s -> function %s @ %s =====\n" % (label, fn.getName(), fn.getEntryPoint()))
    res = ifc.decompileFunction(fn, 60, monitor)
    if res and res.decompileCompleted():
        out.printf("%s\n" % res.getDecompiledFunction().getC())
    else:
        out.printf("DECOMPILE FAILED for %s\n" % fn.getName())

out.printf("===== References to global 0x%08X =====\n" % TARGET_GLOBAL)
target = toAddr(TARGET_GLOBAL)
seen_funcs = set()
for r in getReferencesTo(target):
    fromAddr = r.getFromAddress()
    fn = getFunctionContaining(fromAddr)
    fname = fn.getName() if fn else "<none>"
    out.printf("ref from %s type=%s in %s\n" % (fromAddr, r.getReferenceType(), fname))
    if fn is not None:
        seen_funcs.add(fn.getEntryPoint().getOffset())

out.printf("\n===== Decompiling every unique caller function referencing 0x%08X (%d functions) =====\n" % (TARGET_GLOBAL, len(seen_funcs)))
for off in seen_funcs:
    decompile_and_print(off, "caller")

out.printf("\n===== Constructor / helper functions =====\n")
decompile_and_print(CTOR, "CTOR 0x004A5860")
decompile_and_print(FALLBACK_CHECK, "FALLBACK_CHECK 0x004A7FB0")

out.printf("\n===== Explicit caller-site functions (guard pattern around 0xCF7958==7) =====\n")
for addr_val in CALLER_FUNCS:
    decompile_and_print(addr_val, "explicit-caller")

out.close()
ifc.dispose()
