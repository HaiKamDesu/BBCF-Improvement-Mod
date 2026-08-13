# Replay Archiving: How It Works and Why It Silently Produces No Files

**Status:** diagnosed, not yet fixed.
**Investigated:** 2026-08-07, branch `release/8-3`.
**Symptom that started this:** a user enabled "Auto archive saved replays" and no files ever appeared
under `Save/Replay/archive/`. The user is in Russia and has connectivity problems reaching the replay
database host, and suspected archiving was gated behind a working replay-DB connection.
**Verdict on that theory:** wrong — archiving has no network dependency at all. The real cause is
almost certainly the filename builder, which is broken for non-Latin Steam names.

---

## 1. The mechanism, end to end

### 1.1 The setting

| Piece | Location |
|---|---|
| Setting definition | `src/Core/settings.def:131` — `SETTING(bool, autoArchive, "autoArchive", "0")` |
| Settings-window metadata | `src/Overlay/Window/SettingsIniWindow.cpp:89` |
| SCR-window checkbox | `src/Overlay/Window/ScrWindow.cpp:1734-1738` |
| Manual "Archive replay files" button | `src/Overlay/Window/ScrWindow.cpp:1726-1731` |

Default is off. The checkbox writes through `Settings::changeSetting("autoArchive", ...)` immediately,
so it persists to `settings.ini` on toggle.

### 1.2 The one and only auto-archive trigger

`src/Hooks/hooks_bbcf.cpp:9244-9271`, the naked hook confusingly named `UploadReplayToEndpoint`:

```cpp
DWORD UploadReplayToEndpointJmpBackAddr = 0;
void __declspec(naked) UploadReplayToEndpoint()
{
    _asm {
        PUSH ebp
        MOV  ebp, esp
        sub  esp, 20      // re-executes the 6 patched bytes
        pushad
    }
    LOG_ASM(2, "UploadReplayToEndpoint\n");

    StartAsyncReplayUpload();

    if (Settings::settingsIni.autoArchive)
        g_rep_manager.archive_replay((ReplayFile*)(GetBbcfBaseAdress() + 0x11B0348)); // replay_buffer

    _asm {
        popad
        jmp [UploadReplayToEndpointJmpBackAddr]
    }
}
```

Installed **unconditionally** at `src/Hooks/hooks_bbcf.cpp:9492`:

```cpp
UploadReplayToEndpointJmpBackAddr =
    HookManager::SetHook("UploadReplayToEndpoint", (DWORD)(GetBbcfBaseAdress() + 0xcb0b0), 6, UploadReplayToEndpoint);
```

There is no setting, no version check, and no `if` around that `SetHook` call.

`0x11B0348` is the same replay buffer that `load_replay` targets
(`0x115b470 + 0x54ed8`, `ReplayFileManager.cpp:109`).

### 1.3 What `BBCF+0xCB0B0` actually is

The name `UploadReplayToEndpoint` is misleading — it comes from Tadatys' Ghidra header
(`docs/Research/Tadatys-BBCF-Ghidra/BBCF.h:2618`, `inject_UploadReplayToEndpoint[0x80]`), which
labels the site after *this mod's* use of it, not after the game's own semantics.

Disassembly (`tools/bbcf_disasm.txt`, UTF-16LE; the ASCII companion file does not cover this range):

```
004CB0B0: push        ebp                 <- the 6 bytes the mod patches
004CB0B1: mov         ebp,esp
004CB0B3: sub         esp,20h
004CB0B6: push        esi
004CB0B7: push        edi
004CB0B8: mov         edi,ecx
004CB0BA: mov         dword ptr [ebp-20h],0
004CB0C1: mov         dword ptr [ebp-1Ch],2
004CB0C8: mov         dword ptr [ebp-18h],0
004CB0CF: mov         dword ptr [ebp-14h],0
004CB0D6: call        004B9770            singleton getter
004CB0DB: mov         ecx,eax
004CB0DD: call        004BA860            -> replay data pointer
004CB0E2: mov         esi,eax
004CB0E7: call        004B9770
004CB0EC: mov         ecx,eax
004CB0EE: call        004BA860
004CB0F3: push        16448h
004CB0F8: push        esi
004CB100: call        0040DEC0            memcpy 0x16448 bytes
004CB108: lea         eax,[ebp-20h]
004CB10E: mov         dword ptr [edi+18h],1
004CB115: call        004CB210            SaveUtil__setup_data_to_write
004CB11C: call        004CB5B0            SaveUtil___actually_write_file
004CB126: ret
```

Its neighbours in `BBCF.h` are all `SaveUtil__*` members (`SaveUtil__init` at `0xCB040`,
`SaveUtil___format_filename` at `0xCB130`, `SaveUtil__setup_data_to_write` at `0xCB210`,
`SaveUtil___actually_write_file` at `0xCB5B0`).

**Conclusion: it is BBCF's local replay-file write routine.** Auto-archiving therefore fires exactly
once per replay that the *game itself* saves into `Save/Replay/`. No socket, no HTTP, no endpoint.

### 1.4 The archive write

`src/Game/ReplayFiles/ReplayFileManager.cpp:207-220`:

```cpp
bool ReplayFileManager::archive_replay(ReplayFile* replay_file) {
    std::string replay_archive_folder_path = REPLAY_ARCHIVE_FOLDER_PATH;
    CreateDirectoryA(REPLAY_ARCHIVE_FOLDER_PATH, NULL);

    auto new_fname = build_file_name(replay_file);
    std::ofstream out(replay_archive_folder_path + new_fname, std::ios::binary);

    if (out.is_open()) {
        out.write((char*)replay_file, REPLAY_FILE_SIZE);
        out.close();
        return true;
    }
    return false;
}
```

Paths come from `src/Game/ReplayFiles/ReplayFileManager.h:8-9`:

```cpp
#define REPLAY_FOLDER_PATH         "./Save/Replay/"
#define REPLAY_ARCHIVE_FOLDER_PATH "./Save/Replay/archive/"
```

Both are **relative to the process CWD**, which is normally the BBCF install directory.

Two things matter for diagnosis:

* `CreateDirectoryA` runs **before** anything that can fail.
* the returned `bool` is **discarded by the caller**, and there is **no logging anywhere** in this
  function. A failed archive is completely invisible.

### 1.5 The manual button path

`archive_replays()` (`ReplayFileManager.cpp:222-245`) is a different code path with the *same*
filename bug: it regex-scans `Save/Replay/` for `replayNN.dat`, then per file does
`load_replay(el)` → `build_file_name()` → `save_replay(archive_path + fname)`. `save_replay`
(`ReplayFileManager.cpp:64-79`) is another unchecked narrow-path `ofstream`.

This makes the button a useful reproduction lever: if a user's names trip the bug, "Archive replay
files" will also produce nothing while creating the folder.

---

## 2. Why there is no network gate

`StartAsyncReplayUpload()` (`src/Web/update_check.cpp:103-110`) sits one line above the archive call:

```cpp
void StartAsyncReplayUpload() {
    if (!Settings::settingsIni.uploadReplayData || g_modVals.uploadReplayDataVeto) {
        return;
    }
    CloseHandle(CreateThread(nullptr, 0, (LPTHREAD_START_ROUTINE)UploadReplayBinary, nullptr, 0, nullptr));
}
```

* The early-out is `return` from `StartAsyncReplayUpload`, not from the hook. The `autoArchive`
  block runs either way.
* When it does not bail, it only spawns a thread — the actual HTTP work never runs on the hook's
  stack, so a dead replay-DB host cannot block, slow, or fault the archive call.
* `uploadReplayDataVeto` (set in `src/Network/ReplayUploadManager.cpp:47` when the opponent opts out)
  likewise only affects uploading.
* `uploadReplayData == -1` merely opens the consent popup at `src/Overlay/WindowManager.cpp:260-263`.

**Replay-DB reachability is irrelevant to archiving.** Rule this out and do not spend time on it.

---

## 3. The actual bug: `build_file_name`

`src/Game/ReplayFiles/ReplayFileManager.cpp:169-205`. Target format is
`YYMMDDThh_mm_<p1>_V_<p2>.dat`.

```cpp
std::string ReplayFileManager::build_file_name(ReplayFile* file) {
    ReplayFile& replay_file = *file;
    std::wstring p1ws = std::wstring(replay_file.p1_name);
    std::wstring p2ws = std::wstring(replay_file.p2_name);
    std::string p1n = utf16_to_utf8(p1ws);      // <-- UTF-8 BYTES
    std::string p2n = utf16_to_utf8(p2ws);
    replace_all(p1n, ".", "");
    replace_all(p2n, ".", "");
    p1n = p1n.substr(0, 5);                     // <-- BYTE truncation (BUG 1)
    p2n = p2n.substr(0, 5);
    std::string dt = replay_file.date1;
    std::string month = dt.substr(4, 3);
    std::string day   = dt.substr(8, 2);
    std::string time  = dt.substr(11, 5);
    std::string year  = dt.substr(22, 2);       // <-- unguarded (BUG 3)
    ...
    auto fin = year + month + day + "T" + time + "_" + p1n + "_V_" + p2n + ".dat";
    replace_all(fin, ":", "_");                 // <-- only ':' and ' ' sanitized (BUG 2)
    replace_all(fin, " ", "_");
    return fin;
}
```

Relevant struct fields (`src/Game/ReplayFiles/ReplayFile.h`):

```cpp
char    date1[0x18];       // 0x38 — 24 bytes
wchar_t p1_name[0x12];     // 0xA4 — UTF-16
wchar_t p2_name[0x12];     // 0x16E
```

And `utf16_to_utf8` (`src/Core/utils.cpp:340-347`) converts with `CP_UTF8`.

### BUG 1 — byte-wise truncation of UTF-8, then a narrow `ofstream`

`substr(0, 5)` cuts **bytes**, not characters. Consequences for a Cyrillic name (2 bytes per
codepoint in UTF-8):

* only ~2 characters survive, and
* the cut can land **mid-sequence**, leaving a dangling lead byte such as `0xD0`.

The resulting UTF-8 byte string is then concatenated into a path and handed to a narrow
`std::ofstream`. On MSVC/Windows a narrow path is reinterpreted through the **process ANSI
codepage**, not UTF-8. So:

* best case, the file is created with a mojibake name (`Ð¡Ð¾` style garbage) — bad but visible;
* if a byte in the truncated sequence has no mapping in the active codepage, the conversion fails
  and `is_open()` is false → `return false` → silent no-op.

A player whose matches are mostly against other Cyrillic-named players hits this on essentially
every replay. This is the leading explanation for the reported symptom and it explains why it
correlates with the user's region — while having nothing to do with their connectivity.

### BUG 2 — incomplete filename sanitization

Only `:` and space are replaced. Windows also forbids `\ / ? * " < > |` in filenames, and `.` is
only stripped from the name fragments (not from anything else). A Steam name containing `/` or `?`
in its first bytes yields an invalid path → `is_open()` false → silent no-op. This is
region-independent and can bite any user.

### BUG 3 — unguarded date parsing inside a naked hook

`date1` is `char[0x18]` (24 bytes), so `dt.substr(22, 2)` uses the last valid pair. If the buffer is
ever shorter than expected or not NUL-terminated as assumed, `std::string::substr` throws
`std::out_of_range`, and the exception propagates out of a `__declspec(naked)` function with a live
`pushad` frame. That is a crash, not a silent failure, so it is probably *not* the reported user's
issue — but it must be fixed alongside, since the same untrusted buffer feeds it.

### BUG 4 — the result is discarded and nothing is logged

`hooks_bbcf.cpp:9264` ignores `archive_replay`'s `bool`. There is no `LOG` call in `archive_replay`,
`archive_replays`, `save_replay`, or `build_file_name`. Every failure mode above is indistinguishable
from "the feature never ran".

---

## 4. Diagnosing a user report

`CreateDirectoryA` runs before any failure point, which gives a clean two-way split. Ask whether
`<install dir>/Save/Replay/archive/` exists:

| Observation | Meaning | Next step |
|---|---|---|
| Folder **does not exist** | `archive_replay` was never entered | Setting not actually persisted, no replay was written by the game while it was on, or CWD is not the install dir |
| Folder **exists but is empty** | Hook fired, the write failed | Filename bug (BUG 1/2) |
| Folder has **mojibake-named** files | BUG 1, degraded-but-working case | Still needs the fix |

Second confirmation, independent of the folder: `LOG_ASM(2, "UploadReplayToEndpoint\n")` at
`hooks_bbcf.cpp:9259` logs at level 2, and `DEBUG_LOG_LEVEL` is 5 (`src/Core/logger.h:8`), so with
`GenerateDebugLogs=1` the user's `DEBUG.txt` contains one `UploadReplayToEndpoint` line **per replay
the game saves**. Lines present + no files = confirmed write-side failure.

Note for bug-report handoffs: the shipped zip's `settings.ini` has `GenerateDebugLogs=0`, so the user
must enable it before reproducing.

---

## 5. How to fix it

All changes are confined to `src/Game/ReplayFiles/ReplayFileManager.{h,cpp}` plus one line in
`src/Hooks/hooks_bbcf.cpp`. No settings, localization, or network changes are needed.

### 5.1 Build the filename safely

Replace the name-fragment handling in `build_file_name`:

1. **Truncate in UTF-16, before conversion.** Take at most N `wchar_t` from `p1_name` / `p2_name`
   (they are `wchar_t[0x12]`; also guard against a missing NUL by bounding the `std::wstring`
   construction to 18 elements rather than relying on `std::wstring(ptr)`).
2. **Transliterate or drop non-ASCII.** The archive filename is a human-readable convenience, not
   data — the replay bytes carry the real names. Map each codepoint to `[A-Za-z0-9_-]` and drop or
   replace anything else with `_`. This makes the path codepage-independent and sidesteps BUG 1 and
   BUG 2 in one step.
3. **Sanitize the whole assembled name**, not just `:` and space: reject/replace the full Windows
   set `\ / : * ? " < > |`, plus control characters, and collapse runs of `_`.
4. **Fall back** when a name fragment reduces to nothing: use `getCharacterNameByIndexA(p1_toon)` /
   `(p2_toon)` (`src/Game/characters.h:46`), which are already computed in the function as
   `p1_toonstr` / `p2_toonstr` but currently unused. Last-resort fallback: the last 8 hex digits of
   `p1_steamID64` / `p2_steamID64`. The date prefix alone is not enough — two replays can share a
   minute.

### 5.2 Make the date parsing defensive

Bound `dt` to `date1`'s 24 bytes and verify length before each `substr`; on a malformed buffer, fall
back to `date1_int[6]` (`year, month, day, hour, minute, second` as `uint32_t`, `ReplayFile.h:0x20`),
which needs no string parsing and is the better source anyway. Never let `substr` throw out of this
function — it is called from a naked hook.

### 5.3 Open with a wide path

Convert the final path with `utf8_to_utf16` (`src/Core/utils.cpp:349`) and pass the `std::wstring` to
`std::ofstream` (MSVC supports a `const wchar_t*` overload). Apply to `archive_replay` and to
`save_replay` (`ReplayFileManager.cpp:64-79`), which has the same problem and already has a
commented-out `utf8_to_utf16` line hinting someone hit this before. With 5.1 the path is ASCII
anyway, but this removes the codepage dependency permanently.

### 5.4 Log and check results

* Add `LOG(2, ...)` in `archive_replay` for: the resolved output path, and on failure the path plus
  `GetLastError()` — construct the message *before* the `ofstream` so a throw cannot hide it.
* Check the return value at `hooks_bbcf.cpp:9264`; on `false`, log via `g_imGuiLogger->Log` so the
  user sees it in-game rather than only in `DEBUG.txt`. Note this runs inside a naked hook — keep it
  inside the existing `pushad`/`popad` window and do not add anything that can throw.
* Do the same for the `archive_replays()` loop: count successes/failures and report a summary next to
  the button in `ScrWindow.cpp:1726`.

### 5.5 Collision handling

Two replays in the same minute with the same sanitized names produce the same filename, and the
current code silently overwrites (plain `ofstream`, no exclusive flag). Once names are sanitized down
to ASCII this gets *more* likely, not less. Append a `_2`, `_3`, … suffix when the target exists.

### 5.6 Also worth fixing while in here

`archive_replays()` calls `fs::directory_iterator(REPLAY_FOLDER_PATH)` (`ReplayFileManager.cpp:224`)
with no existence check — it throws if `Save/Replay/` is missing. Guard it.

### 5.7 Verification

No unit tests exist in this repo; validation is manual and in-game.

1. Build `Debug|Win32` (see `CLAUDE.md`). Do **not** use a Deploy config unless asked.
2. Enable `autoArchive` and `GenerateDebugLogs=1`.
3. Play a match that makes the game save a replay; confirm a new file in `Save/Replay/archive/` and
   the new log lines in `DEBUG.txt`.
4. Regression-test the non-Latin case without needing a Russian opponent: craft a `replayNN.dat`
   with Cyrillic `p1_name`/`p2_name` (the offsets are in `ReplayFile.h`), drop it in `Save/Replay/`,
   and use the manual "Archive replay files" button — it runs the same `build_file_name`.
5. Test names containing `/`, `?`, and `|`, and an all-non-ASCII name that sanitizes to empty (should
   fall back to the character name).

---

## 6. Quick reference

| Thing | Location |
|---|---|
| Setting | `src/Core/settings.def:131` |
| Auto-archive call site | `src/Hooks/hooks_bbcf.cpp:9263` |
| Hook installation | `src/Hooks/hooks_bbcf.cpp:9492` (`BBCF+0xCB0B0`, unconditional) |
| Hooked game function | BBCF's local replay write (`SaveUtil` family) — `004CB0B0` in `tools/bbcf_disasm.txt` |
| Replay buffer | `BBCF+0x11B0348` (= `0x115b470 + 0x54ed8`) |
| `archive_replay` | `src/Game/ReplayFiles/ReplayFileManager.cpp:207` |
| `archive_replays` (manual) | `src/Game/ReplayFiles/ReplayFileManager.cpp:222` |
| `build_file_name` (the bug) | `src/Game/ReplayFiles/ReplayFileManager.cpp:169` |
| `save_replay` (same narrow-path issue) | `src/Game/ReplayFiles/ReplayFileManager.cpp:64` |
| Paths | `src/Game/ReplayFiles/ReplayFileManager.h:8-9` |
| Struct layout | `src/Game/ReplayFiles/ReplayFile.h` |
| UTF helpers | `src/Core/utils.cpp:340` / `:349` |
| Upload gating (irrelevant to archiving) | `src/Web/update_check.cpp:103` |
