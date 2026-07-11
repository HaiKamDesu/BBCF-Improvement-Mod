# Ghidra headless Jython script. Locates ranked-search "delay/RTT/netcolor" strings,
# finds functions that reference them, and decompiles those functions to trace the
# 0-4 delay column value shown in the ranked search list.
# Usage after import:
# analyzeHeadless <project_dir> BBCF -process BBCF.exe -noanalysis -scriptPath <script_dir> -postScript DecompileRankedDelay.py <report_path>

from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor
from ghidra.program.model.data import StringDataType

TARGET_STRINGS = [
    "RMSR_CheckingRTT",
    "RANK_RTT_FILTER",
    "RANK_MYAREA",
    "HOST_NETCOLOR",
    "net_col_",
    "net_col_def",
    "DBM_NetworkMeter",
    "DBM_ForceNetworkUISignal",
    "DBM_PacketDelay",
]


def find_string_addrs(out):
    """Return list of (addr, text, matched_query) for defined strings matching a target."""
    results = []
    listing = currentProgram.getListing()
    data_iter = listing.getDefinedData(True)
    for data in data_iter:
        try:
            if not data.hasStringValue():
                continue
        except:
            continue
        val = data.getValue()
        if val is None:
            continue
        s = str(val)
        for q in TARGET_STRINGS:
            if q in s:
                results.append((data.getAddress(), s, q))
                break
    return results


def get_fn(addr):
    fn = getFunctionAt(addr)
    if fn is None:
        fn = getFunctionContaining(addr)
    return fn


def decompile_fn(out, ifc, fn, tag):
    out.printf("----- DECOMPILE %s %s (%s) -----%n", fn.getEntryPoint(), fn.getName(), tag)
    result = ifc.decompileFunction(fn, 120, ConsoleTaskMonitor())
    if result.decompileCompleted():
        out.println(result.getDecompiledFunction().getC())
    else:
        out.printf("Decompile failed: %s%n", result.getErrorMessage())
    out.println()


args = getScriptArgs()
report_file = File(args[0]) if len(args) > 0 else File(File(currentProgram.getExecutablePath()).getParentFile(), "ranked_delay.txt")

out = PrintWriter(report_file, "UTF-8")
ifc = DecompInterface()
try:
    ifc.openProgram(currentProgram)
    out.printf("Program: %s  ImageBase: %s%n%n", currentProgram.getName(), currentProgram.getImageBase())

    out.println("===== MATCHING STRINGS =====")
    str_hits = find_string_addrs(out)
    for addr, s, q in str_hits:
        out.printf("%s  [%s]  %s%n", addr, q, s)
    out.println()

    # Collect referencing functions (unique)
    ref_fns = {}   # entry_str -> (fn, set of (from_addr, string_text))
    out.println("===== REFERENCES TO EACH STRING =====")
    for addr, s, q in str_hits:
        refs = getReferencesTo(addr)
        for ref in refs:
            fa = ref.getFromAddress()
            fn = getFunctionContaining(fa)
            fname = fn.getName() if fn else "<none>"
            out.printf("str %s %s  <- %s in %s%n", addr, s, fa, fname)
            if fn is not None:
                key = fn.getEntryPoint().toString()
                if key not in ref_fns:
                    ref_fns[key] = (fn, set())
                ref_fns[key][1].add((fa.toString(), s))
    out.println()

    out.println("===== DECOMPILED REFERENCING FUNCTIONS =====")
    for key in sorted(ref_fns.keys()):
        fn, hits = ref_fns[key]
        tag = "; ".join(sorted(set(h[1] for h in hits)))
        decompile_fn(out, ifc, fn, tag)
finally:
    ifc.dispose()
    out.close()

print("Wrote %s" % report_file.getAbsolutePath())
