# Spectator Desync Investigation (2026-07-15)

**Verdict: NOT a Steam-side problem. Client-side bug in BBCF's game loop, mod-fixable with a small hook.**

## Architecture (confirmed from disasm + Tadatys BBCF.h)

BBCF netplay is a near-verbatim GGPO rollback port over Steam P2P. The binary contains the
literal source path `..\..\SmallSrc\Rollback\ggpo\lib\backends\steam\steam_spectator.cpp`
(.rdata 0x9860F0) and `steam_p2p.cpp` (0x985C10).

Spectating is input-relay only: the host (`SteamPeer2PeerBackend`, up to 6 spectator slots
`_spectators[6]` @ +0xA60, `_next_spectator_frame` @ +0x21774) relays confirmed inputs;
each spectator runs a `SteamSpectatorBackend` (vtable 0x986098: dtor=0x77E050,
DoPoll=0x77E090, SyncInput=0x77E340, IncrementFrame=0x77E0B0; PollUdpProtocolEvents=0x77E110)
and re-simulates the match locally from frame 0. **No state, no checksums ever cross the
wire for spectators** — correctness relies entirely on deterministic replay.

## BBCF's port is actually MORE robust than upstream GGPO

Confirmed modifications vs pond3r/ggpo:
- Input ring enlarged 64 → **256 entries** (28-byte GameInput stride, ring @ backend+0x5CE4,
  ctor 0x77DE30 inits 0x100 entries). ~4.3 s of buffer.
- **Overwrite IS detected** at read time: SyncInput 0x77E340 compares `entry.frame` vs the
  requested frame; `entry.frame > frame` fires on_event code 13
  (`GGPO_EVENTCODE_HOST_INPUTS_LOST`, string 0x986FC0) → game handler 0x783FA0 case 13 sets
  session-dead flag (+0x112 on wrapper singleton [0x182A920]). Overflow ends the session
  with an error, it does NOT silently read garbage.
- Host fan-out (DoPoll 0x77D280, @0x77D35D–0x77D410): advances at confirmed-input rate,
  never throttled by spectators; a spectator whose per-endpoint `_pending_output` ring
  (also 256) fills with un-acked inputs is gracefully disconnected (0x77FC30).
- Spectator-side disconnect timeout 5000 ms / notify 750 ms (upstream TODO, unimplemented).
- Starvation (input not yet arrived): SyncInput returns error 4 — safe wait, no state change.
- Mid-match join impossible by design: AddSpectator 0x77CF70 returns INVALID_REQUEST unless
  the session is still synchronizing; `_next_input_to_send` / `_next_spectator_frame` start
  at 0; no resync path exists. (Same as upstream.)

## Root cause of the desync (high confidence on code path)

The bug is NOT in the GGPO port — it is in the game loop's error handling at the single
in-match SynchronizeInput call site (0x4E60D5–0x4E610B):

```
004E60D5: mov [ebp-14h],eax        ; eax=0 — input buffer zeroed
004E60E8: call 7839D0              ; SynchronizeInput
004E60ED: test eax,eax / je 4E6105 ; success -> advance
004E60F1: call 55C540 / 55EDB0     ; error path -> condition check
004E6103: je 4E6110                ; condition false -> stall (correct)
004E6105: call 4E6130              ; condition TRUE -> ADVANCE ANYWAY, all-zero inputs
```

`0x55EDB0` returns 0 (never advance-on-error) for P2P player modes 1/2, but for
**spectator mode 3** returns 1 during certain scene-state windows (state pairs
([+0x30],[+0x34]) of (4,5), (5,3), (*,4) with [+0x38] != -1 — transition/loading/catch-up
windows). In those windows a starved SyncInput does not stall; the sim advances with
neutral inputs. `_next_input_to_send` does NOT advance on error, so every such frame
permanently shifts input consumption: real input for frame F gets applied at sim frame
F+k. Silent, cumulative, unrecoverable — exactly the observed "phantom inputs" symptom.

Remaining uncertainty: exact semantics of the scene-state pairs gating 0x55EDB0 (medium
confidence) — confirm at runtime by logging whenever 0x4E6105 is reached in spectator mode.

### Why the spectator sees total chaos, not just a small offset

Objection raised: the observed desync isn't "the fight shifted by a frame" — it becomes a
completely different fight. The theory predicts exactly this, for two stacking reasons:

1. **The shift accumulates.** Every starved-but-advanced frame adds one more frame of
   offset between the input stream and the simulation. Loading/transition windows can
   burn dozens of frames at once.
2. **State divergence is chaotic.** The spectator is a deterministic re-simulation. Once
   even ONE fake neutral frame is inserted, the sim state differs from the real match
   (a block whiffs, a hit connects that didn't, positions drift). From then on every
   later input — even though it's the REAL players' input — is applied to the WRONG
   state: buttons pressed as combo confirms come out as random whiffs, movement runs
   into different positions, rounds end differently. Divergence compounds every frame.
   Within seconds the spectated match shares nothing visible with the real one, even
   though the raw input list is ~identical. (Same reason a BBCF replay desyncs entirely
   if config state mismatches — deterministic re-sim has no error tolerance.)

So "inputs that don't look anything like the original" is the expected end state of a
single early zero-input advance. The instrumentation below verifies the mechanism at
onset, which is the only place it's observable.

## Instrumentation shipped (2026-07-15, log-only + gated fix)

Implemented in `src/Game/SpectatorSyncDiagnostics.{h,cpp}` + two JMP hooks in
`src/Hooks/hooks_bbcf.cpp` (`placeHooks_bbcf`), pattern-scanned and relocation-safe:

- **SpectatorSyncInputEntry** (VA 0x77E340, spectator SyncInput prologue, 9 bytes):
  captures the `SteamSpectatorBackend` ptr (ecx) into `g_spectatorBackendPtrRaw`.
  Only ever fires in spectator mode.
- **SpectatorSyncInputError** (VA 0x4E60F1, the SynchronizeInput error branch, 5 bytes =
  the `call FUN_0055C540`): calls `SpectatorSyncErrorThunk()` on every sync failure.
  The thunk logs session mode, the scene-state triple (+0x30/+0x34/+0x38 at
  scene+0x62B7C), the backend's `_next_input_to_send` (+0x5CD4) vs `_max_received_frame`
  (+0x5CD8) and the lag between them, and whether the game is about to
  ADVANCE-WITH-ZERO-INPUTS (gate replicated from 0x55EDB0). Every zero-input advance is
  logged; plain stalls are logged sparsely (first 20, then every 300th).
- **Candidate fix**: setting `SpectatorSyncFailStall` (settings.def, default 0). When 1,
  the hook returns to the stall path at VA 0x4E6110 instead of letting the zero-input
  advance happen. Player modes (1/2) are never affected — thunk returns 0 for them.

### Fault injection (2026-07-15) — manual trigger for the bug

The organic bug is heavily RNG (may take months to reproduce), so the build carries a
deliberate reproducer to validate the failsafe now:

- Debug overlay → DEBUG window → **"Spectator sync"** section (live backend ptr, input
  cursor vs newest received frame, failure/advance/stall counters), an **"Inject
  desync"** button with a frame count (default 10), and a live checkbox for the fix.
- Mechanism: while armed, the SpectatorSyncInputEntry hook makes spectator SyncInput
  return starvation (error 4) without consuming the queue, and the error hook forces the
  zero-input advance path (jump to VA 0x4E6105, i.e. jmp-back+0x0F), bypassing the
  scene-state gate we can't summon on demand. This reproduces the exact theorized
  mechanism: sim advances N frames on fake neutral inputs while `_next_input_to_send`
  stays put. With `SpectatorSyncFailStall = 1` the same injection resolves into forced
  stalls instead (the error thunk honors the fix before the advance override).
- Expected results while spectating a live match:
  - Fix OFF + inject → spectated match visibly desyncs (and never recovers) = mechanism
    reproduced; the organic bug is this mechanism firing in transition windows.
  - Fix ON + inject → brief ~N-frame pause, match stays correct afterwards (spectator
    simply runs N frames further behind real time) = failsafe production-ready for this
    failure mode.
  - Note: injection validates the *mechanism and the fix*, not that the organic trigger
    (the 0x55EDB0 scene-state windows) is the only path into it — organic confirmation
    still comes from the log-only diagnostics whenever the bug next occurs in the wild.

### Confirmation protocol
1. Build `Debug|Win32`, deploy manually, spectate real matches with
   `SpectatorSyncFailStall = 0`. In DEBUG.txt look for `SpectatorSync:` lines:
   - Theory CONFIRMED if `ADVANCE-WITH-ZERO-INPUTS` events appear and precede/correlate
     with visible desync onset (and lag = maxRecv-nextInput starts growing after them).
   - Theory WRONG (or incomplete) if desync occurs with zero such events → the divergence
     is elsewhere (sim nondeterminism, wrong initial state) — next suspects: config/RNG
     state at match start, IM 0x95 packets on vanilla spectators.
2. If confirmed: set `SpectatorSyncFailStall = 1`, re-test same scenarios (spectator
   slow-load, alt-tab, long sets, 2+ spectators). Fix VALIDATED if spectating stays
   correct and stalls only manifest as brief pauses/catch-ups.

## External corroboration

- ArcSys acknowledged spectator sync issues in the Jan 2022 rollback beta (hotfixes for
  slow-loading spectators, stuck-on-synchronizing) and never touched it again.
- GGXrd's rollback beta added spectator desync *detection* → converted silent desync into
  boot-outs; GGST avoided public spectating entirely; Skullgirls/TFH/+R fixed their
  equivalents via determinism hardening; Slippi solved the class with deep buffering +
  fast-forward catch-up over a relay server.
- No prior third-party mod has fixed spectating in any ArcSys Steam port.

## Fix options (mod-side, feasible)

1. **Cleanest / smallest:** hook the branch at 0x4E60F1/0x4E6103 (or patch 0x55EDB0) so a
   spectator-mode SyncInput failure ALWAYS stalls the frame instead of advancing with zero
   inputs. Safe: host pacing is independent of spectators; ring overflow already ends the
   session cleanly via event 13.
2. Hook spectator SyncInput 0x77E340 to surface starvation and show a "buffering…" pause.
3. Enlarging the ring is unnecessary (256 already, overflow detected).
4. Mid-match join / state-resync = large project (savestate handshake); out of scope.

Secondary desync sources to keep in mind: sim determinism (RNG/config mismatch — verify
`AA_CRandomManager` state @ 0xA135C0 is inside the rollback snapshot), and vanilla-client
handling of IM packets (version 0x95) reaching unmodded spectators — predates-mod evidence
says not the root cause, but worth ruling out as an aggravator.

## Verification plan before shipping a fix

1. Debug build with a log-only hook at 0x4E6105 (spectator mode): spectate matches, confirm
   the advance-with-zero-inputs path fires and correlates with observed desync onset.
2. Log SyncInput return codes + `_next_input_to_send` vs `_max_received_frame`
   (backend+0x5CD4 / +0x5CD8) to watch the shift accumulate.
3. Then flip the hook to stall-instead-of-advance and re-test the same scenarios
   (spectator alt-tab, slow loads, long sets, 2+ spectators).

Sources: tools/bbcf_disasm_ascii.txt (use `command grep -a` — the wrapped grep silently
skips it), docs/Research/Tadatys-BBCF-Ghidra/BBCF.h, src/Game/GhidraDefs.h,
pond3r/ggpo spectator.cpp/p2p.cpp, Steam beta forum snippets (primary threads now dead).
