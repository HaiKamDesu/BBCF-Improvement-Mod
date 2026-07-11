# Ghidra headless Jython script. Traces the D-Code load path (Steam UGC "hDCODE" fetch)
# found at FUN_0041ccf0, its callers/callees, and the parent network-manager struct
# it lives in, to look for shared state with ranked LP/progress persistence.
# Usage after import:
# analyzeHeadless <project_dir> BBCF -process BBCF.exe -noanalysis -scriptPath <script_dir> -postScript DecompileDCodeBug.py <report_path>

from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor


# Core D-Code fetch function and its immediate callees/gate.
FUNCTION_ADDRS = [
    0x0041ccf0,  # hDCODE Steam UGC fetch gate/dispatch
    0x0041c900,  # callee after successful hDCODE fetch
    0x0041d480,  # slot-table insert (param_1+0x3640, 0x200 stride, 0x40 slots)
    0x0041d4c0,  # friends/lobby sync using Steam friend iteration (param_1+0x1220 area)
    0x00407c90,  # gate check used by FUN_0041ccf0 and many other callers
    0x0041ce10,  # neighboring function referencing same param_1 struct
]

INTEREST_ADDRS = [
    0x0041ccf6,
    0x0041cd0a,
    0x0041cd20,
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
    out.println("===== CALLEES (functions called by each target) =====")
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


def dump_listing_for_function(out, fn):
    listing = currentProgram.getListing()
    body = fn.getBody()
    inst_iter = listing.getInstructions(body, True)
    for inst in inst_iter:
        out.printf("%s: %s%n", inst.getAddress(), inst.toString())
    out.println()


def dump_disasm(out, addrs):
    out.println("===== DISASSEMBLY FOR TARGET FUNCTIONS =====")
    seen = set()
    for addr_value in addrs:
        fn = get_fn(addr_value)
        if fn is None:
            continue
        key = fn.getEntryPoint().toString()
        if key in seen:
            continue
        seen.add(key)
        out.printf("----- %s %s ----- %n", fn.getEntryPoint(), fn.getName())
        dump_listing_for_function(out, fn)


args = getScriptArgs()
if len(args) > 0:
    report_file = File(args[0])
else:
    report_file = File(File(currentProgram.getExecutablePath()).getParentFile(), "dcode_bug_decompile.txt")

out = PrintWriter(report_file, "UTF-8")
ifc = DecompInterface()
try:
    if not ifc.openProgram(currentProgram):
        raise Exception("openProgram failed")

    out.printf("Program: %s%n", currentProgram.getName())
    out.printf("Image base: %s%n%n", currentProgram.getImageBase())

    all_addrs = FUNCTION_ADDRS + INTEREST_ADDRS
    decompile_functions(out, ifc, all_addrs, "DECOMPILE TARGETS")
    dump_callers(out, FUNCTION_ADDRS)
    dump_callees(out, ifc, FUNCTION_ADDRS)
    dump_disasm(out, all_addrs)
finally:
    ifc.dispose()
    out.close()

print("Wrote %s" % report_file.getAbsolutePath())
