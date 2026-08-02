# D-Code load failure / ranked progress rollback — root cause candidate

Investigating the long-standing bug where the pre-match D-Code panel (own
and/or opponent) fails to populate, and — when it happens — any ranked
LP/rank/match-count gains earned during that state get rolled back on the
next game restart as if they never happened.

## Summary

The D-Code display and other per-room-member network data (rank prediction,
possibly the match-result confirmation used to trust a ranked outcome enough
to persist it) are all fetched through a single generic **per-room-member
async request state machine**. That state machine has **no timeout and no
automatic recovery** if the underlying P2P/Steam exchange silently stops
producing a completion signal. Once a slot gets stuck, it stays stuck for the
rest of the process lifetime (only a full game restart resets it to idle) —
which matches the reported "sometimes 1 match in, sometimes 8 hours in,
totally unpredictable" pattern, and is consistent with a prior *different but
structurally identical* bug already documented in
`docs/Research/RankedProgress.md` (entries #221/#222): a Steam
persona/leaderboard read that silently corrupts and stays corrupt until the
Steam client itself restarts.

This has not yet been proven to be the exact mechanism behind the ranked
progress rollback (that requires locating the local-save trigger and
confirming it is gated the same way — see Open Questions), but it is a
strong, evidence-backed candidate, and fixing it is low-risk regardless
because it only nudges an internal "give up and retry" transition that the
game already performs for other trigger conditions.

User-observed behavior (2026-07-07) matches the state machine exactly: once
a slot wedges at state 2, it never recovers on its own — the D-Code stays
missing and any ranked outcome earned afterward keeps getting rolled back on
every subsequent restart, until the process is fully restarted (which
reinitializes the state to 0). This rules out a one-off "this match only"
failure mode and confirms the "no retry, no timeout" reading of the code.

## The state machine

Per-room-member object, address relative to a "this" pointer obtained via a
per-row lookup (row stride `0x68a4`, base offset `0x2326c` from the
`get_NetUserData()` singleton at `0x004A0FE0` — see
`docs/Ranked/RankedInternals.md` for the sibling `0x180`-stride per-character
ranked row, a *different* table from this one):

```text
row               = get_NetUserData() + 0x2326c + slotIndex * 0x68a4
subobj            = *(int*)(row + 0x68a0)
state             = *(int*)(subobj + 0xcc)      // the fetch state
```

State values observed:

| state | meaning |
|---|---|
| 0 | idle — no request outstanding |
| 1 | request just queued (set by `FUN_004A26A0`, the trigger) |
| 2 | request handed to the transport, awaiting completion (`FUN_004A25C0` transitions 1→2) |
| 3 | result received and validated — data is ready to read |
| 6 | error — validation failed (wrong size / bad payload) after a response arrived |

Driving functions (Ghidra addresses, `BBCF.exe` image base `0x00400000`):

- `FUN_0049D560` — "get row for display" entry point. Calls `FUN_004A1930`
  (row-found check) first; if not ready, calls `FUN_004A0B80` (must not be in
  hard-error state 100/`iVar==6`... actually returns 100 when state==6) and
  `FUN_004A1AB0` (`state != 0 && state != 3` → "already busy, don't
  re-trigger") and only then calls `FUN_004A26A0(row, 1)` to kick off a fresh
  request. Always returns "not ready" (`0`) on the same call that triggers the
  fetch — the caller only gets data on a later poll once state reaches 3.
- `FUN_004A26A0` — trigger. Gated by `FUN_00407C90` (see below) and
  `FUN_004A25C0`. On success, stores the caller's payload/callback context and
  sets `state = 1`.
- `FUN_004A25C0` — the actual polling/transition function, called every frame
  this slot is active:
  - `state == 1`: issues the real request via `FUN_004B8F70`/`FUN_004B8EB0`
    (a 0x6800-byte buffer op — looks like a P2P packet send). On success,
    `state = 2`.
  - `state == 2`: polls `FUN_004B8CE0()`. **If it returns `-100` ("still
    pending"), this function just returns `-100` too and state stays at `2`
    — no timeout, no retry counter, nothing.** If the transport says data
    arrived, it validates size/content via `FUN_004A1DD0`; success → `state =
    3`; failure → `FUN_004A0D50()` (reset) then `state = 6`.
- `FUN_00407C90` — small generic validity/gate check on a packed
  ID+flags pair (bit-fields at offsets `[0]`/`[1]` of a 2-dword struct: a
  "type" nibble 1–10 and a signed "count" byte 1–4, with extra per-type
  nonzero/range checks). This same gate is reused very broadly (16+ call
  sites across `0x0041xxxx`/`0x0046xxxx`/`0x0070xxxx`), including two call
  sites that sit immediately next to the already-documented ranked
  confirmation/rank-commit helpers from `docs/Research/RankedConfirmGhidraReport.txt`
  (`FUN_004A26A0` itself, and `FUN_004B4360` at `0x004B4360`, whose sole
  caller is `FUN_00656490` at `0x006567FA` — not yet decompiled). This is the
  concrete link between "D-Code fetch plumbing" and "ranked confirm-adjacent
  code," but it is a *generic* slot-validity check reused everywhere, so on
  its own it only shows the two systems share the same kind of room-member
  bookkeeping, not that they share the exact same stuck bit.

**The bug**: nothing ever forces state 2 back to 0 or forward to 3/6 if the
underlying transport silently drops the exchange (packet loss with no NACK,
a P2P route renegotiation that orphans the request, etc.). Once stuck at 2:

- `FUN_004A1930` never reports "ready" (only checks state 3/6).
- `FUN_004A1AB0` reports "already busy" for state 2 same as for a
  healthy in-flight request, so `FUN_0049D560` never re-triggers
  `FUN_004A26A0` — no self-healing retry.
- The slot is wedged until process exit.

## Open questions / next steps

1. **LP-persistence link — traced as far as static analysis allows.**
   `FUN_00656490` (the sole caller of `FUN_004B4360`, which shares the
   `FUN_00407C90` gate with the D-Code path) turned out to be a **UI renderer**
   for a post-match "ONLINE ID / BATTLE RESULT / play count / win / win rate"
   list — it uses the gate only to decide whether a room-member row is ready
   to draw, not to decide whether to persist anything. Not the save trigger.

   Traced the real save machinery instead:
   - The local save file is written by `CSaveDataManager` methods (addresses
     from `docs/Research/Tadatys-BBCF-Ghidra/BBCF.h` are **RVAs relative to
     module base `0x00400000`**, not raw VAs — e.g. its `//000bb460` comment
     is VA `0x004BB460`).
   - `FUN_004BB460` (`set_next_SaveUtil_action_0_write`) and its siblings
     (`FUN_004BB010` is-running, `FUN_004BB2C0` action 7, `FUN_004BB410`
     action 1) all gate on a shared "busy" flag at `CSaveDataManager+0x1B11F0`
     and are called only from the manual Save/Load menu state machine
     (`FUN_006C4990`/`FUN_006C6E50`) — not from post-match code.
   - Separately, `FUN_004B9F70` (`GAME_CSaveTask::update_task`, ticked every
     frame from the main loop via `FUN_00699040` → `FUN_006C4880` →
     `FUN_006C4990`) drives the *same* `CSaveDataManager+0x1B11F0` state
     machine based on a **different, standalone global flag**,
     `DAT_00EA97C8`: `1` = "start an automatic write" (transitions the
     manager to action-state 2), `2` = "write in progress, poll
     `FUN_004CACA0()` for completion", `3` = "finalize." This looks exactly
     like the auto-save-after-match trigger.
   - **Dead end for static analysis**: `0x00EA97C8` is referenced exactly
     once in the entire `.text` section (confirmed directly in
     `tools/bbcf_disasm.txt`, not just Ghidra's xref index) — the read inside
     `FUN_004B9F70` itself. Nothing statically writes it; the write must go
     through a pointer computed elsewhere (e.g. a field written via a
     dynamically-loaded base pointer), which Ghidra's decompiler can't
     resolve to a fixed operand. Closing this loop needs a **live write
     watchpoint** on `0x00EA97C8` during a real save (e.g. `ba w4 0xEA97C8`
     in CDB while attached to a running game, triggered by any ordinary
     post-match save — does not require reproducing the D-Code bug itself,
     just needs to catch one normal auto-save to identify the calling
     function and then check whether that function is gated by the same
     per-room-member state used by the D-Code fetch path).
2. **Find the per-frame caller** that walks active room-member slots and
   calls into the D-Code/rank-prediction state machine (candidates:
   `FUN_0049A230` at `0049a2c4`, `FUN_0049D440` at `0049d453` — the "own" vs
   "opponent" variants of `FUN_0049D560`/`FUN_004A1AB0`). That call site is
   the natural hook point for the watchdog fix.
3. **Confirm `FUN_004B8CE0`'s semantics** (what exactly it polls — likely a
   Steam Networking P2P read-availability check) to judge whether a
   reasonable timeout (a few seconds) can never fire falsely for a healthy
   slow connection.

## Proposed fix (low risk, does not require resolving question 1)

Hook the per-frame poll site (or `FUN_004A25C0` itself) and add a watchdog:
track how long each active slot's `subobj+0xcc` has held value `2`. If it
exceeds a generous threshold (e.g. 5–10 s — normal P2P exchanges complete in
well under a second), force `subobj+0xcc = 0` (idle). This does not invent
new behavior: state 0 is the exact condition `FUN_0049D560`/`FUN_004A1AB0`
already use to justify auto-retrying, so the game's own existing retry path
does the recovery. This should fix the D-Code display hang without a
restart, and — if question 1 confirms the shared gate — likely fixes the
progress rollback too, since the slot would no longer be permanently wedged
in a bad state when the post-match commit logic checks it.

Even before question 1 is resolved, logging when this watchdog fires is
itself the detector the user asked about as a fallback: it gives a real-time,
in-game signal ("network profile stalled, auto-reset") instead of only
discovering the rollback on next boot.

## Correction (2026-07-07): row+0x3C is NOT a per-slot SteamID

An early attempt to also log an "identity" field for each room slot assumed
`row+0x3C` (where `row = netUserData + 0x2326C + slot*0x68A4`, the same row
used for D-Code/rank-prediction fetch state) held a steamId64-shaped value,
based on `FUN_0041CCF0`'s use of `param_1+0x3C`/`+0x40`. This was wrong:
`FUN_0041CCF0`'s `param_1` comes from a completely different object (reached
via a function-pointer/vtable dispatch from `FUN_0046C340`), not the
`netUserData`-relative row from the `FUN_0049D560`/`FUN_004A25C0` investigation.
Live testing confirmed `row+0x3C` never became readable/nonzero for an entire
match despite the row's fetch-state field working correctly at `+0x68A0`/`+0xCC`.
The ranked-list connection filter (`RankedListConnectionFilter`) no longer
depends on this offset — it now captures the connection target directly at
`SteamMatchmakingWrapper::JoinLobby()` (via `GetLobbyOwner`) instead of trying
to read it back out of game memory.

## Reproducing the RE session

Scripts: `docs/Research/ghidra_scripts/DecompileDCodeBug.py` through
`DecompileDCodeBug9.py`. Run via `run_ghidra_dcode_bug.cmd` ..
`run_ghidra_dcode_bug9.cmd`. Reports: `DCodeBugGhidraReport.txt` ..
`DCodeBug9GhidraReport.txt` (same directory).

## 2026-07-12: FIRST LIVE CAPTURE — the wedge is state 6, not state 2

`Debug_DCodeError1.txt` (this directory) is a full session log with the bug
occurring. Session 20:42–21:00, clean shutdown, versus screens at 20:43,
20:47, 20:50, 20:54. Key `[NetStall]` events:

```text
20:43:05  slot 1 (opponent) 0 -> 2      # first match, healthy
20:43:07  slot 1 2 -> 3
20:43:41  slot 0 (self)     0 -> 2
20:43:43  slot 0 2 -> 3
20:50:49.120  slot 0 3 -> 2             # re-fetch right before 3rd match
20:50:50.132  slot 0 2 -> 6             # response arrived, REJECTED
              (no further slot transitions until shutdown — wedged at 6)
20:50:52  GetGameStateVersusScreen      # match 3 starts with slot 0 dead
```

So the theorized "silent stall at state 2 with no timeout" is NOT what
happened here: a response **arrived within ~1s and failed validation**
(size != 0x6800 or checksum failure), producing the state-6 wedge, which is
just as permanent (see below). The 15s state-2 watchdog theory stays as a
secondary failure shape but the state-6 path is the observed one.

Also found in that log: the auto-save-trigger diagnostic printed constant
garbage (`auto-save trigger -2 -> -1956749403`) because
`NetworkStallDiagnostics.cpp` had `kAutoSaveTriggerRva = 0xA97C8` — a dropped
digit; VA `0x00EA97C8` − base `0x00400000` = RVA `0xAA97C8`. Fixed. All
save-manager readings in Debug_DCodeError1.txt for that field are therefore
meaningless; the `save manager actionRunning/nextAction` lines used the
correct address and remain valid (note: at 20:58:21, after the wedge,
`nextAction` pulsed `0 -> 7 -> 0` while `actionRunning` never left 0 — a
possible "save requested but never executed" signature, though the 200ms
poll may simply have missed the run).

## Phase 8/9 static findings (DCodeBug8/9GhidraReport.txt)

- **`FUN_0049D440` is the per-frame pump** (sole caller: `FUN_004A6F70`). It
  walks all 6 rows (`0x273D8 / 0x68A4`), and for each: if `FUN_004A1AB0`
  (busy) and not `FUN_004A1A00` (state==1), ticks `FUN_004A25C0`; then a
  second unconditional tick loop over all rows. This is the hook point used
  for live instrumentation (we hook `FUN_004A25C0`'s entry itself, which
  also covers the `FUN_0049A940` and `FUN_004A26A0` call paths).
- **The row IS the payload.** `FUN_004A1DD0` (validator) takes the row and
  checks `FUN_0040DF10(row, 0x6800)`. The first 0x6800 bytes of each room
  row are the member's profile blob; at `+0xD4` sit 0x28 entries of 0x180
  bytes (the per-character ranked rows — cf. `RankedInternals.md`).
- **`FUN_0040DF10` is a 16-bit ones'-complement checksum** (valid iff the
  running sum ends at 0xFFFF — internet-checksum style). Its other callers
  (`FUN_006C4990` save/load menu machine, `FUN_004BB080`, `FUN_004B0970`,
  `FUN_00428AC0`, `FUN_0042EDD0`) are save-data machinery: the network
  profile blob is validated exactly like a save file.
- **Transport is `GAMESTEAM_COnlineStorageTransfer`** (named vtable in the
  Ghidra project; lazy singleton built by `FUN_004B8F70` /
  ctor `FUN_004717C0`, 0x1C bytes). `FUN_004B8CE0` is a virtual dispatch
  (`obj->vtbl[+8]()` then tail-jump `target->vtbl[+0x18]`), so its return
  codes (-100 = pending, anything else = done/error) come from the concrete
  transfer object; not further resolved statically.
- **State 6 wipes the evidence.** `FUN_004A0D50` (called right before
  `state = 6`) memsets the row's 0x6800 bytes to 0, reinits the 0x28
  per-character entries, and restores the `0x10001` magic at row+8. So by
  the time any poller sees state 6, the offending payload is gone — this is
  why the live hook snapshots the blob at tick entry while state==2.
- **State 6 is permanent, confirmed**: `FUN_004A0B80` returns 100 (hard
  error) for state 6; `FUN_004A1AB0` reports "busy" (state != 0 && != 3);
  `FUN_004A25C0` early-outs for state 6. Nothing in the binary writes the
  state back to 0 except object construction.
- Bonus rollback lead: since slot 0 (self) wedging at 6 leaves your OWN
  profile row zeroed and hard-errored, any post-match commit logic that
  reads or gates on this row would silently skip persisting — consistent
  with "everything after the bug is rolled back on restart".

## Instrumentation shipped 2026-07-12 (branch release/8-0)

- `src/Hooks/hooks_bbcf.cpp`: `DCodeFetchTick` JMP hook at `FUN_004A25C0`
  entry (unique 17-byte signature, 10 bytes stolen). Calls
  `NetworkStallDiagnostics::OnFetchTickEnter(row)` every tick.
- `src/Game/NetworkStallDiagnostics.cpp`:
  - While a slot is at state 2, snapshots the full 0x6800 blob + transport
    context dwords (subobj+0xD0..0xE8) each tick.
  - Logs every state transition with recvSize and time-in-previous-state
    (`[DCodeTick]` tag, active whenever GenerateDebugLogs=1 — no dev gate).
  - On 2→3 logs the accepted payload's checksum as a healthy baseline.
  - On →6 (or a >15s state-2 stall): logs live+snapshot transport context,
    snapshot checksum, first 0x40 bytes hexdump, and dumps the entire
    pre-wipe blob to `BBCF_IM\DCodeBlobFail_slot<N>_tick<T>.bin`.
  - **Auto-recovery watchdog** (`DCodeAutoRecover=1` in settings.ini, new
    setting): forces the slot state back to 0 (the game's own retry
    precondition) after logging, capped at 3 recoveries per slot per
    process to avoid a retry storm against a genuinely corrupt peer.
  - Fixed `kAutoSaveTriggerRva` to `0xAA97C8`.
  - **Persistent incident sink** (added same day, since DEBUG.txt is
    recreated on every launch): every important `[DCodeTick]` line is also
    appended (with its own timestamp) to `BBCF_IM\DCodeIncidents.log`, which
    is never truncated and accumulates across sessions. On each failure the
    current DEBUG.txt is additionally copied to
    `BBCF_IM\DEBUG_DCodeIncident_<date>_<time>_slot<N>.txt` (max 5 copies
    per session), after all the failure lines have been flushed into it.
    So a week of unattended play yields: one cumulative incidents log,
    plus a full-log snapshot and a payload .bin per failure.

2026-07-13 addition: `DCodeForceFailureOnce=1` (settings.ini, default 0)
sabotages the first in-flight fetch of the session by writing 16 bytes of
0xA5 at row+0x1000 while state==2, so the game's own checksum rejects the
payload — an authentic on-demand state-6 wedge (local memory only, nothing
reaches the peer; fires once per launch). Used to verify the
detection/dump/auto-recovery pipeline without waiting for a natural repro.
Two days of healthy `DCodeIncidents.log` data (2026-07-12/13) confirm the
hook and both sinks work: 17 accepted fetches, all checksum 0xFFFF, fetch
completion consistently ~1.4s.

**2026-07-13: forced-failure test PASSED end-to-end** (DCodeIncidents.log,
17:22 session): sabotaged fetch rejected by the game (full 0x6800 received,
checksum 0x2D2D != 0xFFFF, state 2->6), evidence pipeline fired (ctx dump,
hexdump, .bin payload dump, DEBUG.txt snapshot), auto-recover forced state
0, and the game re-queued the fetch on its own **15ms later** (no screen
change needed), completing with a valid checksum ~2.9s after the rejection.
Conclusion: recovery from state 6 is fully self-healing via the game's own
retry path; a natural repro should now recover in ~3s instead of wedging.
Remaining open question is only whether the natural failure is transient
like the test (retry succeeds) or persistent (would exhaust the 3-retry
cap), and whether recovery also prevents the ranked-progress rollback.

## 2026-07-14: rollback WITHOUT a fetch wedge — save path is now primary suspect

User played a long first-to-20 player-match set vs "heythan" on 2026-07-13
(the 19:37 session in DCodeIncidents.log, ran past 20:22), gaining net-color
progress from orange/0 to pink. On 2026-07-14 the progress was rolled back to
orange/0. The incident log for that entire session is **completely healthy**:
slots 1/2 fetched at join, 4/5 during matches, periodic re-fetches at
19:52/20:13/20:22, all checksums 0xFFFF, zero state-6/stall events. Nothing
in the fetch state machine misbehaved, and no crash occurred (last crash
report is 2026-07-11). Conclusion: **the fetch wedge is not the only rollback
path** — a session's progress can silently fail to persist with the D-Code
system fully healthy. (The forced tests earlier that day were separate
launches, 17:22/17:34, followed by two clean sessions before the set; they
corrupted local receive-buffer memory only and recovered to checksum-valid
fetches, so they are unlikely to be the cause — but not impossible to rule
out entirely since the set session's DEBUG.txt was overwritten.)

Save-side facts established from the 2026-07-14 session log (with the fixed
trigger RVA):
- The "auto-save trigger global" DAT_00EA97C8 and CSaveDataManager+0x1B11F0
  (actionRunning) are THE SAME memory — the manager is statically allocated
  (manager base VA 0xC986D8). Explains phase 7's "no static writers".
- Save requests are made by tiny helpers: FUN_004BB2C0 (nextAction=7, the
  one seen after matches), FUN_004BB410 (=1), FUN_004BB300 (=2), etc., mode
  param at manager+0x1B11F8. All are called only from the save-task state
  machine FUN_006C4990 (pumped per frame by FUN_006C4880); the network mode
  drives saves through it. See DCodeBug10GhidraReport.txt.
- In a healthy lobby session, actionRunning pulses 0->2->0 (write) every few
  minutes, roughly correlated with slot-0 own-profile re-fetches.

Instrumentation added 2026-07-14 in response (all in the deployed build):
- **Per-session DEBUG.txt history**: at launch the previous DEBUG.txt is
  rotated to `BBCF_IM\DebugHistory\DEBUG_<lastwrite>.txt`; setting
  `DebugLogSessionHistory` (default 10) controls retention; 0 = old behavior.
  No session's full log can be lost again.
- **[SaveWatch] in DCodeIncidents.log** (not dev-gated): every save-manager
  actionRunning/nextAction transition (with mode param), plus a filesystem
  watch on `Save\bbsave.dat` (logs "WRITTEN size=..." whenever its mtime
  changes) — ground truth that a save reached disk. Every [SaveWatch] line
  carries the current `netcolor=X counter=Y` values, so the next rollback
  will show exactly what progress existed at each save event and whether the
  on-disk file was written with it.

Next rollback should answer: did bbsave.dat get written during the lost
session at all (if not: the request path was gated off — trace FUN_006C4990's
input flags), and if it was written, did the netcolor values in [SaveWatch]
lines at write time already show stale data (if so: the serialization source
is stale — trace what buffer action-2 serializes).

## 2026-07-16: NATURAL CAPTURE — transport-level, session-wide, Steam UGC layer

Session 2026-07-15 22:07 (opponent "Kamui Thanatos", user observed opponent's
D-Code missing). DCodeIncidents.log + 14 blob dumps + 5 DEBUG snapshots:

- Failure signature: **recvSize=0x0 every time** — the transport completed
  with an error and wrote nothing; the "payload" is just the freshly-reset
  buffer (checksum 0x000C, all zeros + 0x10001 magic). NOT corruption.
- **Session-wide breakage**: slots 1/2 fetched fine at 22:08:07; from
  22:08:23 onward EVERY fetch failed (slot 0 six times, slots 4/5 four times
  each) until all auto-recover budgets exhausted. First failure took 9.4s
  (timeout-shaped), subsequent ones 0.7–4s (fast-fail). Retrying at the
  fetch-state layer cannot heal this — the layer below is wedged.
- **Rollback mechanism confirmed by [SaveWatch]**: bbsave.dat kept being
  written all session (9+ writes 22:08–22:38) but `netcolor=2 counter=51`
  NEVER changed across ~30 min of matches. The game doesn't stop saving —
  it stops APPLYING results to the profile once the transport is wedged
  (consistent with the commit path checking FUN_004A0B80's hard-error 100).
  Restart "rollback" = progress was never granted in the persisted profile.

Transport architecture (phases 11–13, DCodeBug11/12/13GhidraReport.txt):

- `GAMESTEAM_COnlineStorageTransfer` (vtable 0089DA60) is a facade; its
  vtbl+0x08 getter returns the **`AASTEAM_CUserManagedStorage`** singleton
  (DAT_00A29E30, RVA 0x629E30), whose +4 is the **`AASTEAM_CUMSTask`**
  worker (0x110 bytes, ctor FUN_00422410).
- CUMSTask registers `CCallResult<RemoteStorageFileShareResult_t>` (0x51B)
  and `CCallResult<RemoteStorageDownloadUGCResult_t>` (0x525): **the D-Code
  profile blob is FileShare()'d to Steam Cloud UGC and downloaded by UGC
  handle** — not direct P2P. The async queue (DAT_00A29E04, thread name
  "ReplayUploader") is shared with the replay uploader/downloader.
- Poll FUN_00422E70: worker+0x1C done flag; **worker+0xC0 bit0 = error latch
  → returns 100 → state 6**. worker+0xB8 = **Steam EResult** (getter
  FUN_00422CC0). Request block (0x60 bytes incl. UGC handle/steamID) at
  worker+0x30.
- Root-cause candidates for "everything fails from moment X": stale cached
  UGC handle (peer re-shared, old handle now invalid — retry with same
  handle fails forever), Steam UGC rate limit, or Steam remote-storage
  session failure (cf. RankedProgress.md #221/222 Steam-side wedge).

Instrumentation added 2026-07-16 (deployed): on every failure,
`LogUMSWorkerState` dumps the CUMSTask worker — done/busy flags, request
ids, **steamEResult** (+0xB8), error flags, recv fields, and the 0x60-byte
request block hex. The EResult value on the next occurrence should decide
between stale-handle (FileNotFound=9), rate limit (LimitExceeded=25), and
generic IO failure — which in turn decides the fix (refresh handle & resub
vs backoff vs unfixable client-side).

## 2026-07-20: second natural capture — the Steam CallResult never fires

Occurrence 2026-07-20 ~02:02 (opponent D-Code missing again). The new
LogUMSWorkerState dumps show, for every failure: done=1, **steamEResult=0**
(field +0xB8 is actually "bytes received" on the generic path / attempt
result on the bbdc path — it stayed 0), recv=-1/-1, errFlags=0x03
(2026-07-16 late session) or 0x3B (2026-07-20). The request block carries
the wide name **"bbdc.dat"**, a per-opponent-stable dword (peer accountID)
and a dword that varies per retry.

Phases 14–17 (DCodeBug14..17GhidraReport.txt) mapped the remaining layers:

- CUMSTask run (FUN_004230A0): +0x94 != 0 -> download (FUN_00422830),
  else +0x90 -> share (FUN_004237B0). Both branch on
  `lstrcmpW(L"bbdc.dat", req+0x44)`: equal -> the DEDICATED bbdc paths
  FUN_00422B00 (download) / FUN_00423A50 (share); otherwise a generic
  chunked-read path (FUN_00779xxx, 5-retry loop).
- FUN_00422B00 (bbdc download): locks mutex DAT_00A29E2C, then up to 3
  attempts of: stash {steamID, ugcHandle} into the **Steam work manager
  singleton DAT_00A5A050** (getter FUN_00427CD0; params at +0xD0..0xDC,
  current work item at +0xE4, created by FUN_004291D0(type 7=download /
  8=share)), then poll `workMgr+4` up to 300x10ms (3s): 7=done, 9=empty,
  0xB=error, else keep waiting. After 3 attempts with no progress -> error
  bit (worker+0xC0 |= 1), bytes stay 0.
- **Observed failure shape = the CallResult never fires**: first natural
  failure took 9.4s = exactly 3x3s poll timeout; workMgr+4 never reached
  7/9/0xB. Subsequent failures fast-fail, i.e. the work manager stays
  latched. So the wedge lives in the work item / Steam async layer: the
  RemoteStorage UGCDownload (or FileShare) call's CCallResult is lost —
  candidate causes: SteamAPICall_t invalid (bad/zero UGC handle -> Steam
  never schedules a result), CallResult re-registration cancelling an
  in-flight one, or the shared "ReplayUploader" async thread wedging.

Instrumentation added 2026-07-20 (deployed): failure dumps now include
`[DCodeTick] SteamWorkMgr: state=.. steamId=.. ugcHandle=.. workItem=..`
(singleton +4/+0xD0..0xDC/+0xE4). Next occurrence shows directly whether
the UGC handle passed to Steam was zero/garbage (-> stale handle from lobby
metadata; fix = refresh handle + reissue) or valid (-> lost CallResult; fix
= reset work item / re-dispatch, or detect + warn).

Remaining static targets if needed: FUN_004291D0 (work item factory),
FUN_00429390 (work item release), the type-7 work class vtable (where
UGCDownload is actually invoked and its OnComplete writes workMgr+4).

## 2026-08-02: ROOT CAUSE — the TUS "storage unavailable" latch (DAT_00CF77A8)

Third-party report (`Bug Reports/Dcode progress reset/Report 1`, another user's
machine, v8.1, 2026-07-30) supplied the missing evidence. Two sessions:
17:01–18:24 and 18:31–20:30, 47 logged failures with the SteamWorkMgr dumps.

**Two earlier conclusions were WRONG and are corrected here:**

1. *"The wedge freezes progress in memory."* No. In session 2 the net-color
   counter moved 50→49→50→51→52→53 while healthy, and then still moved
   53→52 **during** the wedge (20:14). RAM keeps updating fine.
2. *"`ugcHandle=` shows the Steam UGC handle."* No — mislabeled. The two
   dwords I printed are `workMgr+0xD8` = **destination buffer pointer** and
   `workMgr+0xDC` = **0x6800 size** (confirmed: the "handle" always reads
   `00006800xxxxxxxx`, and its low half equals the `reqIds` buffer pointer).
   The download request carries `{steamID, buffer, size}` and NO UGC handle;
   `workMgr+0xD0/0xD4` is the peer steamID (valid `0x0110000100000000`-form
   values that vary per opponent). Field labels fixed in the source comments.

**The actual architecture (phases 18–23).** `bbdc.dat` is the network profile
in **TUS (Title User Storage)** — the filename table at 0x009DF4BC holds
L"bbdc.dat"/L"bbd.dat"/L"bbdp.dat"/L"dummy.dat", and the transfer strategies
are literally named `uei::ThinkLogicStrategyDownloadTUS` (type 7) and
`uei::ThinkLogicStrategyUploadTUS` (type 8), created by FUN_004291D0 and
polled through the work manager DAT_00A5A050 (state at +4: 7 = download done,
8 = share done, 9 = download empty, 0xB = error).

- Download submit: UMS vtbl+0x10 = FUN_00422A10, built by FUN_004B8EB0
  (the path our row-fetch hook already watches).
- **Upload submit: UMS vtbl+0x0C = FUN_00423950, built by FUN_004B9210,
  called from FUN_004A96D0** = "upload my 0x6800 profile blob as bbdc.dat".
  This was completely uninstrumented — and it is the path that makes
  ranked/net-color progress durable.

**`DAT_00CF77A8` (RVA 0x8F77A8) is the bug.** A process-wide "TUS
unavailable" latch:

- Written 1 at 0x4B0ACE when the own-profile sync exhausts its retry counter
  (`mov [esi+0x20],0BB8h` = 3000 ticks ≈ 50 s at 60 fps, then
  `dec`/`jns`/latch), and at 0x4AC098, 0x4AFED2, 0x4B0AB2 on sibling error
  paths. Written 0 only at 0x4B0A1A, on a successful sync.
- Read by **FUN_004A96D0 → early return** (profile upload skipped) and by
  FUN_004B8CF0 / FUN_004B8D30 → return 0 (D-Code reads short-circuit).

That single flag explains every symptom coherently: D-Codes vanish, the
in-memory counters keep moving, `bbsave.dat` keeps being written (local save,
a different store), and on restart the game loads the last *successfully
uploaded* profile — i.e. progress "resets to the last match before the bug",
exactly as reported, no matter how many matches were played afterwards. Only a
process restart clears the latch.

**Fix shipped 2026-08-02** (deployed v8.1 Release): `[TusGate]` lines in
DCodeIncidents.log on every transition of the latch (with the current
net-color/counter), plus setting **`DCodeTusGateAutoClear`** (default 1,
"Recover profile uploads" in the settings window) which writes the latch back
to 0 — the same value the game writes itself on a successful sync — so the
upload path and D-Code reads go live again. Rate-limited to 10 clears per
process with a 30 s cooldown so a genuinely offline session cannot become a
retry storm.

Verification wanted from the next occurrence: a `[TusGate] !!!` line
appearing at the moment D-Codes vanish (proves the mechanism end-to-end),
followed by `auto-clear` and then progress surviving a restart.

Next capture should tell us: whether the rejected payload was all-zero
(transport error), truncated (recvSize != 0x6800), or genuinely corrupt
(full-size, bad checksum) — and whether a forced retry succeeds, which
decides between "transient corruption, watchdog is the full fix" and
"deterministic corruption, need to look at the sender".
