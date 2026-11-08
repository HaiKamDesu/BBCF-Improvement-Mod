# -*- coding: utf-8 -*-
# Pushes the repo's types into a Ghidra project, so the decompiler prints
# `defender->invuln_timer` instead of `*(int *)(param_1 + 0x994)`.
#
# Sources:
#   1. docs/Decompilation/types/structs.json (tools/decomp/gen_struct_layouts.py: the compiler's
#      layout of every game struct the mod declares) -> structures and enums in category /BBCF.
#      pad_* fields stay undefined, so the decompiler shows their offset.
#   2. docs/Decompilation/types/signatures.h -> full typed signatures (convention, return,
#      parameter names and types). Creates the function first if Ghidra never found it (vtable
#      targets), but only when the address is executable and not inside another function.
#   3. docs/Decompilation/*.md ledger notes of the form "thiscall(kbd, gameKey), ret 4" ->
#      convention and parameter names for functions with no line in signatures.h. Applied only
#      when the stack argument count matches ret N for callee-cleanup conventions; existing
#      parameter types are kept.
#
# Parameters get explicit storage (ECX/EDX/stack) so a thiscall's first parameter keeps its real
# name ("attacker") instead of Ghidra's automatic "this".
#
# Run after SyncRepoKnowledge.py (tools/decomp/sync_archive.sh does both). Re-runs are safe.
#
# args: <repo root> [report path]
import io
import json
import os
import re
from java.io import File, PrintWriter
from java.util import ArrayList
from ghidra.program.model.data import (CategoryPath, StructureDataType, UnionDataType, EnumDataType,
                                       PointerDataType, ArrayDataType, DataTypeConflictHandler,
                                       VoidDataType, CharDataType, SignedCharDataType, ByteDataType,
                                       BooleanDataType, ShortDataType, UnsignedShortDataType,
                                       WideChar16DataType, IntegerDataType, UnsignedIntegerDataType,
                                       LongLongDataType, UnsignedLongLongDataType, FloatDataType,
                                       DoubleDataType, Undefined1DataType, Undefined2DataType,
                                       Undefined4DataType, Undefined8DataType)
from ghidra.program.model.listing import ParameterImpl, ReturnParameterImpl, VariableStorage
from ghidra.program.model.listing.Function import FunctionUpdateType
from ghidra.program.model.symbol import SourceType
from ghidra.program.model.address import AddressSet

IMAGE_BASE = 0x00400000
CATEGORY = CategoryPath("/BBCF")
args = getScriptArgs()
repo = args[0]
out = PrintWriter(File(args[1]) if len(args) > 1 else File("apply_repo_types.txt"), "UTF-8")
program = currentProgram
dtm = program.getDataTypeManager()
fm = program.getFunctionManager()
memory = program.getMemory()
REPLACE = DataTypeConflictHandler.REPLACE_HANDLER


def read(path):
    with io.open(path, encoding="utf-8", errors="replace") as handle:
        return handle.read()


PRIMITIVES = {
    "void": VoidDataType.dataType, "code": VoidDataType.dataType,
    "char": CharDataType.dataType, "int8_t": SignedCharDataType.dataType, "signed char": SignedCharDataType.dataType,
    "uint8_t": ByteDataType.dataType, "unsigned char": ByteDataType.dataType, "byte": ByteDataType.dataType,
    "uchar": ByteDataType.dataType, "BYTE": ByteDataType.dataType, "bool": BooleanDataType.dataType,
    "int16_t": ShortDataType.dataType, "short": ShortDataType.dataType,
    "uint16_t": UnsignedShortDataType.dataType, "unsigned short": UnsignedShortDataType.dataType,
    "ushort": UnsignedShortDataType.dataType, "WORD": UnsignedShortDataType.dataType,
    "wchar_t": WideChar16DataType.dataType, "wchar16": WideChar16DataType.dataType,
    "int": IntegerDataType.dataType, "int32_t": IntegerDataType.dataType, "long": IntegerDataType.dataType,
    "BOOL": IntegerDataType.dataType,
    "uint32_t": UnsignedIntegerDataType.dataType, "unsigned int": UnsignedIntegerDataType.dataType,
    "uint": UnsignedIntegerDataType.dataType, "dword": UnsignedIntegerDataType.dataType,
    "DWORD": UnsignedIntegerDataType.dataType, "unsigned long": UnsignedIntegerDataType.dataType,
    "ulong": UnsignedIntegerDataType.dataType, "size_t": UnsignedIntegerDataType.dataType,
    "uintptr_t": UnsignedIntegerDataType.dataType,
    "int64_t": LongLongDataType.dataType, "longlong": LongLongDataType.dataType, "__int64": LongLongDataType.dataType,
    "long long": LongLongDataType.dataType,
    "uint64_t": UnsignedLongLongDataType.dataType, "ulonglong": UnsignedLongLongDataType.dataType,
    "unsigned long long": UnsignedLongLongDataType.dataType,
    "float": FloatDataType.dataType, "double": DoubleDataType.dataType,
    "undefined": Undefined1DataType.dataType, "undefined1": Undefined1DataType.dataType,
    "undefined2": Undefined2DataType.dataType, "undefined4": Undefined4DataType.dataType,
    "undefined8": Undefined8DataType.dataType,
}


def base_type(name):
    name = re.sub(r"^(const|volatile|class|struct|union|enum)\s+", "", name.strip())
    name = re.sub(r"\s+const$", "", name)
    if name in PRIMITIVES:
        return PRIMITIVES[name]
    return dtm.getDataType(CATEGORY, name)


def pointer_to(dt, depth):
    for _ in range(depth):
        dt = PointerDataType(dt, 4, dtm)
    return dt


# ---- 1. structs and enums ----------------------------------------------------------------
layout = json.loads(read(os.path.join(repo, "docs", "Decompilation", "types", "structs.json")))
enum_count = 0
for enum in layout.get("enums", []):
    size = max(1, base_type(enum["base"]).getLength() if base_type(enum["base"]) else 4)
    dt = EnumDataType(CATEGORY, enum["name"], size, dtm)
    for name, value in enum["values"]:
        dt.add(name, value & ((1 << (8 * size)) - 1))
    dt.setDescription("from %s" % enum["header"])
    dtm.addDataType(dt, REPLACE)
    enum_count += 1

# Phase 1: every struct exists at its real size, so others can embed or point to it.
for struct in layout["structs"]:
    if dtm.getDataType(CATEGORY, struct["name"]) is None:
        cls = UnionDataType if struct["kind"] == "union" else StructureDataType
        placeholder = cls(CATEGORY, struct["name"], dtm) if struct["kind"] == "union" else cls(CATEGORY, struct["name"], struct["size"], dtm)
        dtm.addDataType(placeholder, REPLACE)

# Phase 2: fill. Built fresh each run and swapped in with REPLACE, so references stay valid.
struct_count = field_count = untyped = 0
for struct in layout["structs"]:
    is_union = struct["kind"] == "union"
    dt = UnionDataType(CATEGORY, struct["name"], dtm) if is_union else StructureDataType(CATEGORY, struct["name"], struct["size"], dtm)
    dt.setDescription("from %s (layout: MSVC, via gen_struct_layouts.py)" % struct["header"])
    for field in struct["fields"]:
        name, span = field["name"], field["span"]
        if span <= 0:
            continue
        if re.match(r"^(pad|padding)_?", name, re.I) and field["pointers"] == 0:
            continue
        element = base_type(field["type"]) if field["type"] else None
        comment = field.get("comment") or None
        if element is None:
            if field["type"] and field["pointers"]:
                element = VoidDataType.dataType  # pointer to a type we do not lay out
                comment = (comment + "; " if comment else "") + "points to " + field["type"]
            else:
                element = Undefined1DataType.dataType
                if field["type"]:
                    comment = (comment + "; " if comment else "") + "type " + field["type"]
                untyped += 1
        element = pointer_to(element, field["pointers"])
        size = element.getLength()
        if size <= 0:
            continue
        if span == size:
            ftype = element
        elif span > size and span % size == 0:
            ftype = ArrayDataType(element, span // size, size, dtm)
        elif span > size:
            ftype = element  # trailing alignment padding
        else:
            ftype = ArrayDataType(Undefined1DataType.dataType, span, 1, dtm)
        if is_union:
            dt.add(ftype, ftype.getLength(), name, comment)
        else:
            dt.replaceAtOffset(field["offset"], ftype, ftype.getLength(), name, comment)
        field_count += 1
    dtm.addDataType(dt, REPLACE)
    struct_count += 1
out.printf("structs %d (%d fields, %d without a known type), enums %d%n", struct_count, field_count, untyped, enum_count)


# ---- 2/3. signatures -------------------------------------------------------------------------
reg = program.getRegister
EAX, ECX, EDX = reg("EAX"), reg("ECX"), reg("EDX")
CONVENTIONS = {"cdecl": "__cdecl", "stdcall": "__stdcall", "fastcall": "__fastcall", "thiscall": "__thiscall"}


def parse_type(text):
    """'CharData*' -> DataType, or None if unknown."""
    text = text.strip()
    depth = text.count("*")
    dt = base_type(text.replace("*", "").strip())
    return pointer_to(dt, depth) if dt is not None else None


def build_params(convention, params):
    """params: [(name, DataType)] -> [ParameterImpl] with explicit storage."""
    registers = {"__thiscall": [ECX], "__fastcall": [ECX, EDX]}.get(convention, [])
    result, stack = ArrayList(), 4
    for index, (name, dt) in enumerate(params):
        if index < len(registers) and dt.getLength() <= 4:
            storage = VariableStorage(program, registers[index])
        else:
            size = max(4, dt.getLength())
            storage = VariableStorage(program, stack, dt.getLength())
            stack += (size + 3) & ~3
        result.add(ParameterImpl(name, dt, storage, program))
    return result, stack - 4


def ensure_function(addr, name):
    fn = getFunctionAt(addr)
    if fn is not None:
        return fn
    block = memory.getBlock(addr)
    if block is None or not block.isExecute() or fm.getFunctionContaining(addr) is not None:
        return None
    disassemble(addr)
    fn = createFunction(addr, name)
    return fn


def apply(fn, convention, return_dt, params, source):
    plist, stack_bytes = build_params(convention, params)
    if return_dt is None or isinstance(return_dt, VoidDataType):
        ret = ReturnParameterImpl(VoidDataType.dataType, VariableStorage.VOID_STORAGE, program)
    elif return_dt.getLength() <= 4:
        ret = ReturnParameterImpl(return_dt, VariableStorage(program, EAX), program)
    else:
        ret = ReturnParameterImpl(return_dt, VariableStorage(program, EDX, EAX), program)
    fn.setCustomVariableStorage(True)
    fn.updateFunction(convention, ret, plist, FunctionUpdateType.CUSTOM_STORAGE, True, SourceType.USER_DEFINED)
    return stack_bytes


DECL = re.compile(r"^/\*\s*0x([0-9A-Fa-f]+)\s*\*/\s*(.+?)\s+(__cdecl|__stdcall|__fastcall|__thiscall)\s+(\w+)\s*\((.*)\)\s*;")
explicit = set()
typed = typed_failed = 0
for line_no, line in enumerate(read(os.path.join(repo, "docs", "Decompilation", "types", "signatures.h")).splitlines(), 1):
    m = DECL.match(line.strip())
    if not m:
        continue
    va = int(m.group(1), 16)
    va = va + IMAGE_BASE if va < IMAGE_BASE else va
    addr = toAddr(va)
    ret_text, convention, name, arg_text = m.group(2), m.group(3), m.group(4), m.group(5).strip()
    params, bad = [], None
    if arg_text and arg_text != "void":
        for index, arg in enumerate(arg_text.split(",")):
            am = re.match(r"^(.*?[\w*])\s*(\w+)$", arg.strip())
            if not am:
                bad = "cannot read parameter '%s'" % arg.strip()
                break
            dt = parse_type(am.group(1))
            if dt is None:
                bad = "unknown type '%s'" % am.group(1)
                break
            params.append((am.group(2), dt))
    return_dt = parse_type(ret_text)
    if return_dt is None:
        bad = bad or "unknown return type '%s'" % ret_text
    fn = ensure_function(addr, name) if not bad else None
    if fn is None and not bad:
        bad = "no function at the address and none could be created"
    if bad:
        out.printf("  signatures.h:%d %08X %s: %s%n", line_no, va, name, bad)
        typed_failed += 1
        continue
    if fn.getName() != name:
        fn.setName(name, SourceType.USER_DEFINED)
    apply(fn, convention, return_dt, params, "signatures.h")
    explicit.add(va)
    typed += 1
    out.printf("  typed    %08X %s%n", va, fn.getSignature().getPrototypeString())

# Ledger notes: "thiscall(kbd, gameKey), ret 4"
NOTE = re.compile(r"\b(cdecl|stdcall|fastcall|thiscall)\s*\(([^()]*)\)[^|]*?\bret\b(?:\s+(0x[0-9A-Fa-f]+|[0-9A-Fa-f]+h|\d+)\b)?", re.I)


def ident(text, index):
    words = re.findall(r"[A-Za-z_][A-Za-z0-9_]*", re.sub(r"\[.*?\]", "", text))
    words = [w for w in words if w not in ("const", "out", "in", "struct", "class", "unsigned", "char", "int", "void", "uint32_t", "int32_t", "bool")]
    if not words:
        return "arg%d" % index
    if words[0] == "this":
        return "self"
    name = words[0] + "".join(w[:1].upper() + w[1:] for w in words[1:])
    if text.strip().startswith("out "):
        name = "out" + name[:1].upper() + name[1:]
    return name


# 8-byte arguments take two stack slots.
WIDE = re.compile(r"(?:SteamAPICall_t|CSteamID|u?int64(?:_t)?|__int64|long long|\bdouble|\bsteamid)\b(?!_)(?!\s*\*)", re.I)
noted = noted_skipped = 0
docs_dir = os.path.join(repo, "docs", "Decompilation")
for doc in sorted(os.listdir(docs_dir)):
    if not doc.endswith(".md"):
        continue
    for line in read(os.path.join(docs_dir, doc)).splitlines():
        if not re.match(r"^\|\s*0x[0-9A-Fa-f]{6,8}\s*\|", line):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        va = int(re.match(r"0x([0-9A-Fa-f]+)", cells[0]).group(1), 16)
        va = va + IMAGE_BASE if va < IMAGE_BASE else va
        if va in explicit:
            continue
        m = NOTE.search(cells[-1])
        if not m:
            continue
        convention = CONVENTIONS[m.group(1).lower()]
        raw = [a.strip() for a in m.group(2).split(",") if a.strip()]
        if any("..." in a or re.search(r"\bargs?\b", a) for a in raw):
            continue
        fn = getFunctionAt(toAddr(va))
        if fn is None:
            continue
        registers = {"__thiscall": 1, "__fastcall": 2}.get(convention, 0)
        wide = [bool(WIDE.search(a)) for a in raw]
        stack_args = sum(2 if w else 1 for w in wide[registers:])
        # "ret 0Ch" / "ret 0xC" are hex; a bare "ret 10" after 0x10-style notes is ambiguous, so it
        # counts if either reading matches.
        text_n = m.group(3) or "0"
        if text_n.lower().endswith("h"):
            readings = [int(text_n[:-1], 16)]
        elif text_n.lower().startswith("0x"):
            readings = [int(text_n, 16)]
        else:
            readings = [int(text_n), int(text_n, 16)]
        ret_n = readings[0]
        if convention in ("__thiscall", "__stdcall", "__fastcall"):
            if 4 * stack_args in readings:
                ret_n = 4 * stack_args
            if ret_n != 4 * stack_args:
                out.printf("  skipped  %08X %s: %s(%s) has %d stack args but ret %d (%s)%n", va, fn.getName(), m.group(1), m.group(2), stack_args, ret_n, doc)
                noted_skipped += 1
                continue
        elif any(readings):
            out.printf("  skipped  %08X %s: cdecl with ret %d (%s)%n", va, fn.getName(), ret_n, doc)
            noted_skipped += 1
            continue
        existing = fn.getParameters()
        params, used = [], set()
        for index, text in enumerate(raw):
            name = ident(text, index)
            while name in used:
                name += "_"
            used.add(name)
            dt = Undefined8DataType.dataType if wide[index] else Undefined4DataType.dataType
            if index < len(existing) and not wide[index]:
                old = existing[index].getDataType()
                if old.getLength() == 4 and not old.getName().startswith("undefined"):
                    dt = old
            params.append((name, dt))
        old_ret = fn.getReturnType()
        apply(fn, convention, old_ret, params, doc)
        noted += 1

out.printf("%nsignatures.h: %d typed, %d failed; ledger notes: %d applied, %d skipped (count/ret mismatch)%n",
           typed, typed_failed, noted, noted_skipped)
out.close()
print("types: %d structs, %d enums; signatures %d typed (%d failed); ledger conventions %d applied, %d skipped"
      % (struct_count, enum_count, typed, typed_failed, noted, noted_skipped))
