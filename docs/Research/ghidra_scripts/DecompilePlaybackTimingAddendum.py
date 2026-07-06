# Ghidra headless Jython script. Addendum to DecompilePlaybackTiming.py:
# decompiles the top-level per-tick function (0x0056B1F0) that calls both the
# playback-consumer chain (via FUN_00559fe0) and the frame-counter tick chain
# (via FUN_005521b0), to confirm their relative call order in one place, plus
# the direct-global-address playback_control/position handler FUN_006caf90.
#
# Usage (project already imported):
# analyzeHeadless <project_dir> BBCF -process BBCF.exe -noanalysis -scriptPath <script_dir> -postScript DecompilePlaybackTimingAddendum.py <report_path>

from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

TARGETS = [
    0x0056B1F0,  # per-tick Update: calls FUN_00559fe0 (consumer chain) then FUN_005521b0 (frame counter chain)
    0x0056B0C8,  # fixed-timestep pump that calls FUN_0056b1f0 (search will resolve containing function)
    0x006CAF90,  # direct-global-address playback_control/position handler
]


def decompile_function(ifc, out, fn):
    if fn is None:
        out.println("<no function>")
        return
    out.printf("Function: %s at %s%n", fn.getName(), fn.getEntryPoint())
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
    report_file = File(File(currentProgram.getExecutablePath()).getParentFile(), "playback_timing_addendum.txt")

ifc = DecompInterface()
if not ifc.openProgram(currentProgram):
    raise Exception("openProgram failed")

out = PrintWriter(report_file, "UTF-8")
try:
    out.printf("Program: %s%n", currentProgram.getName())
    out.printf("Image base: %s%n%n", currentProgram.getImageBase())

    for addr_value in TARGETS:
        addr = toAddr(addr_value)
        fn = getFunctionAt(addr)
        if fn is None:
            fn = getFunctionContaining(addr)
        out.printf("===== %s =====%n", addr)
        decompile_function(ifc, out, fn)
finally:
    out.close()
    ifc.dispose()

print("Wrote %s" % report_file.getAbsolutePath())
