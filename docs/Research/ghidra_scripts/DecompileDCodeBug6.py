# Ghidra headless Jython script, phase 6: identify the two call contexts that
# reach the CSaveDataManager save-menu state machine (FUN_006C4990), to see if
# either is triggered by returning from a ranked match / post-match flow, and
# whether it's gated by the same per-room-member network state used by the
# D-Code fetch path.
# Usage after import:
# analyzeHeadless <project_dir> BBCF -process BBCF.exe -noanalysis -scriptPath <script_dir> -postScript DecompileDCodeBug6.py <report_path>

from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor


FUNCTION_ADDRS = [
    0x006c4810,
    0x006c4880,
    0x006c48f0,
    0x006b560a,  # containing function will be resolved
    0x0069938d,  # containing function will be resolved
]


def get_fn(addr_value):
    addr = toAddr(addr_value)
    fn = getFunctionAt(addr)
    if fn is None:
        fn = getFunctionContaining(addr)
    return fn


def decompile_functions(out, ifc, addrs):
    seen = set()
    for addr_value in addrs:
        fn = get_fn(addr_value)
        if fn is None:
            out.printf("----- %s -----%nNo function found.%n%n", toAddr(addr_value))
            continue
        key = fn.getEntryPoint().toString()
        if key in seen:
            continue
        seen.add(key)

        out.printf("----- DECOMPILE %s containing %s -----%n", fn.getEntryPoint(), toAddr(addr_value))
        out.printf("Function: %s%n", fn.getName())
        result = ifc.decompileFunction(fn, 120, ConsoleTaskMonitor())
        if result.decompileCompleted():
            out.println(result.getDecompiledFunction().getC())
        else:
            out.printf("Decompile failed: %s%n", result.getErrorMessage())
        out.println()


def dump_callers(out, addrs):
    out.println("===== CALLERS =====")
    seen_targets = set()
    for addr_value in addrs:
        fn = get_fn(addr_value)
        if fn is None:
            continue
        entry = fn.getEntryPoint()
        key = entry.toString()
        if key in seen_targets:
            continue
        seen_targets.add(key)
        out.printf("--- callers of %s (%s) ---%n", entry, fn.getName())
        refs = getReferencesTo(entry)
        count = 0
        for ref in refs:
            from_addr = ref.getFromAddress()
            caller_fn = getFunctionContaining(from_addr)
            caller_name = caller_fn.getName() if caller_fn else "<none>"
            out.printf("%s type=%s caller_fn=%s%n", from_addr, ref.getReferenceType(), caller_name)
            count += 1
            if count >= 100:
                out.println("(truncated)")
                break
        if count == 0:
            out.println("(no callers found)")
        out.println()


args = getScriptArgs()
if len(args) > 0:
    report_file = File(args[0])
else:
    report_file = File(File(currentProgram.getExecutablePath()).getParentFile(), "dcode_bug6_decompile.txt")

out = PrintWriter(report_file, "UTF-8")
ifc = DecompInterface()
try:
    if not ifc.openProgram(currentProgram):
        raise Exception("openProgram failed")

    out.printf("Program: %s%n", currentProgram.getName())
    out.printf("Image base: %s%n%n", currentProgram.getImageBase())

    decompile_functions(out, ifc, FUNCTION_ADDRS)
    dump_callers(out, FUNCTION_ADDRS)
finally:
    ifc.dispose()
    out.close()

print("Wrote %s" % report_file.getAbsolutePath())
