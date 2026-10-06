# -*- coding: utf-8 -*-
# Decompiles every function in the program into a grep-able reference tree.
#
#   <out>/src/<64 KB bucket>.c   functions in address order, each headed by
#                                entry, size, callers and callees
#   <out>/functions.tsv          entry, name, size, bucket file, callers, callees,
#                                calling convention, decompiled ok
#   <out>/strings.tsv            every defined string with its address and the
#                                functions that reference it
#
# The output is decompiled game code: keep it out of the repo.
# Runs the decompiler on all cores (ParallelDecompiler).
#
# args: <output dir> [timeout seconds per function, default 90]
import os
from java.io import File, PrintWriter
from java.util import ArrayList
from ghidra.app.decompiler import DecompInterface, DecompileOptions
from ghidra.app.decompiler.parallel import ParallelDecompiler, DecompilerCallback, DecompileConfigurer
from ghidra.program.model.listing import Function
from ghidra.util.task import ConsoleTaskMonitor

args = getScriptArgs()
out_dir = args[0]
timeout = int(args[1]) if len(args) > 1 else 90
src_dir = os.path.join(out_dir, "src")
if not os.path.isdir(src_dir):
    os.makedirs(src_dir)

program = currentProgram
monitor = ConsoleTaskMonitor()
fm = program.getFunctionManager()


class Configurer(DecompileConfigurer):
    def configure(self, decompiler):
        options = DecompileOptions()
        options.grabFromProgram(program)
        decompiler.setOptions(options)
        decompiler.toggleCCode(True)
        decompiler.toggleSyntaxTree(False)
        decompiler.setSimplificationStyle("decompile")


class Callback(DecompilerCallback):
    def __init__(self):
        DecompilerCallback.__init__(self, program, Configurer())
        self.setTimeout(timeout)

    # Jython overrides both Java overloads of process(): the Function one is the queue's
    # entry point and must go to the base class, which decompiles and calls back here.
    def process(self, results, mon):
        if isinstance(results, Function):
            return DecompilerCallback.process(self, results, mon)
        fn = results.getFunction()
        ok = results.decompileCompleted()
        if ok:
            text = results.getDecompiledFunction().getC()
        else:
            text = "/* decompile failed: %s */\n" % results.getErrorMessage()
        return [fn.getEntryPoint().getOffset(), text, ok]


def bucket_of(offset):
    return "%08X.c" % (offset & ~0xFFFF)


functions = ArrayList()
for fn in fm.getFunctions(True):
    if not fn.isExternal() and not fn.isThunk():
        functions.add(fn)
print("decompiling %d functions" % functions.size())

results = ParallelDecompiler.decompileFunctions(Callback(), functions, monitor)
by_entry = {}
for r in results:
    if r is not None:
        by_entry[r[0]] = (r[1], r[2])

index = PrintWriter(File(os.path.join(out_dir, "functions.tsv")), "UTF-8")
index.println("entry\tname\tsize\tfile\tcallers\tcallees\tconvention\tdecompiled")
writers = {}
failed = 0
for fn in fm.getFunctions(True):
    if fn.isExternal():
        continue
    entry = fn.getEntryPoint().getOffset()
    bucket = bucket_of(entry)
    callers = sorted(set(c.getEntryPoint().toString() for c in fn.getCallingFunctions(monitor)))
    callees = sorted(set(c.getEntryPoint().toString() + " " + c.getName() for c in fn.getCalledFunctions(monitor)))
    size = fn.getBody().getNumAddresses()
    if fn.isThunk():
        text, ok = "/* thunk to %s */\n" % fn.getThunkedFunction(False).getName(), True
    else:
        text, ok = by_entry.get(entry, ("/* not decompiled */\n", False))
    if not ok:
        failed += 1
    w = writers.get(bucket)
    if w is None:
        w = PrintWriter(File(os.path.join(src_dir, bucket)), "UTF-8")
        writers[bucket] = w
    w.printf("//==== %08X %s  size=%d%n", entry, fn.getName(), size)
    w.printf("// callers: %s%n", ", ".join(callers) if callers else "-")
    w.printf("// callees: %s%n", ", ".join(callees) if callees else "-")
    w.println(text)
    index.printf("%08X\t%s\t%d\t%s\t%d\t%d\t%s\t%s%n", entry, fn.getName(), size, bucket,
                 len(callers), len(callees), fn.getCallingConventionName(), "1" if ok else "0")
for w in writers.values():
    w.close()
index.close()

# Strings and who uses them.
strings = PrintWriter(File(os.path.join(out_dir, "strings.tsv")), "UTF-8")
strings.println("address\ttype\tvalue\treferenced_from")
refs = program.getReferenceManager()
count = 0
for data in program.getListing().getDefinedData(True):
    if not data.hasStringValue():
        continue
    value = data.getDefaultValueRepresentation().replace("\t", "\\t").replace("\n", "\\n")
    users = set()
    for ref in refs.getReferencesTo(data.getAddress()):
        f = fm.getFunctionContaining(ref.getFromAddress())
        users.add(f.getName() if f is not None else ref.getFromAddress().toString())
    strings.printf("%s\t%s\t%s\t%s%n", data.getAddress(), data.getDataType().getName(), value, ",".join(sorted(users)))
    count += 1
strings.close()
print("done: %d functions (%d failed), %d buckets, %d strings" % (functions.size(), failed, len(writers), count))
