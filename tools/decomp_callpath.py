#!/usr/bin/env python3
"""How does function A reach function B? Call paths through the BBCF decompile export.

Reads the `// callees:` header of every function in the private export (no Ghidra needed) and
prints the shortest call paths from FROM to TO, plus any up to --slack calls longer.

Only direct calls are edges. Calls through vtables, function-pointer tables and task/state
callbacks are not in the export, so "no path" means "no direct-call path": check the
subsystem doc for the dispatch table before concluding A never reaches B.

FROM and TO are addresses (0x4E60D5, 004e60d5) or function names (FUN_004e60d0 or a name the
export applied from the repo). An address inside a function resolves to that function.

Usage: python3 tools/decomp_callpath.py FROM TO [--slack 1] [--limit 20] [--max-depth 12]
       [--export C:/BBCF_Archive/build_11073075/decomp]
"""

import argparse
import bisect
import collections
import glob
import os
import re
import sys

DEFAULT_EXPORT = "/mnt/c/BBCF_Archive/build_11073075/decomp"
HEADER = re.compile(r"^//==== ([0-9A-Fa-f]{8}) (\S+)")
CALLEES = re.compile(r"^// callees: (.*)")
CALLEE_ADDRESS = re.compile(r"(?:^|, )([0-9A-Fa-f]{8})(?= )")


def load_export(export_dir):
    names, sizes = {}, {}
    with open(os.path.join(export_dir, "functions.tsv"), encoding="utf-8") as handle:
        next(handle)
        for line in handle:
            cols = line.rstrip("\n").split("\t")
            entry = int(cols[0], 16)
            names[entry] = cols[1]
            sizes[entry] = int(cols[2])

    callees = collections.defaultdict(set)
    for path in glob.glob(os.path.join(export_dir, "src", "*.c")):
        current = None
        with open(path, encoding="utf-8", errors="replace") as handle:
            for line in handle:
                match = HEADER.match(line)
                if match:
                    current = int(match.group(1), 16)
                    names.setdefault(current, match.group(2))
                    continue
                match = CALLEES.match(line)
                if match and current is not None:
                    # "-" means none; EXTERNAL: imports and comma-bearing C++ names are skipped
                    for address in CALLEE_ADDRESS.findall(match.group(1)):
                        callees[current].add(int(address, 16))
                    current = None
    return names, sizes, callees


def resolve(token, names, sizes):
    by_name = {name.lower(): entry for entry, name in names.items()}
    if token.lower() in by_name:
        return by_name[token.lower()]
    try:
        address = int(token, 16)
    except ValueError:
        sys.exit(f"'{token}' is neither a known function name nor a hex address")
    if address in names:
        return address
    entries = sorted(sizes)
    index = bisect.bisect_right(entries, address) - 1
    if index >= 0 and address < entries[index] + max(sizes[entries[index]], 1):
        return entries[index]
    sys.exit(f"0x{address:08X} is not inside any exported function")


def distances_to(target, callees, max_depth):
    callers = collections.defaultdict(set)
    for caller, targets in callees.items():
        for callee in targets:
            callers[callee].add(caller)
    distance = {target: 0}
    queue = collections.deque([target])
    while queue:
        node = queue.popleft()
        if distance[node] >= max_depth:
            continue
        for caller in callers[node]:
            if caller not in distance:
                distance[caller] = distance[node] + 1
                queue.append(caller)
    return distance


def paths(source, target, callees, distance, budget, limit):
    found = []
    stack = [(source, [source])]
    while stack and len(found) < limit:
        node, path = stack.pop()
        if node == target:
            found.append(path)
            continue
        steps_left = budget - (len(path) - 1)
        for callee in sorted(callees.get(node, ()), reverse=True):
            if callee in path or distance.get(callee, budget + 1) > steps_left - 1:
                continue
            stack.append((callee, path + [callee]))
    return sorted(found, key=len)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("source", metavar="FROM")
    parser.add_argument("target", metavar="TO")
    parser.add_argument("--slack", type=int, default=1,
                        help="also show paths this many calls longer than the shortest")
    parser.add_argument("--limit", type=int, default=20, help="most paths to print")
    parser.add_argument("--max-depth", type=int, default=12, help="longest path searched")
    parser.add_argument("--export", default=DEFAULT_EXPORT)
    args = parser.parse_args()

    names, sizes, callees = load_export(args.export)
    source = resolve(args.source, names, sizes)
    target = resolve(args.target, names, sizes)
    distance = distances_to(target, callees, args.max_depth)

    label = lambda entry: f"{entry:08X} {names.get(entry, '?')}"
    if source not in distance:
        print(f"No direct-call path from {label(source)} to {label(target)} "
              f"within {args.max_depth} calls (indirect calls are not in the export).")
        return 1

    shortest = distance[source]
    found = paths(source, target, callees, distance, shortest + args.slack, args.limit)
    print(f"{label(source)} -> {label(target)}: shortest {shortest} call(s), "
          f"{len(found)} path(s) shown (slack {args.slack}, limit {args.limit})")
    for path in found:
        print()
        for depth, entry in enumerate(path):
            print(f"  {'  ' * depth}{label(entry)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
