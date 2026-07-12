from java.io import File, PrintWriter
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

# Find the missing "constructor"/populator of the ranked-list MGR singleton at
# DAT_00c97e3c (RVA 0x897E3C, guard DAT_00c97e40 RVA 0x897E40). FUN_00486070 is the
# lazy-init GETTER (277 call sites / 204 unique callers); it only ever writes 0 (init) and
# gets read everywhere else. Somewhere among those 204 callers must be a register-indirect
# WRITE of a real object pointer into *DAT_00c97e3c, and/or a constructor that builds the
# intrusive per-row list (head at object+0xae0, next/prev at node+4/+8, count at +0xae8,
# permutation array at +0xaf4) - finding that constructor's RTTI/vtable-assignment would
# give the concrete class name for both MGR and its list nodes (ENTRY).
#
# Approach: decompile every one of the 204 unique callers of FUN_00486070 (list taken from
# RankedListMgrSingletonXrefsGhidraReport.txt) and dump full C so a later grep pass can
# look for assignment patterns into +0xae0/+0xae8/+0xaf4/+0x1c (vtable) through the
# singleton's returned pointer, rather than just reads.

MGR_GETTER = 0x00486070

CALLER_ADDRS = [
0x00472700,0x00473c60,0x004791c0,0x00479850,0x0047ec20,0x0047f390,0x00496e40,0x00496f20,
0x00496ff0,0x00497320,0x004974e0,0x004979e0,0x00497d40,0x00498480,0x004989e0,0x00499720,
0x00499aa0,0x00499b00,0x0049a810,0x0049a8f0,0x0049aa30,0x0049aae0,0x0049ab40,0x0049aea0,
0x0049c000,0x0049df90,0x0049e2d0,0x0049e630,0x0049e820,0x0049ed30,0x0049ee00,0x0049f590,
0x0049f990,0x004a01e0,0x004a0400,0x004a4110,0x004a41d0,0x004a4440,0x004a47c0,0x004a5dc0,
0x004a6670,0x004a6f70,0x004a73d0,0x004a7b40,0x004a7b90,0x004a8190,0x004a8910,0x004a89d0,
0x004a8ab0,0x004a8b90,0x004a8bd0,0x004a8d50,0x004a94c0,0x004a9780,0x004a97e0,0x004a9de0,
0x004aa890,0x004aaad0,0x004aba50,0x004ac6c0,0x004aced0,0x004ade90,0x004ae6d0,0x004af1a0,
0x004afc90,0x004b01b0,0x004b0970,0x004b1240,0x004b24a0,0x004c8fb0,0x004d2e70,0x004d30c0,
0x004d3df0,0x004d3fe0,0x004ec020,0x00502bb0,0x00503340,0x00505580,0x0050c2b0,0x0050c330,
0x0050c750,0x0051be50,0x00529e20,0x0052a720,0x0052a740,0x0053f5a0,0x0053f790,0x0053fc80,
0x00547490,0x00548540,0x0054a960,0x0054ae00,0x0054afe0,0x0054b280,0x0054beb0,0x0054c140,
0x0054c400,0x0054c490,0x0054c620,0x0054c6e0,0x005500b0,0x005502c0,0x005529c0,0x00567d30,
0x00568cf0,0x0056bfe0,0x0056fce0,0x0057f470,0x005b6310,0x005b6390,0x005b6420,0x005b65e0,
0x005d8f90,0x005d9380,0x005f2830,0x005f5910,0x005f5c70,0x005f6030,0x005f62f0,0x005f7120,
0x0060f7e0,0x0060fd70,0x006103a0,0x00610470,0x00611390,0x00611530,0x00611790,0x00611930,
0x00611ad0,0x00612200,0x00614330,0x0061a350,0x0061b3b0,0x00626d00,0x00635d80,0x00657150,
0x00666880,0x0066bad0,0x0066d9a0,0x00672b40,0x00673250,0x00676390,0x0068a530,0x006926e0,
0x006a1420,0x006aa060,0x006ac1c0,0x006b2620,0x006b2a30,0x006b58c0,0x006c1f40,0x006de420,
0x006eb340,0x006f5340,0x006f65b0,0x00701170,0x00706a70,0x007084c0,0x0070d210,0x0070ee90,
0x007127b0,0x00713390,0x00713b60,0x007144d0,0x007157d0,0x00716590,0x00716bf0,0x00717460,
0x00729210,0x007296e0,0x007298d0,0x00729a90,0x00729f40,0x0072aec0,0x0072b130,0x0072b2e0,
0x0072b530,0x0072b6f0,0x0072b9a0,0x0072bba0,0x0072bd70,0x0072c280,0x007302a0,0x0073ac20,
0x0073c550,0x0073da40,0x007494d0,0x00749680,0x00749e20,0x00749fd0,0x0074a620,0x0074ab00,
0x0074acb0,0x0074ae60,0x0074b040,0x0074b1f0,0x00753450,0x007538c0,0x00754980,0x007552c0,
0x007569f0,0x00757a60,0x00758250,0x00764540,
]

args = getScriptArgs()
report_file = File(args[0]) if args else File(File(currentProgram.getExecutablePath()).getParentFile(), "report.txt")
out = PrintWriter(report_file, "UTF-8")

fm = currentProgram.getFunctionManager()
ifc = DecompInterface()
ifc.openProgram(currentProgram)
monitor = ConsoleTaskMonitor()

out.printf("Decompiling %d unique callers of FUN_00486070 for constructor/write-pattern search\n\n" % len(CALLER_ADDRS))
out.flush()

done = 0
for a in CALLER_ADDRS:
    fn = getFunctionAt(toAddr(a))
    if fn is None:
        fn = fm.getFunctionContaining(toAddr(a))
    if fn is None:
        out.printf("----- %08x: <no function> -----\n\n" % a)
        continue
    out.printf("----- %s %s -----\n" % (fn.getEntryPoint(), fn.getName()))
    try:
        res = ifc.decompileFunction(fn, 45, monitor)
        if res.decompileCompleted():
            out.print(res.getDecompiledFunction().getC())
        else:
            out.printf("<decompile failed: %s>\n" % res.getErrorMessage())
    except Exception as e:
        out.printf("<exception: %s>\n" % e)
    out.printf("\n\n")
    done += 1
    if done % 20 == 0:
        out.flush()

out.flush()
out.close()
ifc.dispose()
