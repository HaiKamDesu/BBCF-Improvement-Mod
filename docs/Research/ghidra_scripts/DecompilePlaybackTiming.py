# Ghidra headless Jython script. Investigates the Unlimited-Playback 1-tick-late
# timing bug: decompiles the GetFrameCounter hook's containing function and its
# callers, the native set_playback_control function and its callers, and the
# playback-consumer function(s) that read playback_control/playback_position
# each frame, plus their callers, so the per-frame call order can be compared.
#
# Usage after import (or with -noanalysis once the project exists):
# analyzeHeadless <project_dir> BBCF -process BBCF.exe -noanalysis -scriptPath <script_dir> -postScript DecompilePlaybackTiming.py <report_path>

from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

# Addresses of interest (Ghidra space, image base 0x00400000).
# Some are exact function entry points, some are mid-function addresses of
# interest (e.g. the hooked instruction, or a specific field access) -- the
# script resolves the containing function for those.
FRAME_COUNTER_HOOK_POINT = 0x004D2409       # 8B 06 FF 46 0C signature match (mov eax,[esi]; inc [esi+0Ch])
FRAME_COUNTER_TICK_FN = 0x004D2400          # containing function entry
FRAME_COUNTER_TICK_CALLER = 0x004D27D0      # the only caller of FRAME_COUNTER_TICK_FN

SET_PLAYBACK_CONTROL_FN = 0x006CFB10        # native __thiscall set_playback_control(this, control, -1)

# playback consumer candidate: reads playback_control (+0x1AC2C) == 3 and
# playback_position (+0x1AC30) to fetch the recorded input value for the frame.
CONSUMER_FN = 0x006CB1F0

# function containing "inc dword ptr [edi+0x1AC30]" (playback_position advance)
POSITION_ADVANCE_POINT = 0x006D25BA

# direct field addresses (statically fixed since training_state_p is a
# compile-time-constant address, base+0x1392D10, not a runtime pointer)
PLAYBACK_CONTROL_ADDR = 0x017AD93C
PLAYBACK_POSITION_ADDR = 0x017AD940

TARGET_ENTRY_POINTS = [
    FRAME_COUNTER_TICK_FN,
    FRAME_COUNTER_TICK_CALLER,
    SET_PLAYBACK_CONTROL_FN,
    CONSUMER_FN,
]

CONTAINING_TARGETS = [
    ("FRAME_COUNTER_HOOK_POINT", FRAME_COUNTER_HOOK_POINT),
    ("POSITION_ADVANCE_POINT", POSITION_ADVANCE_POINT),
]

FIELD_ADDRS = [
    ("playback_control (+0x1AC2C)", PLAYBACK_CONTROL_ADDR),
    ("playback_position (+0x1AC30)", PLAYBACK_POSITION_ADDR),
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


def collect_callers(target):
    callers = []
    refs = getReferencesTo(target)
    for ref in refs:
        from_addr = ref.getFromAddress()
        fn = getFunctionContaining(from_addr)
        callers.append((from_addr, ref.getReferenceType().toString(), fn))
    return callers


def dump_callers_section(ifc, out, label, target_value, decompile_callers=True):
    target = toAddr(target_value)
    out.printf("===== References to %s (%s) =====%n", target, label)
    callers = collect_callers(target)
    for from_addr, ref_type, fn in callers:
        if fn is None:
            out.printf("%s %-16s <no function>%n", from_addr, ref_type)
        else:
            out.printf("%s %-16s %s at %s%n", from_addr, ref_type, fn.getName(), fn.getEntryPoint())
    out.println()

    if decompile_callers:
        seen = set()
        for from_addr, ref_type, fn in callers:
            if fn is None:
                continue
            key = fn.getEntryPoint().toString()
            if key in seen:
                continue
            seen.add(key)
            out.printf("--- Caller decompile: %s via %s ---%n", fn.getName(), from_addr)
            decompile_function(ifc, out, fn)


args = getScriptArgs()
if len(args) > 0:
    report_file = File(args[0])
else:
    report_file = File(File(currentProgram.getExecutablePath()).getParentFile(), "playback_timing_report.txt")

ifc = DecompInterface()
if not ifc.openProgram(currentProgram):
    raise Exception("openProgram failed")

out = PrintWriter(report_file, "UTF-8")
try:
    out.printf("Program: %s%n", currentProgram.getName())
    out.printf("Image base: %s%n%n", currentProgram.getImageBase())

    out.println("############################################")
    out.println("# Section 1: direct entry-point functions")
    out.println("############################################")
    out.println()
    for addr_value in TARGET_ENTRY_POINTS:
        addr = toAddr(addr_value)
        fn = getFunctionAt(addr)
        if fn is None:
            fn = getFunctionContaining(addr)
        out.printf("===== %s =====%n", addr)
        decompile_function(ifc, out, fn)

    out.println("############################################")
    out.println("# Section 2: functions containing specific instructions")
    out.println("############################################")
    out.println()
    for label, addr_value in CONTAINING_TARGETS:
        addr = toAddr(addr_value)
        fn = getFunctionContaining(addr)
        out.printf("===== %s: %s =====%n", label, addr)
        decompile_function(ifc, out, fn)

    out.println("############################################")
    out.println("# Section 3: callers of the frame-counter tick function")
    out.println("############################################")
    out.println()
    dump_callers_section(ifc, out, "FRAME_COUNTER_TICK_FN", FRAME_COUNTER_TICK_FN)

    out.println("############################################")
    out.println("# Section 4: callers of FRAME_COUNTER_TICK_CALLER (one more level up)")
    out.println("############################################")
    out.println()
    dump_callers_section(ifc, out, "FRAME_COUNTER_TICK_CALLER", FRAME_COUNTER_TICK_CALLER)

    out.println("############################################")
    out.println("# Section 5: callers of set_playback_control (native menu / mod trigger)")
    out.println("############################################")
    out.println()
    dump_callers_section(ifc, out, "SET_PLAYBACK_CONTROL_FN", SET_PLAYBACK_CONTROL_FN)

    out.println("############################################")
    out.println("# Section 6: callers of the playback-consumer function")
    out.println("############################################")
    out.println()
    dump_callers_section(ifc, out, "CONSUMER_FN", CONSUMER_FN)

    out.println("############################################")
    out.println("# Section 7: callers of consumer's callers (one more level up, no decompile)")
    out.println("############################################")
    out.println()
    consumer_callers = collect_callers(toAddr(CONSUMER_FN))
    seen_parent = set()
    for from_addr, ref_type, fn in consumer_callers:
        if fn is None or fn.getEntryPoint().toString() in seen_parent:
            continue
        seen_parent.add(fn.getEntryPoint().toString())
        dump_callers_section(ifc, out, "caller-of-caller for %s" % fn.getName(), fn.getEntryPoint().getOffset(), decompile_callers=False)

    out.println("############################################")
    out.println("# Section 8: all references to playback_control / playback_position fields")
    out.println("############################################")
    out.println()
    for label, addr_value in FIELD_ADDRS:
        dump_callers_section(ifc, out, label, addr_value, decompile_callers=False)

finally:
    out.close()
    ifc.dispose()

print("Wrote %s" % report_file.getAbsolutePath())
