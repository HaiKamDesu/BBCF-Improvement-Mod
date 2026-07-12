from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

# FUN_004a0fe0 is the lazy-init getter that returns &DAT_00cad0c0, the
# NetworkUserData static struct (per NetColorWritesGhidraReport.txt). No direct
# fixed-address references to +0x194/+0x195 exist, so the netcolor byte must be
# accessed through this getter's returned pointer with a runtime offset. Find
# every caller of the getter and decompile them, then grep the output for 0x194
# in the caller function bodies.
GETTER_ADDR = 0x004a0fe0

args = getScriptArgs()
report_file = File(args[0]) if args else File(File(currentProgram.getExecutablePath()).getParentFile(), "NetColorGetterCallersReport.txt")

ifc = DecompInterface()
ifc.openProgram(currentProgram)
monitor = ConsoleTaskMonitor()

out = PrintWriter(report_file, "UTF-8")
try:
    target = toAddr(GETTER_ADDR)
    out.printf("===== References to getter FUN_004a0fe0 (%s) =====%n", target)
    refs = list(getReferencesTo(target))
    decompiled_funcs = set()
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
            code = res.getDecompiledFunction().getC()
            out.printf("%s%n", code)
            if "0x194" in code or "0x195" in code:
                out.printf("  *** CONTAINS 0x194/0x195 OFFSET REFERENCE ***%n")
        else:
            out.printf("  (decompile failed)%n")
finally:
    out.close()
    ifc.dispose()
