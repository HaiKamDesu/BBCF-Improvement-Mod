#!/usr/bin/env bash
# Brings the private decompile archive up to date with everything the repo knows.
#
#   tools/decomp/sync_archive.sh [--zip <path.zip>] [--no-export]
#
# 1. SyncRepoKnowledge.py writes every GhidraDefs.h name and every docs/Decompilation ledger row
#    (name, level, notes) into the archive's Ghidra project (plate comments between [repo] lines).
#    ApplyRepoTypes.py adds the struct layouts (docs/Decompilation/types/structs.json, regenerated
#    from the mod's headers by gen_struct_layouts.py when MSVC is available) and the typed
#    signatures (types/signatures.h, plus conventions written in ledger notes).
# 2. ExportFullDecompile.py regenerates decomp/ from that project; the decompiler prints each
#    function's notes as a /* [repo] ... */ comment above it. About 4 minutes.
# 3. Copies the repo's own write-ups (docs/Decompilation, docs/Research/*.md, the struct headers in
#    src/Game, notes.h, the script scanner) into <archive>/knowledge/, stamped with the commit.
# 4. With --zip, packs ghidra/ + decomp/ + knowledge/ + README.txt into one file to hand over.
#    That zip contains BBCF.exe (inside the Ghidra project) and decompiled code: private, never
#    upload it. docs/Decompilation/LocalSetup.md is the shareable route.
#
# Run it after any session that added names or ledger rows, and before handing the project to
# anyone. Close the archive project in the Ghidra GUI first (headless cannot open a locked project).
#
# Env: BBCF_ARCHIVE (default /mnt/c/BBCF_Archive/build_11073075),
#      GHIDRA_HOME  (default /mnt/d/Programs/ghidra_11.4.2_PUBLIC)
set -euo pipefail

REPO=$(cd "$(dirname "$0")/../.." && pwd)
ARCHIVE=${BBCF_ARCHIVE:-/mnt/c/BBCF_Archive/build_11073075}
GHIDRA=${GHIDRA_HOME:-/mnt/d/Programs/ghidra_11.4.2_PUBLIC}
ZIP=""
EXPORT=1
while [ $# -gt 0 ]; do
    case "$1" in
        --zip) ZIP=$2; shift 2 ;;
        --no-export) EXPORT=0; shift ;;
        *) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 1 ;;
    esac
done

[ -f "$ARCHIVE/ghidra/BBCF.gpr" ] || { echo "No Ghidra project at $ARCHIVE/ghidra" >&2; exit 1; }
[ -x "$GHIDRA/support/analyzeHeadless" ] || { echo "Ghidra not found at $GHIDRA (set GHIDRA_HOME)" >&2; exit 1; }
if ls "$ARCHIVE"/ghidra/*.lock >/dev/null 2>&1; then
    echo "The archive project is open in Ghidra (lock file present). Close it and re-run." >&2
    exit 1
fi

COMMIT=$(git -C "$REPO" rev-parse --short HEAD)
DIRTY=""
if [ -n "$(git -C "$REPO" status --porcelain -- docs/Decompilation src/Game)" ]; then
    DIRTY=" + uncommitted changes"
    echo "note: docs/Decompilation or src/Game has uncommitted changes; syncing them anyway." >&2
fi

# Struct layouts come from the compiler. Regenerate them when MSVC is reachable (Windows/WSL);
# elsewhere the committed docs/Decompilation/types/structs.json is used as is.
if command -v cmd.exe >/dev/null 2>&1 && [ -f "/mnt/c/Program Files/Microsoft Visual Studio/2022/Community/VC/Auxiliary/Build/vcvars32.bat" ]; then
    python3 "$REPO/tools/decomp/gen_struct_layouts.py" | head -1
fi

POST=(-postScript SyncRepoKnowledge.py "$REPO" "$ARCHIVE/decomp/sync_report.txt"
      -postScript ApplyRepoTypes.py "$REPO" "$ARCHIVE/decomp/types_report.txt")
if [ $EXPORT -eq 1 ]; then
    POST+=(-postScript ExportFullDecompile.py "$ARCHIVE/decomp" 90)
fi

# The project is owned by the Windows user; Ghidra keys its lock on user.name.
JAVA_TOOL_OPTIONS="-Duser.name=${BBCF_GHIDRA_USER:-Usuario}" MAXMEM=${MAXMEM:-8G} \
"$GHIDRA/support/analyzeHeadless" "$ARCHIVE/ghidra" BBCF -process BBCF.exe -noanalysis \
    -scriptPath "$REPO/docs/Research/ghidra_scripts" "${POST[@]}" \
    > "$ARCHIVE/decomp/sync.log" 2>&1 || { tail -30 "$ARCHIVE/decomp/sync.log" >&2; exit 1; }
# Headless Ghidra exits 0 even when a post-script throws.
if grep -q "^Traceback\|ERROR REPORT SCRIPT ERROR" "$ARCHIVE/decomp/sync.log"; then
    grep -A12 "^Traceback" "$ARCHIVE/decomp/sync.log" | head -30 >&2
    echo "A Ghidra script failed (full log: $ARCHIVE/decomp/sync.log)." >&2
    exit 1
fi
grep -E "GhidraDefs.h names|^types:|^done:" "$ARCHIVE/decomp/sync.log" | sed 's/^.*> //' || true

# The repo's write-ups, next to the project they describe.
rm -rf "$ARCHIVE/knowledge"
mkdir -p "$ARCHIVE/knowledge"
(cd "$REPO" && git ls-files -co --exclude-standard -- docs/Decompilation 'docs/Research/*.md' \
    'src/Game/*.h' 'src/Game/**/*.h' notes.h tools/bbcf_scr_scan.py tools/decomp_progress.py tools/decomp_callpath.py \
    | while read -r f; do [ -f "$f" ] && mkdir -p "$ARCHIVE/knowledge/$(dirname "$f")" && cp "$f" "$ARCHIVE/knowledge/$f"; done)
cat > "$ARCHIVE/knowledge/SYNCED.txt" <<EOF
Synced from BBCF-Improvement-Mod commit $COMMIT$DIRTY on $(date -u +%Y-%m-%dT%H:%MZ).

ghidra/BBCF.gpr  every repo name applied; each function's plate comment holds the repo's notes
                 between [repo] and [/repo] (from src/Game/GhidraDefs.h and docs/Decompilation);
                 structs and enums in the /BBCF category; typed signatures (types/signatures.h)
decomp/src/*.c   the same notes, as a /* [repo] ... */ comment above each function
knowledge/       the write-ups themselves; start at docs/Decompilation/README.md
decomp/sync_report.txt  what was renamed, labelled or kept on this sync
decomp/types_report.txt structs, typed signatures, and ledger conventions applied or skipped
EOF
echo "knowledge/ refreshed ($COMMIT$DIRTY)"

if [ -n "$ZIP" ]; then
    python3 - "$ARCHIVE" "$ZIP" <<'PY'
import os, sys, zipfile
root, target = sys.argv[1], sys.argv[2]
count = 0
with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as z:
    for top in ("ghidra", "decomp", "knowledge"):
        for base, _, files in os.walk(os.path.join(root, top)):
            for name in files:
                if ".lock" in name:
                    continue
                path = os.path.join(base, name)
                z.write(path, os.path.relpath(path, root))
                count += 1
    for name in ("README.txt", "appmanifest_586140.acf", "SHA256SUMS.txt"):
        if os.path.exists(os.path.join(root, name)):
            z.write(os.path.join(root, name), name)
            count += 1
bad = zipfile.ZipFile(target).testzip()
print("zip: %s, %d files, %.0f MB, integrity %s" % (target, count, os.path.getsize(target) / 1e6, "ok" if bad is None else "BAD " + bad))
PY
fi
