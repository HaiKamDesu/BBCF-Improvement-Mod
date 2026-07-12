from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

# Room struct (per src/Game/Room/Room.h + hooks_bbcf.cpp GetRoomTwo, which computes
# pRoom = edi + 0x22D10 where edi is the same NetworkUserData/network-manager
# singleton pointer returned by getter FUN_004a0fe0, whose object lives at
# moduleBase(0x00400000) + kNetworkUserDataRva(0x008AD0C0)).
# Room.member1 is at Room+0x48 (Room.h), RoomMemberEntry.netcolor is at +0x5A
# (RoomMemberEntry.h), stride 0x98 per member, 8 members max.
MEMBER1_BASE = 0x00CCFE18
STRIDE = 0x98
NETCOLOR_OFF = 0x5A
NUM_MEMBERS = 8

ROOM_BASE = 0x00CCFDD0

args = getScriptArgs()
report_file = File(args[0]) if args else File(File(currentProgram.getExecutablePath()).getParentFile(), "RoomNetColorWritesReport.txt")

ifc = DecompInterface()
ifc.openProgram(currentProgram)
monitor = ConsoleTaskMonitor()

out = PrintWriter(report_file, "UTF-8")
try:
    targets = [("Room base", ROOM_BASE), ("member1 base", MEMBER1_BASE)]
    for i in range(NUM_MEMBERS):
        addr_val = MEMBER1_BASE + i * STRIDE + NETCOLOR_OFF
        targets.append(("member%d netcolor (+0x5A)" % (i + 1), addr_val))

    decompiled_funcs = set()
    for label, addr_val in targets:
        try:
            addr = toAddr(addr_val)
        except Exception as e:
            out.printf("===== %s (0x%x) - toAddr failed: %s =====%n", label, addr_val, str(e))
            continue

        out.printf("===== References to %s (%s) =====%n", label, addr)
        refs = list(getReferencesTo(addr))
        if not refs:
            out.printf("  (no references found)%n")
            continue

        for r in refs:
            from_addr = r.getFromAddress()
            ref_type = r.getReferenceType().toString()
            fn = getFunctionContaining(from_addr)
            fn_name = fn.getName() if fn else "<no function>"
            out.printf("  %s  %-16s  %s%n", from_addr, ref_type, fn_name)
            if fn:
                decompiled_funcs.add(fn.getEntryPoint())

    out.printf("%n===== Decompiled callers (%d unique functions) =====%n", len(decompiled_funcs))
    for entry in decompiled_funcs:
        fn = getFunctionAt(entry)
        if not fn:
            continue
        out.printf("----- %s (%s) -----%n", fn.getName(), entry)
        res = ifc.decompileFunction(fn, 60, monitor)
        if res and res.decompileCompleted():
            out.printf("%s%n", res.getDecompiledFunction().getC())
        else:
            out.printf("  (decompile failed)%n")
finally:
    out.close()
    ifc.dispose()
