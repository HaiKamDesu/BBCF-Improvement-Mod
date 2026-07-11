# Ghidra headless Jython script, phase 5: BBCF.h's function-table comments
# (e.g. "//000bb460") are RVAs relative to module base 0x00400000, not raw
# addresses. Corrected VAs for CSaveDataManager save-trigger methods, tracing
# their callers to see if any post-match/ranked-confirm code (and whether it
# is gated by the same per-room-member fetch state used by the D-Code path)
# schedules the actual save-to-disk.
# Usage after import:
# analyzeHeadless <project_dir> BBCF -process BBCF.exe -noanalysis -scriptPath <script_dir> -postScript DecompileDCodeBug5.py <report_path>

from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor


FUNCTION_ADDRS = [
    0x004bb010,  # CSaveDataManager::is_next_SaveUtil_action_running
    0x004bb2c0,  # CSaveDataManager::set_next_SaveUtil_action_7_check
    0x004bb410,  # CSaveDataManager::set_next_SaveUtil_action_to_1_read
    0x004bb460,  # CSaveDataManager::set_next_SaveUtil_action_0_write  <-- the actual save trigger
    0x004b9f70,  # GAME_CSaveTask::update_task
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


def dump_callers(out, addrs, depth_label):
    out.printf("===== CALLERS %s =====%n", depth_label)
    seen_targets = set()
    all_callers = []
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
            if caller_fn is not None:
                all_callers.append(caller_fn.getEntryPoint().getOffset())
            count += 1
            if count >= 200:
                out.println("(truncated)")
                break
        if count == 0:
            out.println("(no callers found)")
        out.println()
    return all_callers


args = getScriptArgs()
if len(args) > 0:
    report_file = File(args[0])
else:
    report_file = File(File(currentProgram.getExecutablePath()).getParentFile(), "dcode_bug5_decompile.txt")

out = PrintWriter(report_file, "UTF-8")
ifc = DecompInterface()
try:
    if not ifc.openProgram(currentProgram):
        raise Exception("openProgram failed")

    out.printf("Program: %s%n", currentProgram.getName())
    out.printf("Image base: %s%n%n", currentProgram.getImageBase())

    decompile_functions(out, ifc, FUNCTION_ADDRS)
    direct_callers = dump_callers(out, FUNCTION_ADDRS, "(direct)")

    # Second hop: decompile the direct callers of the write-trigger specifically,
    # and find who calls THEM (to walk up towards the ranked/post-match code).
    write_fn = get_fn(0x004bb460)
    if write_fn is not None:
        out.println("===== DIRECT CALLERS OF set_next_SaveUtil_action_0_write, DECOMPILED =====")
        decompile_functions(out, ifc, list(set(direct_callers)))
        out.println("===== CALLERS OF THOSE CALLERS (second hop) =====")
        dump_callers(out, list(set(direct_callers)), "(second hop)")
finally:
    ifc.dispose()
    out.close()

print("Wrote %s" % report_file.getAbsolutePath())
