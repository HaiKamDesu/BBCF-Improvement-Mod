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

## 2026-09-08: the wedge captured on the instrumented build (vs "lotus", Nine)

Session `DebugHistory/DEBUG_20260908_190723.txt`, started 18:41:48, ended
19:07:23. Identified by `p1Char=29 p2Char=24` (Nine vs Kokonoe) and eight
`ApplyDefaultCustomPalette char=Nine` match starts.

### It is result 11, not result 9

Every single failure logs **`work manager result 11 (request refused / HTTP
error)`** — the `local_108 != 0` branch of `FUN_00428AC0`, error string
`DAT_00850400` 「TUS読み込み失敗」. Not one `result 9`.

**This invalidates the 2026-09-06 inference.** I had argued from timing that
~700 ms fast-fails implied state 9 (single attempt) rather than 0xB (three
attempts). This capture shows 0xB producing the *whole* observed range: the
first failure of the streak took **9765 ms** and the followers 1563 / 1031 /
969 / 719 / 718 / 625 / 593 ms. The timing argument was worthless, and the
2026-09-05 wedge was almost certainly 0xB too. The failure is **transport
level**, not "200 with an empty body" — so the "no TUS data / empty record"
reading of that earlier section is wrong.

### The frozen session hash is a consequence, not a discriminator

I predicted this line would decide desync-vs-not. It does not. The token only
rotates on a *successful* response, so once requests start failing it cannot
rotate under any hypothesis. Observed: last success 18:42:15 minted
`854D5452`, and that hash then stayed frozen across 26 consecutive failures
over 23 minutes. That is exactly what a stuck token and a dead transport both
look like. The discriminator turned out to be the result code instead.

### The server was NOT down — the client was stuck

The decisive comparison:

| session | start | outcome |
|---|---|---|
| 18:38:06 | login 18:41:14 (token `381BF7F0`) | first `tus/read` 18:41:19 → **11**; never a single success; user quit 18:41:40 |
| **18:41:48** | login 18:42:04 | 3 successes 18:42:06–18:42:15, then **26 consecutive 11s** 18:43:19 → 19:05:17 |
| 19:09:14 | login 19:09:35 | **3/3 `tus/read ok` within 2 s** |
| 19:12:42 | login 19:12:58 | healthy all session, 4+ `tus/write ok` |

The wedged process failed 26/26 for 24 minutes. A relaunch **110 seconds
later** logged in and succeeded immediately. 153.122.81.62 was reachable the
whole time. So the wedge is **client-side and sticky for the process
lifetime**, and a restart is what clears it.

### Working model

One transport-level failure poisons the WebApi client for the rest of the
process:

- 18:38 session — the very first `tus/read` after login fails → poisoned → 100% failure.
- 18:41 session — three succeed, then a **9765 ms timeout** at 18:43:08 (issued
  on entering the vs-lotus match, right at `OnEnterCharSelectFuncEntry`) →
  poisoned → 100% failure for the next 24 min.
- 19:09 / 19:12 sessions — no initial failure → healthy throughout.

Prime suspect: the **shared object at WebApi client +0x70**. `FUN_00434D30`
tears it down (`FUN_00427150` then `free`) together with the twelve per-type
pending slots at +0x40, and nothing recreates it mid-session. The per-request
objects *are* rebuilt each call (`FUN_00438A30` destroy → `FUN_00438890`
create), so the stickiness has to live in something shared — +0x70 is the only
candidate in that object.

### Damage in this session

`tus/write` succeeded exactly once, at 18:42:07, **before any match**. Eight
matches were then played against lotus (saves at 18:48:01, 18:50:23, 18:52:16,
18:56:03, 18:59:01, 19:02:44, 19:05:13, 19:07:08). The next session started at
`rank=32 lp=199656 wins=1269 matches=2946` — **byte-identical to this session's
start**. The entire 25-minute set was discarded.

Note the P2P netplay worked fine throughout — eight full matches. Only the
HTTP path to the ArcSys backend was dead. The user's connection was not the
problem.

### Next RE step

Identify WebApi client +0x70 (`FUN_00427150`, and whoever allocates it) and
`FUN_00438890`'s HTTP layer. If +0x70 is a reusable connection/session handle,
recycling it is the repair — and it is a much better candidate than the forced
re-login from phase 30, because a re-login alone would not rebuild it.

## 2026-09-09 phase 31: the HTTP layer, and why the +0x70 theory is out

`DCodeBug31GhidraReport.txt`.

**BBCF statically links libcurl 7.54.1.** Anchored by their own strings:

| function | is |
|---|---|
| `FUN_007DF840` | `curl_easy_setopt` ("CURLOPT_SSL_VERIFYHOST no longer supports 1 as value!") |
| `FUN_007DE340` | connection-reuse candidate search ("Found pending candidate for reuse") |
| `FUN_007F32E0` | `curl_easy_strerror` |
| `FUN_007E6EC0` / `FUN_007E6F70` | recv / send failure sites |

### The request layer

`FUN_00438890` allocates a **0xE44-byte request descriptor**; `FUN_00438D20`
then runs it on **its own dedicated thread** — it news a `uei::FRunnable`,
re-vtables it to `uei::web::client::HttpRequestThread`, and hands it to
`FUN_0042EFF0(runnable, L"HttpRequest", 1, 1, 0, 5)`.

Descriptor layout, from `FUN_00438890` (init), `FUN_00438A30` (destroy) and
`FUN_00433D90` (parse):

```
+0x000  std::string  URL           (SSO buffer; length +0x10, capacity +0x14)
+0x01C  request body copy          <-- carries the session token
+0x024  response body              std::vector<char> {begin, end, cap}
+0xE38  completed flag             (FUN_00433010 treats != 0 as "finished")
+0xE3C  HTTP-success flag          (FUN_00433D90 refuses to parse unless == 1)
```

### The +0x70 shared-object theory is dead

Phase 30 guessed +0x70 was a shared CURLM carrying a poisoned connection cache.
It is not: there is **a thread and a fresh request descriptor per request**, and
`FUN_00427150` reads as a thread/task-manager teardown (`FUN_0042F0F0` on +0x1C,
then `FUN_00426F70`/`FUN_00426E60`), not a curl handle. Also worth noting the
host is a raw IP, so libcurl's DNS cache is irrelevant either way. Scratch it.

### Two hypotheses remain, and one field separates them

`FUN_00433D90` will not parse a response unless `+0xE3C == 1`. So the 0xB branch
in `FUN_00428AC0` — `iVar2 != 0 && local_108 != 0` — is only *clearly* reachable
when a response really was parsed and carried a non-zero application error code,
the same shape as the `TL_CREATE_USER_ERR_NONUNIQUE` / `_LLIMIT` / `_ULIMIT`
codes visible in `FUN_004287E0`. Whether `FUN_00433FE0` also returns non-zero on
the unparsed path cannot be settled from the decompile.

So:

- **`+0xE3C == 0`** → the HTTP request itself failed (timeout / refused / reset).
  Fix is transport-side: force a fresh connection, or reset the client.
- **`+0xE3C == 1`** → the server answered with valid JSON carrying an error code.
  Fix is session-side (phase 30's forced re-login), and the response body will
  name the actual reason.

The 9765 ms first failure hints at the former and the ~600–1500 ms followers at
the latter, which is exactly the kind of timing argument that has now misled
this investigation twice (first "state 9 not 0xB", then "the frozen session hash
is the discriminator"). **Read the field; stop inferring.**

### Instrumentation added

`LogPendingHttpRequest` dumps, on every `result 9` / `result 11`, for both
`tus/read` and `tus/write`: `done`, **`httpOk`**, the request URL, the response
length, and up to 400 bytes of the response body — with the rotating `session`
value scrubbed to `#`, since the response is what mints the next token. Reads
only, and only after completion, on the same thread that owns the descriptor's
lifetime.

One line from the next wedge decides which of the two fixes to build.

## 2026-09-20: SOLVED — the server rejects the session with status 3

Session `DebugHistory/DEBUG_20260920_005139.txt`, 2026-09-19 23:24:52 →
2026-09-20 00:51:39. Healthy for 80 minutes (15 `tus/read ok`, 29
`tus/write ok`), then wedged at 00:45:31 and stayed wedged (12 × result 11)
until the user quit.

### httpOk = 1

The phase-31 flag came back **`httpOk=1`** on every failure, for both
`tus/read` and `tus/write`. The transport branch is dead: the HTTP request
succeeds, the server answers, and the failure is an **application error**.

### The response, decoded

Responses are `md5hex(32 chars)` + `base64(XOR(payload, "dummy"))`. Decoded:

```json
{"session":"6aaf56c0001f0","result":1,"date":1789875924,"param":{"status":3}}
```

and a successful one for comparison:

```json
{"session":"6aaf56c0001f0","result":0,"date":1789875904,"psnVer":"0.0.1", ...}
```

`result` is the field `FUN_00428AC0` reads as `local_108` — non-zero is what
produces work-manager 0xB. **`param.status == 3` is the actual error.** The
32-char prefix is an MD5 over the payload plus some salt (it matches neither
the base64, the raw bytes, nor the plaintext), which does not matter here.

Incidental confirmation: the token in the JSON is `6aaf56c0001f0` — 13
characters, exactly the `sessionLen=13` our reader reports from client+0x8. The
field map is fully vindicated.

### Status 3 means the session is rejected

| | |
|---|---|
| wedged process quit | 00:51:39 |
| next process started | **00:51:49 — 10 seconds later** |
| its first `tus/read ok` | 00:52:09 |

Same account, same IP, ten seconds apart. The server is not down, not
rate-limiting the account, not in maintenance, and the account is not blocked —
a *fresh login* is served immediately. The only thing that changed is the
session token. So **`status: 3` = "session not accepted"**, and the wedge is a
dead session the client keeps re-sending forever because nothing ever re-logs-in.

The server echoes the rejected token back in the error envelope, which is why
the hash freezes — it is confirmation of the diagnosis, not evidence for it.

### Rollback measured again

Counter at the last successful `tus/write` (00:45:08): **43**. It then moved in
RAM to 44 (00:47:59) and 45 (00:51:07). The next session loaded **43**. Progress
after the last successful write is discarded, exactly as modelled.

### What this settles, and what it does not

**Settled:** the fix is session-side. Phase 30's forced re-login
(`FUN_00428050` → type-1 Login strategy at mgr+0xE0) is the correct repair, and
— importantly — it no longer depends on knowing *why* the session dies. A
restart is functionally a forced re-login, and a restart has now been observed
curing this within 10 s (here) and 110 s (2026-09-08).

**Not settled:** why the session goes invalid. It survived 80 minutes here but
only ~1 minute in the 2026-09-08 18:41 session, so it is not a fixed TTL. A
plausible remaining candidate is a rotation race — the token rotates per
response and requests run on independent `HttpRequestThread`s, so two in-flight
requests both carrying token T could leave the client holding a consumed one.
Unproven, and no longer on the critical path for the fix.

### Blockers to clear before shipping the repair

The two phase-30 questions now matter and are worth closing:

1. which thread ticks the +0xE0 strategy (freeing it from our hook must not race)
2. whether mgr+0x20 can ever be 1, which would make the re-arm a silent no-op

### Instrumentation defect found

`ScrubSessionValue` does not work. It looks for a literal `"session"` in the
response, but the payload is XOR+base64 obfuscated, so it matches nothing and
the token reaches `DEBUG.txt` / `DCodeIncidents.log` in a form anyone can
decode with a five-byte key. Any log already shared contains a live session
token. Fix: decode the envelope before scrubbing, or simply do not log the
first field.

## 2026-09-20 phases 32-37: the scrubber, and the two repair blockers

### Scrubber: fixed and proven

The old `ScrubSessionValue` grepped the raw response for `"session"`. The
payload is XOR+base64 obfuscated, so it matched nothing and live tokens went to
disk. Replaced with decode-then-scrub:

- `Base64Decode` (tolerates the missing `=` tail these payloads have)
- XOR with the five-byte key `dummy`
- validate the result really is one of these responses (`{` … `"result"`)
- scrub `"session":"…"` to `#`
- **and if the envelope does not decode, log only the length — never the bytes.**
  An undecodable blob cannot be scrubbed, so it must not be emitted. A format
  change now costs diagnostics instead of a credential.

Verified, not assumed: `scratchpad/test_scrub.cpp` extracts the three functions
**verbatim from `NetworkStallDiagnostics.cpp`** at test time, compiles them with
g++, and runs them over the real captured envelopes from
`DEBUG_20260920_005139.txt` plus four malformed inputs.

```
FAIL tus/read 135B    decoded=1 -> {"session":"#############","result":1,...,"param":{"status":3}}
FAIL tus/write 255B   decoded=1 -> {"session":"#############","result":0,...}
FAIL tus/write 135B   decoded=1 -> {"session":"#############","result":1,...,"param":{"status":3}}
garbage / empty / hash-only / valid-b64-wrong-key   decoded=0 -> payload withheld
TOKEN LEAKS: 0
```

Bonus: the error is now readable straight from the log instead of needing
manual decoding.

### Blocker 2 — CLOSED: mgr+0x20 is never non-zero

`DAT_00A5A070` is `mgr+0x20`, the guard in `FUN_00428050`. Across the whole
binary it has **exactly one xref, and it is a READ** (phases 30 and 32 agree).
The only write to that offset on the manager is `FUN_004282C0`'s
`*(undefined1 *)(param_1 + 8) = 0`. The two other `+0x20` writes in the
manager-touching set are in `FUN_0046B560`, on a different object — it obtains
the manager from the getter separately, and its write pattern
(`+0x14/+0x18/+0x1c/+0x20=0xffffffff/+0x24/+0x28`, then a vector copy into
`+0x30`) would corrupt the manager's own vector at `+0x24..+0x2c`. So the guard
always passes and `FUN_00428050` will always re-arm the Login strategy.

### Blocker 1 — the ticker is identified, its thread is NOT yet proven

Found exactly. Every `ThinkLogicStrategy` vtable has `+0x0C == FUN_00429430`, a
forwarder to `vftable+0x1C` (the tick). Scanning all 5371 functions in
`0x400000-0x500000` (phase 34) found one caller in the manager's own range:

```c
void __fastcall FUN_00428260(int mgr)      // work manager Update
{
  (**(code **)(**(int **)(mgr + 0xe0) + 0xc))(mgr);   // Login / types 0-6
  (**(code **)(**(int **)(mgr + 0xe4) + 0xc))(mgr);   // DownloadTUS / UploadTUS
}
```

Its only caller is `FUN_0041D410` = `{ mgr = FUN_00427CD0(); FUN_00428260(mgr); }`,
which has no direct callers — it is **slot +0x14 of `AASTEAM_CNetworker`**
(vtable `0084FF54`, RTTI `.?AVAASTEAM_CNetworker@@`, resolved by walking back
from the stored pointer to the COL).

That slot is invoked polymorphically through a base pointer, so no static call
site exists to name a thread. Scanning the 45 callers of the CNetworker getter
`FUN_0041C900` for a `+0x14` indirect call produced one apparent hit,
`FUN_0046A9C0`, which on inspection is a false positive — the match is
`*(int *)(iVar5 + 0x14)`, a comparison.

What *is* established: the ticker **cannot be the CUMSTask worker thread**.
`FUN_00422B00` runs on that worker and blocks in a `Sleep(10)` loop for up to 3 s
waiting on `mgr+4`, which only the strategy ticks write. If the ticker shared
that thread every fetch would deadlock and time out; healthy fetches complete in
~1450 ms. So it is some other thread — but "not the worker" is not "the game
thread", and the repair needs the latter.

**Decisive next step, cheap and reversible:** `AASTEAM_CNetworker::vftable+0x14`
is a single pointer in `.rdata` at **`0x0084FF68`**. Swapping it for a
trampoline that records `GetCurrentThreadId()` once and tail-calls
`FUN_0041D410` answers this in one session, with no code patch and no epilogue
contract to honour. Until that returns, driving `FUN_00428050` from our hook is
an unproven cross-thread free of `mgr+0xE0`, in the exact code path meant to be
protecting the user's progress.

### Strategy-ticker thread probe (shipped)

`InstallStrategyTickProbe` swaps the single `.rdata` pointer at
`AASTEAM_CNetworker::vftable+0x14` (module base + `0x0044FF68`) for a trampoline
that records `GetCurrentThreadId()` and tail-calls the original
(base + `0x0001D410`). Safeguards:

- refuses to patch unless the slot still holds exactly the expected original,
  so a different or already-patched build is left alone
- `VirtualProtect` to RW and back, one install attempt per process
- the trampoline only logs and forwards; on the first tick it prints the ticker
  thread, the game thread (recorded in the 200 ms poll), and whether they match,
  and re-prints only if the ticker ever moves

**This does not need the wedge reproduced.** The ticker runs throughout normal
online play, so the answer arrives in the first online session on this build:

```
[WebApi] strategy ticker (AASTEAM_CNetworker::Update) on thread N (game thread M, same=0|1)
```

`same=1` clears blocker 1 and the forced-re-login repair can be written.
`same=0` means the repair must be marshalled onto the ticker's thread instead,
and the trampoline is already the place to do that from.

### Blocker 1 — CLOSED: the ticker runs on the game thread

Session 2026-09-20 03:53:24 → 04:18:07, first run on the probe build:

```
[WebApi] strategy tick probe installed at vtable slot 0135FF68 (original 00F2D410)
[WebApi] strategy ticker (AASTEAM_CNetworker::Update) on thread 36756 (game thread 36756, same=1)
```

**`same=1`.** The strategy ticker and our 200 ms poll are the same thread.

The probe's own arithmetic checks out, which independently confirms the RVAs:
module base = `0135FF68 - 0x44FF68` = `00F10000`, and the original it captured,
`00F2D410`, is exactly base + `0x1D410`. The slot held precisely what was
expected, so the guarded install fired rather than bailing.

The trampoline also proved itself safe in practice — it forwarded every frame
across 25 minutes of online play (reads and writes succeeding through 04:15),
with a clean shutdown and no crash.

**Both blockers are now closed**, so the forced re-login can be written:
`FUN_00428050` is reachable from our game-thread hook without racing the ticker
that consumes `mgr+0xE0`, and the `mgr+0x20` guard will always let it through.

## 2026-09-20: the repair

### `DCodeAutoRelogin` (default on)

Two consecutive work-manager rejects (result 9 or 0xB) trigger
`FUN_00428050(workMgr)`, which releases the strategy at mgr+0xE0 and arms a
type-1 Login in its place — a fresh `user/login`, which is exactly what a
restart does. Rate-limited to 8 per session with a 20 s cooldown, and it
refuses to run unless it is on the ticker's thread. A following result 7 or 8
logs `recovered after N consecutive failure(s)`.

Threshold of 2 rather than 1 because a single reject could be a blip, while the
wedge never heals on its own: 26/26 failures over 24 min (2026-09-08), 12/12
over 6 min (2026-09-20), both cured instantly by a restart.

Why each safety property holds, verified rather than assumed:

| risk | why it is not a risk |
|---|---|
| frees mgr+0xE0 under the ticker | the only consumer is `AASTEAM_CNetworker::Update`, measured on the game thread (`same=1`), the same thread as this poll |
| the `DAT_00A5A070` guard no-ops it | that address has exactly one xref in the binary and it is the read itself (phases 30/32) |
| disturbs an in-flight transfer | Login is mgr+0xE0, the TUS transfers are mgr+0xE4 — separate slots |
| wrong calling convention | `FUN_00428050` is `__fastcall(ecx)`; the typedef passes ECX+EDX, no stack args either way, so no imbalance |

One accepted caveat: `matching/*` shares the session, so a re-login mid-match is
not free in principle. In practice it only fires once the session is *already*
being rejected, at which point matchmaking calls are failing too — re-logging in
can only improve that.

### `DCodeForceSessionWedge` (default off, test only)

Flips one byte of the live session token at WebApi client +0x8, once per launch,
after a real token exists. The server then rejects every request with the
genuine `result 11` / `param.status 3` — the actual failure, not a simulation —
so the whole chain can be proven in one session instead of waiting days:

```
[WebApi] TEST: DCodeForceSessionWedge corrupted the session token ...
[WebApi] work manager result 11 (request refused / HTTP error) ...
[WebApi] tus/read response: {"session":"#############","result":1,...,"param":{"status":3}}
[WebApi] !!! forcing a fresh user/login (profile server rejecting the session, 1/8 this session)
[WebApi] session token replaced: ... sessionHash=<new>
[WebApi] work manager result 7 (tus/read ok) ...
[WebApi] recovered after N consecutive failure(s)
```

That exact sequence is the pass condition.

## 2026-09-20 test run: the repair was a no-op, and why

First run with `DCodeForceSessionWedge=1` (session 06:35:16 → 06:36:37).

**The harness itself worked perfectly.** It produced the genuine failure, and
the new decoder rendered it in the clear with the token masked:

```
[WebApi] TEST: DCodeForceSessionWedge corrupted the session token (first byte '6' -> 'X')
[WebApi] work manager result 11 (request refused / HTTP error) ...
[WebApi] tus/read response: {"session":"#############","result":1,"date":...,"param":{"status":3}}
[WebApi] !!! forcing a fresh user/login (profile server rejecting the session, 1/8 this session)
```

Identical to the natural wedge. Then **nothing** — no new session, no result 7
or 8, and 400 ms later the game dropped back to the menu (`MatchState::OnMatchEnd`,
`EndGameMode`). The user could not get online at all.

### Root cause: phase 32's "blocker 2 closed" was WRONG

`FUN_00428050` only arms a Login `if (DAT_00A5A070 == 0)`. Phase 32 concluded
that always passes because mgr+0x20 has exactly one xref and it is a read.

It is set — by the Login strategy itself. `FUN_0042E660`, on a successful login:

```c
else if (local_ac == 0) {
  *(undefined4 *)(param_1 + 4) = 2;
  *(undefined1 *)(param_2 + 8) = 1;   // param_2 is the manager as undefined4*
  *param_2 = 1;                        //  -> byte at mgr+0x20 = 1
}
```

`param_2 + 8` on an `undefined4 *` is byte offset **0x20** — `DAT_00A5A070`.
The write goes through a pointer, which is *exactly* the loophole phase 32
flagged and then dismissed anyway. So after the boot login succeeds the latch
is 1 and **`FUN_00428050` is a permanent no-op for the rest of the process** —
which is also why the game has no re-login path and why only a restart has ever
cured this.

The forced-wedge harness earned its keep on its first run: this would otherwise
have shipped as a fix that does nothing.

### Three fixes

1. **Clear the latch.** `TryForceRelogin` now writes 0 to mgr+0x20 before
   calling `FUN_00428050`, and clears the session token with it so the login
   starts from the same state a fresh process does (`sessionLen=0`). The token
   is worthless at that point by definition — this only runs after the server
   has rejected it repeatedly. Both values are logged.
2. **Watch the login result.** Login and UserCreate publish to **mgr+0x00**,
   not mgr+4 like the TUS strategies, so nothing was watching them — which is
   why the test could not say whether the login had even been attempted.
   `SampleWorkMgrLoginState` now logs mgr+0x00 (1 = ok, 2 = rejected,
   4 = user/create ok, 0xB = failed/timed out) and the latch transitions.
3. **Stop the harness firing during online entry.** It corrupted the token in
   the same millisecond the boot login completed, i.e. mid online-entry, and
   `matching/*` and `lobby/*` share the session — hence "could not get online".
   It now waits for 3 successful transfers, so it wedges mid-session the way
   the real bug does.

## 2026-09-20 second test: the latch fix works, the token clear was a wipe loop

Session 15:48:28 → 16:08:41. Harness fired correctly this time (after 3
successful transfers, mid-session, not during online entry).

### What worked

The latch fix is confirmed. Every forced attempt now reads:

```
[WebApi] !!! forcing a fresh user/login (..., login latch 1 -> 0)
[WebApi] login state 1 (user/login ok), latch=1
[WebApi] session token replaced: ... sessionLen=13 sessionHash=<new>
```

`FUN_00428050` + clearing mgr+0x20 genuinely re-runs `user/login`, and the
server issues a fresh token every time. Six attempts, six successful logins.

### What broke: clearing the session token

The token timeline says it plainly — every good token was wiped within a second,
and never by us (the cooldown is 20 s, the next forced attempt is minutes away):

```
15:52:26 len=13   boot login
15:55:41 len=0    <- our clear
15:55:42 len=13   <- re-login succeeds
15:55:43 len=0    <- WIPED 0.6s later
15:57:41 len=13 -> 15:57:41 len=0   (0.2s)
15:59:07 len=13 -> 15:59:08 len=0   (1.0s)
16:03:02 len=13 -> 16:03:02 len=0   (0.4s)
16:08:23 len=13 -> survived (session ended 18s later)
```

Mechanism: requests already in flight when the token is cleared go out with an
empty session, the server rejects them and **echoes `"session":""` back**, and
the client stores the session from every response. That empty value overwrites
the token the login had just minted. Those rejections then feed the failure
counter and drive another attempt — self-sustaining.

Result for the session: 29 × result 11, 3 × result 7 (all pre-wedge), zero
recoveries. The wedge persisted into the second match even with the setting
turned off, because the loop was ours, not the harness's.

Telling detail: the one attempt whose token survived (16:08:23) was immediately
followed by a **47604-byte** `tus/read` response — about the size of a real
0x6800 profile payload, i.e. very likely the successful read. The session ended
before it could be confirmed.

### Three changes

1. **Do not clear the session token.** Only the latch. The latch clear is what
   makes the login happen; the token clear only destroyed its result.
2. **Grace window.** 5 s after a forced re-login, failures do not count toward
   the next trigger — they are almost certainly requests that were already in
   flight carrying the old token.
3. **Decode large responses instead of rejecting them.** `Base64Decode` returned
   0 on output overflow, so a *successful* ~47 KB read logged as "undecodable
   envelope" — exactly the response worth reading. It now truncates; the JSON
   header carrying `result` comes first, so the leading bytes suffice.

## 2026-09-20 third test: RECOVERY CONFIRMED — the repair works end to end

Session 16:14:57 → 16:33. The harness wedged it mid-set, and it came back:

```
[2026-09-20 16:31:55.733] !!! forcing a fresh user/login (..., 7/8 this session; login latch 1 -> 0)
[2026-09-20 16:31:56.495] login state 1 (user/login ok), latch=1
[2026-09-20 16:32:00.365] work manager result 7 (tus/read ok) sessionHash=F685C2AB sessionLen=13
[2026-09-20 16:32:00.365] recovered after 11 consecutive failure(s)
[2026-09-20 16:32:02.973] work manager result 8 (tus/write ok) sessionHash=0F046C62 sessionLen=13
```

Reads and writes both restored. **This is the first time the wedge has ever
been cleared without restarting the game.** The whole chain — detect,
clear the latch, re-login, resume — is proven against the genuine failure.

### But it took 7 attempts over 10 minutes

The reason is visible in the session hash on every result line: it stays
`BF18E6D7` (the poisoned value) from 16:22:01 right through to 16:31:55,
across **six** successful logins. The login works every time and the token it
mints keeps disappearing.

Same mechanism as the previous round, with a different culprit value. The
client stores the session from *every* response, and the responses to tus
requests already in flight echo back the poisoned token they were sent with —
restoring it over the good one, typically within a second. Caught mid-act at
16:29:51.370, where the live token was briefly empty and the server replied
`{"session":null,"result":1,"param":{"status":1}}` — note status **1**, a
different code for a null session.

Attempt 7 only worked because it happened to land in a ~4 s gap with no stale
response in flight: login at 16:31:56.495, first request after it at
16:32:00.365 used the new token and succeeded.

So recovery was a race, and it won by luck.

### Fix: defend the new token, narrowly

`DefendReloginSession` records the exact fingerprint that was being rejected
when the re-login fires. Once the login lands, it snapshots the new token, and
for 20 s afterwards, if the live token reverts to **that specific poisoned
value**, it puts the good one back. Any other value is left untouched, so a
legitimate rotation on a successful response still takes effect.

It runs every frame from the ticker trampoline rather than the 200 ms poll,
because the overwrite lands inside a second and the poll is not reliably fast
enough to beat the next outgoing request.

Expected result now: recovery on the **first** attempt, seconds after the
wedge, instead of the seventh.

## 2026-09-20 fourth test: PASS — recovery in 3.5 seconds

Session 16:42:26 → 16:48:23. The defence works, and it was caught doing its job:

```
16:46:06.002  !!! forcing a fresh user/login (..., 2/8 this session; login latch 1 -> 0)
16:46:06.767  new session 67BB6E90 held against the rejected one 507CBC28
16:46:06.921  login state 1 (user/login ok), latch=1
16:46:07.439  tus/read response: {"session":"###...","result":1,...,"param":{"status":3}}   <- stale echo
16:46:07.453  a stale response restored the rejected session; put the new one back (1)
16:46:09.520  work manager result 8 (tus/write ok) sessionHash=E9CBAACD sessionLen=13
16:46:09.520  recovered after 3 consecutive failure(s)
16:46:09.603  [Upload] profile upload recovered after 1 consecutive failure(s), netcolor=2 counter=41
```

The overwrite happened exactly as modelled — a stale response echoed the
rejected token back at 16:46:07.439 — and the defence undid it **14 ms later**.
Two seconds after that the profile upload went through.

**Trigger to recovery: 3.5 s**, against 10 minutes and 7 attempts before the
defence existed. The session then stayed healthy to the end: reads at 16:48:11
and 16:48:12, write at 16:48:15, upload #2 ok, counter steady at 41.

Most importantly `[Upload] profile upload recovered` — the profile actually
persisted. That is the whole point: the wedge no longer costs progress.

### Remaining imperfection

Attempt 1 (16:43:35) did not recover. No `held against` line was logged for it,
so the defence never armed: the token never held a non-poisoned value at any
frame boundary for it to snapshot. Recovery came on attempt 2.

The 2.5-minute gap between the two attempts was **not** the cooldown (20 s) —
no tus request was issued at all between 16:43:40 and 16:46:06 because the
player was in a match. So the practical worst case is "recovers at the next
profile request after the wedge", and that request is the match-end write —
precisely the one that matters. No progress was lost even on the miss.

Tightening attempt 1 is polish, not correctness: it would need the snapshot to
key off the login-state transition rather than observing the token change, and
that state is only sampled in the 200 ms poll.

## 2026-09-20: CONFIRMED FIXED (user-visible verification)

Verified by the reporter against the symptoms rather than the log:

1. `DCodeForceSessionWedge=1`, play a match → D-Code visibly broken, as in the real bug
2. play a second match → **D-Code came back**
3. restart → **progress had saved**

That is the whole bug, reproduced and then repaired in-process for the first
time. `DCodeAutoRelogin` ships on by default.

### The fix, end to end

| stage | mechanism |
|---|---|
| detect | 2 consecutive work-manager rejects (result 9 / 0xB) |
| unblock | clear the login latch at mgr+0x20, which the boot login sets and which made `FUN_00428050` a permanent no-op |
| repair | `FUN_00428050` arms a type-1 Login → `user/login` → fresh session |
| hold | `DefendReloginSession` undoes any revert to the specific rejected token, which stale in-flight responses echo back within ~1 s |
| resume | next `tus/read` / `tus/write` succeeds; profile upload persists |

Measured: trigger → recovery **3.5 s**.

### Known limitation

The D-Code display returns on the **next** match, not inside the one that broke.
Per-slot auto-recovery is capped at 3 tries (`kMaxAutoRecoveries`), and a slot
that burns its budget during the outage is never re-armed when the transport
comes back — in the 16:42 capture slot 0 exhausted its budget at 16:43:37,
2.5 minutes before recovery landed at 16:46:09, and stayed at state 6 for the
rest of that room. Slots 4/5, which still had budget, recovered normally and
accepted payloads at 16:48:11.

**Fixed.** `ReArmAllSlots` — which already existed for the TUS latch — is now
also called when the re-login recovers the transport. It returns every slot's
retry budget and forces any slot still sat at state 6 back to 0, which is the
precondition the game itself uses to issue a fresh fetch. The log line names
the caller, so `[WebApi] re-armed N wedged slot(s)` distinguishes it from the
latch path's `[TusGate] re-armed ...`.

The D-Code should now return inside the same match rather than the next one.

## 2026-09-20 fifth test: recovery did NOT fire, and there was no crash

Session 21:06:34 → 21:23:02, build `32aa9ce7` (clean). Mod fully initialised
this time (`SteamAPI_Init` 1, `WindowManager::Initialize` 3, probe installed),
so the earlier no-GUI launches were the `2f22758e-dirty` build, not a
regression — see below.

### No crash occurred

All three triage layers are clean for the window:

- `DEBUG.txt` ends through the full clean path — `BBCF_IM_Shutdown` →
  `WindowManager::Shutdown` → `CleanupInterfaces` → `BBCF_FIX STOP`
- newest `BBCF_IM/CrashReports/` folder is 2026-09-14
- no Application/System error events since 20:00 local
- newest WER `AppCrash_BBCF.exe_*` is **2026-09-05**

`DLL_PROCESS_DETACH` ran, which neither a fault nor a Task Manager kill
produces. The process exited in an orderly way at `gameMode=15 gameState=14
round=intro` — the match loading screen. The game quit; it did not crash.

### The recovery did not complete

```
21:22:29.558 TEST: DCodeForceSessionWedge corrupted the session token
21:22:48.553 result 11 ... sessionHash=D382B3C5
21:22:49.285 !!! forcing a fresh user/login (1/8; login latch 1 -> 0)
21:22:50.053 login state 1 (user/login ok), latch=1
21:22:54.345 result 11 ... sessionHash=D382B3C5      <- unchanged
```

The login succeeded and the session hash never moved off the poisoned value
across all six results spanning 21:22:48–21:22:54. **No `new session … held
against` line at all**, so `DefendReloginSession` never armed.

Two candidates, and the capture cannot separate them:

1. the login genuinely minted no new token, or
2. the defence was not running, so a brief good value went unobserved

Candidate 2 is plausible and specific: the defence ran *only* from the ticker
trampoline, the ticker is `AASTEAM_CNetworker::Update`, and this wedge happened
**during match load** — there is no guarantee the networker updates in that
context. The successful 16:46 run was in the lobby, where it clearly does.

The 1.8 s gaps between result lines are also wide enough to hide a flip, and
`WatchWebApiSession` no longer logs rotations (only length changes), so nothing
recorded one either way.

### Three changes

1. **Run the defence from the 200 ms poll as well as the ticker.** The poll is
   driven off the overlay update and keeps running when the networker does not.
   Whichever fires first wins; the function is idempotent. This is the actual
   fix if candidate 2 is right.
2. **Count ticker invocations** and print the count with the re-login
   (`tickerTicks=N`). If that number stops advancing across a failing window,
   candidate 2 is proven outright rather than argued.
3. **Log every token change inside the defence window**, with the tick offset.
   Window-scoped so the volume stays trivial, and a flip can no longer hide
   between two result lines.

Next capture distinguishes the two candidates by itself.

## 2026-09-20 sixth test: PASS, with the mechanism visible frame by frame

Session 21:41:21 → 21:47:15, build `b8e57e54`. Clean shutdown, no crash.

The new window logging shows the whole thing happening, two frames wide:

```
21:42:33.865 !!! forcing a fresh user/login (1/8; login latch 1 -> 0, tickerTicks=4100)
21:42:34.597 session F3591994 -> D7218029 during the defence window (tick 44)   <- login minted it
21:42:34.598 new session D7218029 held against the rejected one F3591994
21:42:34.626 session D7218029 -> F3591994 during the defence window (tick 46)   <- stale response stole it
21:42:34.626 a stale response restored the rejected session; put the new one back (1)
21:42:34.647 session F3591994 -> D7218029 during the defence window (tick 47)   <- defence won
```

Two frames from theft to restoration. The same pattern repeated on attempt 2
at ticks 87→88, and then:

```
21:44:50.738 work manager result 8 (tus/write ok) sessionHash=AADD805A
21:44:50.738 recovered after 4 consecutive failure(s)
21:44:50.738 re-armed 1 wedged slot(s) and reset all retry budgets
21:44:50.930 [Upload] profile upload recovered ..., netcolor=2 counter=41
```

**The `re-armed` line is the polish working** — exactly one slot was still
wedged and got un-wedged the moment the transport came back, instead of
waiting for the next match. Session healthy afterwards: reads at 21:44:54,
21:46:55, 21:46:57, write at 21:47:00, upload #2, counter steady at 41.

### The ticker was alive, so the previous run stays unexplained

`tickerTicks=4100` at attempt 1 and `11976` at attempt 2 — ~7900 ticks over
2m14s, i.e. ~59/s, the frame rate. So candidate 2 (dead ticker) is ruled out
**for this run**. The 21:22 failure remains unexplained; the poll-driven
defence added in `b8e57e5` covers that case either way, but it was not the
thing that saved this run and should not be credited for it.

### Recovery took 2m17s, not 3.5s

Attempt 1 armed, held the token, and reads appear to have come back — but
`tus/write` kept returning status 3 and the work-manager result stayed 11, so
the failure counter never cleared. Full recovery came on attempt 2, two
minutes later, when the next match-end write went out.

Why attempt 1 did not finish cannot be read off this capture: the `tus/read`
responses at 21:42:37 and 21:42:38 were 33329 bytes and logged as
"undecodable envelope" — and those are precisely the ones that would say
whether the read had recovered. The `break`-on-overflow fix was not enough;
something else about those responses fails the decode.

Added: on a decode failure the log now reports whether the 32-char prefix is
hex and shows the first 40 characters. The prefix is an MD5 over the payload,
not a credential, and 8 base64 characters past it decode to ~6 plaintext
bytes — nowhere near the session value.

## 2026-09-26: a session that ran out of retries — and a correction

Session 12:39:04 → 15:19 (`DebugHistory/DEBUG_20260926_151941.txt`). The
wedge recurred every few minutes for two hours. `DCodeAutoRelogin` recovered
it **twelve times**, then hit the flat cap of `8/8` at 14:32:55, and from then
on: 71 consecutive rejections with the session hash frozen at `C9A1E227`,
nothing left to try, game still running.

### Correction: ranked LP DOES roll back with the profile

I told the reporter, in the moment, that LP and rank were safe because they
ride Steam leaderboards, citing the 2026-09-20 session where matches carried
across a restart. That reading was wrong — that session simply had a
successful write near its end. Measured today:

| | rank | lp | wins | matches |
|---|---|---|---|---|
| session end (15:10) | 35 | 233992 | 1423 | 3196 |
| next session load (15:20:19) | 34 | 227754 | 1411 | 3183 |

Rank 35 → 34, −6238 LP, −12 wins, −13 matches — back to the last confirmed
write. **Steam leaderboard uploads succeeding (198 of them, the last seconds
before exit) protect nothing**; the profile blob is authoritative on load and
the displayed rank comes from it. Everything the mod shows in the ranked
overlay lives in the thing the wedge breaks.

### Cap → sliding window (`a5aa808`)

A cap that can run out while the player is still playing is the wrong shape.
Now 6 attempts per 10-minute window, reopening on its own; the 20 s cooldown
still prevents a tight loop.

### The cause: what nine onsets rule out

Extracting the events immediately before each first rejection:

```
onset  last success              first reject
  1    r7  CE7A0A66 12:40:12     r11 CE7A0A66 12:40:26   (14 s)
  3    r8  2115E389 13:24:34     r11 2115E389 13:26:47   (2m13s)
  4    r8  3F669A8E 13:44:00     r11 3F669A8E 14:06:17   (22 min)
  6    r8  05042F40 14:17:12     r11 05042F40 14:18:44   (92 s)
  9    r8  C9A1E227 14:34:57     r11 C9A1E227 14:35:56   (59 s)
```

In every case the rejected request carries **the newest token the client
knows — the one minted by the immediately preceding success.** That rules out
the rotation-race idea (two in-flight requests sharing a token): a raced
request would carry an *older* token than the last success, and none does.

An idle timeout is ruled out too: onset 2 had a 3m47s gap that **succeeded**,
onset 1 a 14 s gap that failed.

What survives: the client is not the only thing talking to the server on this
session. `lobby/get_status`, `matching/start|confirm|end` and the catalog
calls all carry and rotate the same token. If any of their responses rotates
the server-side session but the client's handler for that endpoint does not
store the returned token, the next `tus/*` request sends a token the server
has already moved past. It is intermittent because it depends on whether such
a call lands between two profile calls, and it is more frequent now because
ranked lobbies generate more of that traffic.

### Diagnostics to settle it (`a5aa808`)

- **URL read fixed.** The descriptor's URL is a `std::wstring`; reading it as
  narrow stopped at the first wchar's NUL, which is why every line so far said
  `url="h"`. Each line now names its endpoint.
- **Session → fingerprint, not `###`.** A decoded response's session value is
  replaced by `#` + its FNV-1a hash — the same hash the `sessionHash=` lines
  use — so what a response *carried* can be compared with what the client
  *holds*, without the token reaching the log.
- **All twelve endpoints dumped on rejection.** One line each: endpoint, done,
  httpOk, byte count, and the decoded header (session fingerprint, result,
  date). If a `lobby/get_status` or `matching/*` response shows a session the
  client does not hold, the cause is found.

### "Steam uploads" dialog (this commit)

Right-click on the ranked progress or network square colour window → a dialog
with the last confirmed-saved state beside the live one, the upload time, and
an explicit "unsaved" delta. This is the thing that would have told the
reporter, at 14:40, that nothing since 14:34 was being kept.
