from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor
from ghidra.program.model.address import Address

# Goal: pin down the real argument passed to the ranked-list row-entry's tier-getter
# vtable call (slot 7, offset 0x1c) inside FUN_00661060 (the confirmed ranked-list row
# renderer). The decompiler pretty-prints this as:
#     uVar2 = (**(code **)(*piVar7 + 0x1c))(uVar10);
# where uVar10's only visible assignments in the pretty-print are `uVar10 = local_bf0;`
# (a field of param_1, i.e. the list-window object, NOT per-row data) at multiple points
# BEFORE and inside the loop, but never inside the "valid room" (else) branch that leads
# to this call. Per the project's own prior finding that this decompiler mis-renders
# argument counts near FPU-register-passing idioms in this exact function family
# (FUN_00655260 call, corrected via raw disasm to 6 real args vs 4 shown), this script
# dumps the raw x86 instructions in the vicinity of the vtable-slot-7 CALL site so the
# actual register/stack value fed to the call can be confirmed independently of the
# decompiler's naming.

TARGET_FUNC = 0x00661060

args = getScriptArgs()
report_file = File(args[0]) if args else File(File(currentProgram.getExecutablePath()).getParentFile(), "report.txt")
out = PrintWriter(report_file, "UTF-8")

fm = currentProgram.getFunctionManager()
listing = currentProgram.getListing()

fn = fm.getFunctionAt(toAddr(TARGET_FUNC))
out.printf("===== Full disassembly of %s (%s) =====\n" % (fn.getName(True), fn.getEntryPoint()))

body = fn.getBody()
ii = listing.getInstructions(body, True)

# Collect (addr, instr-text) pairs so we can print a window around any CALL [reg+0x1c]-shaped
# instruction (vtable slot 7 dispatch) found anywhere in the function.
all_instrs = []
for instr in ii:
    all_instrs.append((instr.getAddress(), instr.toString()))

out.printf("Total instructions: %d\n\n" % len(all_instrs))

# Print the whole disassembly (function is large but this is the ground truth we need).
for addr, text in all_instrs:
    out.printf("%s  %s\n" % (addr, text))

out.printf("\n\n===== Candidate CALL sites matching vtable-slot-7 (+0x1c) dispatch pattern =====\n")
for idx, (addr, text) in enumerate(all_instrs):
    if "CALL" in text and ("0x1c" in text or "+ 0x1c" in text or "[" in text and "1c]" in text.replace(" ", "")):
        lo = max(0, idx - 25)
        hi = min(len(all_instrs), idx + 5)
        out.printf("\n---- window around %s : %s ----\n" % (addr, text))
        for j in range(lo, hi):
            marker = ">>" if j == idx else "  "
            out.printf("%s %s  %s\n" % (marker, all_instrs[j][0], all_instrs[j][1]))

out.close()
