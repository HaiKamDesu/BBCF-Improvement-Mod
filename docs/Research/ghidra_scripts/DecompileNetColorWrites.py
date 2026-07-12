from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

# NetworkUserData static struct base = moduleBase(0x00400000) + 0x008AD0C0 (per
# src/Overlay/Window/NetworkSquareColorWindow.cpp kNetworkUserDataRva).
# netcolor byte at +0x194, counter byte at +0x195.
STRUCT_BASE = 0x00CAD0C0
NETCOLOR_ADDR = 0x00CAD254
NETCOLOR_COUNTER_ADDR = 0x00CAD255

# Old-version addressing (kOldNetworkUserDataRva = 0x004AD0C0) in case the
# currently-imported exe matches that convention instead.
OLD_STRUCT_BASE = 0x008AD0C0
OLD_NETCOLOR_ADDR = 0x008AD254
OLD_NETCOLOR_COUNTER_ADDR = 0x008AD255

TARGETS = [
    ("NetworkUserData struct base", STRUCT_BASE),
    ("netcolor byte (+0x194)", NETCOLOR_ADDR),
    ("netcolor counter byte (+0x195)", NETCOLOR_COUNTER_ADDR),
    ("OLD NetworkUserData struct base", OLD_STRUCT_BASE),
    ("OLD netcolor byte (+0x194)", OLD_NETCOLOR_ADDR),
    ("OLD netcolor counter byte (+0x195)", OLD_NETCOLOR_COUNTER_ADDR),
]

args = getScriptArgs()
report_file = File(args[0]) if args else File(File(currentProgram.getExecutablePath()).getParentFile(), "NetColorWritesReport.txt")

ifc = DecompInterface()
ifc.openProgram(currentProgram)
monitor = ConsoleTaskMonitor()

out = PrintWriter(report_file, "UTF-8")
try:
    decompiled_funcs = set()
    for label, addr_val in TARGETS:
        try:
            addr = toAddr(addr_val)
        except Exception as e:
            out.printf("===== %s (0x%x) - toAddr failed: %s =====%n", label, addr_val, str(e))
            continue

        out.printf("===== References to %s (%s) =====%n", label, addr)
        refs = list(getReferencesTo(addr))
        if not refs:
            out.printf("  (no references found)%n")
            continue

        for r in refs:
            from_addr = r.getFromAddress()
            ref_type = r.getReferenceType().toString()
            fn = getFunctionContaining(from_addr)
            fn_name = fn.getName() if fn else "<no function>"
            out.printf("  %s  %-16s  %s%n", from_addr, ref_type, fn_name)
            if fn:
                decompiled_funcs.add(fn.getEntryPoint())

    out.printf("%n===== Decompiled callers (%d unique functions) =====%n", len(decompiled_funcs))
    for entry in decompiled_funcs:
        fn = getFunctionAt(entry)
        if not fn:
            continue
        out.printf("----- %s (%s) -----%n", fn.getName(), entry)
        res = ifc.decompileFunction(fn, 60, monitor)
        if res and res.decompileCompleted():
            out.printf("%s%n", res.getDecompiledFunction().getC())
        else:
            out.printf("  (decompile failed)%n")
finally:
    out.close()
    ifc.dispose()
