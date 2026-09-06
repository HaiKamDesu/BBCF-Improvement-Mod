# -*- coding: utf-8 -*-
# Phase 29: every WebApi request body carries a "session" token
# (FUN_00432730: json_pack("{ss,ss,si,si,ss,si}", "steamId", .., "session",
# singleton+8, ...)). Find where that token is obtained (user/login, endpoint
# index 1, uei::tl::LoginRequestParam) and written back into the singleton
# (ResponseParameterParser<ResponseLogin>), so we can tell whether a stale
# session can be detected and refreshed from the mod.
from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

WANT = ["LoginRequestParam", "ResponseLogin", "ReadTusRequestParam",
        "WriteTusRequestParam", "WebApiRequestParam", "UserCreateRequestParam"]

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
out = PrintWriter(File(args[0]) if len(args) > 0 else File("dcode_bug29.txt"), "UTF-8")
ifc = DecompInterface()
seen = set()
try:
    ifc.openProgram(currentProgram)
    out.printf("Program: %s%nImage base: %s%n%n", currentProgram.getName(), currentProgram.getImageBase())
    st = currentProgram.getSymbolTable()
    targets = []
    for sym in st.getAllSymbols(True):
        n = sym.getName(True)
        if any(w in n for w in WANT):
            targets.append((n, sym.getAddress()))
    out.println("===== MATCHING SYMBOLS =====")
    for n, a in targets:
        out.printf("  %s  %s%n", a, n)
    out.println()

    rm = currentProgram.getReferenceManager()
    for n, a in targets:
        out.printf("===== XREFS TO %s (%s) =====%n", n, a)
        for ref in rm.getReferencesTo(a):
            fa = ref.getFromAddress()
            fn = get_fn(fa)
            out.printf("  from %s in %s%n", fa, fn.getName() if fn else "?")
            if fn is not None and fn.getEntryPoint().toString() not in seen:
                seen.add(fn.getEntryPoint().toString())
                targets.append(("__decompile__", fn.getEntryPoint()))
        out.println()

    out.println("===== DECOMPILES OF XREF SITES =====")
    for n, a in targets:
        if n != "__decompile__":
            continue
        fn = get_fn(a)
        if fn is None:
            continue
        dec(out, ifc, fn)
finally:
    ifc.dispose(); out.close()
print("done")
