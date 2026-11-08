#!/usr/bin/env bash
# Builds the BBCF decompile reference on YOUR machine, from YOUR copy of BBCF.exe.
#
# Nothing decompiled is ever downloaded or committed: everyone who works on the decompile runs
# this against the exe from their own Steam install and gets the same Ghidra project and C export
# Kam has (docs/Decompilation/LocalSetup.md explains why it is done this way).
#
#   tools/decomp/build_local_decomp.sh <path to BBCF.exe> <output dir> [ghidra install dir]
#
# Output:
#   <out>/ghidra/BBCF.gpr          analysed Ghidra project, with the repo's names and notes
#   <out>/decomp/src/XXXX0000.c    every function as C, in 64 KB address buckets
#   <out>/decomp/functions.tsv     entry, name, size, file, callers, callees, convention
#   <out>/decomp/strings.tsv       every string and the functions that use it
#   <out>/sync_report.txt          which names and notes from the repo were applied
#
# Needs Ghidra 11.x (tested with 11.4.2) and a JDK 21. Works on Linux natively: Ghidra is Java,
# so a Linux/Steam Deck/WINE player does not need WINE for this part. Pass the Ghidra folder as the
# third argument or set GHIDRA_HOME. A full run (import + auto-analysis + 20k-function export) takes
# a while, roughly an hour on a desktop CPU.
#
# Running a Windows Ghidra from WSL: export JAVA_TOOL_OPTIONS="-Duser.name=$USER_ON_WINDOWS" first
# (Ghidra keys its project lock on the user name and otherwise refuses to open the project).
set -euo pipefail

EXPECTED_SHA256="9a94ac4003cc8dc77226cadc7496a0eb112f69ccc92e50ded27262ed80551f25"

if [ $# -lt 2 ]; then
    sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
fi

EXE=$1
OUT=$2
GHIDRA=${3:-${GHIDRA_HOME:-}}
REPO=$(cd "$(dirname "$0")/../.." && pwd)

if [ -z "$GHIDRA" ] || [ ! -x "$GHIDRA/support/analyzeHeadless" ]; then
    echo "Ghidra not found: pass its install folder as the third argument or set GHIDRA_HOME." >&2
    exit 1
fi
if [ ! -f "$EXE" ]; then
    echo "No BBCF.exe at: $EXE" >&2
    exit 1
fi

actual=$(sha256sum "$EXE" | cut -d' ' -f1)
if [ "$actual" != "$EXPECTED_SHA256" ]; then
    echo "warning: this BBCF.exe is not Steam build 11073075 (sha256 $actual)." >&2
    echo "         Every address in the mod and the docs is for that build; names will land in the wrong places." >&2
    read -r -p "Continue anyway? [y/N] " answer
    [ "$answer" = "y" ] || exit 1
fi

mkdir -p "$OUT/ghidra" "$OUT/decomp"

"$GHIDRA/support/analyzeHeadless" "$OUT/ghidra" BBCF \
    -import "$EXE" -overwrite \
    -scriptPath "$REPO/docs/Research/ghidra_scripts" \
    -postScript SyncRepoKnowledge.py "$REPO" "$OUT/sync_report.txt" \
    -postScript ApplyRepoTypes.py "$REPO" "$OUT/types_report.txt" \
    -postScript ExportFullDecompile.py "$OUT/decomp" 90

echo
echo "Done. Grep $OUT/decomp/src for a function, e.g.:  grep -n '^//==== 00573E50' $OUT/decomp/src/*.c"
echo "Read docs/Decompilation/README.md for what is already mapped."
echo "After a git pull, refresh the names and notes:  BBCF_ARCHIVE=$OUT GHIDRA_HOME=$GHIDRA BBCF_GHIDRA_USER=\$USER tools/decomp/sync_archive.sh"
