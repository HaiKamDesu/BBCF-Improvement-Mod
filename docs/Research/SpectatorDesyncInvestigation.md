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

### FIELD RESULT 2026-07-20 — mechanism captured organically

First live spectate session with the diagnostics (Azrael vs Izanami, fix OFF all
session, no injection used): 21 sync failures logged; #0–#19 in non-window scene states
`(0,*,-1)/(2,*,-1)/(3,2,-1)` all correctly stalled; **#20 hit scene=(4,5,1) →
ADVANCE-WITH-ZERO-INPUTS** — the exact predicted window, ~2 min into the match. The gate
replication matches the game's behavior 21/21. No visible desync followed, which is
consistent with theory: a single neutral-frame insert during a transition window (both
characters idle) shifts the whole stream by one frame without changing the trajectory.
Visible desyncs need an insert (or a burst of them) landing while inputs are held
(block/charge) — hence the RNG feel. Mechanism status: CONFIRMED ORGANIC. Fix efficacy:
still untested (fix was never active; injection never triggered — see protocol below).

Also confirmed: the game loaded at a relocated base (hooks found at 0x006BE340 /
0x004260F1, base 0x340000) — pattern scanning is mandatory here, never hardcoded VAs.

## CRASH FOUND AND ROOT-CAUSED (2026-07-20)

The `SpectatorSyncFailStall` fix (force-stall on the caller-side error branch, VA
0x4E60F1) **crashes the game** with a novel signature never seen in ~20 months of prior
crash reports: `Exception code 0xC0000409` (`__fastfail(FAST_FAIL_FATAL_APP_EXIT=7)`,
i.e. a generic `abort()`), fault offset `0x3A7B59`, landing in BBCF.exe's own
statically-linked CRT abort routine. Confirmed by clean bisection: hooks off (a full
spectate-to-match-end run) = no crash; hooks back on, same scenario = crash, reproduced
twice. On the third repro the actual on-screen error surfaced before the CRT abort:

> **GGPO Assertion Failed** — `_event_queue.push(evt) @ ..\..\SmallSrc\Rollback\ggpo\
> lib\network\steam\steam_udp_proto.cpp:380 (pid:38500)`

This pins the mechanism exactly: `SteamUdpProtocol`'s internal event queue (a fixed-size
ring, matching upstream GGPO's `RingBuffer<Event,N>` design) overflowed and asserted,
which then aborts the process — landing at the same generic `abort()` path WER reported.

### Why the fix caused this (traced in the disassembly, VA 0x4E6000 caller)

```
004E60E8: call 007839D0        ; SynchronizeInput
004E60ED: test eax,eax
004E60EF: je   004E6105        ; SUCCESS -> falls through to calling 0x4E6130 too
004E60F1: call 0055C540        ; FAILURE -> scene getter (our old hook site)
004E60F6: lea  ecx,[eax+62B7Ch]
004E60FC: call 0055EDB0        ; "should we advance anyway?" gate
004E6101: test eax,eax
004E6103: je   004E6110        ; gate says no -> real stall, 0x4E6130 skipped (ALWAYS SAFE -- this is ordinary vanilla starvation, has fired constantly all along with zero incidents)
004E6105: lea  eax,[ebp-14h]   ; gate says yes -> apply frame:
004E6108: push eax
004E6109: mov  ecx,edi
004E610B: call 004E6130        ; <-- called on genuine SUCCESS *and* on the "advance-with-zero-input" case. NEVER on real vanilla stall.
004E6110: <epilogue>            ; real vanilla stall lands here too, without calling 0x4E6130
```

`0x4E6130` is not the bug — it's the game's unconditional "apply this frame's input"
step, invoked whenever the frame is meant to advance (real sync OR the vanilla
zero-input fallback). Vanilla **only** skips it in the ordinary starvation case (gate
says no), which is what almost all real starvations hit, and that path has been
exercised harmlessly forever. **The `SpectatorSyncFailStall` fix made the rare
advance-window case (`(4,5,*)/(5,3,*)/(*,4,*)` scene states — evidently
match-transition/round-end states, not generic mid-fight lulls) *also* skip `0x4E6130`
— something vanilla never does there.** Since `0x4E6130` evidently also services the
GGPO transport's event queue drain (thematically consistent with the exact assertion
hit), skipping it during those specific windows lets the queue back up until it
overflows and aborts — which lines up with "crashes right as the match ends."

**Verdict: the `SpectatorSyncFailStall` implementation is unsafe and must not ship.**
It was off by default and only ever enabled for our own testing, so no user was
affected. Retired entirely in favor of the redesign below.

## Redesigned fix (2026-07-20): substitute last input, never touch the caller

Instead of intervening on the game loop's frame-advance decision (shared code, used by
both players and spectators, with side effects we don't fully understand), the new
approach intervenes **only inside the spectator-exclusive `SyncInput`** (VA 0x77E340,
`SteamSpectatorBackend::SyncInput`), which the caller can only reach in spectator mode.

Full disassembly of `SyncInput` (traced personally, not inferred):
- Not-yet-synchronizing guard -> return 6.
- `edx = _next_input_to_send` (backend+0x5CD4); ring index `idx = edx mod 256`
  (signed-mod idiom); `entry = backend + 0x5CE4 + idx*28` (28-byte `GameInput` stride:
  `frame`@0, `size`@4, `bits[18]`@8).
- `cmp entry.frame, edx`:
  - `entry.frame < edx` (not arrived yet) -> **`return 4`** — VA `0x77E3AD`, an 18-24
    byte pop/return sequence. **This is the only site the redesign touches.**
  - `entry.frame > edx` (ring overflow) -> fires `on_event(13)` (`HOST_INPUTS_LOST`),
    `return -1`. Untouched.
  - equal -> compute `copySize = backend[0x5CD0](_input_size) * backend[0x5CCC]
    (_num_players)`, assert if the caller's buffer is too small (the *other*, unrelated
    "GGPO Assertion Failed" path, at VA 0x77E43E), else `memcpy(destBuf, &entry.bits,
    copySize)`, `*disconnectFlags = 0`, `_next_input_to_send++`, `return 0`. Untouched.

### The fix

Hook only the starvation return site (VA `0x77E3AD`, 24 bytes, ends at the shared tail
`0x77E3E3`: `pop edi/pop esi/pop ebx/<SEH+security-cookie restore>/ret 0Ch` — a tail that
never touches `eax`, so it's reusable for any return value). At entry to this site,
`esi`=backend, `ebx`=destBuf, `edi`=disconnectFlags ptr are all still live (not yet
popped).

New behavior: if `_next_input_to_send > 0` (we've delivered at least one real input
before) **and** the immediately-preceding ring slot (`idx = (_next_input_to_send-1) &
0xFF`) still holds exactly that frame's data (sanity check against ring overwrite),
**repeat that previous input**: `memcpy` its `bits` into the caller's buffer using the
same `_input_size * _num_players` size vanilla computes, zero the disconnect flags,
return `eax=0` (**success** — not 4) by jumping to the shared tail. Crucially,
**`_next_input_to_send` is *not* incremented** — the real, still-pending frame stays
queued and will be delivered normally, whenever it actually arrives, with no data ever
skipped. If there's no valid previous entry yet (session just started, or the ring
already wrapped past it), fall back to genuine vanilla `return 4` — unchanged behavior,
proven safe by 20+ months of history.

Why this is safe and doesn't reintroduce the crash: the caller only ever sees `eax==0`
(exactly like a real success) or the ordinary vanilla starvation (`eax==4`, handled by
existing, untouched code). **The caller's advance-window gate (`0x55EDB0`) and its
`0x4E6130` call are completely bypassed in the common case** because we resolve the
"failure" before it ever reaches the caller — so nothing about the event-queue-servicing
call is ever skipped differently from vanilla. The rare cases where our substitution
can't help (very first frame, or 256+ frame outage) fall through to the exact vanilla
code path that's always been there, gate and all — unmodified, so whatever invariant it
relies on remains intact.

This is also strictly better UX than either the old zero-fill bug or the stall fix:
repeating the last known input during a brief gap is a standard rollback-netcode
prediction technique (much less visually disruptive than snapping to neutral), and
since `_next_input_to_send` never moves during a substitution, there is no cumulative
permanent frame-shift risk either — the original desync mechanism is prevented at its
source rather than patched after the fact.

## Second crash found and fixed (2026-07-20, same day, after the redesign)

With the redesigned fix in place, one spectate session worked correctly (log shows
startups falling back correctly at `nextInput=0`, then real starvations correctly
`SUBSTITUTED`, no crash, clean match). On the *next* spectate session, opening the
DEBUG window's "Spectator sync" section while still on the versus-screen transition
(before that match's `SyncInput` had run even once) crashed the game again. Same
`0xC0000409`/fault-offset-`0x3A7B59` signature at the Windows Application-log level,
but this time `LocalDumps` (enabled earlier this session) actually captured a full
`.dmp`, analyzed with `cdb` (see `tools/cdb_spectator_crash_cmds.txt`).

The real call stack showed the `0xC0000409` was **downstream fallout from a genuine
access violation inside our own `dinput8.dll`**, not a game-code issue at all:
`KiUserExceptionDispatcher` dispatching into `dinput8+0x15671a`, which (resolved by
disassembling our own Release `dinput8.dll` at the matching hash) is exactly:

```
SpectatorSyncDiagnostics::GetStatus():
  mov ecx, [g_spectatorBackendPtrRaw]
  test ecx, ecx
  je <skip>
  mov edi, [ecx+5CD4h]      ; <-- faulted here: _next_input_to_send read
```

Root cause: `g_spectatorBackendPtrRaw` is only ever refreshed by `SpectatorSyncInputEntry`
when `SyncInput` actually runs. Between matches (menu/versus-screen, before the new
spectate session's first `SyncInput` call) it still points at the **previous match's
already-destroyed** `SteamSpectatorBackend`. `GetStatus()` (called every frame the
DEBUG window's "Spectator sync" section is open, for the live counters display) only
checked the pointer for null, not for backend liveness — a classic use-after-free, and
completely unrelated to the actual fix mechanism (`SpectatorSyncSubstituteThunk`, which
is only ever invoked with a genuinely live backend since it's called from inside a real
`SyncInput` call or from the entry hook at the exact moment one is happening).

**Fix:** wrapped the two raw reads in `GetStatus()` with `__try`/`__except
(EXCEPTION_EXECUTE_HANDLER)` (precedented elsewhere in this codebase, e.g.
`ControllerOverrideManager.cpp`, `UnlimitedReplayTakeoverManager.cpp`) so a stale
pointer degrades to "no data" for the diagnostics display instead of crashing the
process. This is display-only code with no way to know backend lifetime short of
hooking the destructor too, so a defensive SEH guard is the appropriate fix rather than
trying to track staleness precisely.

## Third report (2026-07-20, same day): crash recurred + "didn't fix the desync"

Reproduced the *exact* same crash signature/stack again. Root cause this time: **the
SEH-guard fix was never actually deployed** — `bin/Release/dinput8.dll` (and
`bin/Debug/`) get deleted by a post-build step (`tools/package_release.ps1`, wired into
`BBCF_IM.vcxproj`'s post-build `Exec`, which zips the DLL into
`BBCF.IM.win-x86.v*.zip` then `Remove-Item`s the loose copy). The file actually on the
D: drive still hash-matched the pre-SEH-fix build from earlier that day. **Lesson: after
building, always extract the loose DLL from the freshly-built zip (or check hash
against the zip's contents) before telling the user to redeploy — the loose file in
`bin/<Config>/` does not survive a normal build in this repo.**

On the "didn't fix the desync" complaint: the DEBUG.txt from this session shows organic
network conditions far worse than earlier test sessions — sustained bursts of
**11 to 30+ consecutive real starvations**, all substituted correctly (no crash from the
substitution logic itself, `_next_input_to_send` correctly held steady through each
burst, confirmed by the log). This is an inherent limitation of "repeat the last input"
as a smoothing strategy: it prevents the crash and the permanent stream-shift, but
holding one input for 0.5+ seconds straight (which is what a 30-frame burst at 60fps is)
is still going to look visibly wrong to a spectator, even though nothing is permanently
corrupted. The fix trades "instant catastrophic desync" for "briefly looks
unresponsive/repeats an action during bad connection stretches" — a real improvement,
but not literally invisible during severe, sustained connection trouble. Worth
discussing with the user whether that's an acceptable tradeoff or whether longer bursts
should behave differently (e.g. an explicit "buffering" pause/indicator once the
substitution streak crosses some threshold, rather than continuing to repeat input
indefinitely).

## Freeze redesign (2026-07-20) — the correct, desync-eliminating fix

The "repeat last input" substitution stopped the crash but still advanced the sim with a
fabricated frame, so it did not eliminate the underlying stream-shift (confirmed by a
112-frame substitution streak at match end in the logs). Replaced with a true freeze.

**Key RE findings that made this possible:**
- `0x4E6130` is the full fight-frame advance (fans out into the whole battle update,
  incl. `BATTLE_CObjectManager::update_inputs_online` at `0x55A3E0`). Calling it = one
  simulated frame = a phantom frame if the input is fabricated.
- `SynchronizeInput` dispatch (`0x7839D0` -> `0x77C8A0`) does NOT pump the network.
- `SteamSpectatorBackend::DoPoll` (`0x77E090`) = `Udp::Poll` (`0x781C70`, fills the event
  queue from the socket) + `PollUdpProtocolEvents` (`0x77E110`, drains it into the input
  ring). This drain is only run as part of the frame-advance, so **freezing (skipping
  the advance) skips the drain → the receive event queue overflows → the crash the first
  freeze attempt hit** (`steam_udp_proto.cpp:380`).
- **Vanilla has NO separate spectator "match ended" detector.** Its only mechanism for
  progressing the spectator through round-ends/match-end when input isn't arriving is the
  advance-with-zero-input in scene states `(4,5)/(5,3)/(*,4)` — the exact code causing the
  desync. Detection and bug are the same code; there is no clean signal to reuse.
- Vanilla DOES define `_disconnect_timeout = 5000ms` on the spectator backend — its own
  "the host is gone" threshold.

**The fix (`SpectatorSyncFreezeDecisionThunk` + two hooks in hooks_bbcf.cpp):**
- Entry hook `SpectatorSyncInputEntry` (`0x77E340`): captures the backend pointer; also
  hosts test-only injection (forces `eax=4` starvation on demand).
- Caller gate hook `SpectatorSyncInputError` (`0x4E60F1`): on every starved frame, calls
  the decision thunk, which:
  1. Manually pumps `DoPoll(backend, 0)` — keeps the receive queue drained (no overflow)
     AND lets the awaited input actually arrive.
  2. Tracks a freeze streak keyed on `_next_input_to_send` progress (the cursor only
     advances when a real input is consumed, so a moved cursor = stream flowing = reset).
  3. Returns FREEZE (force the vanilla stall path, `0x4E6110`, so the fight does NOT
     advance — zero phantom frames) while `streak <= 300` frames, else RESUME (let vanilla
     proceed). 300 frames ≈ vanilla's own 5000ms disconnect timeout.
- Everything is wrapped in SEH; a torn-down backend fails safe to vanilla behavior.

Result: the freeze self-terminates the instant the real input arrives (zero desync for
any hiccup shorter than the cap), can't overflow the queue, and can't hang — after the
cap (input stream genuinely ended) it resumes vanilla, consistent with the game's own
disconnect-timeout semantics. The retired substitution thunk (`SpectatorSyncSubstituteThunk`)
and the `0x77E3AD` starvation-site hook were removed.

**Open question for testing:** whether the ~cap-length freeze is even reached at match
end depends on whether BBCF's results-screen transition is time-driven (freeze never
blocks it) or frame-driven (freeze delays it up to ~5s then resumes). Observe in-game.

## Freeze retired; back to instrumentation-only (2026-07-20)

The freeze broke match-end: at KO the input stream stops, the freeze held the sim (~5s,
to the freeze cap) instead of letting vanilla's advance-with-zero play the ending, so the
protocol's own 5000ms disconnect timeout expired first -> "connection lost" -> kicked to
lobby. Confirmed by the log timeline (last input 17:54:26, disconnect exactly ~5s later).
This is a lobby-breaking regression for rematch rooms, so the freeze was removed.

Critically, this proved the transition-state advance-with-zero is **load-bearing**:
vanilla NEEDS it to progress the spectator through round-ends and match-end when input
isn't arriving. It cannot be blanket-suppressed. And it shares the `count=1` frame path
with normal match-end, so there's no way to freeze mid-match transitions without also
breaking match-end.

Catch-up mechanism (fully mapped): `SCENE_CBattle__run_frames_online_` (0x4E55E0) each
render tick calls `0x783430` (ggpo idle / network pump — runs EVERY tick regardless of
frame count) then `FUN_007833D0` which returns how many frames to run:
`lag = [session+0x10C] - [session+0xF4]` (host frame - local frame); `lag > 60` -> return
2 (2x catch-up) with hysteresis until `lag < 10` -> return 1. This is the observed
"speeds up, returns to lag 0." It is **time-based pacing, independent of input arrival** —
so "always lag 0" would fast-forward playback, and it can't by itself fix an
input-availability desync.

Current build (deployed) is **instrumentation-only**: vanilla behavior unchanged. On each
starved frame it logs the scene-state triple, cursor vs newest-received, and whether
vanilla is about to advance-with-zero. Injection now forces raw vanilla starvation so its
real response can be observed on demand.

### Digging plan (to pin the ACTUAL dominant desync trigger)
The transition-state advance-with-zero is only a *theory* — we've caught it fire exactly
once, benignly. Candidate triggers to distinguish, with tests:
1. **Advance-with-zero phantom frames.** Test: inject starvation during an actual mid-match
   transition (round start) and see if it visibly desyncs. If yes, confirmed dominant.
2. **256-input-ring overflow.** If catch-up (2x) never outpaces a large burst, lag in the
   ring could exceed 256 -> `HOST_INPUTS_LOST` (event 13) / stale reads. Test: log ring
   fill (maxRecv - nextInput) peaks; watch for it approaching 256, and for event-13 fires.
3. **Sim nondeterminism** (RNG/config mismatch — `AA_CRandomManager` @ 0xA135C0 in the
   snapshot?). Would desync even with a perfect input stream. Test: compare a spectator's
   per-frame state checksum (Sync saved-frame checksum) against the host's for the same
   frame — a divergence with matching inputs = nondeterminism.
4. **IM 0x95 packets** reaching a vanilla (unmodded) spectator's game validation. Test:
   desync only when a modded player is in the room?
The organic log (advance-with-zero events + scene states) now accumulates automatically;
whenever a real desync next occurs, correlate its DEBUG.txt against these to identify #1
vs #2 vs #3.

## ROOT CAUSE CONFIRMED + targeted fix (2026-07-23/24)

A real organic desync was finally captured (`Bug Reports/Cuack desync/Desync DEBUG 1 -
rachel vs kokonoe.txt`, a friend's mid-session save). It **confirms the advance-with-zero
theory** the benign sessions couldn't:
- 45 `ADVANCE-WITH-ZERO-INPUT` events (benign sessions had 0-1), zero `RING NEAR FULL`,
  all `lag=-1` (stall at the live edge).
- Worst burst (20:47:49): 18 consecutive advance-with-zero at scene `(4,4,1)` with
  `nextInput` frozen at 3247 over ~0.28s — 18 phantom zero-input frames inserted while
  waiting for frame 3247. The input THEN arrived (`nextInput` jumped 3247->3280, catch-up
  drained the backlog), but the 18 phantom frames had already permanently desynced the
  deterministic re-sim.
- Advance-with-zero scene states seen: `(5,4,0) (4,5,0) (5,4,1) (4,4,1) (4,5,1)` — all
  match the gate condition `(a,b) in {(4,5),(5,3),(*,4)} && c != -1`.

So: a brief network stall coinciding with a transition scene state -> vanilla advances
the fight with fake neutral inputs -> phantom frames -> permanent desync. The stalls are
TEMPORARY (input arrives ~0.3s later), which is exactly what makes freezing the fix.

### The fix (shipped, `SpectatorSyncOnStarvationThunk` + caller hook 0x4E60F1)
Freeze the fight ONLY in the advance-with-zero states, within a bounded absorb window,
pumping DoPoll each frozen frame:
- **advance-with-zero state + within window (streak <= 90 frames)** -> return 1 -> jump to
  the vanilla stall path (0x4E6110); fight does not advance -> zero phantom frames. Pump
  `SteamSpectatorBackend::DoPoll` (RVA 0x37E090) so the awaited input arrives and the
  event queue can't overflow. Streak resets when `_next_input_to_send` advances (input
  consumed -> stall absorbed with no desync).
- **absorb window exceeded (streak > 90)** -> return 0 -> vanilla advance-with-zero. This
  is the match-end / dead-connection escape: 90 frames (~1.5s) is well under the game's
  5000ms disconnect timeout, so vanilla plays the ending and reaches victory before the
  connection drops (a 300-frame/5s cap re-broke match-end; 90 does not).
- **normal (non-transition) starvation** -> return 0 -> vanilla stalls (already correct).

Why this succeeds where earlier freezes failed: (1) only freezes the narrow desync-prone
states, not all starvation; (2) pumps DoPoll (the first freeze crashed for lack of it);
(3) short cap preserves match-end (the 300-frame freeze broke it). All three lessons
folded in, and now justified by real captured data rather than theory.

Needs in-game validation: confirm a spectated match no longer desyncs under the same
network conditions, and that match-end/rematch lobbies still work (watch for the
`FREEZE (absorb stall...)` vs `advance-with-zero LEAKED` counts in DEBUG.txt / the
DEBUG window).

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
