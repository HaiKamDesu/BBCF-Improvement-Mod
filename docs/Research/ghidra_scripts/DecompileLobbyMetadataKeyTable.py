from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

# Goal: FUN_0046fcc0 (confirmed writer of GAMESTEAM_SearchResultNode's tier byte at +0x74)
# reads up to 18 Steam lobby-data metadata KEY/VALUE pairs per row (via a SteamMatchmaking
# vtable call pattern, thiscall-through-SteamInternal_ContextInit) and string-compares each
# KEY against a fixed table of 18 key-name string pointers at PTR_s_NETWORK_VERSION_009d9610,
# writing the corresponding parsed VALUE into a specific SearchResultNode field depending on
# which key matched (index 2 -> +0x74, our tier byte; index 3 -> +0x10, a wide string; index
# 5/6/7 -> +0x6c/+0x6f/+0x6e; etc.). This script dumps the actual 18 string literals in that
# key-name table (so we know the game's OWN name for the "+0x74 tier" field) and fully
# decompiles FUN_0041c8f0 (the per-value parser called right before every field write) to
# learn exactly how a raw Steam lobby-data string becomes the stored value (e.g. atoi).

KEY_TABLE_ADDR = 0x009d9610
KEY_TABLE_COUNT = 18
PARSER_FN = 0x0041c8f0
WRITER_FN = 0x0046fcc0

args = getScriptArgs()
report_file = File(args[0]) if args else File(File(currentProgram.getExecutablePath()).getParentFile(), "report.txt")
out = PrintWriter(report_file, "UTF-8")

listing = currentProgram.getListing()
fm = currentProgram.getFunctionManager()
mem = currentProgram.getMemory()

out.printf("===== Key-name string table at 0x%08x (%d entries) =====\n" % (KEY_TABLE_ADDR, KEY_TABLE_COUNT))
base = toAddr(KEY_TABLE_ADDR)
for i in range(KEY_TABLE_COUNT):
    entryAddr = base.add(i * 4)
    try:
        ptrVal = mem.getInt(entryAddr) & 0xffffffff
        strAddr = toAddr(ptrVal)
        data = listing.getDataAt(strAddr)
        s = None
        if data is not None and data.hasStringValue():
            s = data.getValue()
        else:
            # Manually read a bounded C-string if Ghidra didn't auto-define it as string data.
            b = []
            a = strAddr
            for _ in range(128):
                byteVal = mem.getByte(a) & 0xff
                if byteVal == 0:
                    break
                b.append(chr(byteVal))
                a = a.add(1)
            s = "".join(b)
        out.printf("  [%d] 0x%08x -> %r\n" % (i, ptrVal, s))
    except Exception as e:
        out.printf("  [%d] !!! error: %s\n" % (i, e))

out.printf("\n\n===== Decompile of FUN_0041c8f0 (per-value parser) =====\n")
ifc = DecompInterface()
ifc.openProgram(currentProgram)
monitor = ConsoleTaskMonitor()
fn = fm.getFunctionAt(toAddr(PARSER_FN))
if fn is None:
    out.printf("!!! No function at 0x%08x\n" % PARSER_FN)
else:
    res = ifc.decompileFunction(fn, 60, monitor)
    if res.decompileCompleted():
        out.print(res.getDecompiledFunction().getC())
    else:
        out.printf("!!! decompile failed: %s\n" % res.getErrorMessage())

out.printf("\n\n===== Full decompile of FUN_0046fcc0 (confirmed +0x74 tier-byte writer) for reference =====\n")
fn2 = fm.getFunctionAt(toAddr(WRITER_FN))
if fn2 is not None:
    res2 = ifc.decompileFunction(fn2, 60, monitor)
    if res2.decompileCompleted():
        out.print(res2.getDecompiledFunction().getC())
    else:
        out.printf("!!! decompile failed: %s\n" % res2.getErrorMessage())

out.printf("\n\n===== Callers of FUN_0046fcc0 (to learn when/how often a row's metadata gets (re)populated) =====\n")
refs = getReferencesTo(toAddr(WRITER_FN))
for ref in refs:
    callerFn = fm.getFunctionContaining(ref.getFromAddress())
    if callerFn is not None:
        out.printf("  called from %s at %s\n" % (callerFn.getName(True), ref.getFromAddress()))
    else:
        out.printf("  called from (no function) at %s\n" % ref.getFromAddress())

out.close()
