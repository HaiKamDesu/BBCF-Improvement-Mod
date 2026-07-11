# Ghidra headless Jython script, phase 2: traces the fetch-trigger chain used by
# FUN_0049d560 (opponent profile row getter) to see what gates whether a Steam
# profile/D-code fetch is (re)triggered, and whether it can get permanently stuck.
# Usage after import:
# analyzeHeadless <project_dir> BBCF -process BBCF.exe -noanalysis -scriptPath <script_dir> -postScript DecompileDCodeBug2.py <report_path>

from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor


FUNCTION_ADDRS = [
    0x004a26a0,  # fetch-trigger, called from FUN_0049d560 when row not cached
    0x004a1930,  # row-found lookup used by FUN_0049d560
    0x004a0b80,  # state check ("!= 100" gate) used by FUN_0049d560
    0x004a1ab0,  # "already fetching" gate used by FUN_0049d560
    0x004a0fe0,  # get_NetUserData base pointer
    0x004b4360,  # other caller of the 00407c90 gate, near ranked-confirm addresses
    0x00407c90,  # shared gate function (already decompiled in phase 1, re-included for xrefs)
]


def get_fn(addr_value):
    addr = toAddr(addr_value)
    fn = getFunctionAt(addr)
    if fn is None:
        fn = getFunctionContaining(addr)
    return fn


def decompile_functions(out, ifc, addrs, label):
    seen = set()
    out.printf("===== %s =====%n", label)
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
    out.println("===== CALLERS (references) =====")
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


def dump_callees(out, ifc, addrs):
    out.println("===== CALLEES =====")
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
        out.printf("--- callees of %s (%s) ---%n", entry, fn.getName())
        called = fn.getCalledFunctions(ConsoleTaskMonitor())
        for callee in called:
            out.printf("  %s %s%n", callee.getEntryPoint(), callee.getName())
        out.println()


args = getScriptArgs()
if len(args) > 0:
    report_file = File(args[0])
else:
    report_file = File(File(currentProgram.getExecutablePath()).getParentFile(), "dcode_bug2_decompile.txt")

out = PrintWriter(report_file, "UTF-8")
ifc = DecompInterface()
try:
    if not ifc.openProgram(currentProgram):
        raise Exception("openProgram failed")

    out.printf("Program: %s%n", currentProgram.getName())
    out.printf("Image base: %s%n%n", currentProgram.getImageBase())

    decompile_functions(out, ifc, FUNCTION_ADDRS, "DECOMPILE TARGETS")
    dump_callers(out, FUNCTION_ADDRS)
    dump_callees(out, ifc, FUNCTION_ADDRS)
finally:
    ifc.dispose()
    out.close()

print("Wrote %s" % report_file.getAbsolutePath())
