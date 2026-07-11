# Ghidra headless Jython script, phase 7: find all writers to DAT_00EA97C8,
# the global flag that drives GAME_CSaveTask::update_task (FUN_004B9F70) to
# actually perform a save-to-disk. This is the real "please save now" trigger;
# tracing its writers should reveal what post-match/ranked-confirm logic (if
# any) requests the save, and whether that logic is gated by the same
# per-room-member network state that gets stuck during the D-Code bug.
# Usage after import:
# analyzeHeadless <project_dir> BBCF -process BBCF.exe -noanalysis -scriptPath <script_dir> -postScript DecompileDCodeBug7.py <report_path>

from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor


TARGET_DATA_ADDRS = [
    0x00ea97c8,
]


def decompile_functions(out, ifc, addrs):
    seen = set()
    for fn in addrs:
        key = fn.getEntryPoint().toString()
        if key in seen:
            continue
        seen.add(key)
        out.printf("----- DECOMPILE %s -----%n", fn.getEntryPoint())
        out.printf("Function: %s%n", fn.getName())
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
    report_file = File(File(currentProgram.getExecutablePath()).getParentFile(), "dcode_bug7_decompile.txt")

out = PrintWriter(report_file, "UTF-8")
ifc = DecompInterface()
try:
    if not ifc.openProgram(currentProgram):
        raise Exception("openProgram failed")

    out.printf("Program: %s%n", currentProgram.getName())
    out.printf("Image base: %s%n%n", currentProgram.getImageBase())

    writer_fns = set()
    for data_addr in TARGET_DATA_ADDRS:
        addr = toAddr(data_addr)
        out.printf("===== REFERENCES TO %s =====%n", addr)
        refs = getReferencesTo(addr)
        count = 0
        for ref in refs:
            from_addr = ref.getFromAddress()
            fn = getFunctionContaining(from_addr)
            fn_name = fn.getName() if fn else "<none>"
            out.printf("%s type=%s fn=%s%n", from_addr, ref.getReferenceType(), fn_name)
            if fn is not None:
                writer_fns.add(fn)
            count += 1
            if count >= 300:
                out.println("(truncated)")
                break
        out.println()

    out.println("===== DECOMPILE EACH REFERENCING FUNCTION =====")
    decompile_functions(out, ifc, list(writer_fns))

    out.println("===== CALLERS OF EACH REFERENCING FUNCTION =====")
    for fn in writer_fns:
        entry = fn.getEntryPoint()
        out.printf("--- callers of %s (%s) ---%n", entry, fn.getName())
        refs = getReferencesTo(entry)
        count = 0
        for ref in refs:
            from_addr = ref.getFromAddress()
            caller_fn = getFunctionContaining(from_addr)
            caller_name = caller_fn.getName() if caller_fn else "<none>"
            out.printf("%s type=%s caller_fn=%s%n", from_addr, ref.getReferenceType(), caller_name)
            count += 1
            if count >= 50:
                out.println("(truncated)")
                break
        if count == 0:
            out.println("(no callers found)")
        out.println()
finally:
    ifc.dispose()
    out.close()

print("Wrote %s" % report_file.getAbsolutePath())
