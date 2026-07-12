# Precisely decompile + disassemble thunk_FUN_0046a820 (the actual function called by
# row-scanning code, entry ~0x004a29f0) and FUN_0046a820 itself, to nail down the exact
# pointer chain: does the thunk return &DAT_00a5d270 directly, or dereference it once more?
from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

CANDIDATES = [0x004a29f0, 0x0046a820]

def get_fn(a):
    f = getFunctionAt(toAddr(a))
    return f if f else getFunctionContaining(toAddr(a))

def dump_listing(out, fn):
    out.printf("----- RAW DISASM %s %s -----%n", fn.getEntryPoint(), fn.getName())
    listing = currentProgram.getListing()
    for inst in listing.getInstructions(fn.getBody(), True):
        out.printf("%s: %s%n", inst.getAddress(), inst.toString())
    out.println()

args = getScriptArgs()
report = File(args[0]) if args else File("ranked_delay8.txt")
out = PrintWriter(report, "UTF-8")
ifc = DecompInterface(); ifc.openProgram(currentProgram)
try:
    # Also try resolving by name in case "thunk_FUN_0046a820" is a distinct symbol.
    st = currentProgram.getSymbolTable()
    for sym in st.getAllSymbols(True):
        name = sym.getName()
        if name == "thunk_FUN_0046a820" or name.startswith("thunk_FUN_0046a820"):
            out.printf("SYMBOL %s at %s (isFunction=%s)%n", name, sym.getAddress(),
                       getFunctionAt(sym.getAddress()) is not None)

    out.println()
    seen = set()
    for a in CANDIDATES:
        fn = get_fn(a)
        if fn is None:
            out.printf("no function at %s%n", toAddr(a)); continue
        key = fn.getEntryPoint().toString()
        out.printf("Looked up %s -> function entry %s name=%s isThunk=%s%n",
                   toAddr(a), fn.getEntryPoint(), fn.getName(), fn.isThunk())
        if fn.isThunk():
            thunked = fn.getThunkedFunction(True)
            out.printf("  thunkedFunction -> %s %s%n", thunked.getEntryPoint() if thunked else None,
                       thunked.getName() if thunked else None)
        if key in seen:
            continue
        seen.add(key)
        dump_listing(out, fn)
        out.printf("----- DECOMPILE %s %s -----%n", fn.getEntryPoint(), fn.getName())
        r = ifc.decompileFunction(fn, 120, ConsoleTaskMonitor())
        out.println(r.getDecompiledFunction().getC() if r.decompileCompleted() else ("FAIL " + r.getErrorMessage()))
        out.println()
finally:
    ifc.dispose(); out.close()
print("done")
