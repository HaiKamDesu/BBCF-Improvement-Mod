from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

# Goal: find whatever function actually WRITES GAMESTEAM_SearchResultNode's tier byte at
# +0x74 (the field read verbatim, no transform, by the confirmed getter FUN_0046e880 /
# ENTRY vtable slot 7). Approach:
#   1. Fully decompile known related functions (field-initializer, +0x78 writer, sample
#      table lookup, both getters) to nail down the value SHAPE (raw ms vs. bucketed tier).
#   2. Fast INSTRUCTION-LEVEL scan (no decompiler - too slow across the whole binary) of
#      every instruction in the program for a MOV of a byte-sized operand to
#      "[reg + 0x74]" (a raw store, cheap to detect via instruction text), then group hits
#      by containing function and print the function name + a decompile of just the
#      shortlisted candidates whose body ALSO references another confirmed
#      SearchResultNode offset (0x6c/0x110/0x114/0x78), to avoid false positives from the
#      many unrelated structs in this binary that also happen to use offset 0x74.

FULL_DECOMPILE_TARGETS = [0x0046dc00, 0x0046db40, 0x0046e9e0, 0x0046e880, 0x0046e890]

args = getScriptArgs()
report_file = File(args[0]) if args else File(File(currentProgram.getExecutablePath()).getParentFile(), "report.txt")
out = PrintWriter(report_file, "UTF-8")

fm = currentProgram.getFunctionManager()
listing = currentProgram.getListing()

ifc = DecompInterface()
ifc.openProgram(currentProgram)
monitor = ConsoleTaskMonitor()

def full_decompile(addr):
    fn = fm.getFunctionAt(toAddr(addr))
    if fn is None:
        out.printf("!!! No function at 0x%08x\n" % addr)
        return
    out.printf("===== Decompile of %s (%s) =====\n" % (fn.getName(True), fn.getEntryPoint()))
    res = ifc.decompileFunction(fn, 60, monitor)
    if res.decompileCompleted():
        out.print(res.getDecompiledFunction().getC())
    else:
        out.printf("!!! decompile failed: %s\n" % res.getErrorMessage())
    out.printf("\n\n")

for addr in FULL_DECOMPILE_TARGETS:
    full_decompile(addr)

out.printf("===== Fast instruction-level scan for byte stores to [reg+0x74] across the whole program =====\n")
out.flush()

candidate_funcs = {}
total_instr = 0
hits = 0
ii = listing.getInstructions(True)
for instr in ii:
    total_instr += 1
    if total_instr % 2000000 == 0:
        out.printf("...scanned %d instructions...\n" % total_instr)
        out.flush()
    mnem = instr.getMnemonicString()
    if mnem != "MOV":
        continue
    text = instr.toString()
    if "0x74]" not in text and "+ 0x74]" not in text:
        continue
    # Only interested in stores: operand 0 is the memory operand (destination).
    try:
        op0 = instr.getDefaultOperandRepresentation(0)
    except:
        op0 = ""
    if "[" not in op0 or "0x74" not in op0:
        continue
    # Skip EBP-relative negative-offset stores - those are stack locals (e.g. "[EBP + -0x74]"),
    # not a store into an object field at offset +0x74. We only want true "[reg + 0x74]" object
    # field writes (reg != EBP, or EBP without the leading minus, which never occurs for +0x74
    # as a positive field offset on this calling convention anyway).
    if "EBP" in op0 and "-0x74" in op0:
        continue
    hits += 1
    fn = fm.getFunctionContaining(instr.getAddress())
    if fn is None:
        continue
    key = fn.getEntryPoint()
    if key not in candidate_funcs:
        candidate_funcs[key] = []
    candidate_funcs[key].append((instr.getAddress(), text))

out.printf("Total instructions scanned: %d\n" % total_instr)
out.printf("Total raw '[reg+0x74]' MOV-destination hits: %d\n" % hits)
out.printf("Distinct containing functions: %d\n\n" % len(candidate_funcs))

for key in candidate_funcs:
    fn = fm.getFunctionAt(key)
    out.printf("---- function %s (%s), %d store(s) to +0x74 ----\n" % (fn.getName(True), key, len(candidate_funcs[key])))
    for addr, text in candidate_funcs[key]:
        out.printf("   %s  %s\n" % (addr, text))

out.printf("\n\n===== Decompiling shortlisted candidates that ALSO reference a sibling SearchResultNode offset (0x6c/0x110/0x114/0x78/0x6d) =====\n")
for key in candidate_funcs:
    fn = fm.getFunctionAt(key)
    res = ifc.decompileFunction(fn, 30, monitor)
    if not res.decompileCompleted():
        out.printf("---- %s: decompile failed (%s) ----\n\n" % (fn.getName(True), res.getErrorMessage()))
        continue
    code = res.getDecompiledFunction().getC()
    hasSibling = any(tok in code for tok in ["0x6c)", "0x110)", "0x114)", "0x78)", "0x6d)", "0x6c]", "0x110]", "0x114]", "0x78]"])
    marker = "LIKELY MATCH" if hasSibling else "no sibling-offset reference found"
    out.printf("---- %s (%s) [%s] ----\n" % (fn.getName(True), fn.getEntryPoint(), marker))
    out.print(code)
    out.printf("\n\n")

out.close()
