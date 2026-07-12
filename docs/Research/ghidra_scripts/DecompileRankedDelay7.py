# Confirm the 64-bit value at session+0x28/+0x2c is a CSteamID by decompiling its consumer.
from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor
TARGETS = [0x004a8ab0, 0x004a5450]
def get_fn(a):
    f=getFunctionAt(toAddr(a)); return f if f else getFunctionContaining(toAddr(a))
args=getScriptArgs(); out=PrintWriter(File(args[0]) if args else File("rd7.txt"),"UTF-8")
ifc=DecompInterface(); ifc.openProgram(currentProgram)
try:
    for a in TARGETS:
        fn=get_fn(a)
        if not fn: out.printf("no fn %s%n",toAddr(a)); continue
        out.printf("----- %s %s -----%n", fn.getEntryPoint(), fn.getName())
        r=ifc.decompileFunction(fn,120,ConsoleTaskMonitor())
        out.println(r.getDecompiledFunction().getC() if r.decompileCompleted() else "FAIL"); out.println()
finally: ifc.dispose(); out.close()
print("done")
