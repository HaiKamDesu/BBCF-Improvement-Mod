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

Next capture should tell us: whether the rejected payload was all-zero
(transport error), truncated (recvSize != 0x6800), or genuinely corrupt
(full-size, bad checksum) — and whether a forced retry succeeds, which
decides between "transient corruption, watchdog is the full fix" and
"deterministic corruption, need to look at the sender".
