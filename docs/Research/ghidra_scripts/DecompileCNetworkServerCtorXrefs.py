from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

# Find every xref TO the GAME_CNetworkServer / GAMESTEAM_CNetworkServer vftable addresses
# (constructors write "*obj = ClassName::vftable" - a direct address reference Ghidra CAN
# resolve, unlike the virtual-call-only tier getter). Decompile each referencing function
# (the constructor) to see what singleton/global it stores the constructed object into -
# if one of them stores into DAT_00c97e3c (RVA 0x897E3C, the confirmed-live ranked-list MGR
# singleton slot), that conclusively identifies MGR's concrete class.

VTABLES = [
    (0x0089c9f4, "GAME_CNetworkServer::vftable"),
    (0x0089cb4c, "GAMESTEAM_CNetworkServer::vftable"),
]

args = getScriptArgs()
report_file = File(args[0]) if args else File(File(currentProgram.getExecutablePath()).getParentFile(), "report.txt")
out = PrintWriter(report_file, "UTF-8")

fm = currentProgram.getFunctionManager()
ifc = DecompInterface()
ifc.openProgram(currentProgram)
monitor = ConsoleTaskMonitor()

for vt_addr_int, name in VTABLES:
    vt_addr = toAddr(vt_addr_int)
    out.printf("\n===== xrefs to %s (%s) =====\n" % (name, vt_addr))
    refs = getReferencesTo(vt_addr)
    seen_fns = []
    for r in refs:
        fn = fm.getFunctionContaining(r.getFromAddress())
        out.printf("  %s  (%s)  in %s\n" % (r.getFromAddress(), r.getReferenceType(), fn.getName() if fn else "?"))
        if fn is not None and fn not in seen_fns:
            seen_fns.append(fn)
    out.flush()

    out.printf("\n  ---- Decompiling %d unique constructor candidate(s) ----\n" % len(seen_fns))
    for fn in seen_fns:
        out.printf("\n  ----- %s %s -----\n" % (fn.getEntryPoint(), fn.getName()))
        res = ifc.decompileFunction(fn, 60, monitor)
        if res.decompileCompleted():
            out.print(res.getDecompiledFunction().getC())
        else:
            out.printf("  <decompile failed: %s>\n" % res.getErrorMessage())
        out.flush()
        # also list callers of the constructor, to trace up to the allocation site
        out.printf("\n    callers of %s:\n" % fn.getName())
        for r2 in getReferencesTo(fn.getEntryPoint()):
            cfn = fm.getFunctionContaining(r2.getFromAddress())
            out.printf("      %s  from %s\n" % (r2.getFromAddress(), cfn.getName() if cfn else "?"))
        out.flush()

out.close()
ifc.dispose()
