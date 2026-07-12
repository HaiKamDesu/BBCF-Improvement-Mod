from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

# DecompileNetworkLobbyDataCallers.py found that 0046a820 (the CNetworkLobbyData singleton
# getter) has exactly one caller: thunk_FUN_0046a820 @ 0x004a29f0. That means real usage sites
# call the THUNK, not 0046a820 directly - this script finds callers of the thunk instead, plus
# callers of the two sibling vtables discovered as DATA-only refs to the shared slot6/7/1/2
# functions (CSTEAMNetworkLobbyData @ 0089c88c and a third vtable referencing 004a2a00 near
# 0089c34c/58 - resolve what class that is too), to find the real code path from "get the
# lobby-data singleton" to "read a per-row value" that a UI draw routine could consume.

TARGETS = {
    0x004a29f0: "thunk_GetSingleton",
}

args = getScriptArgs()
report_file = File(args[0]) if args else File(File(currentProgram.getExecutablePath()).getParentFile(), "report.txt")
out = PrintWriter(report_file, "UTF-8")

ifc = DecompInterface()
ifc.openProgram(currentProgram)
monitor = ConsoleTaskMonitor()
fm = currentProgram.getFunctionManager()
symtab = currentProgram.getSymbolTable()

def qname_at(addr):
    syms = symtab.getSymbols(addr)
    if syms:
        try:
            return syms[0].getName(True)
        except Exception:
            return syms[0].getName()
    fn = fm.getFunctionContaining(addr)
    return fn.getName() if fn else str(addr)

for tv, label in TARGETS.items():
    target = toAddr(tv)
    out.printf("===== Callers of %s (%s) =====\n" % (target, label))
    refs = list(getReferencesTo(target))
    out.printf("  (%d references)\n" % len(refs))
    seen_callers = set()
    for r in refs:
        from_addr = r.getFromAddress()
        caller_fn = getFunctionContaining(from_addr)
        cname = caller_fn.getName() if caller_fn else "<no function>"
        out.printf("  %s  %-12s caller=%s\n" % (from_addr, r.getReferenceType(), cname))
        if caller_fn is not None:
            seen_callers.add(caller_fn.getEntryPoint())

    out.printf("\n  ---- Decompiling %d unique caller functions ----\n" % len(seen_callers))
    for caddr in seen_callers:
        caller_fn = fm.getFunctionAt(caddr)
        out.printf("\n  ----- caller %s %s -----\n" % (caddr, caller_fn.getName()))
        res = ifc.decompileFunction(caller_fn, 60, monitor)
        if res.decompileCompleted():
            out.printf(res.getDecompiledFunction().getC())
        else:
            out.printf("  <decompile failed: %s>\n" % res.getErrorMessage())
    out.printf("\n\n")

# Identify the class that owns the vtable near 0089c318-0089c360 (referenced 004a2a00 from
# 0089c34c/0089c358) and confirm CSTEAMNetworkLobbyData's vtable at 0089c88c layout.
out.printf("===== Sibling vtable identification =====\n")
for probe_addr in [0x0089c318, 0x0089c88c]:
    a = toAddr(probe_addr)
    syms = symtab.getSymbols(a)
    names = [ (s.getName(True) if hasattr(s, 'getName') else str(s)) for s in syms ]
    out.printf("Address %s symbols: %s\n" % (a, names))
    # dump nearby symbols within -0x40..+0x100 to identify the enclosing class label
    it = symtab.getSymbolIterator(a.subtract(0x40), True)
    count = 0
    while it.hasNext() and count < 20:
        s = it.next()
        out.printf("   near %s -> %s\n" % (s.getAddress(), s.getName(True)))
        count += 1

out.close()
ifc.dispose()
