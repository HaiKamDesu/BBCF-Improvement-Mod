# Ghidra headless Jython script: search all defined strings in the binary for
# room/network/matchmaking error message text or message keys (e.g. "NM_..."),
# to find the generic message-display function used for popups like
# "Failed to connect to room" / "An error has occured while attempting to
# create a room" / room list duplicates, so a single hook can log all of them.
# Usage after import:
# analyzeHeadless <project_dir> BBCF -process BBCF.exe -noanalysis -scriptPath <script_dir> -postScript FindNetworkErrorStrings.py <report_path>

from java.io import File, PrintWriter
from ghidra.program.model.data import StringDataType, TerminatedStringDataType, UnicodeDataType, TerminatedUnicodeDataType
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor


KEYWORDS = [
    "room", "connect", "network", "matchmak", "lobby", "fail", "error",
    "NM_", "occured", "occurred",
]


def matches_keyword(s):
    lower = s.lower()
    for kw in KEYWORDS:
        if kw.lower() in lower:
            return True
    return False


def dump_string_hits(out):
    out.println("===== STRING HITS =====")
    listing = currentProgram.getListing()
    data_iter = listing.getDefinedData(True)
    count = 0
    hits = []
    for data in data_iter:
        dt = data.getDataType()
        try:
            value = data.getValue()
        except:
            continue
        if value is None:
            continue
        s = str(value)
        if len(s) < 4:
            continue
        if matches_keyword(s):
            out.printf("%s len=%d : %s%n", data.getAddress(), len(s), s.replace("\n", "\\n"))
            hits.append(data.getAddress())
            count += 1
            if count >= 500:
                out.println("(truncated)")
                break
    out.printf("total hits: %d%n%n", count)
    return hits


def dump_refs_and_callers(out, addrs):
    out.println("===== REFERENCES TO EACH STRING, AND THE FUNCTION USING IT =====")
    seen_fns = {}
    for addr in addrs:
        refs = getReferencesTo(addr)
        for ref in refs:
            from_addr = ref.getFromAddress()
            fn = getFunctionContaining(from_addr)
            fn_name = fn.getName() if fn else "<none>"
            out.printf("%s -> string@%s fn=%s%n", from_addr, addr, fn_name)
            if fn is not None:
                key = fn.getEntryPoint().toString()
                if key not in seen_fns:
                    seen_fns[key] = fn
    out.println()
    return seen_fns.values()


def decompile_functions(out, ifc, fns):
    out.println("===== DECOMPILE EACH USING FUNCTION =====")
    for fn in fns:
        out.printf("----- DECOMPILE %s (%s) -----%n", fn.getEntryPoint(), fn.getName())
        result = ifc.decompileFunction(fn, 120, ConsoleTaskMonitor())
        if result.decompileCompleted():
            out.println(result.getDecompiledFunction().getC())
        else:
            out.printf("Decompile failed: %s%n", result.getErrorMessage())
        out.println()


args = getScriptArgs()
if len(args) > 0:
    report_file = File(args[0])
else:
    report_file = File(File(currentProgram.getExecutablePath()).getParentFile(), "network_error_strings.txt")

out = PrintWriter(report_file, "UTF-8")
ifc = DecompInterface()
try:
    if not ifc.openProgram(currentProgram):
        raise Exception("openProgram failed")

    out.printf("Program: %s%n", currentProgram.getName())
    out.printf("Image base: %s%n%n", currentProgram.getImageBase())

    hit_addrs = dump_string_hits(out)
    using_fns = dump_refs_and_callers(out, hit_addrs)
    decompile_functions(out, ifc, using_fns)
finally:
    ifc.dispose()
    out.close()

print("Wrote %s" % report_file.getAbsolutePath())
