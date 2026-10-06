# "Cannot find player information. Create a new profile?" investigation

Date: 2026-10-05. Static RE only (headless Ghidra + `tools/bbcf_disasm_ascii.txt` + the
shipped `BBCF.exe` / `data/localize/eng_idlist.txt`). Nothing was run in-game.
Addresses are Ghidra VAs at image base 0x00400000. RVA = VA - 0x400000. BBCF is ASLR'd,
so code must always use `GetBbcfBaseAdress() + RVA`.

Background reading: `docs/Research/DCodeNetworkStallBug.md` (transport, TUS latch,
session rejection), `src/Network/ProfileBlobSeal.h` (blob checksum).

---

## TL;DR

1. **The prompt is shown by the network-entry D-Code sync task, `FUN_004B0970`, inner state 7.**
   It shows that prompt for every way the own-profile download can fail. The game cannot tell
   "the server has no record" apart from "the request failed". A rejected session
   (`param.status 3`), an HTTP or transport error, a 3 x 3 s poll timeout, a size mismatch and
   a checksum failure all end up in the same place: loader state 6, poll value 100, then
   "Cannot find player information. Would you like to create a new profile?".
2. Pressing **OK clears the TUS latch and uploads a freshly zeroed default blob straight away**.
   This happens in inner state 6 through `FUN_004A96D0`, not only after a match. If that upload
   succeeds, the real server record is overwritten.
3. The game ships an **unused local-backup fallback**: on a failed fetch it first tries
   `Save/bbdc.dat`, and if that file is valid it restores from it **without uploading**,
   because the latch stays 1. The shipped game never writes that file, so the fallback is
   dead code. The mod can write it in the game's own format and turn the dead path back on.
4. Fully implemented but dormant dialogs also exist: `AskRetry` ("Failed to retrieve player
   information. Would you like to try again?") and `AskRecreate` ("...the information on the
   server will be reset..."). The shipped state machine never reaches them.
5. **Vanilla workaround (confirmed by RE):** on the prompt press **Cancel**. At "Return to the
   Main Menu?" press **Cancel** again. That runs state 5 again, which re-fetches. With the mod's
   `DCodeAutoRelogin`, the session will usually have been renewed by then.

---

## 1. How the network profile is loaded (CONFIRMED unless marked)

### Storage

- The own profile is a 0x6800-byte blob at `netUserData + 0xD0`. `netUserData` is
  `DAT_00CAD0C0` (RVA 0x8AD0C0), returned by `FUN_004A0FE0`. The blob getter is
  `FUN_0049D5C0`. This blob is `g_gameVals.playerAvatarBaseAddr`.
- Its loader sub-object is at `blob + 0x68A0`. The loader state is at `subobj + 0xCC`, and the
  bytes received are at `subobj + 0xD0`. This is the same row layout as the room-member rows.
  The own row is simply `netUserData + 0xD0`, outside the 6 room rows.
- Transport: ArcSys HTTP API `http://153.122.81.62/steam/api/tus/read` and `tus/write`, under
  the name `bbdc.dat`. The payload is encrypted with a key derived from the SteamID (see
  `DCodeNetworkStallBug.md`). It is **not** Steam Cloud. `bbsave.dat` is a different, local store.

### Call chain on network entry

`FUN_004A6F70`, the network task driver, runs outer task 3, which is `FUN_004B0970`, the
D-Code sync task. Its inner state is at `task+4`:

| inner state | what happens |
|---|---|
| 0 | `-> 5` |
| **5 entry** | `FUN_004A0CF0` resets the loader. `FUN_0049A230` calls `FUN_004A26A0`, which sets loader cc = 1. Timer `task+0x20 = 3000` ticks (about 50 s) |
| **5 poll** (`0x4B0A06`) | `FUN_0049A940` = `FUN_004A25C0(own row)` (tick), then `FUN_004A0B80(own row)` (poll) |
| | poll = -100 (pending): count down. When the timer runs out: **latch = 1**, `-> 2` (NTER_InitError, back to the menu). Not destructive |
| | poll < 100 (cc 3/4), or `[netObj+0xC] != 0`: **latch = 0** (`0x4B0A1A`), `-> 0x10` (online) |
| | **poll >= 100 (cc 6): `0x4B0A35`**. Try `Save/bbdc.dat` (below). If it is valid: `-> 6`. Otherwise `FUN_004A1F70` builds a default blob (cc = 4), `-> 7`. Both set **latch = 1** (`0x4B0AB2`) |
| **7** | Dialog `0x30` = **`NTTSK_UpdateDCodeAskCreate`**, buttons OK/Cancel. OK: **latch = 0**, `-> 6`. Cancel: `-> 0xB` |
| 6 entry | `FUN_004A96D0` = seal plus **upload own blob**. Returns 0 at once if latched. Then it polls `FUN_004B8D30` (returns 0 if latched). Result < 100: `-> 0x10`. Error: `-> 2` |
| 0xB | Dialog `0x34` `NTTSK_UpdateDCodeAskReturn`. OK: `-> 2` (leave). Cancel: **`-> 5` (re-fetch)** |
| 8, 9, 0xA | `AskRetry` / `AskRecreate` / `AskRestore` dialogs. Fully coded, but **no writer of `task+4 = 8` or `0xA` exists in `FUN_004B0970`**. State 9 is reached only from 8 or 0xA. Dormant |

Dialog ids map through the table `0x9DE870 + id*4`. Button labels come from
`0x9DE9CC + id*4`: 1 = MSGW_OK, 2 = MSGW_NG ("Cancel"), 3 = NTTSK_SelectMsgCreateNew. The
English text comes from `data/localize/eng_idlist.txt`:

- `0x30 NTTSK_UpdateDCodeAskCreate`: "Cannot find player information. Would you like to create a new profile?"
- `0x31 NTTSK_UpdateDCodeAskRetry`: "Failed to retrieve player information. Would you like to try again?"
- `0x32 NTTSK_UpdateDCodeAskRecreate`: "If you create a new profile, the information on the server will be reset. Would you like to continue?"
- `0x33 NTTSK_UpdateDCodeAskRestore`: "Player information found on the server. Would you like to retrieve it?"
- `0x34 NTTSK_UpdateDCodeAskReturn`: "Return to the Main Menu. Do you wish to continue?"

### Why every failure looks like "not found"

- `FUN_004A25C0` is the loader tick. In cc 1 it submits through `FUN_004B8EB0` and moves to
  cc 2. In cc 2, when the transfer is done: if `subobj+0xD0 == 0x6800` **and** `FUN_004A1DD0`
  passes (the checksum check `FUN_0040DF10`), it sets cc = 3. **Otherwise it runs
  `FUN_004A0D50` (memset) and sets cc = 6**, whatever the reason was.
- `FUN_004A0B80` maps cc 3/4 to 0, cc 6 to 100, and anything else to -100.
- `FUN_00422B00` is the CUMSTask download (worker thread). It makes up to 3 attempts, polling
  the work manager for up to 300 x 10 ms each:
  - 7 means bytes = 0x6800.
  - **9 ("no TUS data") means bytes = 0.**
  - **0xB sets the error bit.**
  - **No answer within 3 s per attempt (about 9 s in total) also sets the error bit with bytes = 0.**
- `FUN_00428AC0` is the DownloadTUS tick:
  - `result != 0` in the JSON (for example `param.status 3`, a rejected session) gives **0xB**.
  - A submit failure gives **0xB**.
  - `result == 0` but the payload is the wrong size gives **9**.
  - A size match whose decrypted checksum fails gives **0xB**.

So a **session rejection, a slow or dead server, or a corrupt payload all produce exactly the
same prompt** as a genuinely new account.

### Best hypothesis for the false prompt (HYPOTHESIS, strongly supported)

The trigger is a transient transport or session failure on the very first `tus/read` after
the boot login. The two most likely forms are a `status 3` session rejection and a request
that does not answer within the 3 x 3 s CUMSTask budget. This is the failure class documented
in `DCodeNetworkStallBug.md`:

- 2026-09-08: the 18:38 session's first `tus/read` after login returned 11, and the user quit
  21 s later. That fits the prompt appearing.
- 2026-09-20: the server answers `{"result":1,"param":{"status":3}}` to rejected sessions.

Neither of these is a "no record" answer, yet both reach cc 6.

The response a genuinely new account receives (9 or 0xB) is **unverified**. No natural
`result 9` exists in the local DebugHistory: 94 x 7 and 74 x 8 outcomes, none 9 or 11 at entry.

Secondary candidate (HYPOTHESIS): a mod write into the own blob that lands between the
worker's memcpy (`FUN_00428AC0`) and `FUN_004A1DD0`'s checksum. That would fail validation and
set cc 6. The mod writers are `LobbyAvatarManager.cpp:99` and `PageOnline.cpp:124` (sliders).
Each is followed by `Reseal()`, but a write during the in-flight download would be overwritten
or would race. This is low probability: LobbyAvatarManager waits at least 8 s after connect.

Note: the phrase "you don't have a profile" in `ProfileBlobSeal.h` and `settings.def:329`
describes the **TUS latch** state (uploads and D-Code reads short-circuited). That is a
different symptom from this dialog. Only a failed state-5 fetch shows `AskCreate`.

### What OK actually destroys (CONFIRMED path, outcome partly HYPOTHESIS)

1. State 7 OK sets `DAT_00CF77A8 = 0`.
2. State 6 then calls `FUN_004A96D0`, which seals the **default** blob (zeroed by
   `FUN_004A0D50` and marked cc = 4 by `FUN_004A1F70`) and submits a `tus/write`.

If the session is still dead, the write fails and the game goes to state 2. The next launch
would then load the real profile, so nothing is lost. The loss happens when that write, or the
first post-match upload, **succeeds**. The mod's auto re-login could make the second case more
likely (HYPOTHESIS).

### The dead local fallback (CONFIRMED)

- `0x4B0A35`: `FUN_00778FE0("Save/bbdc.dat")` checks the file exists (`CreateFileA`).
  `FUN_00779020(path, blob, 0x6800)` reads it into the **live blob**. If 0x6800 bytes are read
  and `FUN_0040DF10` passes, the task goes to `-> 6` with **latch = 1**. In state 6,
  `FUN_004A96D0` then returns 0 without uploading, `FUN_004B8D30` returns 0, and the task goes
  to `-> 0x10`: online with the restored profile and **no upload for the session**.
- File format, from `FUN_007791B0` (writer) and `FUN_00779020` (reader):
  - `[0x40-byte SHA-512 of the plaintext]` followed by `[0x6800 bytes encrypted]`.
  - The hash is `FUN_00778D00`, `CryptCreateHash(CALG_SHA_512 = 0x800E)`.
  - The key is `SHA-512({0x84A9E134, 0x7B8315F0, SteamID64})`. The SteamID comes from
    `SteamInternal_ContextInit(0x9D3210)`, then SteamUser, then GetSteamID.
  - Encryption is `FUN_00778EE0`; decryption is `FUN_00778E00`.
  - The file therefore only validates for the SteamID that wrote it.
- `FUN_007791B0`:
  - Calling convention: `__thiscall` (ecx unused, the game passes `FUN_00779390()` =
    `0x182A764`), `(const char* path, const void* data, uint32 size)`, returns a bool in `al`,
    `ret 0Ch`.
  - It opens with `OPEN_ALWAYS` and does not truncate. That is fine because the size is constant.
  - It **leaks `operator_new(size)` on every call** (0x6800 bytes).
  - Its only callers are `0x4238F6` / `0x423923`, inside the CUMSTask generic share path, which
    is reached only when the name is **not** `bbdc.dat`. So the shipped game never writes
    `Save/bbdc.dat`. The install on this machine has no such file.
- `Save/` contains `steam_autocloud.vdf`, so Steam Auto-Cloud is active for that folder. Whether
  `bbdc.dat` would sync depends on Steam-side rules (UNVERIFIED).

---

## 2. What the mod already has in this path

| what | where |
|---|---|
| JMP hook at entry of `FUN_004A25C0` (fires for the **own** row too, ecx = `netUserData+0xD0`) | `src/Hooks/hooks_bbcf.cpp:8830-8851` (thunk), `:9733-9736` (placement) |
| Own row ignored: `DeriveSlotIndex` returns -1 for it, but `SampleWorkMgrState` runs every tick, so own-fetch outcomes 7/9/11 **are** logged as `[WebApi]` | `src/Game/NetworkStallDiagnostics.cpp:430-444`, `:1511`, `:1968-1995` |
| TUS latch `DAT_00CF77A8` watcher + `DCodeTusGateAutoClear` (writes 0, 10 per process, 30 s cooldown) | `NetworkStallDiagnostics.cpp:79-91`, `:1700-1735`; `settings.def:105` |
| Forced re-login on `status 3` (`TryForceRelogin`, clears login latch mgr+0x20 = `0xA5A070`, re-arms `FUN_00428050`) | `NetworkStallDiagnostics.cpp:680`, `:1593`; `settings.def:113-114` |
| Login state watcher (mgr+0x00: 1 ok, 2 rejected, 4 created, 0xB failed) | `NetworkStallDiagnostics.cpp:1375-1410` |
| Upload-success detector (`[Upload] profile upload finished ok`) | `NetworkStallDiagnostics.cpp:1798` (`ObserveProfileUploads`) |
| Blob checksum reseal | `src/Network/ProfileBlobSeal.{h,cpp}` |
| RE constants: blob, sealer `FUN_0040DEC0`, verifier `FUN_0040DF10`, upload `FUN_004A96D0`, fetch tick, validator, reset | `src/Game/GhidraDefs.h:446-466`, `:931-971` |

Nothing in the mod currently hooks `FUN_004B0970`, `FUN_0049A940`, `FUN_004A1F70`, or the
`Save/bbdc.dat` path.

---

## 3. Design

The design has three layers. Layer 1 is the real fix. Layer 3 is the safety net that makes the
loss impossible. Layer 2 (the backup and restore) is useful, but it is the least important of
the three.

### Layer 1: retry instead of prompting (the real fix)

Replace `FUN_0049A940` (RVA 0x9A940) with a mod function.

- It has **one caller**, `0x4B0A06`, which is used only by the state-5 own-profile poll.
- Its 32 bytes are `call 4A0FE0 / lea ecx,[eax+D0] / call 4A25C0 / call 4A0FE0 / lea ecx,[eax+D0] / jmp 4A0B80`.
- No branch lands inside it. This was checked per `reference_hook_site_branch_targets`.
- The wrapper must preserve ebx/esi/edi (the caller keeps live values in them) and return the
  poll value in eax.

Wrapper logic:

1. Call the original tick `FUN_004A25C0(own)`, then read cc.
2. If **cc == 3**, take the Layer 2 snapshot and return 0.
3. If **cc == 6** and the retry budget is not spent (suggest 3 per network entry):
   - Classify the last own outcome from the work-manager sample (9 / 0xB / none means timeout)
     and the decoded `status`.
   - If it was a rejection or 0xB, call the existing `TryForceRelogin`, then wait for login
     state 1 (mgr+0x00).
   - Set cc = 1 (`FUN_004A25C0` case 1 re-submits with the `+0xE8` param `FUN_004A26A0`
     already stored) and **return -100**.
   - Log `[ProfileGuard] own profile fetch failed (outcome X), retry n/3`.
4. Otherwise return the original `FUN_004A0B80` result.

Budget: state 5 counts 3000 ticks of -100 (about 50 s at 60 fps). Each attempt can take about
9 s (3 x 3 s), so 3 retries plus a re-login fit. If the budget runs out anyway, the game goes
to state 2 (InitError, latch 1), which is non-destructive.

For a genuine new account (outcome 9 with result 0), retry at most once, so a real first-time
player only waits a few seconds before seeing the vanilla prompt.

A cheaper alternative is a byte patch, but it is riskier:

- Change `0x4B0A9F` (the `07` in `C7 46 04 07 00 00 00`) to `08`. Failures then show the dormant
  `AskRetry` (OK = re-fetch, Cancel = AskReturn, Create New = AskRecreate).
- Caveat: AskRecreate OK goes `9 -> 6` **without clearing the latch**. Vanilla would then enter
  online with a blank profile that never uploads, and the mod's TusGate auto-clear would later
  upload it.
- If it is used at all, it needs a matching `latch = 0` on that edge and the Layer 3 guard.

### Layer 2: last-known-good backup and restore

**When to snapshot.** The backup must never lag behind the server:

- (a) In the Layer 1 wrapper when the own loader reaches **cc 3** (a validated server load).
- (b) On every own upload the mod sees confirmed (`ObserveProfileUploads` success / work-manager
  result 8). Capture a copy of the blob at the `FUN_004A96D0` seal into a pending buffer, then
  promote it on success.
- Never snapshot when cc == 4 (a default blob), when the session's profile came from a restore,
  or when the copy fails `FUN_0040DF10`.

**What to store.** Copy all 0x6800 bytes, then **seal the copy, not the live blob**. Use the
`FUN_0040DEC0` algorithm or `ProfileBlobSeal`'s math on the copy. Reason (HYPOTHESIS, verify):
`FUN_004A1DD0` writes `+0x08 = 0x10001` and clamps `+0xC0` after verifying, so the live blob may
not pass the checksum between uploads.

**Where to store it (recommended: the native path).** Write `Save/bbdc.dat` in the game's own
format, by calling `FUN_007791B0` on the game thread or by reimplementing it (SHA-512 +
`FUN_00778EE0`, which avoids the per-call leak). Then the **game itself** restores the backup,
and only in the exact false-prompt condition, which is the state-5 failure branch. When the
server answers normally the file is ignored.

The alternative is a mod-owned `BBCF_IM/ProfileBackup/<SteamID64>.bin`. It is rejected because
restoring it through the success path would run `0x4B0A1A latch = 0`, which turns uploads back
on and opens a rollback vector.

**Anti-rollback (the restore is read-only by construction):**

- The restore happens only when the server fetch has failed (after the Layer 1 retries). A
  player cannot trigger it while the server responds.
- The native restore keeps the latch at 1 for the session. The mod **must** then:
  - suppress `DCodeTusGateAutoClear` for the process, or until a later state-5 load reaches
    cc 3 from the server;
  - have the Layer 3 guard refuse `FUN_004A96D0`.
- So a restored, or deliberately kept stale, copy can **never be written to the server**.
  Firewalling the server plus an old file therefore shows an old card locally and saves nothing.
  The next session with the server reachable loads the server copy and overwrites the file.
- The file is bound to the SteamID by both the key and the hash, so another account's file fails
  validation and is treated as absent. A crash mid-write gives a hash mismatch, which is also
  treated as absent, so it is safe.
- Display caveat (HYPOTHESIS): after the native restore the own loader stays at cc 6, and
  `FUN_004A1930` (via `FUN_0049D560`) treats 6 as "not ready". The mod may need to set cc = 3
  once the task reaches 0x10, but only while the latch is 1. With the latch at 1, opponent
  D-Codes are also hidden for that session.

### Layer 3: never upload a fresh default over a known profile (safety net)

Hook `FUN_004A96D0` (prologue `55 8B EC 81 EC D0 00 00 00`, 9 bytes, several callers). If the own
loader cc == 4 (written only by `FUN_004A1F70`), or the session profile is "restored", **and** a
valid backup exists for this SteamID:

- set the latch to 1;
- return 0 without sealing or uploading;
- show a notification: "BBCF couldn't load your profile from the server and offered to create a
  new one. The mod blocked the reset so your real profile is safe. Restart the game to try again."

Add an explicit opt-out (a setting, or "I really want a fresh profile") for the rare player
whose server record really was wiped.

`DCodeTusGateAutoClear` also needs a guard: never clear the latch while own cc is 4 or 6, or
while the session profile is not from the server. This is needed whichever path is enabled.

### Risks

- **Checksum and handshake**: never write into the live blob during state 5 or 6, or between
  seal and the upload tick (`ProfileBlobSeal.h`). The native restore lets the game do the write,
  which is safe. Also gate the mod's avatar writers on own cc == 3 and the task not being in
  state 5 (this closes the secondary hypothesis above).
- **Hook epilogue contract** (`docs/HookEpilogueContract.md`): the `FUN_0049A940` replacement is
  frameless and has no args, which makes it the simplest kind. The `FUN_004A96D0` early return
  must restore `ebp`/`esp` the way the original `0x4A96EC` path does.
- Steam must be initialised before `FUN_007791B0` and the SteamID read (see the missed
  SteamAPI_Init race note).
- The SteamID must be the current one. Switching accounts on the same install simply fails the
  hash, so the file is treated as absent.
- `TryForceRelogin` must not run during online entry before the boot login completes (lesson from
  the 2026-09-20 harness test).

---

## 4. Confirmed vs hypothesis

**Confirmed (static RE, addresses above):**

- the dialog id, its text, the state machine, and its transitions;
- that every fetch failure collapses into cc 6 and then the prompt;
- that OK clears the latch and uploads the default blob immediately;
- the Cancel -> Cancel re-fetch workaround;
- the dead `Save/bbdc.dat` fallback, its format, and the fact that it is never written;
- that the restore path keeps the latch at 1 and does not upload;
- that the AskRetry, AskRecreate and AskRestore states are dormant;
- that the mod's fetch-tick hook already sees the own row, and that cc 4 is written only by
  `FUN_004A1F70` for the own loader.

**Hypotheses, needing an in-game or log check:**

- that the false prompt in the field is caused by status-3 rejections or first-request timeouts.
  Check by correlating a `[WebApi] work manager result 11` / timeout at network entry with a
  user's prompt report;
- what the server returns for a genuinely new account (9 vs 0xB);
- whether the live blob fails the checksum after `FUN_004A1DD0`'s post-verify writes;
- the cc-6 display effect after the native restore;
- whether Steam Auto-Cloud syncs `Save/bbdc.dat`;
- the avatar-write race as a secondary trigger;
- that the mod's re-login raises the chance that the destructive upload succeeds.

Scripts used: an ad-hoc `DecompList.py` that decompiles a list of addresses and lists their
callers, run against `FUN_004B0970`, `0049A940`, `004A25C0`, `004A0B80`, `004A1F70`, `004A96D0`,
`004A1DD0`, `004A0D50`, `00428AC0`, `00422B00`, `00779020`, `007791B0`, `00778D00`,
`004A6F70`, `004A1930`. The script was run from the session scratchpad and is not checked in.

---

## 5. Implementation (2026-10-06): the sync task rewritten

`src/Network/ProfileSync.{h,cpp}` replaces `FUN_004B0970` outright. A 5-byte JMP at its entry
lands on `ProfileSyncTask`, a C++ `__fastcall(task)` that returns with a plain `ret`, the same
contract as the original, so the game's own `call` at `0x4A70BF` is unaware. Every state is
reimplemented from the disassembly. The jump table at `0x4B1128` maps 0/2/5..0x10 to the cases
and 1/3/4 to the bare `ret`. Every changed line carries a `[mod]` comment.

**What changed vs. vanilla:**

- **State 5 failure**, which vanilla sends to `Save/bbdc.dat` or else blank + AskCreate:
  - Classify the failure from the work manager result (`mgr+4`, `0xA5A054`): 9 = no data,
    0xB = refused, anything else = no answer.
  - Request a fresh login (`NetworkStallDiagnostics::RequestRelogin`, consumed on the
    strategy-ticker thread).
  - Retry up to 3 times, after 2 s, 4 s and 6 s, behind the same "connecting" screen.
  - After that:
    - If the outcome is "no data" and a verified backup for this SteamID exists, the backup is
      copied in, `FUN_004A1DD0` validates it (loader cc = 3), the latch is cleared, and the
      task goes to state 6, which uploads it.
    - Otherwise the task shows the dormant **AskRetry** dialog (0x31) with OK / Cancel /
      Create New, plus a mod notification that the profile is not lost.
  - **State 7 (AskCreate) is never entered.**
- **State 9 (AskRecreate) OK** builds the default (`FUN_004A1F70`) and clears the latch before
  uploading. Vanilla did neither, so the upload was skipped and a later latch clear would have
  sent the empty profile.
- **Retry counters** reset on network entry and on every player-chosen retry.

**Upload guard.** A JMP at `FUN_004A96D0` replaces its 9-byte prologue. The upload is refused
(returning 0, as the latched path does) when `OwnProfileSafeToUpload()` is false: the own
profile is loading or failed, the loader is at cc 6, or it is at cc 4 without the player having
confirmed Create New twice.

**TUS latch auto-clear.** `HandleTusGate` holds the latch under the same condition. It would
otherwise reopen uploads with the zeroed buffer that every failed download leaves behind.

**Backup.**
- Location: `BBCF_IM\ProfileBackup\bbdc_<SteamID64>.dat`.
- Format: the game's `bbdc.dat` format, using the same CryptoAPI calls as `FUN_007791B0`.
- Written atomically (temp file + `MoveFileEx`) on every validated server load (cc 3) and on
  every upload the CUMSTask worker reports successful. The copy for an upload is taken in the
  guard as that upload starts.
- Deliberately **not** `Save/bbdc.dat`: the vanilla restore path would read that file and go
  online with uploads off, a rollback/no-save mode nobody should be able to switch on.

**Why this is not a rollback tool.**
- The backup always equals the last state the server confirmed.
- It is read only when the server answers that it has nothing for the account, after the
  retries and re-logins.
- It is bound to the SteamID by the key and the hash.
- Swapping in an old file by hand still needs the server to have lost the record, which a
  player cannot cause.

**Settings:** `ProfileSyncRewrite` (default 1; 0 = pure vanilla, nothing patched),
`ProfileBackup` (default 1). Test only: `ProfileSyncTestFailures=N` forces the next N own
downloads to fail as a real failure does (`FUN_004A0D50` + cc 6). A forced failure never
triggers a restore. The `ProfileSyncTestNoData` switch used for test 3 was removed afterwards:
it let anyone fake "no data" from `settings.ini` and restore a backup copy kept aside.

**Install safety:** both sites are byte-checked before patching (task head + its `cmp eax,10h`
at +0x29; upload prologue + `mov eax,[cookie]`). A disassembly scan found no branch landing
inside either patch.

**Status (2026-10-06, Release build, live server):**
- Test 1 PASSED: forced failure, re-login (0.8 s), retry after 2 s, loaded; `backup refreshed`.
- Test 2 PASSED: four failures, retries at 2/4/6 s, AskRetry dialog rendered (OK / Cancel /
  Create New), OK re-fetched, and the real profile showed (LV34, 3,353 matches).
- No upload was blocked or attempted during either test.
- Test 3 PASSED:
  - Four failures read as "no data" led to the restore of the 16:14 backup, then the upload
    (`tus/write ok`) and a refreshed backup.
  - Verified with an independent PowerShell/CryptoAPI decrypt
    (`C:\BBCF_Archive\profile_safety\decrypt_backup.ps1`): a fresh server download afterwards
    differs from the pre-test copy only in the checksum (+0x00), the stamp (+0x04) and the
    last-save time (+0x0C, written by `FUN_004A2740` before every upload).
  - The same check also proves the backup file format round-trips.
- Blob field found: +0x0C/+0x10 = 64-bit last-save time, from the clock singleton
  `FUN_0040BD00` vtable+0xC.

Test plan:

1. `ProfileSyncTestFailures=1`: one retry, then a normal entry; `backup refreshed` in the log.
2. `=4`: three retries, then the AskRetry dialog; OK enters normally.
3. (Removed switch; historical) `=4` plus `ProfileSyncTestNoData=1`: restore, then upload. **This
   writes to the live profile**, although it uploads what was just downloaded.

### Abuse analysis (2026-10-06)

**Closed:**
- **The test switch.** `ProfileSyncTestNoData` is removed, and a forced failure is excluded
  from the restore in code as well.
- **Stale backup swap.** The registry value `HKCU\Software\BBCF_IM\ProfileBackup\<SteamID64>`
  holds the newest profile save time (+0x0C) the machine has backed up. A backup older than it
  is refused.

**Remaining, by design accepted:**
- **Faking "no data" plus a stale file plus the registry value.** The profile API is plain
  HTTP, so someone running an intercepting proxy can fake "no data", and also roll back both
  the file and the registry value.
- **Why that is accepted.** Vanilla already offers easier ways to avoid saving a loss: block the
  `tus/write` at match end; the server trusts the client completely. A modified client can
  upload any profile, since the blob format and crypto are client-side. This system does not
  make any of that easier. A client-side mod cannot prevent server-trusting tampering.

## 6. Pre-update snapshot and restore (2026-10-06)

The way back if an update ever breaks a profile. Code: `src/Core/SaveDataSafety.{h,cpp}` and
`ProfileSync` (snapshot/restore); UI `src/Overlay/Window/ProfileRestorePanel.cpp`.

**Where the UI lives:**
- A collapsed red header, "Restore from a pre-update copy (cannot be undone)", at the very
  bottom of Settings > Advanced. It never appears in the settings search.
- Both buttons stay disabled until "I understand a restore cannot be undone" is ticked; the
  tick resets each time the window opens and after every restore.
- The confirmation says "This cannot be undone." with Cancel first and focused.
- The replaced data is still quietly kept in `ProfileBackup\replaced\` for manual recovery.

**What is copied, and when** (`BBCF_IM\ProfileBackup\preupdate\<mod version>\`):
- **Local save:** `Save\bbsave.dat`, `bbrd.dat` and `replay_list.dat` are copied from
  `DllMain` the first time a mod version runs, before the game's `WinMain`.
- **Network profile:** `profile_<SteamID64>.dat` is saved at the first server download under
  that version, before that version can upload anything. It uses the same encrypted format as
  the backup.
- Neither copy is ever overwritten.

**Restoring:**
- **Network profile:** the button writes `preupdate\restore_profile.pending`. On the next
  server load, the sync task:
  1. saves the current profile to `replaced\profile_<id>_<time>.dat`;
  2. copies the snapshot in;
  3. runs `FUN_004A1DD0`;
  4. clears the latch and goes to State_Upload.

  The marker is deleted first, so a restore runs once.
- **Local save:** the button writes `preupdate\restore_local.pending`. The next launch applies
  it in `DllMain`; the replaced files go to `replaced\<time>\`.

This is an explicit rollback the player chooses; the dialog says progress since the copy is lost and that it cannot be undone.

**Fixed alongside:** backups taken from an upload now carry the save time the upload actually
sent (`FUN_004A2740` writes +0x0C after our pre-seal copy), so the stale-backup registry mark
always matches the server.

**Verified in-game 2026-10-06:**
- Snapshots taken; the local copies are byte-identical to the originals; the profile snapshot
  decrypts independently and equals the server copy.
- Both restores were applied: the local files now match the snapshot and the originals were
  kept; the profile was restored, uploaded (`tus/write ok`) and re-downloaded equal to the
  snapshot apart from the checksum, stamp and save time.
- The dialog's Cancel schedules nothing.
