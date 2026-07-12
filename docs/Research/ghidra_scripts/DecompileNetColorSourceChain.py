from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

# Chase the source of RoomMemberEntry+0xc4 (the byte gated by +0xc6>=0x1e in
# FUN_004a1110, which is the netcolor-tier compute/gate function). FUN_0049ef50
# populates a room member entry's +0xc0/0xc4/0xc8/0xcc fields from param_5[0..3]
# (a caller-supplied 4-dword blob) - need to see where param_5 comes from.
# Also decompile FUN_0049d5c0, whose return value feeds FUN_004a1110 as the
# implicit fastcall arg in several callers.
TARGETS = [
    0x0049d5c0,
    0x0049ef50,
]

args = getScriptArgs()
report_file = File(args[0]) if args else File(File(currentProgram.getExecutablePath()).getParentFile(), "NetColorSourceChainReport.txt")

ifc = DecompInterface()
ifc.openProgram(currentProgram)
monitor = ConsoleTaskMonitor()

out = PrintWriter(report_file, "UTF-8")
try:
    for addr_val in TARGETS:
        fn = getFunctionAt(toAddr(addr_val))
        out.printf("----- 0x%x: %s -----%n", addr_val, fn.getName() if fn else "<none>")
        if fn:
            res = ifc.decompileFunction(fn, 60, monitor)
            if res and res.decompileCompleted():
                out.printf("%s%n", res.getDecompiledFunction().getC())

        out.printf("===== Callers of 0x%x =====%n", addr_val)
        target = toAddr(addr_val)
        caller_funcs = set()
        for r in getReferencesTo(target):
            from_addr = r.getFromAddress()
            cfn = getFunctionContaining(from_addr)
            fn_name = cfn.getName() if cfn else "<no function>"
            out.printf("  %s  %s%n", from_addr, fn_name)
            if cfn:
                caller_funcs.add(cfn.getEntryPoint())
        for entry in caller_funcs:
            cfn = getFunctionAt(entry)
            if not cfn:
                continue
            out.printf("----- caller %s (%s) -----%n", cfn.getName(), entry)
            res = ifc.decompileFunction(cfn, 60, monitor)
            if res and res.decompileCompleted():
                out.printf("%s%n", res.getDecompiledFunction().getC())
finally:
    out.close()
    ifc.dispose()
