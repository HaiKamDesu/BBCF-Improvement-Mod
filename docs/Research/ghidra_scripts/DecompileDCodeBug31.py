# -*- coding: utf-8 -*-
# Phase 31: BBCF statically links libcurl 7.54.1. The WebApi client
# (DAT_00A5A168) keeps ONE shared object at +0x70 that only FUN_00434D30 ever
# tears down (via FUN_00427150) -- prime suspect for the process-lifetime
# stickiness, most likely a CURLM multi handle carrying libcurl's connection
# cache. Goals:
#   1. decompile the request create/destroy/pump layer
#   2. find every access to WebApi client +0x70
#   3. anchor curl_easy_setopt / curl_multi_* via their distinctive strings, so
#      a fix can force a fresh connection instead of reusing a dead one
from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

FUNCS = [
    (0x00438890, "create/start HTTP request -> handle"),
    (0x00438a30, "destroy HTTP request"),
    (0x00438d20, "kick/advance HTTP request"),
    (0x00427150, "tears down WebApi client +0x70"),
    (0x00433d90, "parse response (sets the +0x108 error the 0xB branch reads)"),
    (0x00434d30, "WebApi client teardown (frees +0x40 slots and +0x70)"),
    (0x00432f50, "?"),
    (0x00434a30, "?"),
    (0x00434a40, "?"),
]

# Distinctive libcurl strings -> the function containing the reference is the
# corresponding libcurl entry point.
STRING_ANCHORS = [
    "CURLOPT_SSL_VERIFYHOST no longer supports 1 as value!",
    "Found pending candidate for reuse",
    "A libcurl function was given a bad argument",
    "An unknown option was passed in to libcurl",
    "Recv failure",
    "Send failure",
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
out = PrintWriter(File(args[0]) if len(args) > 0 else File("dcode_bug31.txt"), "UTF-8")
ifc = DecompInterface()
try:
    ifc.openProgram(currentProgram)
    out.printf("Program: %s%nImage base: %s%n%n", currentProgram.getName(), currentProgram.getImageBase())
    rm = currentProgram.getReferenceManager()

    out.println("===== WHO TOUCHES THE WEBAPI CLIENT SINGLETON (DAT_00A5A168) =====")
    for ref in rm.getReferencesTo(toAddr(0x00A5A168)):
        fn = get_fn(ref.getFromAddress())
        out.printf("  %s from %s in %s%n", ref.getReferenceType(), ref.getFromAddress(),
                   fn.getName() if fn else "?")
    out.println()

    out.println("===== LIBCURL STRING ANCHORS =====")
    mem = currentProgram.getMemory()
    for want in STRING_ANCHORS:
        found = None
        addr = mem.findBytes(currentProgram.getMinAddress(),
                             want.encode("ascii"), None, True, ConsoleTaskMonitor())
        if addr is None:
            out.printf("  %-55s NOT FOUND%n", want)
            continue
        out.printf("  %-55s @ %s%n", want, addr)
        refs = list(rm.getReferencesTo(addr))
        if not refs:
            out.printf("      (no direct xrefs)%n")
        for ref in refs:
            fn = get_fn(ref.getFromAddress())
            out.printf("      from %s in %s%n", ref.getFromAddress(),
                       fn.getName() if fn else "?")
    out.println()

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
