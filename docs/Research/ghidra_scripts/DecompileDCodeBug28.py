# -*- coding: utf-8 -*-
# Phase 28: the "TUS" layer is an HTTP client talking to
# http://153.122.81.62/steam/api (endpoint table at 009D4C54: 9=tus/read,
# 10=tus/write). FUN_00434750 only issues a new request when
#   FUN_00433010(type) == 0  ||  pending[type] && *pending[type] && *(pending+0xE38) == 0
# otherwise it silently skips and the caller re-parses the STALE response ->
# length 0 -> "TUS data absent" -> workmgr state 9. This phase decompiles the
# gate and the HTTP request object so we can see what +0xE38 is and whether it
# can be reset from outside.
from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

FUNCS = [
    (0x00433010, "gate: may a new request of this type be issued?"),
    (0x00433d90, "parse response into ResponseParameterParser"),
    (0x00438890, "create/start HTTP request (returns handle)"),
    (0x00438a30, "destroy HTTP request"),
    (0x00438d20, "kick/advance HTTP request"),
    (0x004309b0, "TUS client singleton ctor (0x148 bytes)"),
    (0x00434060, "poll TUS write result"),
    (0x0042b130, "base-URL builder"),
    (0x00432730, "serialize request params"),
    (0x00434a40, "?"),
    (0x00427150, "?"),
    (0x00432f50, "?"),
]

def get_fn(addr):
    fn = getFunctionAt(addr)
    if fn is None:
        fn = getFunctionContaining(addr)
    return fn

def dec(out, ifc, fn, note=""):
    out.printf("----- DECOMPILE %s %s -----%n", fn.getEntryPoint(), note)
    r = ifc.decompileFunction(fn, 180, ConsoleTaskMonitor())
    if r.decompileCompleted():
        out.println(r.getDecompiledFunction().getC())
    else:
        out.printf("Decompile failed: %s%n", r.getErrorMessage())
    out.println()

args = getScriptArgs()
out = PrintWriter(File(args[0]) if len(args) > 0 else File("dcode_bug28.txt"), "UTF-8")
ifc = DecompInterface()
try:
    ifc.openProgram(currentProgram)
    out.printf("Program: %s%nImage base: %s%n%n", currentProgram.getName(), currentProgram.getImageBase())
    for addr, note in FUNCS:
        fn = get_fn(toAddr(addr))
        if fn is None:
            out.printf("----- %08X: no function -----%n%n", addr)
            continue
        dec(out, ifc, fn, "(%s)" % note)
        out.printf("--- callers of %08X ---%n", addr)
        for c in fn.getCallingFunctions(ConsoleTaskMonitor()):
            out.printf("  %s %s%n", c.getEntryPoint(), c.getName())
        out.println()
finally:
    ifc.dispose(); out.close()
print("done")
