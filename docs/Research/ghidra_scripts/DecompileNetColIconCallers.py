from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

# Working BACKWARD from the render/asset-selection code for the net_col_A..G.hip tiered icons
# (RankedDelayGhidraReport.txt string refs), rather than forward from a guessed data container
# (per the mandatory constraint in this investigation - three prior forward-from-data-write
# passes all dead-ended, see RankedListConnectionFilter_Progress.md).
#
# FUN_00533d10 (0x00533d10) and FUN_00655260 (0x00655260) are both confirmed (by decompile,
# already in RankedDelayGhidraReport.txt) to be small "pick net_col_<tier>.hip by an 0-7 index
# argument and hand it to the sprite-draw call FUN_006916b0" helpers - i.e. these ARE the
# icon-render functions for the 8-tier (def,A..G) connection-quality glyph. This script finds
# every direct caller of both, decompiles two levels up (caller, and caller-of-caller) to find
# where the index argument actually comes from, and prints enough of each caller's decompile to
# see the source of the index value (struct field, local computed from ping/rtt, etc).

TARGETS = {
    0x00533d10: "NetColIcon_Draw_A",
    0x00655260: "NetColIcon_Draw_B",
}

args = getScriptArgs()
report_file = File(args[0]) if args else File(File(currentProgram.getExecutablePath()).getParentFile(), "report.txt")
out = PrintWriter(report_file, "UTF-8")

ifc = DecompInterface()
ifc.openProgram(currentProgram)
monitor = ConsoleTaskMonitor()
fm = currentProgram.getFunctionManager()


def decompile_fn(fn, indent="  "):
    res = ifc.decompileFunction(fn, 60, monitor)
    if res.decompileCompleted():
        out.println(res.getDecompiledFunction().getC())
    else:
        out.printf("%s<decompile failed: %s>\n" % (indent, res.getErrorMessage()))


for tv, label in TARGETS.items():
    target = toAddr(tv)
    out.printf("===== Callers of %s (%s) =====\n" % (target, label))
    refs = list(getReferencesTo(target))
    out.printf("  (%d references)\n" % len(refs))
    seen_callers = set()
    for r in refs:
        from_addr = r.getFromAddress()
        caller_fn = getFunctionContaining(from_addr)
        cname = caller_fn.getName(True) if caller_fn else "<no function>"
        out.printf("  %s  %-12s caller=%s\n" % (from_addr, r.getReferenceType(), cname))
        if caller_fn is not None:
            seen_callers.add(caller_fn.getEntryPoint())

    out.printf("\n  ---- Decompiling %d unique direct caller functions ----\n" % len(seen_callers))
    grandparents = set()
    for caddr in seen_callers:
        caller_fn = fm.getFunctionAt(caddr)
        out.printf("\n  ----- caller %s %s -----\n" % (caddr, caller_fn.getName(True)))
        decompile_fn(caller_fn)

        # Also collect this caller's own callers (grandparent level) for a second pass.
        for r2 in getReferencesTo(caddr):
            gp_addr = r2.getFromAddress()
            gp_fn = getFunctionContaining(gp_addr)
            if gp_fn is not None:
                grandparents.add(gp_fn.getEntryPoint())

    out.printf("\n  ---- Decompiling %d unique grandparent (caller-of-caller) functions ----\n" % len(grandparents))
    for gaddr in grandparents:
        gp_fn = fm.getFunctionAt(gaddr)
        out.printf("\n  ----- grandparent %s %s -----\n" % (gaddr, gp_fn.getName(True)))
        decompile_fn(gp_fn)

    out.printf("\n\n")

out.close()
ifc.dispose()
