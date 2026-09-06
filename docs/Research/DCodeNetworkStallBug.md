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

## 2026-08-03: v8.2 report — a SECOND, distinct failure mode; TusGate fix is not universal

Third-party report (`Bug Reports/.../Report 1`, another user, v8.2, session
2026-08-02 18:46–19:48). Symptom differed slightly from earlier reports: own
D-Code loaded fine, only the opponent's was invisible; progress still rolled
back. Log analysis:

- Opponent DOWNLOAD (slot 5) failed 4 times 18:49–18:53, exhausting the
  existing 3-retry auto-recover budget (working as designed, just
  insufficient — the underlying transport stayed broken).
- ~2.5 minutes later, the LOCAL PLAYER's own profile UPLOAD started failing
  **every single attempt for the remaining ~52 minutes of the session — 7503
  consecutive failures**, each taking ~3.5s, never once succeeding.
- **`DAT_00CF77A8` (the TusGate latch from the 2026-08-02 root-cause fix)
  never set — it logged "available" once at session start and never
  transitioned again.** The existing `DCodeTusGateAutoClear` fix therefore
  had literally nothing to do here; it cannot detect or help this failure.

**Static root cause (phases 24–26, DCodeBug24/25/26GhidraReport.txt):** the
upload strategy's tick method — `uei::ThinkLogicStrategyUploadTUS::vftable+0x1C`
= `FUN_0042EDD0` — at its very first step (item-state 0), checksums the
buffer it's about to upload using the *same* `FUN_0040DF10` 16-bit
ones'-complement check used to validate downloads, **before attempting any
Steam call**:

```c
if (*(param_1 + 4) == 0) {
    iVar1 = FUN_0040df10(*(param_2 + 0xd8), *(param_2 + 0xdc));  // checksum own buffer
    if (iVar1 == 0) {
        *(param_1 + 4) = 3;
        *(param_2 + 4) = 0xb;   // immediate failure, Steam never contacted
    } else { ... actually call FUN_00434750 (FileShare) ... }
}
```

The buffer being checksummed is `netUserData + 0xD0` — traced through
`FUN_0049D5C0() == FUN_004A0FE0() + 0xD0`, and `FUN_004A0FE0` is the *same*
netUserData singleton getter (`kNetworkUserDataRva`) used everywhere else in
this file. **This is not a stack copy or a fresh rebuild — it is the live,
persistent, in-memory profile blob itself.** Nothing in the traced code path
ever rewrites this region between attempts, so once it goes
checksum-invalid, every subsequent retry re-checksums the exact same bytes
and fails identically, forever, with no possibility of self-healing by
retrying. This is a fundamentally different shape of problem than the TUS
latch: that was a state-machine flag we could safely reset to a value the
game itself produces; this looks like standing corruption of live profile
data, and we do not yet know what "corrupted" means here (all-zero? garbage?
subtly-wrong single field?) or whether it is safe to touch.

**Deliberately NOT auto-fixed this round.** Overwriting or "repairing" a
0x6800-byte live game structure without knowing what's actually wrong with
it is a materially bigger risk than resetting a boolean latch back to its
own natural value — a bad guess here could corrupt the profile further or
introduce new failure modes. Instead, shipped (v8.2 deploy) only detection:
`ObserveProfileUploads` now tracks a consecutive-failure streak and, on the
first 3 occurrences of a streak, dumps the own-buffer checksum and a hex
preview to `DCodeIncidents.log`; further failures in the same streak log only
a periodic heartbeat (every 200) to avoid repeating the 7503-line flood seen
in this report's raw log (1.5MB from near-duplicate lines). A `[Upload]
profile upload recovered after N consecutive failure(s)` line fires if it
ever does start succeeding again.

Next capture needs the ACTUAL corrupted bytes (now captured automatically)
to determine: is the buffer all-zero (suggests the same reset/wipe path seen
elsewhere ran against the wrong region), all-garbage (heap corruption
elsewhere clobbering it), or plausibly-structured-but-wrong (a stale/partial
write) — each points to a different, and only then would a targeted repair
be safe to design.

## 2026-09-06: ROOT CAUSE — it was never Steam. It is an ArcSys HTTP web API.

Natural repro captured on 2026-09-05 (session 22:09:27–23:15, `BBCF_IM\DEBUG.txt`,
`DCodeIncidents.log`, blob dumps `DCodeBlobFail_slot{0,4,5}_tick7036*`).
Phases 27–29 (`DCodeBug27/28/29GhidraReport.txt`) resolve the transport, and
**every Steam-side theory in the sections above is wrong**: there is no UGC
handle, no FileShare, no lost CallResult, and `worker+0xB8` is not an EResult.

### The actual transport

`uei::ThinkLogicStrategyDownloadTUS::Tick` (`FUN_00428AC0`) builds a
`uei::tl::ReadTusRequestParam` and hands it to `FUN_00434750`, which does:

```c
FUN_0042b130(PTR_u_http___153_122_81_62_steam_api_009d4c48);  // base URL
psVar4 = (&PTR_u_user_create_009d4c54)[iVar2];                 // endpoint by type
```

- base URL `http://153.122.81.62/steam/api` (plain HTTP, hardcoded IP, Apache + PHP 5.3.3)
- endpoint table at `009D4C54`: `0 user/create`, `1 user/login`, `2 catalog/get_region`,
  `3 catalog/get_area`, `4 catalog/get_lobby`, `5 lobby/get_status`,
  `6 matching/start`, `7 matching/confirm`, `8 matching/end`,
  **`9 tus/read`**, **`10 tus/write`**, `11 tss/read`
- `ReadTusRequestParam` sets type 9, `WriteTusRequestParam` type 10
  (`FUN_0042EDD0`), `LoginRequestParam` type 1 (`FUN_0042E660`).

So the D-Code / net-color profile blob (0x6800 bytes, encrypted with the 16-byte
key `{0x84A9E134, 0x7B8315F0, steamID_lo, steamID_hi}` — `FUN_00428950`) lives on
ArcSys's own PHP server, not in Steam Cloud.

Every request body is Jansson `json_pack` (`FUN_00432730`):

```c
sprintf_s(buf, 0x20, "%lld", singleton[0], singleton[1]);      // steamID64
json_pack("{ss,ss,si,si,ss,si}",
          "steamId",  buf,
          "session",  singleton + 2,        // inline token at singleton+0x8
          "language", singleton[0xB],
          <DAT_00850A58>, singleton[0xC],
          "version",  "0.0.1",
          "platform", singleton[0xE]);
```

The WebApi singleton is `DAT_00A5A168` (RVA `0x65A168`, 0x148 bytes, ctor
`FUN_004309B0`); the per-request-type pending slots are at `+0x40 + type*4`.

### The failure

`FUN_00428AC0` phase 1, on a completed response:

```c
if (httpErr == 0) {
    if (recvLen == mgr[0xDC/4]) { ...checksum, decrypt, memcpy...; mgr[1] = 7; }
    else { state = 3; report(&DAT_008503DC); mgr[1] = 9; }      // <-- observed
} else       { state = 3; report(&DAT_00850400); mgr[1] = 0xB; }
```

- `DAT_008503DC` (UTF-16) = 「TUSデータなし…メッセージ」 — **"no TUS data"** → 9
- `DAT_00850400` = 「TUS読み込み失敗…メッセージ」 — "TUS read failed" → 0xB (not our case)

`FUN_00422B00` then turns state 9 into the CUMSTask error bit
(`requested 0x6800 != received 0`, `worker+0xB8 = 0`), which the fetch state
machine reports as **state 6**. `FUN_0040DF10` is confirmed as a 16-bit
ones'-complement sum returning `sum == 0xFFFF`, so the blob dumps' 83 non-zero
bytes out of 26624 are simply a buffer nothing was ever written into.

A bare probe of the live endpoint reproduces the exact shape:

```
$ curl -i http://153.122.81.62/steam/api/tus/read
HTTP/1.1 200 OK
Server: Apache
X-Powered-By: PHP/5.3.3
Content-Length: 0
```

**200 OK with an empty body** — no HTTP error, zero-length payload → "no TUS data".

Note `FUN_00433010` returns 1 exactly when a pending object exists with
`+0xE38 == 0`, so the guard in `FUN_00434750` reads `(gate == 0) || <the same
condition that made gate == 1>` and is always true: requests really are issued
every time. The ~700 ms fast-fails are real HTTP round-trips coming back empty,
not skipped requests.

### Evidence from the 2026-09-05 capture

- Work-manager steamIDs are stable per room slot: slots 0/4 =
  `0110000113455C91` (opponent), slot 5 = `01100001088DE5A1` = **76561198103782817,
  the user's own account**. `tus/read` returned "no data" for the user's own
  profile, 25 minutes after that same account read back fine at 22:35.
- First failure of a streak is slow (4.2 s / 4.7 s / 9.4 s across captures),
  every later one ~700–780 ms.
- Session-wide from 23:00:16 onward; all auto-recover budgets exhausted; healed
  only by restart.
- Ranked LP is **not** affected — rank went 33→31 and `UploadLeaderboardScore`
  kept succeeding at 23:13 and 23:15, because LP rides Steam leaderboards. What
  froze is the net-color counter (netUserData+0x195): 54→55→54→53→52→51 up to
  22:28:29, then **51 for the rest of the session**, ~30 min *before* the first
  read failure — consistent with `tus/write` (same `session` field) dying first.

### Leading hypothesis and the decisive test

One `session` token is obtained at boot by `FUN_0042E660` (`user/login`, retries
up to 0xB4 polls) and reused by every later request. A server-side session that
lapses or is invalidated explains all of it at once: self and opponent break
together (one token, not per-account data), retries at every layer are useless,
and only a restart heals it because only a restart re-runs `user/login`. It is
**not proven** — `tus/read` could also be answering empty for another reason
(per-IP throttling on the PHP box, backend hiccup).

Decisive next capture: log the live `session` string (`DAT_00A5A168 + 0x8`) at
each fetch, plus the raw response length, and check whether the token is
unchanged across the healthy→wedged boundary. If the token is the same and the
server starts answering empty, it is server-side session invalidation.

### Repair path, if confirmed

`FUN_00428050(workMgr)` releases the strategy at `mgr+0xE0` and creates a type-1
(Login) strategy when `DAT_00A5A070 == 0` — i.e. it re-runs `user/login` and
refreshes the token in place. Forcing that on a state-9 streak is the natural
in-process fix, and is far more likely to work than the existing state-6
auto-recovery, which only resets the fetch state machine above a layer that is
already wedged. Needs verification that a mid-session re-login does not disturb
matchmaking (`matching/*` uses the same session).

### Instrumentation bug found while reading the capture

`ObserveProfileUploads` dedupes on `worker+0x90`, but `FUN_00423950` writes the
**source buffer pointer** there, and bbdc always uses the same static buffer. So
`uploadReq != g_lastUploadReqId` suppresses every upload after the first, in
every session — which is why `DCodeIncidents.log` shows exactly one
`[Upload] profile upload finished ok` per session (20:33:46, 20:47:48, 21:06:29,
22:10:15), always ~1.5 s after the first save. Upload failures are currently
invisible. Fix: edge-trigger on `busy` (`worker+0x1D`) 0→1→0 instead.

## 2026-09-06 (later): first capture with the new instrumentation

Session 00:04:01–00:20:55 on the build carrying the `[WebApi]` logging. No
failures this session (every outcome was 7/8), so this is the healthy baseline.

**The field map is confirmed live.** The self-check line reads
`steamId=76561198103782817` — the correct account — and `date` advances 797 s
across 799 s of wall clock (1788663844 → 1788664641). `DAT_00A5A168` and the
+0x00/+0x08/+0x2C/+0x30/+0x38 layout are right.

**New: the session token rotates on every request.** `sessionLen=13`, constant,
but the hash changes after every single completed request — 9 rotations for 9
work-manager results, each ~1–150 ms after the result:

```
[WebApi] session acquired: ... sessionLen=0  sessionHash=00000000   (pre-login)
[WebApi] session CHANGED  ... sessionLen=13 sessionHash=FC779650    (login, +16s)
[WebApi] work manager result 7 (tus/read ok)
[WebApi] session CHANGED  ... sessionHash=53744944
[WebApi] work manager result 7 (tus/read ok)
[WebApi] session CHANGED  ... sessionHash=700CF627
```

This **refutes the simple "the session expires after ~50 minutes" story** from
the section above. A rolling token suggests a better mechanism for the wedge:
**desynchronisation.** If one response is lost or times out, the client keeps a
token the server has already rotated past, and every later request is rejected
with 200/empty forever. That fits the signature exactly — the first failure of
every streak on record is slow (4.2 s / 4.7 s / 9.4 s) and every one after it is
a ~700 ms fast-fail. On this reading the timeout *is* the desync event, not an
incidental symptom.

Still undetermined: whether the token is server-issued in each response or
client-generated per request. Either way `FUN_00432730` sends whatever sits at
+0x08 as `"session"`, so it is the field that matters.

**Predicted wedge signature, now directly testable:** during a wedge,
`[WebApi] work manager result 9` should repeat while the session hash STOPS
rotating. If instead it keeps rotating through the wedge, the token is fine and
the empty body is about the record rather than the session.

### The rollback, measured exactly

Ranked state at the end of the wedged 2026-09-05 session vs. the start of the
next one:

| | rank | lp | wins | matches |
|---|---|---|---|---|
| 22:49:56 (last durable) | 32 | 200680 | 1269 | 2944 |
| 23:15:37 (session end)  | 31 | 187368 | 1270 | 2951 |
| next session start      | 32 | 200680 | 1269 | 2944 |

The restart reverted to **exactly** the 22:49:56 state — **7 matches lost**, and
the 33→32→31 demotion undone. The first fetch failure was at **23:00:16**, and
the match that finished at 23:00:17 (matches=2945) was already not persisted.
So reads and writes die together, within the same ~10 min window, on one shared
session — as expected for a single transport.

### Correction to the section above

"the net-color counter froze at 22:28, ~30 min before the first read failure,
consistent with `tus/write` dying first" is **wrong**. Writes were still landing
at 22:49:56 — that state is exactly what the next session restored. The
counter=51 freeze is therefore *not* an early write failure and remains an open,
separate question. Supporting that: in this healthy 00:04 session the counter
sits at 51 through three confirmed `tus/write ok` uploads. 51 is plausibly just
a clamp within net-color band 2 rather than a stall.

### Upload detector

Works. Three uploads observed, each preceded by its `result 8 (tus/write ok)`:
`[Upload] profile upload finished ok (#1/#2/#3 this session)`. Under the old
value-dedupe only #1 would have been logged.

## 2026-09-06 phase 30: can a re-login be forced safely? (partly)

`DCodeBug30GhidraReport.txt`. The work manager exposes one request function per
strategy type, and they split across **two independent strategy slots**:

| fn | type | slot |
|---|---|---|
| `FUN_00427E40` | 0 (Idle), both slots | +0xE0 and +0xE4 |
| **`FUN_00428050`** | **1 (Login)** | **+0xE0** |
| `FUN_00427EC0` | 2 | +0xE0 |
| `FUN_00427FD0` | 3 | +0xE0 |
| `FUN_00427F60` | 4 | +0xE0 |
| `FUN_00428020` | 5 | +0xE0 |
| `FUN_00428110` | 6 | +0xE0 |
| `FUN_00427EF0` | 7 (DownloadTUS) | +0xE4 |
| `FUN_00428180` | 8 (UploadTUS) | +0xE4 |
| `FUN_004281E0` | 0, resets both | +0xE0 and +0xE4 |

**This is the good news for a repair:** Login lives at +0xE0, the TUS transfers
at +0xE4. Re-arming Login therefore cannot free or disturb an in-flight
`tus/read` / `tus/write`. The two TUS request functions are also the only ones
called from the CUMSTask worker thread (`FUN_00422B00` and `FUN_00423A50`),
while the +0xE0 family is called from the game-side `FUN_0046Bxxx` handlers --
so the slots are thread-separated as well.

`DAT_00A5A070` is **mgr+0x20**, not an unrelated global. It has exactly one
direct xref (the read inside `FUN_00428050`), and `FUN_004282C0` clears it via
`*(undefined1 *)(param_1 + 8) = 0`. No writer that *sets* it was found -- but
because it lives inside the manager object a computed `mgr+0x20` write elsewhere
cannot be excluded from xrefs alone.

`FUN_00428050`'s own caller is `FUN_0046BF10` -- the game's native "start login"
entry point. Driving *that* (or whatever calls it) is a safer repair than
poking `FUN_00428050` directly, because it is a path the game already takes.

### Still open before any auto-repair can ship

1. **Which thread ticks the strategies?** Something calls `vftable+0x1C` on the
   +0xE0 / +0xE4 objects every frame; `FUN_00422B00` only submits and polls
   `mgr+4`. Until the ticker is identified, `FUN_00429390` freeing the +0xE0
   strategy from our hook is an unproven cross-thread free.
2. **Can mgr+0x20 ever be 1?** If it can, the re-arm silently no-ops.
3. **The hypothesis itself is unconfirmed.** No wedge has yet been captured on
   the instrumented build, so "re-login fixes it" is still an inference.
