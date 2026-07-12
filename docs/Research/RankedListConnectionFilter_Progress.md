# Ranked List Connection Filter — Progress Log

Read this before touching `RankedListConnectionFilter.*` / `RankedListFilterWindow.*` /
ranked-list sorting. It exists so agents don't re-discover the same dead ends. Update it
every time you try something and learn whether it worked — append, don't rewrite history.

## Goal (unchanged, still the target)

BBCF's ranked search list has long-standing pain: some listed players are permanently
unreachable ("Failed to connect to room" every time), the list order is effectively
meaningless, and there's no way to sort by level/name/connection. The mod adds:

1. **Hide-unreachable filter** (`enableRankedListConnectionFilter`, default OFF) — proactively
   probes P2P reachability for every listed player before they ever render, hides
   confirmed-bad ones, no user interaction, no UI freeze (probing must happen behind the
   game's own "Searching" popup, never block the calling thread).
2. **Sorting** (`rankedListSortMode`, default 0/"Default") — fully independent of the filter;
   either can be on/off in any combination. Nine modes: Default, Best/Worst Connection,
   Closest/Furthest-from-my-Level, Highest/Lowest Level, Names A-Z/Z-A.
3. **"Ranked List Config" window** — separate from the F1 main menu. Sort dropdown at top,
   separator, hide-filter checkbox below, and (only when the filter is on) status +
   hidden-players list with per-player Restore / Restore all. Must auto-show only while the
   actual ranked search list screen is on screen, auto-hide otherwise. A single F1 checkbox
   ("Show ranked list config window") gates whether it's allowed to appear at all; closing it
   manually (X button) is equivalent to unchecking that setting.
4. Connection-based sort should ideally match the game's own in-list "delay" column (0-4
   dots), honestly labeled as an estimate if it can't be made to match exactly.

## Current file map

- `src/Network/RankedListConnectionFilter.h/.cpp` — reputation model, probe lifecycle,
  Steam CCallResult proxy/interception, sort comparator, `IsLobbyListLikelyOpen()`.
- `src/Overlay/Window/Ranked/RankedListFilterWindow.h/.cpp` — the dedicated config window;
  `UpdateAutoVisibility()` drives open/close.
- `src/Game/MatchState.cpp` — calls `UpdateAutoVisibility()` every frame; calls
  `OnMatchStarted()`/probing hooks at the right lifecycle points.
- `src/SteamApiWrapper/SteamMatchmakingWrapper.h/.cpp` — wraps `ISteamMatchmaking`, routes
  `RequestLobbyList`/`GetLobbyByIndex`/`JoinLobby`/`LeaveLobby` through the filter.
- `src/Hooks/hooks_detours.cpp` — detours `SteamAPI_RunCallbacks`, `SteamAPI_RegisterCallResult`,
  `SteamAPI_UnregisterCallResult` to intercept the game's lobby-list delivery.
- `src/Core/settings.def` — `enableRankedListConnectionFilter` (default "0"),
  `showRankedListFilterWindow` (default "1"), `rankedListSortMode` (default "0").
- Existing (pre-existing, unrelated to this feature) Ghidra research already covers the
  ranked-list row array in gory detail: `docs/Research/RankedDelay*GhidraReport.txt` (8 rounds)
  and `docs/Research/ghidra_scripts/DecompileRankedDelay*.py`. **Read `RankedDelay8GhidraReport.txt`
  before starting new RE on the row array** — see "Dead end" below, it's the same
  `DAT_00a5d270` container this doc already disproved as a live signal.

## What's confirmed working (don't re-litigate these)

- **Proactive P2P probing instead of reactive-only hiding.** Early version only hid a peer
  *after* a real connection failure, which never actually removed them from the visible list
  (fail → back to list → same guy still there → fail again). Fix: probe every listed player's
  reachability *before* building the UI, using `SendP2PPacket` on a private channel
  (`kProbeChannel=200`), and hide/compact based on the verdict. User confirmed this is the
  right shape ("go ahead and build this, accepting that trade-off" re: added latency).
- **Non-blocking delivery via CCallResult proxy.** First async attempt blocked synchronously
  inside `GetLobbyByIndex` waiting on probes → froze the UI entirely (confirmed bad by user).
  Fix: hook `SteamAPI_RegisterCallResult`/`UnregisterCallResult` to substitute a proxy
  (`LobbyListResultProxy`) for the game's own lobby-list `CCallResult` handler. The proxy holds
  the real Steam payload, we evaluate/compact/sort it in the background during
  `SteamAPI_RunCallbacks` polling, then forward a patched copy to the real game handler. This
  holds the result *behind the game's own "Searching" popup* — no thread ever blocks.
- **A `Steam_GetAPICallResult` detour does not exist in this game's `steam_api.dll`** —
  confirmed via `HookOptionalDetour` logging "Skipping missing export". Don't try to patch the
  lobby count that way again; the CCallResult proxy above is the only working interception
  point.
- **Reputation model over per-refresh snap judgments.** Treating a single probe timeout (3s)
  as "unreachable" was too aggressive — real Steam P2P dead-peer detection can take ~20s, so
  merely-slow-but-fine peers got hidden, and verdicts flapped between refreshes (user reported
  "only 2-3 reachable at a time" out of 10+ real entries). Fix, now in place: benefit-of-the-doubt
  for unresolved peers (shown, not hidden, until a probe actually reports failure), TTLs
  (`kReachableTtlMs=5min`, `kProbeUnreachableTtlMs=10min`), and reactive failures only earn a
  short hide (`kReactiveFailHideMs=2min`) unless they repeat (`kSessionBlockFailCount=2` →
  session-long block).
- **`GetOtherRoomMemberEntriesInCurrentMatch()` over `GetIMPlayersInCurrentRoom()`** for
  detecting "did the pending join actually succeed" — the IM-only variant misses vanilla-client
  opponents (confirmed live with user "WaddleDDD", a non-modded player).
- **The row array at `container(RVA 0x65D270)+0x1510`, stride `0x68` (function
  `FUN_004AAAD0`) is a dead end for both the delay-column value and list-visibility.**
  Confirmed three separate ways across this session and the pre-existing `RankedDelay*`
  Ghidra reports: address math is correct byte-for-byte (`DAT_00a5d270` is a direct static,
  not a dereferenced pointer), the lazy-init guard (`DAT_00A624E0` bit 0) is confirmed set
  (container genuinely constructed), yet every row read back as all-zero bytes
  (`populatedRows=0`, `firstNonZeroRow=-1`) even while a real, visibly-populated 32-lobby list
  was on screen. **Do not re-investigate this exact array/function** unless new evidence
  specifically contradicts the zero-bytes finding — instead look for a *different* function
  that actually writes the on-screen delay dots (search for what touches the list's UI draw
  call / widget objects, not this data container).

## RESOLVED: window-visibility screen discriminator (2026-07-11, later same day)

**Found it — no new RE needed, it was already in the codebase.**
`src/Hooks/RankedAutomationHarness.cpp` (the ranked-menu autopilot, built and validated in an
earlier unrelated task) already reverse-engineered the precise per-value meaning of `state1`
while `state==4`:

- `IsRankedSearchEntryMenuState`: `state1==30` — the pre-search "press search" screen, **no
  results on screen**. This is exactly the screen you land on after backing out of results.
- `IsRankedSearchResultsState`: `state1 in {36,38,39}` — **results are actually rendered**.
- `IsRankedPostSearchBackState`: `state1==46`.

The bug in both prior attempts (A and B below) was gating on the whole non-confirmation
`state==4` range, which includes `state1==30` — so backing out of results to the "press
search" screen never closed the window, matching the user's report ("shows when I get into
the list but doesn't hide when I back out until I am out of any ranked mode menu").

**Fix applied:** `IsLobbyListLikelyOpen()` in `RankedListConnectionFilter.cpp` now gates on
`state1 == 36 || state1 == 38 || state1 == 39` (`isSearchResultsState`) instead of "not a
confirmation value". The `inFunctionalRoomWithOpponent` exclusion (Attempt B, below) is kept
alongside it, since character-select still shares `state1==39` with genuine results per prior
live testing — that part of the discriminator problem (results vs. character-select, both
`state1==39`) is NOT solved by this alone; the opponent-in-room check is still the mechanism
that ends it. **Not yet re-tested live as of this writing** — verify the 46-second stuck-open
repro doesn't recur, and separately re-verify character-select still closes it correctly.

Also removed `kRankedNetworkState1ConfirmationMin/Max` (43/48) — unused after this change.

### 1. Window visibility — history (superseded by the fix above, kept for context)

Two attempts, in order:

- **Attempt A (dead):** `onList = gstate==27 && state==4 && state1 not in [43,48] && recentListActivity`.
  Bug: this band (`state1` 0-42) covers the raw list *and* ranked character select *and*
  apparently other ranked submenus — window stayed open into character select.
- **Attempt B (current, still wrong):** added `!IsRoomFunctional()` as a hard exclusion,
  reasoning that a real room only becomes functional once a specific opponent is picked.
  This introduced a **new false-close**: `RoomManager::IsRoomFunctional()` (a raw read of the
  game's live `Room` struct `roomStatus == RoomStatus_Functional`) goes true from background
  RoomOne/RoomTwo housekeeping (`hooks_bbcf.cpp` `GetRoomOne`/`GetRoomTwo`, which fire on the
  game's own internal matchmaking prep) **even while merely sitting on the list with zero user
  input** — live-confirmed: `roomFunctional` flipped `0→1` at 14:29:32.172 with `state1` stuck
  at 39 (never reached the 43-48 confirmation band), window self-closed 8s into a fresh list.
  **Fix applied this session:** require `GetOtherRoomMemberEntriesInCurrentMatch()` to be
  non-empty in addition to `IsRoomFunctional()` (`inFunctionalRoomWithOpponent`), on the theory
  that a phantom housekeeping room has no other member yet. This build was tested next and
  **still shows the original Attempt-A-shaped bug**: log from 15:15:14 to 15:16:00 (test session
  in `DEBUG.txt`, thread T35028) shows `onList=1` continuously for ~46 seconds while `state1`
  cycles through 30/36/38/39/42 — i.e. the user backed out of the list to some other ranked
  screen (mode select? results of a previous search? unclear which) and the window did **not**
  close, only closing once `gstate` finally left the ranked flow entirely (`state1=0, state=1`
  at 15:16:00.280). So `state1`'s "list" band genuinely is shared by more than one visible
  screen and gstate/state/state1 alone cannot disambiguate them — **this is not a bug in the
  exclusion logic, it's a missing signal.**

**What's needed:** real RE work to find a variable that is true/set *specifically* while the
literal ranked-search-list UI (the scrollable list of lobbies with delay dots) is the active
screen, and false the instant it's replaced by any other screen (mode-select, previous-search
results, character select, etc.) — even though the underlying `state`/`state1` machine doesn't
change. Candidates to search for: a UI widget/panel "active" flag, a screen/menu ID distinct
from the ranked network state struct, or the same struct at a *different offset* not yet
explored (only `+0x00` and `+0x04` have been read; the struct is likely larger — see
`RankedProgressWindow.cpp` around line 3200-3250 and 4620-4650 for the existing offset reads,
and consider dumping a wider byte range around `kRankedNetworkStructRva=0x008F7958` while
manually walking list → back-out → list to see what actually flips).

### 2. Connection sort "still doesn't make sense at all"

Root cause found this session: `SortShownCandidates()` runs once, synchronously, at delivery
time — but delivery was gated on a fixed 2s hold (`kHoldDeadlineMs`), while live probe
establishment times observed in `DEBUG.txt` ranged ~1.6s up to ~8.5s. At the 2s mark only 1 of
16 candidates had a resolved `probeElapsedMs`; the other 15 had `keyKnown=false` and sank to
the bottom in original (arbitrary) order — the sort looked essentially random.

**Fix applied this session (untested against real gameplay yet):** added
`kConnectionSortHoldDeadlineMs=6000` and use it instead of `kHoldDeadlineMs` when
`rankedListSortMode` is `BestConnection`/`WorstConnection`, so more probes land before the
order is locked in. **This is very likely still insufficient** — 6s won't cover every peer
(some measured 8.5s+), and more importantly:

**User's explicit new direction (2026-07-11), which supersedes the hold-deadline patch above:**
a one-shot sort at delivery time can never be "perfect" because (a) probes keep completing
after delivery, (b) the user has observed the game's own delay/connection indicator visibly
*change* for an already-listed entry after the fact (implying the real in-game connection
quality signal is live/polled, not a one-time value). The ask is: **re-sort the already-on-screen
list in real time** as each probe resolves or re-fires, mirroring however the game's own delay
column updates live.

**Confirmed from `DEBUG.txt` (2026-07-11 15:14-15:16 session):** the game DOES auto-refresh the
list on its own every ~8-16 seconds while sitting on the results screen (repeated
`proxied lobby-list call result` → `lobby list held` → `delivering` cycles at 15:14:45,
15:15:01, 15:15:15, 15:15:24, 15:15:50, 15:16:04 - irregular but consistently a handful of
seconds apart). Since verdicts are cached (`kReachableTtlMs`/`kProbeUnreachableTtlMs`) and
`SortShownCandidates` re-runs on every delivery, **the sort already re-settles on every
natural auto-refresh** using whatever `probeElapsedMs` values have matured by then - this is
real, already-working, incremental reordering, just gated by the game's own refresh cadence
(seconds), not true frame-by-frame. Combined with the `kConnectionSortHoldDeadlineMs=6000`
fix above (first delivery no longer locks in after only 1/16 probes resolved), this should
measurably improve what the user sees within a couple of refreshes - **but is still not
"real time" in the sense the user wants** (reorder the instant a probe lands, without waiting
for the game's own refresh timer).

**RE agent findings (2026-07-11, background investigation) on the genuine live delay source —
inconclusive, needs a follow-up pass:**
  - The dead row container (`DAT_00a5d270`/`FUN_004AAAD0`) has an RTTI-recovered class name per
    Ghidra decompile: `CNetworkLobbyData`. A flat global-symbol keyword search for
    "Lobby"/"CNetwork"/etc. found nothing (`docs/Research/NamedSymbolsGhidraReport.txt`) - the
    vtable name is namespace/demangled and wasn't caught by a flat scan. **Next step: a
    namespace-aware Ghidra symbol/datatype search to resolve `CNetworkLobbyData`'s vtable and
    enumerate its virtual methods** - those methods are a far stronger lead than guessing more
    byte offsets by hand.
  - Possible the *specific* container instance/offset already tried is simply the wrong member
    of `CNetworkLobbyData` (reads zero) rather than proof the whole class is dead - worth
    checking other members once the vtable is mapped.
  - A different, already-partially-RE'd subsystem exists for in-*match* connection quality:
    `src/Overlay/Window/NetworkSquareColorWindow.cpp` reads a live `netcolor` byte
    (`netUserData+0x194`) per room member. This drives the colored square shown during an
    active match, not the pre-match list - likely a different underlying mechanism, but worth
    a quick check of whether the same `netcolor`-style live field exists for lobby-list entries
    too (i.e. is there a lobby-side equivalent of `netUserData`, polled the same way).
  - Full agent report/transcript reference: this doc is the durable record - the agent's raw
    transcript is not preserved, so treat the bullets above as the complete transferable
    findings, not a pointer to more detail elsewhere.
  - New Ghidra scripts from this pass (kept for reuse):
    `ghidra_scripts/DecompileRankedListScreenDiscriminator.py`,
    `DecompileRankedListScreenDiscriminator2.py`, `SearchNamedSymbols.py`, and their
    `RankedListScreenDiscriminator*GhidraReport.txt` / `NamedSymbolsGhidraReport.txt` outputs.
    (The "ScreenDiscriminator" scripts were investigating window-visibility Q1, which turned
    out to be solved a different way - see the RESOLVED section above - but they're still
    useful Ghidra-script templates.)

## Namespace-aware Ghidra pass on `CNetworkLobbyData` vtable (2026-07-11, follow-up)

**Root cause of the earlier zero-hit keyword search, found and fixed:** the prior
`SearchNamedSymbols.py` called `sym.getName()`, which for a namespace-scoped C++ symbol
(Ghidra's demangler puts recovered vtables/RTTI/methods under a `GhidraClass` namespace) only
returns the *local* child name (`"vftable"`), never the qualified path
(`"CNetworkLobbyData::vftable"`) - that's why a keyword scan for `"CNetwork"`/`"Lobby"` found
**zero** matches even though the decompiler's own pretty-printer clearly showed
`CNetworkLobbyData::vftable` in `RankedDelay5GhidraReport.txt`. Fix: use `sym.getName(True)`
(fully-qualified). New script `ghidra_scripts/DecompileNetworkLobbyDataVtable.py` redid the
same keyword scan this way and immediately surfaced **165 matches for "Lobby" alone**,
including a real resolvable vtable address:

- `CNetworkLobbyData::vftable` at Ghidra addr `0x0089c7cc` (preceded by the RTTI Complete
  Object Locator pointer at `0x0089c7c8`, standard MSVC layout).
- This is exactly the vtable pointer written into the confirmed-dead singleton
  (`DAT_00a5d270`/RVA `0x65D270`) via `FUN_00848550`: `_DAT_00a5d270 = CNetworkLobbyData::vftable;`.

**`CNetworkLobbyData::vftable` slot dump (36 slots, most are `__purecall` stubs - i.e. this is
an abstract base/interface class):**

| slot | addr | role |
|---|---|---|
| 0 | `FUN_0046a1a0` | scalar-deleting destructor |
| 1 | `FUN_004a2a50` | aggregator: loops `idx=0..9`, calls vtable-slot2 `(this,a,b,idx)`, sums results |
| 2 | `FUN_004a2970` | row accessor: clamps `(a,b)` into a row index `<10`, validity check at `this+8+row*0xa8`, returns DWORD at `this+0x18+row*0xa8+col*0x10` (`col` clamped `<4`) |
| 6 | `FUN_0046a9a0` | returns constant `0x20` (32) |
| 7 | `FUN_004a2a00` | returns constant `10` |
| 10 | `FUN_0046be80` | returns constant `100` |
| 21 | `FUN_006a9da0` | returns constant `0` |
| 3,4,5,8,9,11-20,22-35 | `__purecall` | pure virtual, not implemented on the base |

**Sibling concrete class found: `CSTEAMNetworkLobbyData::vftable` at `0x0089c88c`.** It shares
slots 1, 2, 6, 7, 10 verbatim with the base (confirmed via `NetworkLobbyDataCallersGhidraReport.txt`
showing those five functions are referenced as DATA from *both* vtables), and **overrides every
slot that was `__purecall` in the base** with a real implementation
(`SteamNetworkLobbyDataVtableGhidraReport.txt`, all 35 slots decompiled). This is a real,
concrete-implementation class - a small connection-handshake state machine, gated throughout on
two fields: `this+0x30a0` (an int phase enum, `4` = active/connected-ish) and `this+0x3112`
(a byte sub-phase), with a bitmask at `this+0x30b0` tracking which handshake steps completed.
The base's own row-accessor at `this+8`/stride `0xa8`/max-10-rows is inherited unchanged - so
even in the concrete class, that specific array is capped at 10 entries, not 32; unlikely to be
the 32-row on-screen list itself (more likely a small per-region or per-attempt sub-table).

**The big find - slot 32 (`FUN_0046b9c0`) is a live ping/RTT-style WRITE path, into a
completely different, never-before-examined singleton:**

```c
undefined4 __thiscall FUN_0046b9c0(int param_1,int param_2,undefined4 *param_3)
{
  ...
  if (*(int *)(param_1 + 0x30a0) == 4) {                       // only while phase==4 (active)
    uVar4 = __Xtime_get_ticks();
    uVar4 = __allmul((ticks_now - *(uint*)(param_2+8)), ...) ;  // elapsed ticks since a
    uVar4 = __alldiv(uVar4, 1000000, 0);                        // timestamp in param_2, -> ms
    iVar2 = FUN_0041c900();                                     // <-- DIFFERENT container
    iVar3 = *(int *)(param_2 + 4) * 0x68;                       // index * stride 0x68
    iVar1 = *(int *)(iVar3 + 0x129c + iVar2);                   // previous value (sentinel -1?)
    *(int *)(iVar3 + 0x129c + iVar2) = (int)uVar4;              // WRITE elapsed-ms sample
    if (iVar1 == -1) { /* log new-sample event, ids 0x2b0010 */ }
  }
}
```

`FUN_0041c900()` is its own lazy-init singleton getter (guard `DAT_00a291d8` bit 0, `atexit`
teardown `FUN_008480b0`) returning `&DAT_00a25788` - **RVA `0x625788`**, an object whose vtable
pointer is set to `AASTEAM_CNetworker::vftable` (`FUN_008480b0`'s teardown resets it to
`AA_CNetworker::vftable` on exit) - i.e. this is the low-level networker object, not
`CNetworkLobbyData` at all. **This is a genuinely different static object from the
confirmed-dead `DAT_00a5d270`/RVA `0x65D270`** - different base address entirely. The write
target is:

```
container = base + 0x625788        // AASTEAM_CNetworker singleton (RVA 0x625788)
array     = container + 0x129c     // stride 0x68 (104 bytes), same stride as the old dead
                                    // array but a totally different base+offset - not
                                    // previously tried
value     = elapsed ms since a timestamp carried in the callback's param_2 struct
index     = *(param_2 + 4)         // row/peer/slot index, range not yet confirmed
gate      = *(this + 0x30a0) == 4  // only fires while the owning lobby-data object is
                                    // "active" (matches the user's live-reorder observation:
                                    // this is not a one-shot value, it's written continuously)
```

This shape (elapsed-time-since-timestamp, gated on active-session state, written per index,
sentinel `-1` for "never sampled yet") is exactly what a ping/RTT sample cache looks like, and
critically it is **written repeatedly, not once** - the missing "why does the game's own delay
dot visibly change for an already-listed row" mechanism the user described. Only one call site
was found for slot 32 (`0089c90c`, a `DATA` xref from the vtable itself - it's only ever invoked
via virtual dispatch, so no direct-call cross-reference exists for Ghidra to show argument
values at the call site).

**Confidence: medium, not yet confirmed.** Strong structural evidence this is a live per-index
connection-quality write path; not yet proven to be the specific value that drives the on-screen
0-4 delay dots, and the index's real range/meaning (peer slot 0-9? row 0-31? team member 0-3?)
is unconfirmed - no read-side/UI accessor was found for this exact array in this pass.

**Concrete next steps:**
1. **Live-read RVA `0x625788 + 0x129c` onward** (stride `0x68`, try ~40-64 slots to be safe)
   while sitting on a populated ranked search list, and watch for values that change over time
   and correlate with a specific on-screen row's delay dots changing - this is the fastest way
   to either confirm or kill this candidate, since (unlike the old dead end) this container is
   guarded by an *active* IsBadReadPtr + lazy-init check (`DAT_00a291d8` bit 0) that should
   already be true once the list screen is up.
2. If confirmed live/non-zero: dump `AASTEAM_CNetworker::vftable` itself (starts at
   `DAT_00a25788+0`, i.e. `this+0` since the vtable ptr *is* the object's first field) the same
   namespace-aware way as this pass, to find a paired GETTER for the same `+0x129c` array (units
   are almost certainly raw milliseconds here, not a 0-4 dot count - the UI must bucket it).
3. Finding the actual caller of slot 32 (to learn what `index` really enumerates) needs either
   a live CDB breakpoint at `0x0046b9c0` while a real search list with multiple entries is on
   screen (dump `param_2` and `this` on hit), or a call-graph widen via Ghidra's reference
   manager on the *vtable slot offset* patten (`(**(code**)(*obj+0x80))(...)`) across the whole
   binary rather than a direct-address xref (this pass didn't attempt that; it's a bigger,
   slower scan).
4. New Ghidra scripts from this pass (kept for reuse), all in `docs/Research/ghidra_scripts/`:
   `DecompileNetworkLobbyDataVtable.py` (namespace-aware keyword search + vtable slot dumper -
   reusable template for any future "found a class name in decompile, need its real address"
   investigation), `DecompileNetworkLobbyDataCallers.py`, `DecompileNetworkLobbyDataThunkCallers.py`,
   `DecompileSteamNetworkLobbyDataVtable.py`, `DecompileLobbyPingWritePath.py`,
   `DecompileLobbyPingContainerXrefs.py`. Matching report files:
   `NetworkLobbyDataVtableGhidraReport.txt`, `NetworkLobbyDataCallersGhidraReport.txt`,
   `NetworkLobbyDataThunkCallersGhidraReport.txt`, `SteamNetworkLobbyDataVtableGhidraReport.txt`,
   `LobbyPingWritePathGhidraReport.txt`, `LobbyPingContainerXrefsGhidraReport.txt`.

## Live diagnostic added for the `AASTEAM_CNetworker` candidate (2026-07-11, same day)

Per next-step #1 above, added `RankedListConnectionFilter::DiagnosticDumpNetworkerPingArray()`
(declared in the header, implemented right after `CountPopulatedGameRows()` in the .cpp) and
wired it into `IsLobbyListLikelyOpen()`'s existing diagnostic block, throttled to once/second.
It reads `base + kNetworkerSingletonRva(0x625788) + kNetworkerPingArrayOffset(0x129C)`, stride
`kNetworkerPingStride(0x68)`, for `kNetworkerPingSlots(64)` slots, guarded by
`kNetworkerInitGuardRva(0x6291D8)` bit 0 (the `AASTEAM_CNetworker` lazy-init flag). It logs one
line per second: `[RankedListFilter] networkerPingDiag: slotsRead=.. nonSentinelCount=..
changedSinceLastLog=.. values: [idx]=value [idx]=value ...` (only non -1 slots are listed to
keep it readable). **Diagnostic only - does not feed any real decision, purely for correlation.**
Purely additive, does not affect the working visibility/sort code. Compiles clean
(Debug|Win32, verified).

## RESULT: `AASTEAM_CNetworker` candidate confirmed DEAD (2026-07-11, later same day)

Live `DEBUG.txt` from a real ranked-list session (17 lobbies on screen, multiple auto-refreshes
over ~2 minutes) shows `networkerPingDiag` logged 128 times across the whole session:
**every single sample was `nonSentinelCount=64 changedSinceLastLog=0`, and every one of the 64
slots read exactly `0`** - not the `-1` sentinel the write path checks for, just flat `0`,
unchanging, the entire time. This fails criterion (a) from the "next step" above outright - the
array is either never constructed with real data in this code path, or this specific instance
is the wrong one. **Removed** `DiagnosticDumpNetworkerPingArray()` (header + .cpp), its call site
in `IsLobbyListLikelyOpen()`, and the `kNetworkerSingletonRva`/`kNetworkerInitGuardRva`/
`kNetworkerPingArrayOffset`/`kNetworkerPingStride`/`kNetworkerPingSlots` constants - build
verified clean after removal. **Do not re-add this without new evidence** (e.g. a live CDB
breakpoint at `FUN_0046b9c0`/`0x0046b9c0` proving the write actually fires with a different
`this` than assumed, per next-step #3 of the previous section, which was never attempted).

Both RE-discovered candidates for the game's own live delay/connection data (`DAT_00a5d270` row
array and now `AASTEAM_CNetworker`'s ping array) are dead ends. **Recommendation: stop chasing a
third candidate via more blind Ghidra vtable spelunking** - the earlier constant-write-path lead
was reasonable, so a similar-shaped find is not guaranteed to fare better. If someone wants to
try again, the highest-value next move for genuine RE progress is a **live CDB breakpoint on
`FUN_0046b9c0` (`0x0046b9c0`)** while a real search list with reachable and unreachable entries
is on screen, to dump `this`/`param_2` on an actual hit and learn what object/index it's really
touching - static analysis alone has now dead-ended twice.

## Sort quality: root-cause split into two separate problems (2026-07-11, later same day)

User report: "the connection sorting is still not correct... it's not properly sorted", after
the window-visibility fix was confirmed working. Checked a fresh `DEBUG.txt` (verified deployed
`dinput8.dll` md5sum matched the freshly built one first).

**Confirmed from the log:** on the *first* delivery of a session (17 lobbies), only 4 of 17
candidates had a resolved `probeElapsedMs` by the 6s connection-sort hold deadline - the
remaining 13 took 6.6s to 19.2s to resolve, well past the hold. Those 13 correctly sink to the
bottom in original order per the comparator's `keyKnown` priority (see
`SortShownCandidates`) - **but "13 of 17 entries in arbitrary order" reads as "not sorted at
all" to the user**, and that's an accurate perception, not a misunderstanding on their part.

**Second, more fundamental problem, newly identified this session:** even once every peer
*does* resolve (later "all resolved" auto-refreshes in the same log, at 16:00:40, 16:00:56,
16:01:51), the metric itself - `probeElapsedMs`, i.e. how long the P2P **handshake** took to
establish - is not actually equivalent to connection quality/ping. Handshake establishment time
is dominated by NAT-traversal/relay-negotiation overhead and probe-scheduling order (we fire
probes to potentially a dozen-plus peers nearly simultaneously, so completion order reflects
contention/negotiation luck as much as anything about the peer's real link quality). A peer who
takes 15s to punch through NAT can easily have excellent in-match latency once connected, and
vice versa. **This means even a "perfectly sorted by probeElapsedMs" list can look wrong to the
user, because the metric doesn't measure what they think it measures.** This is very likely a
second, independent reason the sort "doesn't make sense," on top of the resolution-timing issue.

**Fix applied this session:** added a diagnostic log line after every sort in
`SortShownCandidates()`: `[RankedListFilter] sort order (mode=N): #0 owner=.. known=.. key=..
#1 ...` (or `text=".."` for name modes). This lets a future session directly verify whether the
comparator itself is behaving correctly (ascending/descending, keyKnown priority) independent of
whether the underlying data is any good - separates "is the code right" from "is the metric
good enough" for good. Build verified clean.

**No fix attempted yet for the metric-quality problem itself** - there is no known way to get a
real ping/RTT proxy from the classic `ISteamNetworking` P2P API used here (`GetP2PSessionState`
has no latency field; that only exists on the newer `SteamNetworkingSockets`
`GetConnectionRealTimeStatus`, which this game may not even use, unconfirmed). Concrete next
steps, in order of effort:
1. **Live-test first**, using the new `sort order` log lines, on both an early ("hold deadline,
   unknowns shown") and a later ("all resolved") delivery in the same session. If the "all
   resolved" order still looks wrong to the user *and* the log shows a clean ascending/descending
   `key=` sequence, that conclusively proves the metric-quality problem (not a code bug) and
   further work should focus on finding a better metric, not tuning the sort code.
2. If a better metric is wanted: check whether this game's `steam_api.dll` exports any
   `SteamNetworkingSockets`/`SteamNetworkingMessages` functions at all (grep the exports the way
   `HookOptionalDetour` already does for other optional exports) before investing in it - if
   absent, this path is closed the same way `Steam_GetAPICallResult` was.
3. Alternatively, honestly reframe the feature: keep `probeElapsedMs` as a rough "how fast did we
   manage to connect" indicator but relabel the UI help text to say so explicitly (avoid
   implying it approximates the game's in-list delay dots), and/or drop "worst→best connection"
   sort precision as a goal in favor of just keeping it as a coarse tiebreaker.
4. Do NOT keep extending `kConnectionSortHoldDeadlineMs` as the primary fix - even a hold long
   enough to resolve every peer (some took >19s in one session) only fixes problem #1
   (resolution timing), not problem #2 (metric quality), and a 19s+ wait before the list ever
   appears is a bad trade for partial benefit.

## Suggested next steps (in order)

1. **Live-test the window-visibility fix** (see RESOLVED section above) - confirm the
   46-second stuck-open repro no longer happens, and confirm character-select still closes
   the window via the `inFunctionalRoomWithOpponent` path.
2. **DONE (2026-07-11 follow-up):** namespace-aware Ghidra pass resolved `CNetworkLobbyData`'s
   vtable and its concrete sibling `CSTEAMNetworkLobbyData` - see the section above. Produced a
   strong new candidate: a live per-index ping/RTT write into `AASTEAM_CNetworker` singleton
   (RVA `0x625788`) at `+0x129c`, stride `0x68`. **Not yet live-verified.** Next action: read
   that RVA range in-game (IsBadReadPtr-guarded) while a populated ranked list is on screen and
   check for nonzero, changing values correlated with a specific row's delay dots. If confirmed,
   that's the "re-polled per row" signal the sort should piggyback on instead of needing an
   invasive redraw hook.
3. Once the live source is found, decide the resort mechanism: cheapest is updating
   `m_reachableLobbies`' order in `PollProbes()` so the *next* natural auto-refresh (confirmed
   happening every ~8-16s, see above) picks up the new order for free; if the user needs
   sub-refresh-interval reordering, a more invasive live-redraw hook is needed instead - don't
   build that without first confirming the cheaper option is insufficient.

## Sort comparator confirmed CORRECT via live test (2026-07-11, later same day)

Live-tested the `sort order (mode=N): #idx owner=.. known=.. key=..` diagnostic added last
session, against a fresh `DEBUG.txt` (deployed `dinput8.dll` md5sum verified matching the
freshly built one first). At every "all resolved" delivery (i.e. once every candidate's probe
had actually resolved), the logged key sequence was **strictly ascending** end to end - e.g.
`1375, 1578, 1797, 2125, 3078, 4937, 4985, 5282, 6094, 6547, 6610, 7625, 9828, 10172, 10953,
11609, 14500`. **The comparator/sort code itself is correct, full stop.** The user's "not
properly sorted" complaint is entirely a data-quality problem (see prior session's write-up:
`probeElapsedMs` measures P2P handshake establishment time, not real connection quality/ping),
not a code bug. Do not re-investigate the comparator logic in `SortShownCandidates` again
without new contradicting evidence.

## User's explicit new direction (2026-07-11, later same day): use the game's own tag, live-reorder without waiting for refresh

Two concrete asks, both now acted on:

1. **"The metric should be what is internally used to tag the connections as 4 to 0 ... this
   was already reverse engineered before."** A background RE agent is running to trace this -
   see the next section for a promising lead already found this session
   (`RANK_RTT_FILTER`/`net_col_A..G.hip`/`HOST_NETCOLOR` strings from the pre-existing
   `RankedDelayGhidraReport.txt`, and the already-implemented `netcolor` byte read in
   `src/Overlay/Window/NetworkSquareColorWindow.cpp`). **Not resolved yet as of this writing** -
   whoever picks this up next should check whether that agent's findings landed in a section
   below this one (it was instructed to append its own findings here) before starting fresh.

2. **"In the perfect system, we live-update the order of the UI so that without a refresh we
   see it reorder itself as the probes finish."** Implemented this session, independent of the
   metric-quality question above (works with whatever metric ends up being used):
   - `BuildCompactedListAndDeliver()` gained a `forceDeliver` parameter (default `true`, so all
     existing call sites are unaffected). When `false`, it still recomputes the full
     shown/hidden/sorted-order state, but only actually re-invokes the game's list handler if
     the recomputed `m_reachableLobbies` differs from what was last delivered - otherwise it's a
     silent no-op (logged as `"(unchanged, skipping re-delivery)"`).
   - Added `m_lastGameLobbyListHandler`, a copy of the game's real `CCallResult` handler pointer
     that survives past the point `m_gameLobbyListHandler` gets nulled out after each delivery
     (that member still tracks "is a delivery in flight right now", separate from "what handler
     do we call when we want to push a live update").
   - Added `TryLiveResort()`, called from `OnSteamCallbacksPump()` whenever the pipeline is
     `Idle` (i.e. a list has already been delivered and nothing new is currently being held).
     It's a no-op if nothing's been delivered yet, both features are off
     (`IsPipelineActive()`), or there's no handler to redeliver to. Throttled to once per 750ms
     (`kLiveResortIntervalMs`) since re-invoking the game's own list-rebuild handler isn't
     something to do every single pump, even though the recompute itself is cheap.
   - Net effect: as `PollProbes()` resolves verdicts in the background (already running every
     pump regardless of pipeline state), `TryLiveResort()` picks up the change within ~750ms and
     pushes an updated order to the game **without waiting for the game's own next auto-refresh
     (which the earlier session's log showed happens every ~8-16s)**. This also means the
     hide-filter benefits from the same mechanism for free - a peer proven unreachable while
     already listed will drop out of the visible list within ~750ms too, not just on next
     refresh.
   - Build verified clean (Debug|Win32). **Not yet live-tested** - needs verification that (a)
     the game's list UI actually accepts/renders a re-`Run()` of the same handler without
     visual glitches or duplicate-call confusion, and (b) rows visibly reorder in place while
     sitting on the list, matching the user's ask.

## `AASTEAM_CNetworker` candidate: dead (already noted above, cross-reference)

See the "RESULT: `AASTEAM_CNetworker` candidate confirmed DEAD" section above - fully removed
from the codebase this session (diagnostic function, call site, and constants), build verified
clean after removal.

## RE follow-up on the `netcolor` lead (2026-07-11, background agent, this session)

Picked up the exact lead flagged in the "User's explicit new direction" section above: trace
where the already-implemented `netcolor` byte (`src/Overlay/Window/NetworkSquareColorWindow.cpp`,
read at `moduleBase + kNetworkUserDataRva(0x008AD0C0) + 0x194`, and also
`RoomMemberEntry::netcolor` at `+0x5A`) actually gets **written** by the game, to see if it's the
same 0-4-ish tag the user wants for the ranked list. New Ghidra scripts (all under
`docs/Research/ghidra_scripts/`, matching `*GhidraReport.txt` files in `docs/Research/`):
`DecompileNetColorWrites.py`, `DecompileNetColorGetterCallers.py`, `DecompileRoomNetColorWrites.py`,
`DecompileNetColorComputeFn.py`, `DecompileNetColorSourceChain.py`.

**Structural finding: `Room` and the static `NetworkUserData` struct are the same singleton
object.** `hooks_bbcf.cpp`'s `GetRoomTwo` hook computes `pRoom = edi + 0x22D10`, and `edi` there
is confirmed (via decompile of `FUN_0049ed30`/`FUN_0070d730`/etc., all of which call the same
lazy-init getter `FUN_004a0fe0` that returns `&DAT_00cad0c0` = `moduleBase + kNetworkUserDataRva`)
to be that exact static object. So `Room` (and its `RoomMemberEntry member1..8` array, per
`src/Game/Room/Room.h`) is not a separate allocation - it's embedded at
`NetworkUserData + 0x22D10`, with `member1` therefore at `NetworkUserData + 0x22D10 + 0x48 =
NetworkUserData + 0x22D58`.

**Found the write site for the local player's own `netcolor`, and it's gated, not raw:**

```c
// FUN_004a1110 (fastcall, param_1 = NetworkUserData + 0xD0, i.e. FUN_0049d5c0()'s return value)
undefined1 __fastcall FUN_004a1110(int param_1)
{
  if (*(byte *)(param_1 + 0xc6) < 0x1e) {   // sample-count gate: needs >= 30 samples
    return 0;                                // otherwise always reports tier 0 ("White"/unknown)
  }
  return *(undefined1 *)(param_1 + 0xc4);    // the actual stored tier byte
}
```

Since `param_1 = NetworkUserData + 0xD0` (from `FUN_0049d5c0`), `param_1 + 0xc4` resolves to
`NetworkUserData + 0x194` - **exactly** `kNetUserDataNetColorOffset` already used in
`NetworkSquareColorWindow.cpp`. So the mod's existing static read is reading the right field, but
what it displays as "local color" already has this 30-sample warm-up gate baked in (before 30
samples land, the game itself shows tier 0 regardless of the true value; the mod's raw read of
`+0x194` doesn't currently apply this gate, so it can show a stale/premature value the game itself
wouldn't yet display). `param_1 + 0xc6` = `NetworkUserData + 0x196` is the sample-count byte - one
byte further than the mod's guessed "counter" field at `+0x195`
(`kNetUserDataNetColorCounterOffset`), so that constant is likely off by one and should be
double-checked/fixed if the gate is ever wired in.

**`FUN_004a1110`'s result is then copied into the local `RoomMemberEntry.netcolor` (confirms the
two mechanisms - static self-color and per-room-member color - are the same value, not two
independent systems):**

```c
// FUN_0049f990 (only runs while both a Room exists (`+0x22d10 != 0`) and the local room-member
// pointer (`+0x23218`, room-member-entry-in-current-room) is non-null - i.e. this is an
// in-room/in-match-only code path, never runs while merely browsing the search list)
iVar6 = *(int *)(iVar5 + 0x23218);      // local RoomMemberEntry pointer
uVar1 = FUN_004a1110();                  // gated tier value described above
*(undefined1 *)(iVar6 + 0x5a) = uVar1;   // RoomMemberEntry.netcolor = gated tier
```

**Formula for turning a raw ping/RTT sample into the 0-8 tier byte at `NetworkUserData+0x194`
itself was NOT found this pass.** Static xref search for direct writes to that fixed address found
nothing (the field is only ever reached through register-relative arithmetic off the
`FUN_0049d5c0()`/`FUN_004a0fe0()` base, never a literal absolute-address memory operand, so
Ghidra's `getReferencesTo()` can't resolve it). A broader grep for `+ 0xc4)` / `+ 0xc6)` writes
across ~100 decompiled callers of `FUN_0049d5c0` came back too noisy to use - offset `0xc4` is a
generic small struct offset reused by many unrelated functions/structs in this binary, so most
hits are unrelated state-machine resets (`= 0`), not netcolor writes. **This remains open** - the
concrete next step is a live CDB breakpoint on `FUN_004a1110` (`0x004a1110`) or on writes to
`NetworkUserData+0x194` directly (data breakpoint, since static analysis can't disambiguate the
address-taken form) while sitting in an actual match with a known-good or known-bad connection, to
capture what raw value feeds the tier and where the 0-8 boundaries actually fall.

**Important scope caveat - this mechanism is probably the wrong subsystem for the user's actual
ask.** `FUN_0049f990` (the only confirmed write site into a `RoomMemberEntry.netcolor`) explicitly
early-returns unless a `Room` already exists (`NetworkUserData+0x22d10 != 0`) **and** the local
player already has a room-member-entry pointer populated (`NetworkUserData+0x23218 != 0`) - both
of which only become true **after joining an actual room**, i.e. this is confirmed to be an
in-room/in-match connection-quality mechanic (matches what `NetworkSquareColorWindow.cpp` already
uses it for: the colored square shown once you're in a match). The **ranked search list** (the
screen with the 0-4 delay dots per unconnected lobby row, which is what the user is actually asking
about) shows quality for lobbies **nobody has joined yet** - there is no `RoomMemberEntry` for those
rows, so this exact write path cannot be what populates the list's dots. It may still be the *right
family* of value (same underlying ping-sample/tier-bucketing logic, reused for a different,
not-yet-found per-lobby-row array), but it is very unlikely to be literally the same memory this
session traced. The already-confirmed-dead `DAT_00a5d270`/`CNetworkLobbyData` row array (see
"Dead end" section near the top of this doc) is a **different, separate object** from the
`NetworkUserData` singleton explored this session (`0x00A5D270` vs `0x00CAD0C0` - not the same
struct, ruling out "the list array was secretly netUserData all along").

**Recommendation for whoever continues this:** don't wire `NetworkUserData+0x194`/`+0x196` into
the ranked-list sort as-is - it answers "what tier is my own connection right now, in a room I've
already joined," not "what tier will lobby row N show before I've joined it." If the user's
"already reverse engineered before" memory refers to something more specific than the
`RankedDelay*GhidraReport.txt` string hits this doc already investigated, ask them for any
surviving screenshot/description of exactly where they recall seeing that RE result - static
analysis of this ~1.5MB binary has now spent three sessions searching for a pre-join per-row
0-4/0-8 array and found only dead ends (`DAT_00a5d270` container, `AASTEAM_CNetworker` ping array)
plus this session's in-room-only `netcolor` mechanism. A live CDB approach (breakpoint on paint/UI
code for the list's delay-dot widget itself, working backwards from the render call rather than
forwards from guessed data offsets) is now the highest-value next step, per the recommendation
already recorded above for the `AASTEAM_CNetworker` dead end.

## RE follow-up: found the ACTUAL ranked-list delay-dot render call chain, working backward from the icon draw code (2026-07-11, new session)

Per the standing instruction to stop guessing data containers forward and instead work
**backward from the render/asset-selection code**, this pass started from the `net_col_A.hip`..
`net_col_G.hip` tiered icon strings already documented in `RankedDelayGhidraReport.txt` (Ghidra
`0x0094d088`-`0x0094d0e8`, referenced from two small "pick icon by 0-7 index, draw it" helper
functions: `FUN_00533d10` and `FUN_00655260`) and traced every caller of those two helpers instead
of guessing another offset.

**New Ghidra scripts, all under `docs/Research/ghidra_scripts/`, matching `run_ghidra_*.cmd` in
`docs/Research/` and `*GhidraReport.txt` output in `docs/Research/`:**
`DecompileNetColIconCallers.py` -> `NetColIconCallersGhidraReport.txt`,
`DecompileRankedListRowTierAccessor.py` -> `RankedListRowTierAccessorGhidraReport.txt`,
`DecompileRankedListRowEntryClass.py` -> `RankedListRowEntryClassGhidraReport.txt`,
`DecompileRankedListMgrSingletonXrefs.py` -> `RankedListMgrSingletonXrefsGhidraReport.txt`.

### Step 1 — the two icon-draw helpers, and which screen each belongs to

`FUN_00533d10` and `FUN_00655260` are near-identical: both take a 0-7 tier index parameter,
index into a fixed local array of 8 strings (`"net_col_def.hip","net_col_A.hip".."net_col_G.hip"`),
load that sprite, and hand it to the low-level draw call `FUN_006916b0`. Their callers split
cleanly into two unrelated screens:

- **`FUN_00533d10`'s only 2 callers (`FUN_00534a00`, `FUN_00535270`, both invoked from
  `FUN_00534360`)** draw the **post-match result screen** — `"SkillRank_%02d"`, `"Chara_Name"`,
  `"Match :   %d"`, `"Win :   %d    (%d%%)"` labels for the local player and up to 3 rivals. One
  of these two (`FUN_00535270`) explicitly calls the already-known-dead-end
  `FUN_004a1110()` (the gated, 30-sample-warmup `NetworkUserData+0x194` netcolor tier read
  documented in the previous session's section above) to get its icon index — **confirms this
  screen is the in-room/post-match one, not the pre-join list, consistent with prior findings.**
  Not investigated further (out of scope, already understood).

- **`FUN_00655260`'s 5 callers** are more interesting: `FUN_00724dc0` (2 call sites) is *also* the
  post-match/rival-card family (`"mm_win_02.hip"`/`"mm_win_03.hip"`, `"NM_PlayCount"`, `"NM_Win"`,
  `"NM_WinRate"`) and also calls `FUN_004a1110()` for its icon index — another confirmation that
  mechanism is post-match-only, not the list. The remaining 3 callers —
  **`FUN_00657150`, `FUN_00659960`, `FUN_00661060`** — are all "loop up to 0x32 (50) room slots,
  draw a row" list renderers. **`FUN_00661060` is confirmed to be the RANKED list specifically**
  (not a shared/casual room list) because its "nothing to show yet" fallback path uses the string
  literal `"NTER_RankMatch_NotMatching"` (localization key, ranked-match-specific wording) — no
  other candidate caller has a ranked-specific string. `FUN_00657150` by contrast uses the
  generic `"NM_NoRoom"` fallback with no ranked-specific wording and was NOT confirmed
  ranked-specific this pass (likely the casual Player Match / lounge room-list screen, sharing
  the same icon-draw helper — not investigated further since `FUN_00661060` is the confirmed
  ranked target). `FUN_00659960` not investigated this pass (time-boxed; same shape, lower
  priority once `FUN_00661060` was confirmed ranked-specific).

### Step 2 — `FUN_00661060`: the confirmed ranked-list row renderer, and its tier-icon call

`FUN_00661060(param_1)` loops `iVar4 = 0 .. 0x31` (50 rows). Per iteration:

```c
iVar5 = FUN_00649100(iVar4);
if (*(int *)(iVar5 + 4) != 0) {                 // slot is a real/populated room
  ...
  piVar7 = (int *)FUN_004a7b40(iVar5);          // <-- get the row-entry object for this slot
  if (piVar7 != (int *)0x0) {
    iVar8 = (**(code **)(*piVar7 + 4))();       // vtable slot 1: validity/type check
    if (iVar8 == 0) { /* draws "NM_NoRoom" text, no icon */ }
    else {
      uVar2 = (**(code **)(*piVar7 + 0x20))();          // slot 8:  raw member count
      uVar2 = (**(code **)(*piVar7 + 0x24))(uVar2);     // slot 9:  member-count formatter -> "%d/%d"
      ...
      uVar2 = (**(code **)(*piVar7 + 0x1c))(uVar10);    // slot 7:  <-- THE TIER GETTER
      uVar6 = FUN_00406530(*(undefined4 *)(local_be4 + 0x1150),0x3f800000,0x3f800000,0x3f800000,uVar2);
      ...
      FUN_00655260(local_bd4, ..., local_bd8, uVar6);   // draws the row's cell background color,
                                                          // modulated by uVar2 (see below)
```

**Important correction from manual disassembly, not visible in the decompile:** the pretty-printed
C for this specific `FUN_00655260(...)` call shows only 4 arguments, but `FUN_00655260`'s own
recovered signature takes 6 (`param_5` = the 0-7 net-col tier index used to pick
`net_col_<def,A..G>.hip`, `param_6` = a secondary color). Manually walking the raw x86 at the call
site (`0x006579CF` in the sibling `FUN_00657150`, byte-identical calling pattern in `FUN_00661060`
at its own `FUN_00655260` call) confirms via the `add esp,18h` after the call that **6 dwords are
genuinely pushed** — Ghidra's own decompiler under-reports the args for this call for reasons not
fully diagnosed (likely the FPU-register argument-passing idiom used here confuses its parameter
recovery). Manually mapping cdecl right-to-left push order to the 6 formal parameters shows:

- **`param_5` (the net-col tier index, 0-7) is exactly `uVar2` — the return value of vtable slot 7
  (`+0x1c`) on the row entry object (`piVar7`), zero-extended from `AL` (a single byte return).**
- `param_6` is a separate, earlier-pushed value (one more level removed; not fully traced this
  pass — lower priority since it's clearly a *display* color, not the delay-dot index itself).

**This is the actual, confirmed, backward-traced data source for the ranked list's per-row delay
dots: it's the return value of a virtual method (vtable slot 7, i.e. offset `+0x1c`) called on a
per-row "entry" object, not a static struct offset.** This is structurally different from — and
NOT yet disproven the way — the three previously dead-ended candidates (`DAT_00a5d270`
row array, `AASTEAM_CNetworker` ping array, `NetworkUserData+0x194` netcolor byte all confirmed
dead/wrong-scope in the sections above).

### Step 3 — where the row-entry object (`piVar7`) comes from

`FUN_004a7b40(uint rowIndex)`:

```c
undefined4 FUN_004a7b40(uint param_1)
{
  puVar1 = (undefined4 *)FUN_00486070();                       // singleton getter, returns &DAT_00c97e3c
  iVar2 = (**(code **)(*(int *)*puVar1 + 0x1c))();              // vtable slot 7 on *DAT_00c97e3c
  if (*(uint *)(iVar2 + 0xae8) <= param_1) { return 0; }        // iVar2+0xae8 = row/entry COUNT
  puVar1 = (undefined4 *)FUN_00486070();
  iVar2 = (**(code **)(*(int *)*puVar1 + 0x1c))();
  uVar3 = FUN_004a5450(*(undefined4 *)(iVar2 + 0xaf4 + param_1 * 4));  // iVar2+0xaf4 = per-row index/permutation array
  return uVar3;
}
```

`FUN_004a5450(mgr, targetIndex)` is a cached doubly-linked-list walk (head at `mgr+0xae0`, cursor
cache at `mgr+0xaec`/`mgr+0xaf0`, next/prev pointers read at node`+4`/node`+8`) that returns the
**node pointer itself** — and since `FUN_00661060` immediately does `*piVar7` (dereference for a
vtable pointer) and `(**(code**)(*piVar7+4))()` etc., the node **is** the row-entry object
(intrusive list: vtable at node+0, next/prev at node+4/+8, payload beyond that), not a
separate-payload wrapper.

**So the concrete chain is:**

```
base + 0x897E3C                          // DAT_00c97e3c, RVA = 0x00C97E3C - 0x00400000
  -> dereference (ptr to a polymorphic "ranked room-list manager" object, call it MGR)
MGR vtable slot 7 (MGR_vtable + 0x1c) -> call, no args
  -> returns a struct with:
       + 0xae0  list head pointer
       + 0xae8  row/entry count (uint)
       + 0xaec  traversal cursor cache (node ptr)
       + 0xaf0  traversal cursor cache (index)
       + 0xaf4  uint[count] array (permutation/underlying-index per visible row)
  -> FUN_004a5450(MGR, MGR[0xaf4 + row*4]) walks the intrusive list from the cursor cache
     to the target logical index, returns the row-entry NODE pointer (ENTRY)
ENTRY vtable slot 7 (ENTRY_vtable + 0x1c) -> call
  -> returns the 0-7 net-col tier byte (zero-extended from AL) that selects
     net_col_def/A/B/C/D/E/F/G.hip for that row's delay-dot icon
```

### Why this is NOT yet a ready-to-add live diagnostic, and the one blocking unknown

**`DAT_00c97e3c` (the `base+0x897E3C` slot) is confirmed, via an unrestricted raw-text grep of
`tools/bbcf_disasm_ascii.txt` for every literal occurrence of `C97E3C` in the whole binary, to be
written to a literal `0` in exactly one place (`FUN_00486070`'s lazy-init, `0x004860AF`) and reset
to `0` again at process-exit teardown (`0x008486E2`) — there is NO third, non-zero write anywhere
in the disassembled text.** Since `FUN_00486070` has 277 call sites across 204 unique functions
(confirmed live/working subsystems among them, e.g. `FUN_0049f990`, the already-validated netcolor
writer from the section above), this singleton obviously **is** populated with a real pointer at
runtime whenever ranked networking is active — the missing write must happen through a
register-indirect store (`p = FUN_00486070(); *p = someObjectPtr;`, i.e. `mov [eax], reg` right
after the call, with **no literal `C97E3C` operand** for a disasm-text grep to catch) inside
*one specific* constructor among those 204 callers, not yet located. **This is the one remaining
gap**: the pointer chain above is correct and load-bearing, but a live diagnostic needs to confirm
(a) that `*(base+0x897E3C)` is actually non-null while sitting on a populated ranked list (this
alone would be strong corroboration even without going further), and (b) the entry object's vtable
slot 7 implementation address, to know whether it's a trivial field read (cheap to also read
directly as a raw offset) or a real computation (in which case the diagnostic should just *call*
the same vtable slot rather than trying to guess a raw offset).

**Confidence: medium-high on the call chain (backed by two independent decompiles —
`FUN_00657150` and `FUN_00661060` — using the byte-identical pattern, plus the ranked-specific
`"NTER_RankMatch_NotMatching"` string confirming `FUN_00661060` is the right screen), but
low-to-unconfirmed on whether `base+0x897E3C` is ever actually non-zero at runtime** (static
analysis cannot prove a register-indirect write exists; it can only fail to disprove it, unlike
the three previous candidates which were *definitively* killed by live non-zero-read checks).

### Concrete next steps, in order of effort/value

1. **Cheapest, highest-value first test:** add a throttled, `IsBadReadPtr`-guarded live log of
   `*(void**)(base + 0x897E3C)` (just the raw pointer value, no further dereference yet) while
   sitting on a populated ranked search list. If it is ever non-null, that alone is strong
   confirmation this whole chain is real and live (matching the pattern already used for the
   now-removed `AASTEAM_CNetworker` diagnostic — see that section above for the throttle/guard
   style to copy). If it's always null even with a real list on screen, this candidate is dead
   the same way the other three were, and should be marked as such.
2. **If step 1 shows non-null:** extend the diagnostic to read `+0xae8` (count) and cross-check it
   against the number of rows actually visible on screen — an exact match is a second strong
   confirmation independent of the vtable-call structure.
3. **If steps 1-2 both confirm:** the entry's own tier getter (`ENTRY_vtable+0x1c`) is virtual, so
   the cleanest live read is to **call it** (via the existing vtable-pointer arithmetic, exactly
   as the game does) rather than guess a raw struct offset — log the return byte per visible row
   index and correlate directly against the visually-observed delay dots in the same play
   session. This sidesteps ever needing to fully reverse the entry class's raw memory layout.
4. **To find the missing constructor** (lower priority, only needed if a from-scratch static
   confirmation is wanted instead of the live-log approach above): scan the 204 callers of
   `FUN_00486070` (listed in full in `RankedListMgrSingletonXrefsGhidraReport.txt`) for the one
   whose very next instruction after `call 00486070` is a bare `mov dword ptr [eax], reg` (a
   *write* through the returned singleton-slot address, offset 0) rather than a further-offset
   *read* — that function is the constructor, and its RTTI/vtable-assignment literal would give
   the concrete class name the way `DecompileNetworkLobbyDataVtable.py` did for
   `CNetworkLobbyData` in an earlier session.

## Live diagnostic added for the ranked-list-manager singleton candidate (2026-07-11, new session)

Per next-step #1 of the section above, added `RankedListConnectionFilter::DiagnosticLogRankedListMgrSlot()`
(declared in the header, implemented right after `CountPopulatedGameRows()` in the .cpp) and wired
it into `IsLobbyListLikelyOpen()`'s existing diagnostic block, throttled to once/second like the
prior (now-removed) `AASTEAM_CNetworker` diagnostic. It reads
`base + kRankedListMgrSlotRva (0x00897E3C)` as a pointer (`DAT_00c97e3c`), and if non-null, also
peeks the first 4 bytes at that address (the presumed vtable pointer) purely for a sanity check.
Logs one line per second: `[RankedListFilter] rankedListMgrDiag: mgr=0xADDR vtable=0xADDR` (or
`mgr=null` / `slot unreadable`). **Diagnostic only - does not feed any real decision, purely for
correlation.** Purely additive, does not affect the working visibility/sort/live-resort code.
Compiles clean (Debug|Win32, verified).

**Next step for whoever picks this up:** get a fresh `DEBUG.txt` from a session where the user
sits on a populated ranked search list, then grep for `rankedListMgrDiag` and check whether
`mgr` is ever non-null while the list is genuinely on screen (cross-reference against
`visibility check: ... onList=1` lines at the same timestamps). If it's always `null`/unreadable
throughout, this is a fourth dead end - mark it as such in the "confirmed working"/dead-end
section near the top of this doc and remove `DiagnosticLogRankedListMgrSlot` + its call site +
`kRankedListMgrSlotRva`, the same way `AASTEAM_CNetworker`'s diagnostic was removed. If it's
non-null, proceed to next-steps #2/#3 from the previous section (cross-check the `+0xae8` count
against visible row count, then live-call the entry's vtable slot 7 per row instead of guessing a
raw offset).

## Independent cross-check of the ranked-list-manager candidate (2026-07-11, separate session)

A separate RE pass (this session), briefed with instructions to work backward from the
`net_col_A.hip`..`net_col_G.hip` asset-selection code without being shown the section immediately
above, independently arrived at the **exact same chain**: `net_col_` string xrefs -> the two
tier-icon-draw helpers `FUN_00533d10`/`FUN_00655260` -> the ranked search-result-list row renderer
(confirmed via `FUN_0064bfb0`'s literal `"NetworkRankMatchSearchResultWindow"` string) ->
`FUN_004a7b40` (row-entry-by-index) -> `FUN_00486070`'s singleton at `DAT_00c97e3c` (RVA
`0x897E3C`, guard `DAT_00c97e40` at RVA `0x897E40`) -> a per-row entity object whose own vtable
slot `0x1c` (offset 28, virtual slot 7) returns the 0-7 tier byte that indexes into the
`net_col_def/A/B/C/D/E/F/G.hip` array. This session's independent manual raw-disassembly walk of
the call site at `0x00657919`-`0x006579CF` (inside `FUN_00657150`, the structural twin of the
confirmed-ranked `FUN_00661060`) reached the same conclusion as the section above about the
"missing" 2 arguments the decompiler doesn't show for the `FUN_00655260` call (`add esp,18h` = 6
dwords truly pushed; the tier byte is pushed once at `0x00657969` and survives untouched through
the intervening `FUN_00406530` color-lookup call to land in the correct stack slot for
`FUN_00655260`'s `param_5`, `ebp+0x18`) — this raw-disasm reconstruction was performed independently
in this session (not copied from the existing report), and having two independent traces land on
byte-identical addresses and stack-slot math is a meaningful confidence boost for this candidate.
**No new contradicting evidence found; nothing in this cross-check should change the "medium-high,
not yet live-verified" confidence rating or the next-steps ordering already recorded above.**

**Current `DEBUG.txt` status (checked this session):** the live log at
`BBCF_IM/DEBUG.txt` (session ending `2026-07-11 16:14:46`, thread `T49520`) shows the ranked search
list genuinely on screen for an extended period (`onList=1` repeating, `rowDiag:
populatedRows=0 firstNonZeroRow=-1` confirming the old dead `DAT_00a5d270` container is still
correctly flat-zero as expected) but **contains zero occurrences of `rankedListMgrDiag`** — i.e.
this log predates the `DiagnosticLogRankedListMgrSlot()` build described in the section above; the
deployed `dinput8.dll` used for that session does not yet contain it. **Whoever picks this up next
should confirm a fresh build has actually been deployed (md5sum check, per the testing-protocol
reminder below) before treating an absent/all-null `rankedListMgrDiag` reading as a live
disconfirmation** — as of this writing, this candidate has not yet been tested against real
gameplay at all.

## RESULT: ranked-list-manager candidate CONFIRMED LIVE (2026-07-11, new session)

Fresh `DEBUG.txt` (md5sum-verified deployed build) from a real ranked-list session shows
`rankedListMgrDiag` logged 46 times over the session, **every single one identical:
`mgr=0x0438E480 vtable=0x00CFC3A4`** - a stable, non-null, plausible-looking pointer/vtable pair
the entire time the list was on screen. **This is the strongest confirmation any candidate has
had in this whole investigation** - unlike the three dead ends (all flat-zero/null the entire
time), this one is genuinely populated and stable. The RE chain traced backward from the
delay-dot render code (see the two sections above) is real, not a guess.

Also re-confirmed from the same log: the sort comparator itself remains provably correct
(`sort order` lines show a clean ascending `key=` sequence at every delivery, live-resort
throttling working exactly as designed - see e.g. lines around `17:35:35` through `17:35:58`,
repeated `(unchanged, skipping re-delivery)` between genuine probe-driven changes). The user's
"still not properly ordered" report continues to be a data-quality issue (wrong metric), not a
code bug - this has now been independently re-verified across two separate sessions/datasets.

**Extended the diagnostic this session** (`DiagnosticLogRankedListMgrSlot`, same function) to go
one step further per next-step #2: it now also **calls the mgr's own vtable slot 7 (`+0x1c`)** -
thiscall, `this=mgr`, no explicit args, exactly matching the game's own
`(**(code**)(*mgr+0x1c))()` call shape from the decompile - to obtain the row-list struct, then
reads that struct's `+0xae8` field (the documented row/entry COUNT). Logs now look like:
`[RankedListFilter] rankedListMgrDiag: mgr=0xADDR vtable=0xADDR rowCount=N shownCount=M` (where
`shownCount` is the mod's own `m_lastShownCount`, logged alongside for a direct, same-line
cross-check). **This is a real virtual call into game code** - deliberately restricted to the
one call site whose calling convention/argument count is unambiguous from the decompile (zero
explicit args); the actual tier-getter call on the per-row entry object was NOT added yet, since
its decompile shows an explicit argument (`uVar10`) whose origin/meaning is not yet confirmed,
and guessing it risks a crash. Build verified clean (Debug|Win32).

**Next step for whoever picks this up:** get a fresh `DEBUG.txt` from a session with a populated
ranked list, grep `rankedListMgrDiag`, and check whether `rowCount` matches `shownCount` (and
matches the number of lobbies actually visible on screen) - if `rowCount` tracks the real,
changing lobby count correctly, that's a second independent confirmation of the whole chain and
the next step is finding/confirming `uVar10` (the tier-getter's mystery argument) so the actual
per-row 0-7 tier value can be read live, the same careful way. If `rowCount` reads garbage or
never matches, re-examine whether `+0xae8` is really the count field before going further.

## RESOLVED: `uVar10` in the entry's tier-getter call is a decompiler artifact — the call takes ZERO explicit arguments (2026-07-11, new session)

Per the standing blocker (`(**(code **)(*piVar7 + 0x1c))(uVar10)` inside `FUN_00661060`, where
`uVar10`'s origin was unconfirmed), did a full re-decompile of `FUN_00661060` plus a manual raw
x86 disassembly walk of the actual call site, then independently cross-checked against the
structural twin `FUN_00657150` via the raw disasm dump. **Conclusion: `uVar10` is not a real
argument at all — it's the decompiler mis-rendering a leftover/stale SSA value that happens to be
in scope, the same class of pretty-printer unreliability already documented in this doc for the
neighboring `FUN_00655260` call (there it undercounted args; here it hallucinates one that isn't
pushed). The call is a plain thiscall with `this = piVar7` (the row-entry object) and NO explicit
stack arguments whatsoever.**

New Ghidra script/report: `docs/Research/ghidra_scripts/DecompileRankedListTierGetterArg.py` ->
`docs/Research/RankedListTierGetterArgGhidraReport.txt` (full raw disassembly dump of
`FUN_00661060`). Command file: `docs/Research/run_ghidra_rankedlist_tier_arg.cmd`.

**Step 1 — full re-decompile of `FUN_00661060` (report `NetColIconCallersGhidraReport.txt`, lines
1001-1240, already existed from a prior session's pass but had not been read end-to-end for this
specific question).** Reading the *entire* body (not the earlier truncated excerpt) shows every
assignment to `uVar10` in the function:

- `uVar10 = local_bf0;` — unconditionally, once, **before the per-row loop even starts** (right
  after computing the row-highlight-frame draw args).
- `uVar10 = *(undefined4 *)(local_be4 + 0x1300);` then, a few lines later in the *same* sub-branch,
  `uVar10 = local_bf0;` again — both **only inside the `iVar8 == 0` ("no room"/invalid slot)
  branch**, itself gated further on `local_c00 == iVar4` (the currently-selected row).
- **No assignment to `uVar10` anywhere in the `else` branch** (the "valid room" path that actually
  reaches the tier-getter call at line 1167, `uVar2 = (**(code **)(*piVar7 + 0x1c))(uVar10);`).

So per the decompiler's own pretty-print, whatever reaches the tier-getter call is simply whatever
`local_bf0` was (a fixed field of `param_1`, the list-window object itself — `local_bf0 =
*(undefined4 *)(param_1 + 0x11a8);`, used everywhere else in the function purely as a UI
color/alpha value passed to draw calls like `FUN_0068f210`/`FUN_006916b0`). That alone was already
a strong hint this isn't really a per-row "index" argument — a fixed color field would be a very
odd thing for a tier-getter to need — so the raw disassembly was checked to settle it for real
rather than trust the pretty-print again.

**Step 2 — raw disassembly of the actual call site (Ghidra addr range `0x0066183d`-`0x00661872`,
from `RankedListTierGetterArgGhidraReport.txt`):**

```
0066183d  MOV ECX,EDI                       ; ECX = this = piVar7 (row-entry object)
00661845  FLD  float ptr [EBP+...]          ; unrelated FPU work interleaved by the compiler
0066184b  MOV EAX,dword ptr [EDI]           ; EAX = *piVar7 (vtable pointer)
0066184d  FADD double ptr [EBP+...]
00661853  MOV EAX,dword ptr [EAX + 0x1c]    ; EAX = vtable slot 7 (the tier getter)
00661856  FSTP double ptr [EBP+...]
0066185c  FLD  float ptr [EBP+...]
00661862  FSUB double ptr [0x008a9080]
00661868  FSTP double ptr [EBP+...]
0066186e  CALL EAX                          ; <-- the tier-getter call. NO PUSH anywhere
                                             ;     between 0066183d and here.
00661870  FLD1
00661872  MOVZX EAX,AL                      ; return value: single byte, zero-extended from AL
```

**There is no `PUSH` instruction anywhere between `MOV ECX,EDI` (0066183d) and `CALL EAX`
(0066186e)** — every intervening instruction is either FPU stack traffic (unrelated float math for
the row's draw-position computation, interleaved by the compiler) or the vtable-pointer load
itself. This is a pure thiscall with `ECX = piVar7` and **zero explicit arguments**. `EDI` is
confirmed to hold `piVar7` continuously from `MOV EDI,EAX` right after the `CALL 0x004a7b40` (row
entry accessor, address `0x00661475`) all the way through to this call site — verified by reading
every `MOV EDI,...` instruction in the function; the only other EDI reassignment
(`0x0066148e`) is in the mutually-exclusive `iVar8 == 0` ("no room") branch that jumps around this
code entirely, and the branch that does reach `0x0066183d` (jump target `0x00661607`) never
touches EDI beforehand.

**Step 3 — independent cross-check against the twin `FUN_00657150` via the raw disasm text dump
(`tools/bbcf_disasm_ascii.txt`, byte-identical shape, addresses `0x00657919`-`0x00657982`), not
Ghidra's decompiler at all this time:**

```
00657925: mov    ecx,edi
00657933: mov    eax,dword ptr [edi]
00657941: mov    eax,dword ptr [eax+1Ch]
00657962: call   eax
00657964: fld1
00657966: movzx  eax,al
```

Same pattern, same conclusion, from a completely different tool path (raw text grep of the
pre-existing disasm dump vs. a fresh Ghidra decompile/disassembly) — no `push` between `mov
ecx,edi` and `call eax` in the twin either. Two independent confirmations.

**Answer to the blocking question: `uVar10` is not a real argument — the tier-getter call
(`ENTRY_vtable + 0x1c`) takes ZERO explicit arguments, exactly the same calling-convention shape
already confirmed for the outer MGR singleton's own vtable-slot-7 call
(`(**(code**)(*mgr+0x1c))()`, no args, already implemented in
`DiagnosticLogRankedListMgrSlot`).** The mod does **not** need to derive, guess, or pass any
per-row value at all — it's a plain `(**(code**)(*entryPtr + 0x1c))(entryPtr-as-thiscall-this)`
call, identical in shape to the already-live-tested MGR call. This removes the last blocker: the
full live-read chain (mgr singleton -> mgr's own slot-7 call, no args -> `+0xae8` count / `+0xaf4`
permutation array -> `FUN_004a5450` linked-list walk, one explicit index arg (already solved) ->
entry pointer -> entry's own slot-7 call, **no args**) is now fully calling-convention-safe end to
end.

**Confidence: high.** Backed by (a) a full, non-truncated re-read of the decompile showing no
`uVar10` write reaches the call in the taken branch, (b) a from-scratch raw-disassembly
instruction-by-instruction walk of the exact call site showing zero pushes, and (c) an independent
confirmation of the identical instruction pattern in the structural twin function via a completely
separate tool (grep over the pre-existing disasm text dump, not Ghidra). No contradicting evidence
found.

**Concrete next step for whoever wires this up live:** extend
`RankedListConnectionFilter::DiagnosticLogRankedListMgrSlot()` (or a new diagnostic function,
per this doc's usual pattern) to, per visible row (`0..rowCount` from the already-confirmed
`+0xae8` count, using the row index the same way the game does via the `+0xaf4` permutation array
and `FUN_004a5450`), call the resolved entry pointer's own vtable slot 7
(`(**(code**)(*entryPtr + 0x1c))(entryPtr)` in thiscall form, i.e. `this = entryPtr`, no other
args) and log the returned byte (0-7) alongside the row index, then correlate against the
on-screen delay dots in a real play session for the final live confirmation before wiring it into
the actual sort/filter logic. This is now a solved calling-convention case, same risk tier as the
already-tested MGR slot-7 call — no more argument-guessing risk remains for this call site.

## Live diagnostic extended to read the real per-row tier byte (2026-07-11, same day)

Per the "concrete next step" above, extended `RankedListConnectionFilter::DiagnosticLogRankedListMgrSlot()`
(same function, `src/Network/RankedListConnectionFilter.cpp`) to go all the way down the confirmed
chain per visible row (capped at `kMaxDiagnosticRows=32` to bound log spam/virtual-call cost, not a
real game limit):

1. Read the underlying/logical row index from the list struct's `+0xaf4` permutation array (already
   read for the row-count check).
2. Call `FUN_004a5450` (new constant `kWalkRowListRva = 0x000A5450`) as `__thiscall(this=listStruct,
   explicit arg=underlyingIndex)` - matches its own decompiled signature exactly
   (`int __thiscall FUN_004a5450(int param_1,int param_2)`, `RankedListRowEntryClassGhidraReport.txt`) -
   to get the row's ENTRY object pointer. Note this is `this=listStruct` (the struct returned by the
   MGR's own slot-7 call), **not** the MGR object itself - a subtlety worth flagging since both are
   loosely called "the manager" in earlier notes.
3. Call the entry's own vtable slot 7 (`+0x1c`) as `__thiscall(this=entry)`, **zero explicit args** -
   confirmed safe by the previous session's disassembly work, no more guessing.
4. Log every row's tier byte on one line: `[RankedListFilter] rankedListMgrDiag: mgr=0xADDR
   vtable=0xADDR rowCount=N shownCount=M tiers: [0]=T [1]=T ...` (only the raw byte value, 0-7
   expected per the `net_col_def/A-G.hip` asset count, though the diagnostic doesn't assert that
   range - if real gameplay ever shows a byte outside 0-7 that's itself useful information, not a
   bug to "fix" in the diagnostic).

`FUN_004a5450` mutates the list struct's own traversal cache (`+0xaec`/`+0xaf0`) as a side effect -
harmless, since it's the exact same call the game itself makes every frame per visible row with the
same real index values; calling it an extra time from the diagnostic only ever reflects a state the
game would already produce on its own next frame.

Build verified clean (Debug|Win32) after fixing one `std::min`/Windows.h `min` macro collision
(replaced with a plain ternary - this codebase does not define `NOMINMAX` globally, so `std::min`/
`std::max` are unsafe to use bare in files that transitively include `Windows.h`; prefer a ternary
or `(std::min)(...)`-with-parens idiom here instead).

**Next step for whoever picks this up:** get a fresh `DEBUG.txt` from a session where the user sits
on a populated ranked list and notes, for at least one specific row/player, what delay-dot tier they
visually see on screen (a screenshot or a clear verbal description + rough position in the list is
enough). Grep `rankedListMgrDiag` for the `tiers:` values at that same timestamp and check:
(a) are the values ever outside 0-7 (would suggest a wrong offset/misread), (b) do they correlate at
all with the user's own probe-based sort order already logged (`sort order` lines) - a real
correlation either direction (positive OR negative - e.z. if our probe elapsed-time metric turns out
to correlate inversely with the real tier, both are consistent with the *same* underlying network
condition, just measured differently), (c) most importantly, do the logged tier values actually
match what the user visually sees for specific rows. If (c) confirms, wire this real tier value into
`RankedListSortMode_BestConnection`/`WorstConnection` in `SortShownCandidates()` in place of
`probeElapsedMs`, and update the sort-mode help text in `RankedListFilterWindow.cpp` (currently
honestly labeled as "the mod's own reachability check timing... not the list's exact connection
column" - once this is wired in for real, that caveat should be removed/rewritten since it would no
longer apply). If tiers never populate meaningfully (all one value, always garbage, etc.), mark this
whole chain dead in the "confirmed working"/dead-end section near the top of this doc, alongside the
other three.

## RESULT: real tier data confirmed live, and wired into the actual sort (2026-07-11, same day)

Fresh `DEBUG.txt` from the user (md5sum-verified deployed build) shows the per-row tier
diagnostic producing **real, plausible, non-garbage data** for the first time:
`rowCount=24 shownCount=24 tiers: [0]=4 [1]=0 [2]=3 [3]=4 [4]=5 [5]=3 [6]=4 [7]=4 [8]=6 [9]=2
[10]=4 [11]=2 [12]=5 [13]=3 [14]=1 [15]=4 [16]=4 [17]=5 [18]=5 [19]=6 [20]=1 [21]=4 [22]=5
[23]=3` - a genuine spread across the 0-6 range (matching the expected `net_col_def/A-G.hip`
8-tier asset count), completely uncorrelated with our own probe-based sort order at the same
moment (confirming the user's report that the visible order didn't reflect this column - because
until this session, the diagnostic only *logged* the tier, it was never actually used for
sorting). Also reconfirmed the `rowCount==shownCount` correlation still holds (brief
0-vs-stale-shownCount lag right after each delivery, same pattern as before, already handled by
the guard described below).

**Wired the confirmed-real tier into the actual sort and into live resort, this session:**

- `PeerVerdict` gained a `gameTier` field (`-1` = never observed), alongside the existing
  `probeElapsedMs`.
- New method `RankedListConnectionFilter::PollGameTiers()` (declared in the header, implemented
  right after `PollProbes()`): reuses the exact confirmed-live chain from the diagnostic (mgr
  singleton -> mgr's own slot-7 call -> `+0xae8` count / `+0xaf4` permutation array ->
  `FUN_004a5450` walk -> entry's own slot-7 call, no args) but instead of just logging, it maps
  each row's position to the peer at that same position in `m_reachableLobbies` (our own
  last-delivered order) and stores the tier into `m_verdicts[ownerSteamId].gameTier`. Guarded the
  same way the diagnostic already was: only trusts the read when the game's own row count
  exactly equals `m_reachableLobbies.size()`, to avoid pairing a row's tier with the wrong peer
  during the brief post-delivery lag window. Throttled to 2/sec (500ms).
- Called from `OnSteamCallbacksPump()` alongside `PollProbes()`/`PollPendingConnectionConfirmation()`,
  so it keeps running in the background exactly like the existing probe polling.
- `SortShownCandidates()`'s `BestConnection`/`WorstConnection` cases now check `gameTier >= 0`
  FIRST (negated, so ascending numericKey still means "best first", matching the existing
  `probeElapsedMs` convention) and only fall back to `probeElapsedMs` when the real tier hasn't
  been observed yet for that peer.
- **This also directly answers the "no live re-sort" half of the report**: `probeElapsedMs` is a
  fixed one-shot value per peer (measured once, never changes again), so once every peer's probe
  resolves, `TryLiveResort()` has nothing left to react to - it was working correctly, there was
  just nothing new to resort by. The real game tier, by contrast, is polled continuously and can
  genuinely change over time - since `PollGameTiers()` updates `m_verdicts` and `TryLiveResort()`
  already recomputes+diffs+redelivers-if-changed every ~750ms, **wiring in `gameTier` is what
  makes live resort actually do something visible**, with no additional plumbing needed beyond
  what already existed.
- Updated the sort-mode help text (`RankedListFilterWindow.cpp` + `Localization.csv`, all three
  columns) to describe the new priority (real tier when observed, live-updating; probe estimate
  only as a fallback) instead of the old "always an estimate" wording.
- Build verified clean (Debug|Win32) - the localization CSV edit triggered the codegen
  pre-build step and a broader recompile, no new warnings/errors beyond pre-existing ones.

**Not yet visually confirmed against the actual on-screen delay dots** - the tier VALUES are
confirmed real/live/non-garbage via the diagnostic and the row-count cross-check, but no one has
yet visually compared a specific row's logged tier number against what dot-pattern that row
actually shows on screen. That remains the final confirmation step.

**Next step for whoever picks this up:** get a fresh `DEBUG.txt` from a session with the "Best to
Worst Connection" (or worst-to-best) sort mode active, and this time have the user note what
they visually observe: does the list actually appear sorted by delay dots now, and does it
visibly reorder itself in place over a few seconds without hitting refresh? If yes, this feature
is essentially done pending general polish. If the visible order still looks wrong even though
`sort order` log lines show a clean ascending/descending `key=` sequence using real `gameTier`
values (not the `probeElapsedMs` fallback), that would mean the row-to-tier mapping itself is
subtly wrong (e.g. an off-by-one, or the assumption that game row order always matches our
delivered order breaks under some condition) and needs further live cross-checking - at that
point, correlate specific `tiers:` log entries against a screenshot/description of the actual
on-screen dots for at least one identifiable player.

## MAJOR FINDING: "live resort" has never actually done anything - `Run()` re-invocation is a no-op (2026-07-11, new session)

User reported the list still isn't ordered by the real tier, AND still doesn't visibly reorder
live. Fresh `DEBUG.txt` (md5sum-verified) revealed the actual root cause, and it invalidates a
core assumption this whole feature was built on.

**The evidence:** in one continuous ~40-second window, the log shows **42 `delivering (...)`
events but only 3 bursts of `GetLobbyByIndex` calls** - one burst per genuinely new
`LobbyMatchList_t` delivery from Steam (`hold deadline`/`all resolved` events at `18:14:51.075`,
`18:15:05.707`, `18:15:23.623`). **Every one of the other 39 `delivering (live resort)` events -
i.e. every single `TryLiveResort()`-triggered re-invocation of the game's own `CCallResult`
handler via `handler->Run()` - produced ZERO subsequent `GetLobbyByIndex` calls from the game.**
`GetLobbyByIndex` is logged unconditionally, every real call, no throttling - so this isn't a
logging gap, it's proof the game genuinely never asks again.

**What this means:** Steam `CCallResult` callbacks are inherently single-shot - they fire exactly
once per async API call. The game's lobby-list handler evidently reads the list (via
`GetLobbyByIndex(0..count-1)`) exactly once, when the callback first fires, builds its room-slot
array from that one read, and then just renders/keeps whatever it built. **Calling the same
handler's `Run()` a second time with an updated payload does not cause the game to re-fetch or
rebuild anything - it's a complete no-op from the game's perspective.** All of this session's
(and previous sessions') `TryLiveResort()` activity, `sort order` logs, and
`(unchanged, skipping re-delivery)`/genuine-reorder messages were 100% correctly computed on our
side - the bug is not and never was in our sort logic (confirmed clean yet again by every
previous session's testing) - but none of those recomputations were ever actually reaching the
screen. **This retroactively invalidates the earlier claim in this doc that "the hide-filter
benefits from the same mechanism for free"** (from the live-resort implementation section above)
- that was never verified and is now known to be false: a peer that becomes confirmed-unreachable
*after* the initial delivery will NOT be removed from the visible list until the game's own next
natural refresh (or the user manually searches again), not within ~750ms as originally claimed.

**Why the tier-based sort specifically could never have worked via this mechanism even in
principle**, independent of the no-op problem above: `PollGameTiers()` can only read a peer's real
tier by observing it on an ALREADY-RENDERED row - i.e. it needs a list to already be on screen,
built from a PRIOR delivery's order. There is no way to know a peer's real tier *before* first
displaying them. So even if live re-delivery worked, gameTier-based sorting has a one-refresh-cycle
lag: values gathered while looking at list N can only ever inform the sort order of list N+1 (the
next genuinely new Steam-driven delivery), never list N itself. Combined with the no-op finding,
list N+1 currently never arrives faster than the game's own natural refresh cadence (or a manual
re-search), which was separately confirmed (see much earlier section) to happen only every
~8-16 seconds while idling on the list.

**Practical, honest state of the feature right now:**
- The hide-unreachable filter genuinely works on every REAL delivery (confirmed extensively,
  peers are removed from `m_reachableLobbies` and the compacted count is what's served) - but
  newly-confirmed-unreachable peers only actually disappear from the user's screen on the game's
  own next natural refresh, not sooner.
- Sorting (all modes, `gameTier` now included) is computed correctly on every real delivery and
  does get served to the screen **the first time that specific list/count is legitimately
  delivered** - the bug is specifically that repeated re-computation between real deliveries
  (`TryLiveResort`) never reaches the screen, not that the initial delivery's order is wrong.
- Given `gameTier` requires a prior on-screen observation, and the ONLY point at which a
  freshly-observed `gameTier` can affect what the user sees is the NEXT real Steam-driven refresh,
  users should expect any effect of the real-tier sort to show up gradually, refresh over refresh
  (~8-16s apart, or on manual re-search), not instantly or continuously - this is now the
  realistic ceiling for what this delivery mechanism can achieve, not a bug to keep chasing.

**Options going forward (not yet decided/implemented):**
1. **Remove/neuter `TryLiveResort()` and the `forceDeliver=false` path entirely** - it currently
   does nothing but burn cycles and add log noise every ~750ms. Keep `PollGameTiers()` (it's
   valuable for *accumulating* tier data across the session, feeding future real deliveries) but
   stop pretending re-invoking `Run()` accomplishes anything.
2. **Accept refresh-cadence-limited updates as the real behavior** and communicate this honestly
   in the UI help text (already partially done, but the "updating live as it changes" phrasing
   added this session is now known to overclaim and should be walked back once a decision is
   made).
3. **(Bigger, unexplored, more invasive) Investigate whether the game's room-slot array itself
   can be directly manipulated in memory** (move/remove entries post-hoc) instead of relying on
   re-invoking the Steam callback - this would need locating the WRITE side of the same
   `FUN_00661060`/manager-singleton chain already traced (this session only traced the READ
   side), and is a substantially larger RE undertaking with real risk of UI corruption if done
   incorrectly. Not recommended without explicit user buy-in given the risk/effort.
4. **(Alternative, much simpler) Programmatically trigger the game's own real refresh** (simulate
   pressing "Search" again) on some cadence shorter than its natural ~8-16s cycle, so accumulated
   `gameTier`/hide-filter state reaches the screen sooner - trades a possible visible
   flicker/reload for faster convergence. Feasibility not yet investigated (would need to find
   and safely invoke whatever UI action fires `RequestLobbyList` normally).

**This is a decision point, not something to keep silently iterating on** - the next step should
be discussing these options with the user rather than another blind attempt at "make live resort
work," since the live-resort mechanism as built is now proven structurally incapable of reaching
the screen at all.

## Two real bugs found and fixed, plus a deeper analysis of "sort doesn't persist across refreshes" (2026-07-11, same day)

User reported two more issues: (1) even on genuine refreshes, players who showed as top-tier in
an earlier search aren't at the top on a later one, and (2) the "hide unreachable players"
checkbox doesn't seem to actually disable all filtering.

### Bug #1 (confirmed, fixed): the hide-filter checkbox was NOT gating hiding at all

`BuildCompactedListAndDeliver()` called `ShouldHidePeer()` unconditionally for every candidate,
with **no check of `Settings::settingsIni.enableRankedListConnectionFilter`** anywhere in that
function. `IsPipelineActive()` (which decides whether the whole delivery pipeline runs at all)
correctly treats the filter and a non-default sort mode as independent triggers - but once the
pipeline was running for ANY reason (e.g. just because a sort mode was selected), reputation-based
hiding applied regardless of whether the user had the checkbox on or off. **Fixed:** added a
`filterEnabled` check gating the entire hide branch - when the checkbox is off, every candidate is
always shown, full stop, independent of sort mode or accumulated reputation state. Build verified
clean.

### Investigation of "sort doesn't persist across refreshes"

Traced this in detail against a fresh `DEBUG.txt` covering 3 genuinely-new deliveries (confirmed
via the `GetLobbyByIndex` burst-count method established earlier) across ~30 seconds. Findings:

- **The gameTier CACHE itself is working correctly for persisting players.** Steam IDs that
  appeared in multiple consecutive real deliveries (the game's own natural ~14s auto-refresh
  reused much of the same player pool in this session) kept a STABLE, correct
  gameTier-derived sort key across every subsequent real delivery - e.g. one peer held `key=-5`
  (tier 5, best) consistently from the moment it was first observed through two later real
  refreshes. This is exactly the "remember and reuse" behavior the user asked for, and it was
  already happening before this session's changes (this session only added averaging on top).
- **The FIRST search of a session can never be sorted by real tier**, unavoidably: gameTier can
  only be learned by observing an already-on-screen row, so the very first list anyone ever sees
  in a session necessarily falls back entirely to the (arbitrary, unsorted for `known=0` peers)
  default order or the probe-timing estimate. This is not a bug, just a hard limit of the
  approach - the very first search a user does after launching ranked search WILL look unsorted.
- **Any brand-new peer appearing in a given search (never seen before in that session) similarly
  cannot be tier-sorted for THAT specific search** - confirmed directly in the log: two peers
  whose very first-ever P2P probe completed at the SAME instant as a real delivery correctly fell
  back to the probe-timing estimate (large positive key) rather than a gameTier key, because they
  genuinely had no prior observation to draw on. This, too, is an inherent limit, not a bug.
- **What IS a confirmed, separate bug** (see "MAJOR FINDING" section above, not re-litigated here):
  because `TryLiveResort()`'s repeated `Run()` calls are proven no-ops, whichever gameTier/verdict
  state existed at the EXACT INSTANT of a real delivery is what's frozen on screen for that entire
  refresh cycle - any tier/probe data that resolves moments later (even a few hundred ms later,
  before the next real refresh) never reaches the screen even though our own internal state and
  `sort order` logs correctly reflect it. Combined with the previous point, a peer who is brand-new
  to a given search AND resolves their probe a few hundred ms after that exact delivery instant
  will look wrong for that whole refresh cycle even though our data was correct moments later.

**Conclusion:** for this specific session's data, "does a returning top-tier player stay at/near
the top on subsequent real refreshes" already checked out correctly. If the user's actual
experience still looks wrong even for players confirmed present across multiple prior searches,
the likely explanations, in order of likelihood, are: (a) their actual play session had far more
list churn (different, less-persistent player pool between searches) than this sample, so most
players they compare across refreshes are effectively "brand new" each time from the mod's
perspective, or (b) the deeper `TryLiveResort`-no-op timing-window problem above is catching more
peers than in this sample. **Not yet ruled out and worth checking in the NEXT test session:** get
a fresh `DEBUG.txt` and, for a SPECIFIC named/identifiable player the user can visually track
across at least two consecutive real searches, cross-reference their `owner=<steamId>` position
and `key=` value in the FIRST `sort order` log immediately preceding each real
`GetLobbyByIndex` burst (that log is the only one that's ever actually screen-visible - all later
`sort order`/`delivering (live resort)` lines before the NEXT burst are provably invisible
no-ops, do not use them for this comparison) against what the user visually saw on screen at each
of those two moments.

### Implemented per explicit user request: average the tier across observations instead of using only the latest

Added `PeerVerdict::gameTierAverage` (double) and `gameTierSampleCount` (int), alongside the
existing raw `gameTier` (kept for diagnostics). `PollGameTiers()` now updates a cumulative average
on every successful per-row tier read (`newAverage = (oldAverage*count + newSample)/(count+1)`),
rather than just overwriting the single latest value. `SortShownCandidates()`'s
`BestConnection`/`WorstConnection` cases now key off `gameTierAverage` (fixed-point x100 for a
stable integer sort key) whenever `gameTierSampleCount > 0`, instead of the raw single-sample
`gameTier`. This directly addresses the user's request ("store the delay result for each user and
then average it out") and additionally smooths out any single noisy/mismapped row-to-peer
correlation, which is valuable regardless of whether the "row order matches our delivered order"
assumption in `PollGameTiers` ever turns out to be imperfect. Build verified clean (Debug|Win32).

## User-provided screenshots + fresh DEBUG.txt analysis: ambiguous but concerning row-order mismatch (2026-07-11, same day)

User provided two screenshots of the actual in-game ranked list (with visible delay-dot tags and
player names) after refreshing, clearly showing an unsorted delay column, plus a fresh
`DEBUG.txt` (md5sum-verified deployed build matching this session's changes).

**Notable observation from the screenshots themselves:** the same 2-3 player names
("LA BESTIA DEL...", "Jabroni", "Emerald") appear at the exact same top positions with the exact
same delay tags in BOTH screenshots (taken after a refresh), while the players further down
differ between the two. This is consistent with the top rows being anchored by something other
than our sort (e.g. cursor/selection retention, or literal front-of-Steam's-raw-list stability),
though not conclusive on its own.

**Numeric cross-check attempted:** compared the exact `sort order` log line for the one real
delivery in this session (`all resolved` at `18:37:52.944`, confirmed real via a matching
`GetLobbyByIndex` burst) against the `rankedListMgrDiag` tier-read-back ~0.8s later
(`18:37:53.732`). For the ~16 peers that already had a real `gameTier` average at delivery time,
roughly half the positions land close to what was delivered (e.g. position 0 delivered as highest
average, read back as tier 6 - consistent), but several positions are substantially off (e.g. a
peer delivered with `key=-340` (avg≈3.4) read back moments later as tier 6 at a different
position; another delivered at avg≈3.25 read back as tier 1). **This is genuinely ambiguous** -
it's consistent with either (a) real per-tick tier fluctuation (plausible, matches the user's own
original observation that the delay indicator visibly changes over time) landing in a
coincidentally-partial correlation, or (b) an actual row-to-peer mapping error in
`PollGameTiers`/the diagnostic. **Could not be resolved conclusively because neither log
included player names** - only steamIds, which the user's screenshots (understandably) can't be
cross-referenced against without a lookup step that wasn't available in this session.

**Fixed this session: both the `sort order` log and the `rankedListMgrDiag` per-row tier dump now
include the peer's display name** (`SortShownCandidates`'s diagnostic gained `name="..."`; the
per-row tier diagnostic in `DiagnosticLogRankedListMgrSlot` now emits `[row]=tier(Name)` instead
of just `[row]=tier`, looked up the same way `PollGameTiers` already maps `m_reachableLobbies[row]`
back to a candidate). This is the key missing piece - the **next** test session's `DEBUG.txt` can
now be directly, visually compared name-by-name against a screenshot of the actual on-screen list,
which is a far stronger correlation method than steamId matching. Build verified clean
(Debug|Win32).

**Concrete next step:** get a fresh `DEBUG.txt` from a session where the user takes a screenshot
of the ranked list IMMEDIATELY after a fresh search (as close as possible to the moment of
refresh/search, to minimize the chance of comparing against a later, possibly-drifted
`rankedListMgrDiag` sample) and grep both `sort order`/`delivering (` (to find the one REAL
delivery, matched via the `GetLobbyByIndex`-burst method) and the CLOSEST-in-time
`rankedListMgrDiag` line, then compare the logged `(Name)` sequence directly against the
screenshot's visible name order, top to bottom. This will conclusively answer whether (a) the
on-screen order matches what we intended to deliver (in which case the remaining problem is
purely tier volatility/averaging tuning), or (b) the row/name mapping itself is wrong (in which
case the `PollGameTiers`/diagnostic row-index assumption needs to be reinvestigated - possibly the
"row i in the permutation array corresponds to the i-th GetLobbyByIndex we served" assumption,
unverified until now beyond the count matching, is actually false).

## MAJOR NEW FINDING: the underlying row/entry association itself rotates over time, independent of any real delivery (2026-07-11, same day, name-tagged logs)

With the newly-added name tags in both logs, got conclusive (not ambiguous this time) evidence of
a deeper problem than metric quality or delivery no-ops. Three consecutive `rankedListMgrDiag`
reads, one second apart, with **zero real delivery or even a `live resort` call that could have
changed `m_reachableLobbies` in between** (confirmed: the only intervening `delivering (...)`
lines in that window are `live resort` - already proven to be no-ops that never reach
`GetLobbyByIndex` - so our OWN internal delivered order was provably unchanged across all three
reads):

```
18:47:30.373 tiers: [0]=4(Dead) [1]=6(Jabroni) [2]=3(Buggosluggo) [3]=6(mengdream) [4]=3(DacciXIV) [5]=0(GerardJS84) [6]=4(wari) [7]=4(Lyxas) [8]=7(wokewaifu95) [9]=4(Obi) [10]=5(Koyanskaya Of Light) [11]=4(U10) [12]=4(Citolo) [13]=3(Kirtle) [14]=5(Alanor) [15]=5(slayraptor64) [16]=6(Yugi)
18:47:31.372 tiers: [0]=4(mengdream) [1]=6(Dead) [2]=3(Alanor) [3]=6(Jabroni) [4]=3(DacciXIV) [5]=0(wokewaifu95) [6]=4(GerardJS84) [7]=4(wari) [8]=7(Kirtle) [9]=4(Lyxas) [10]=5(Yugi) [11]=4(Obi) [12]=4(U10) [13]=3(Citolo) [14]=5(Buggosluggo) [15]=5(slayraptor64) [16]=6(Koyanskaya Of Light)
18:47:32.374 tiers: [0]=4(Dead) [1]=6(mengdream) [2]=3(Jabroni) [3]=6(Alanor) [4]=3(Kirtle) [5]=0(Yugi) [6]=4(DacciXIV) [7]=4(GerardJS84) [8]=7(wari) [9]=4(Lyxas) [10]=5(Buggosluggo) [11]=4(Obi) [12]=4(U10) [13]=3(Citolo) [14]=5(slayraptor64) [15]=5(wokewaifu95) [16]=6(Koyanskaya Of Light)
```

**The exact same 17 tier values, as a multiset, appear identically in all three reads** -
`{4,6,3,6,3,0,4,4,7,4,5,4,4,3,5,5,6}` every time - but **which name is attached to which value
rotates by roughly one step each second.** E.g. "mengdream" alternates cleanly `6,4,6,4...`
between consecutive reads; "Jabroni" moves `6(pos1)->6(pos3)->3(pos2)`. This is not noise or a
mapping off-by-one - it's a real, periodic, near-exactly-1-second rotation of which underlying
entry occupies which logical row/permutation slot, **happening entirely on its own, with zero
real delivery in between.**

**Two possible causes, not yet distinguished:**
1. The game's own internal room-slot list is genuinely, continuously reshuffling on its own
   internal cadence (plausibly connected to the earlier-found `FUN_0046b9c0` "write elapsed-ms
   sample" mechanism gated on `phase==4`, which fires repeatedly over time) - independent of
   anything the mod does, and NOT controllable via `GetLobbyByIndex` serve order beyond the very
   first instant of a fresh delivery.
2. **The mod's OWN repeated calls into this chain (`PollGameTiers` + the diagnostic, both calling
   `FUN_004a5450`/the entry's tier-getter roughly once per 500ms-1s) might themselves be causing
   or contributing to this rotation** - the entry's actual tier-getter implementation was only
   ever confirmed by its CALLING CONVENTION (zero explicit args, thiscall) via manual
   disassembly of the CALL SITE, never by decompiling its full body - it's entirely possible this
   "getter" is not a pure read and has some internal tick/rotation side effect that firing it more
   often than the game's own render cadence would intensify or destabilize.

**This directly explains the user's screenshots**, which show an unsorted, shuffled delay column
even immediately after a fresh search/refresh - if the underlying row/identity association is
rotating on a ~1s cycle regardless of what we deliver, then even a PERFECTLY sorted initial
delivery would look scrambled within a second or two of the user actually looking at it, and
worse, it means **there may currently be no stable "true" row order to sort by at all** - the
target itself moves.

**This is a serious, not-yet-resolved blocker, more fundamental than the "live resort is a no-op"
finding** (which was about our own delivery not reaching the screen; this is about the screen's
own underlying data not staying still even when nothing new is delivered). It calls into question
whether "reorder via serve order" can ever produce a stable sorted list at all, independent of
metric quality.

**Isolation test IMPLEMENTED this session (not yet run by the user):** disabled
`PollGameTiers()`'s call site in `OnSteamCallbacksPump()` (commented out) and gutted the per-row
loop inside `DiagnosticLogRankedListMgrSlot()` (`rowsToRead` forced to `0` - the mgr/rowCount
liveness check above it is left intact so we still confirm the chain is present, just without
the per-row `FUN_004a5450`/entry-vtable-slot-7 calls that are the suspect). Build verified clean
(Debug|Win32). **The ask for the user:** rebuild/deploy this, search ranked, and simply WATCH the
on-screen list sit still for 10-15 seconds without touching anything or searching again. If the
delay dots visibly keep changing/swapping on their own even with the mod now making ZERO calls
into this chain, that proves cause #1 (game-internal, not the mod's fault, and a much harder
problem to fix). If the list stays visually stable with the mod hands-off, that proves cause #2
(the mod's own polling was the culprit) - in which case the fix is to poll this chain far less
often, find a way to read it that doesn't perturb shared state, or accept read-only observation
via a single one-shot call per real delivery instead of continuous polling. Either way, once
answered, remember to also **re-enable `PollGameTiers()` and the per-row diagnostic loop** if the
verdict is cause #1 (since disabling them isn't a fix for that case, only a diagnostic).
Since `PollGameTiers()` is off for this test, `BestConnection`/`WorstConnection` sort will have
zero gameTier data all session and fall back entirely to `probeElapsedMs` - expected and fine,
this test is purely about visual stability, not sort correctness.

## RESOLVED (differently than expected): the "rotation" was a mod-side mapping bug, not a game-side or polling-caused instability (2026-07-11, isolation test result)

Ran the isolation test from the section above (disabled `PollGameTiers()` and gutted the per-row
loop in the diagnostic, keeping only the mgr/rowCount liveness check). User refreshed 3 times
(confirmed via 3 `GetLobbyByIndex` bursts in `DEBUG.txt`, md5sum-verified matching build) and
watched the list. **Critical clarification from the user: the visible list has NEVER, even
before any of this session's changes, visually reordered itself - names/rows are static once
created, and only the delay ICON progressively appears per row over time as that row's own ping
resolves.** No shuffling, no rearranging, ever, on the actual screen.

**This rules out both hypotheses from the previous section** (game-internal continuous reshuffle,
or the mod's own polling causing/worsening a real on-screen instability) **and points to a third,
simpler explanation that fits every observation:** the earlier "rotation" (same 17 tier values,
different name attached each second) was never something the user could have seen on screen - it
only ever existed in the mod's OWN internal reads. The mod's assumption that "row index `i` in the
tier-lookup chain (permutation array `+0xaf4` -> `FUN_004a5450` walk -> entry vtable slot 7)
corresponds to the same row `i` used for the name/level columns (i.e., the `i`-th lobby served via
`GetLobbyByIndex`)" was **never actually verified beyond a COUNT match** (`rowCount ==
m_reachableLobbies.size()`) - and the observed rotation is direct proof that assumption is false.
The name/level columns are provably locked to serve order and genuinely static (confirmed now by
the user); the tier-lookup chain is very likely reading from a **different, separately-indexed
internal structure** (plausibly something like an independent probe/round-robin rotation queue
used for connection re-sampling, unrelated to visual row identity) that the renderer ALSO happens
to consult for the icon specifically, without that consultation being tied to the same positional
index as the name/level text (which was likely cached once at row-creation time and never
re-fetched, while the delay icon specifically is re-queried/re-resolved independently per row over
time - this exactly matches "everything is static except the delay tag, which fills in later").

**Practical consequence: every `gameTier`/`gameTierAverage` value collected so far via
`PollGameTiers()` should be treated as UNRELIABLE** - likely attributed to the wrong player at
least some of the time, for the entire period this feature has existed. This is a plausible root
cause for why sorting by it never visibly looked correct even on paper-correct comparator runs:
the DATA fed into the comparator was probably wrong at the source, not the comparator or the
delivery mechanism (both independently confirmed correct multiple times already).

**Current code state:** `PollGameTiers()`'s call site is still commented out (from the isolation
test) and the diagnostic's per-row loop still gutted (`rowsToRead` forced to `0`) - **do not
simply re-enable these as-is**, since doing so would resume collecting the same
unreliable/mismapped data. `BestConnection`/`WorstConnection` sort currently falls back entirely
to `probeElapsedMs` (the original, known-weak-but-at-least-honestly-labeled estimate) with zero
`gameTier` contribution, which is the current safe/honest state of the feature.

**Concrete next steps, in order, before re-attempting real-tier sorting:**
1. **Find out what the tier-lookup chain's "row index" actually correlates to**, if anything
   identity-stable. The most promising lead: check whether the ENTRY object itself (the node
   returned by `FUN_004a5450`) carries an identity field directly readable from its own memory
   (a steamId, lobbyId, or similar), rather than relying on POSITIONAL correlation via the
   permutation array. If such a field exists, tier data could be correctly attributed to the
   right player regardless of whatever internal reordering this structure does on its own - this
   would fully sidestep the mapping problem rather than trying to "fix" the position-based
   correlation.
2. If no direct identity field is found on the entry object, this candidate chain should probably
   be abandoned for sorting purposes (same fate as the three earlier dead-end candidates) - a
   value we cannot reliably attribute to a specific player is not usable for per-player sorting,
   regardless of how "real" the underlying data is.
3. Independent of (1)/(2): the honest, currently-working state of the feature (probeElapsedMs-only
   sort, comparator/delivery logic repeatedly confirmed correct, hide-filter checkbox now properly
   gated) is a reasonable place to stabilize if the identity-correlation problem turns out to be
   unsolvable or not worth the further RE effort - this should be discussed with the user as a
   real option, not just a fallback.

## RE follow-up on ENTRY identity fields, per next-step #1 of the "rotation" section above (2026-07-11, new session)

Picked up the standing question from the "RESOLVED (differently than expected)" section:
does the ENTRY object (the node `FUN_004a5450` returns) carry any directly-readable identity
field (steamId/lobbyId/similar), so tier data could be attributed to a player without relying
on positional correlation? Result: **inconclusive but not a dead end** - found a real,
concrete precedent for "list entries with an identity key" elsewhere in the binary, a
significant correction to how live diagnostic pointer values can be reused in static analysis,
and a clarification of the permutation-array mechanics, but did **not** conclusively identify
ENTRY's own concrete class/vtable or a confirmed identity field on it. New Ghidra scripts, all
under `docs/Research/ghidra_scripts/`, matching `run_ghidra_rankedlist_*.cmd` /
`*GhidraReport.txt` in `docs/Research/`: `DecompileRankedListMgrVtableAndEntry.py`,
`DecompileRankedListMgrConstructorSearch.py`, `DecompileRankedListClearFn.py`,
`DecompileRankedListPermArrayOtherReaders.py`, `DecompileRankedListEntryClassSearch.py`,
`DecompileCNetworkServerVtable.py`, `DecompileEntryIdResolveFn.py`,
`DecompileEntryIdWritePath.py`, `DecompileCNetworkServerCtorXrefs.py`,
`DecompileCNetworkServerSingletonGetter.py`.

### Important correction: live-logged pointer VALUES cannot be plugged directly into static Ghidra analysis - this binary has ASLR enabled

Tried the obvious first move: take the live-confirmed `rankedListMgrDiag` values from an
earlier session (`mgr=0x0438E480 vtable=0x00CFC3A4`) and look up `0x00CFC3A4` directly as a
Ghidra address, on the assumption (stated as fact earlier in this doc and in
`bbcf-re-workflow`'s SKILL.md: *"Image base is 0x00400000... maps directly to Ghidra address
0x00400000 + 0xXXXXXX"*) that this game's module always loads at a fixed base. **Reading that
exact address in the static Ghidra project returns all zero bytes** - not a plausible vtable.
Checked the actual PE header of `BBCF.exe` to confirm:

```
ImageBase 0x400000
DllCharacteristics 0x8140  ASLR (IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE, bit 0x40) is SET
```

**ASLR is enabled for this binary.** The RVA-based offset math this whole doc relies on
(`base + 0xXXXXXX`, computed at runtime via `GetBbcfBaseAdress()`/`GetModuleHandle`, exactly
like every hook in `hooks_bbcf.cpp`/`hooks_detours.cpp` already does) is completely unaffected
by this and remains correct - that's *how* ASLR-safe code is supposed to work. The mistake
this session almost repeated is different and narrower: **a raw pointer VALUE read live from
memory and printed in a log (e.g. a vtable pointer, or any other pointer-typed field) is only
a valid Ghidra static address if that specific process happened to load at the Ghidra
project's assumed base (0x00400000) for that session** - which cannot be assumed true in
general with ASLR on, and empirically was NOT true for the session that produced
`vtable=0x00CFC3A4`. **Static-vs-heap distinction still matters too**: `mgr=0x0438E480`
itself is a heap pointer (irrelevant to ASLR/image-base math either way, heap addresses aren't
meaningful in Ghidra at all), but `vtable=0x00CFC3A4` is supposed to be a static `.rdata`
address, and that's exactly the category ASLR relocates.

**Practical fix for any future session wanting to correlate a live pointer against Ghidra:**
have the live diagnostic ALSO log the module's own runtime base (`GetBbcfBaseAdress()`) on the
same line as any raw pointer value, so a later session can compute
`ghidra_addr = 0x00400000 + (live_ptr - live_base)` before looking anything up statically. This
was not done for the existing `rankedListMgrDiag` log line - **worth adding** if static
correlation of the MGR/ENTRY vtable is attempted again. Absent that, the only way to get a
Ghidra-comparable address for MGR/ENTRY's vtable is a live CDB session that reports the actual
loaded base directly (`lm` command), not a replay of an old DEBUG.txt.

### Clarified: the `+0xaf4` permutation array is a static IDENTITY table, never reshuffled - the "rotation" is NOT a reindexing of this array

`FUN_004a5430` (RVA `0xA5430`, immediately adjacent to the already-known `FUN_004a5450`
walker) decompiles to a trivial loop:

```c
void __fastcall FUN_004a5430(int param_1)
{
  int iVar1 = 0;
  int *piVar2 = (int *)(param_1 + 0xaf4);
  do {
    *piVar2 = iVar1;      // perm[i] = i
    iVar1 = iVar1 + 1;
    piVar2 = piVar2 + 1;
  } while (iVar1 < 0x32);  // 50 entries
}
```

This sets `+0xaf4[i] = i` for `i` in `0..49` - i.e. it (re)initializes the permutation array to
the **identity mapping**, not a real shuffle. A full raw-disasm-text sweep of the whole binary
for every literal `0xAF4`-offset memory operand (`tail -c +3 tools/bbcf_disasm_ascii.txt | grep
"0xaf4"`, 12 raw hits, most immediately excluded as unrelated immediate-constant false
positives like `850AF4h`/`960AF4h`) found **exactly one writer** (this identity-reset function)
and **five readers** (the already-known `FUN_004a7b40`/`FUN_004ac6c0`/`FUN_004ae6d0`, plus two
new sightings, `FUN_004a89d0` and one inside `FUN_004ac6c0` itself, both confirmed to be
ranked-match state-machine functions that resolve "the row the user just selected" to an ENTRY
via this same array - not writers). **No function anywhere in the disassembled binary writes a
non-identity value into `+0xaf4`.** (Caveat: a write expressed as e.g. `mov [eax+8], reg` where
`eax` was pre-computed as `param_1+0xaec` would be invisible to a literal `0xaf4` text grep -
this sweep cannot rule out that specific disguised form, only literal-operand writes.)

**This meaningfully sharpens the earlier "MAJOR NEW FINDING" root-cause writeup** (the session
that found the same 17 tier values rotating between different names one second apart with zero
real delivery in between). That section correctly concluded the row-index/identity assumption
was false, but left open *how* the rotation happens. Given the permutation array itself is
never reshuffled (per this session's sweep), the rotation must come from the **underlying
intrusive linked list nodes themselves changing** - i.e. list position `i`'s underlying node
(`head -> next -> next -> ...`) is not a stable "this row's entry, always" association; nodes
are most likely being **recycled/reassigned to a different lobby** by some other, not-yet-found
mechanism (a round-robin re-probe/re-sample cursor, consistent with the original hypothesis),
while the identity-index mapping (`+0xaf4`) stays pinned at `i -> i` throughout. **The write
side of the node list itself (whatever replaces/reorders the linked nodes matching indices to
different lobbies over time) was not located this session** - `FUN_004a5430` only touches the
permutation array, never `+0xae0` (list head) or the node `next`/`prev` fields at node`+4`/`+8`.

### Investigated GAME_CNetworkServer / GAMESTEAM_CNetworkServer as an ENTRY-class candidate - structurally related but NOT the ranked-list's own MGR/ENTRY (ruled out as literally the same object)

Since MGR's own concrete vtable address cannot be resolved statically (per the ASLR finding
above), tried a different angle: a namespace-aware keyword search (`sym.getName(True)`, the
same fix that found `CNetworkLobbyData` in an earlier session) plus a structural sweep of every
`::vftable` symbol in the whole binary for classes with 38+ resolvable slots (ENTRY is known,
from methods already seen in use, to have real vtable calls at offsets up to at least `0xb0`,
i.e. 45 slots minimum - too large for `CNetworkLobbyData`/`CSTEAMNetworkLobbyData`, already
confirmed-different in an earlier session at only 36 slots, mostly `__purecall`).

Found **`GAME_CNetworkServer::vftable`** (`0089c9f4`, 57 slots, all `__purecall` - an abstract
base) and its concrete sibling **`GAMESTEAM_CNetworkServer::vftable`** (`0089cb4c`, 57 slots,
same override-every-purecall-slot pattern already seen for `CSTEAMNetworkLobbyData` vs.
`CNetworkLobbyData`). Both are genuine RTTI-recovered names (Complete Object Locator + Type
Descriptor present, not a guess), and critically, `GAMESTEAM_CNetworkServer`'s own vtable slot
44 (`FUN_0046db40`, off `0xb0` - **one of the exact offsets ENTRY is confirmed to use**) does:

```c
undefined4 __fastcall FUN_0046db40(int param_1)
{
  ...
  for (iVar4 = 0; iVar4 < *(int *)(param_1 + 0xae8); iVar4++) {
    iVar2 = FUN_004a5450(iVar4);                 // <-- same generic list-walker!
    if (*(int *)(iVar2 + 0x110) != 0) {
      iVar3 = FUN_0046e9e0(*(undefined4 *)(*(int *)(iVar2 + 0x114) + 0xc),
                            *(undefined4 *)(*(int *)(iVar2 + 0x114) + 0x10));
      if (-1 < iVar3) { *(int *)(iVar2 + 0x78) = iVar3; *(undefined4 *)(iVar2 + 0x110) = 0; }
    }
  }
  return uVar1;
}
```

This is the exact same `+0xae8` count / `FUN_004a5450` walk shape as the confirmed ranked-list
chain - strong evidence `FUN_004a5450` is a **shared generic intrusive-list-walk utility**,
reused by multiple unrelated container classes throughout the binary, not something unique to
the ranked list's own MGR. `FUN_0046e9e0` is a genuinely interesting identity-keyed lookup:

```c
uint __thiscall FUN_0046e9e0(int param_1, int param_2, uint param_3)
{
  piVar3 = (int *)(param_1 + 8);
  // linear-search up to 20 rows (stride 0x1a dwords = 104 bytes) for a row whose
  // first two dwords match (param_2, param_3) - a 2-dword (64-bit-shaped) key
  while (piVar3[0x17] != 0 || *piVar3 != param_2 || piVar3[1] != param_3 || piVar3[0x18] == 0) {
    ... if (++count > 0x13) return 0xffffffff;   // 20-row cap
  }
  // once found: average 5 (low,high) dword-pairs at row+0xc, return a packed result
}
```

called as `FUN_0046e9e0(entry+0x114 -> [+0xc], entry+0x114 -> [+0x10])` - i.e. the ENTRY-like
object here carries a pointer (`+0x114`) to a sub-struct whose first two dwords (`+0xc`/`+0x10`
of that sub-struct) are used as a 2-dword identity key into a per-peer RTT-sample table. **This
is a real, concrete, already-shipped example in this codebase of "a list entry carrying an
identity key for cross-referencing," which is exactly the shape being searched for** - but
traced far enough to also rule it out as literally the same object:

- `GAMESTEAM_CNetworkServer`'s own vtable slot 7 (off `0x1c`, `FUN_0046e540`) is a connection
  handshake state-machine step (checks/advances a byte state machine at `this+0x25e0`) - **not**
  a simple "return a 0-7 byte, zero args" tier getter. Since ENTRY's slot 7 is independently,
  robustly confirmed (two separate raw-disassembly passes, see the earlier "RESOLVED" section
  above) to be exactly `MOV ECX,EDI; ... CALL [EAX+0x1c]` with **zero pushes** and a **single
  byte return**, and `FUN_0046e540` doesn't match that shape (it's `__fastcall`, does real
  branching, no simple byte return), **ENTRY's concrete class is NOT
  `GAMESTEAM_CNetworkServer`.**
- Traced both `GAME_CNetworkServer`/`GAMESTEAM_CNetworkServer` constructors
  (`FUN_004a53b0`/`FUN_0046c880`/`FUN_0046cdd0`/`FUN_0046c6e0`) and their own lazy-init/atexit
  guard chain (`0x008485a5` is a plain `JMP FUN_0046cdd0`, i.e. part of a static
  initializer/atexit table, not gated by `DAT_00c97e40` or reachable via `FUN_00486070`
  anywhere in their decompiled bodies) - **no reference to `DAT_00c97e3c`/`FUN_00486070`
  (the confirmed ranked-list MGR singleton) anywhere in this construction chain.** This is a
  separate static singleton for an unrelated subsystem - almost certainly the low-level
  network-session/QoS layer (`GAMESTEAM_QoSListner::vftable` is also assigned inside
  `FUN_0046cdd0`, and `FUN_0046e540`'s state values gate on `RMSR_CheckingRTT`-adjacent
  behavior), tracking *actual established* P2P connections (up to ~20 of them, matching
  `FUN_0046e9e0`'s 20-row cap - plausible as "concurrently probed/connected peers", not
  "everyone in a 32+ entry search-results list").

**Net result of this sub-investigation: confirms `FUN_004a5450` is a shared utility (so its
presence elsewhere isn't evidence of shared identity), and demonstrates the "identity key on a
list entry" pattern is a real, precedented shape in this codebase's own network code - but does
NOT identify the ranked-list ENTRY's own concrete class or a confirmed identity field on it.**
The search for ENTRY's own vtable/class via keyword search (`RankMatch`, `SearchResult`,
`RoomEntry`, `Entry`, etc., all namespace-aware) came back with only generic/unrelated matches
(no `GhidraClass` namespace whose name obviously matches "the ranked search-result row" was
found) - either the class has an unhelpful generic name not caught by these keywords, or its
RTTI was stripped/not recovered by Ghidra's analyzer for this specific class.

### Answer to the original task question: viable lead, not a dead end, but NOT resolved this session

**Task item 1 (identity field on ENTRY):** not confirmed, not disproven. The one closest-shaped
precedent found (`GAMESTEAM_CNetworkServer`'s `+0x114`-indirected 2-dword key) belongs to a
different object, not ENTRY. **Task item 2 (steamId-shaped 8-byte value via a known Steam API
caller)**: not found this session - no direct call site could be traced from ENTRY's own vtable
slots to a `ISteamFriends`/`ISteamMatchmaking` import, because ENTRY's concrete vtable/class was
never statically resolved (the ASLR blocker above prevents dumping it from a live pointer, and
the keyword/structural search for its class name came back empty). **Task item 3 (what
rebuilds/invalidates the list causing the "rotation")**: partially answered - ruled out the
permutation array as the mechanism (confirmed static identity table, never reshuffled), which
narrows the real cause to "the underlying node list itself mutates" but the actual writer of
that mutation was not located.

**Concrete next steps, in order of value:**
1. **A live CDB breakpoint is now the clearly highest-value next step, more so than before** -
   not just for confirming the tier-getter's calling convention (already done, high confidence)
   but because it is now the *only* way to get a Ghidra-comparable address for ENTRY's vtable at
   all, given the ASLR finding above. Breakpoint at the tier-getter call site
   (`FUN_00661060`'s `CALL EAX` at RVA `0x261872`/Ghidra `0x00661872`, or equivalently
   `FUN_004a5450` RVA `0xA5450`) while a real populated ranked list is on screen; on hit, dump
   `ECX`/`this` (the ENTRY pointer), `[ECX]` (its vtable pointer), and the process's actual
   loaded base (`lm` in CDB) so the vtable address can be converted to a real Ghidra RVA
   (`ghidra_addr = 0x00400000 + (live_vtable - live_base)`) and looked up/dumped for real. This
   single step would unblock: ENTRY's concrete class name, its full vtable (all 45+ slots,
   letting the remaining unexplored slots like `+0x00/+0x08/+0x0c/+0x14/+0x18/+0x28/+0x2c` etc.
   be checked for a string/steamId getter), and a raw memory dump of the object itself (for
   task item 2's raw-offset approach, scanning for a Steam64-ID-shaped 8-byte value directly).
2. **If a live session is set up for step 1 anyway**, also dump raw bytes of the ENTRY object
   itself (e.g. `dd <entryptr> L40` in CDB) at the same breakpoint hit, independent of the
   vtable-call approach - a direct memory scan for a plausible Steam64 ID pattern (high dword
   often `0x0110xxxx`-shaped for individual accounts, matching a real friend/lobby-member ID
   already known from the same session) could shortcut past needing the vtable at all.
3. **Separately, worth adding to the next diagnostic build regardless of outcome**: have
   `DiagnosticLogRankedListMgrSlot()` (or a new diagnostic) also log `GetBbcfBaseAdress()` once
   per session-start, so any future live pointer values logged (mgr, vtable, or anything else)
   can be converted to a real Ghidra RVA after the fact without needing a fresh CDB session just
   to learn the base - this session's ASLR discovery means every previously-logged raw pointer
   value in old `DEBUG.txt` files (not just this feature's) is a session-specific number, not a
   reusable static address.
4. If, after a live CDB pass, ENTRY's class turns out to have no plausible identity field at
   all (every remaining slot is confirmed non-identity, and a raw memory scan finds nothing
   steamId-shaped), then per the earlier session's own fallback plan: abandon `gameTier`-based
   sorting for good (same fate as the three earlier dead-end candidates) and stabilize on the
   already-working `probeElapsedMs`-only sort - this remains a reasonable, honest place to stop
   if identity correlation turns out to be unsolvable via static+live RE effort actually spent.

## Live CDB debugging attempted, twice crashed the game - DO NOT set a breakpoint on this function again (2026-07-12)

Per user request, attempted live CDB attach to find an identity field on the ENTRY object (see
previous section's next-step #1), since static RE alone couldn't resolve it. Full writeup for
future agents:

**Setup that worked fine, repeatedly, with zero issues:**
- `cdb.exe` (x86, from Windows Kits 10 Debugging Tools) requires elevation (SeDebugPrivilege) to
  attach to another process - the user's shell runs under UAC split-token (admin group present
  but deny-only), so plain attach fails with Win32 error 5 (access denied). Fix: launch cdb via
  `Start-Process -Verb RunAs` from PowerShell, which silently elevated without an interactive UAC
  prompt in this environment (`ConsentPromptBehaviorAdmin=5`, i.e. simple consent, and it went
  through without visibly blocking - possibly cached/pre-approved for this binary this session).
- `-pv` (noninvasive attach) CANNOT resume execution (`g` fails with "No runnable debuggees") -
  it's read-only-ish snapshot access, not a real controllable session. Use plain `-p <pid>` for
  anything requiring `g`/breakpoints - this DOES suspend all threads at attach until `g` runs.
- **Critical safety flag: always pass `-pd`** (auto-detach instead of kill on debugger exit). The
  FIRST crash below happened specifically because `-pd` was omitted - killing cdb via `taskkill`
  without it caused Windows to tear down the debuggee along with the dying debugger (default
  "kill on close" behavior of the underlying debug object). With `-pd` set, an isolation smoke
  test (attach, `lm m BBCF`, `qd`) completed cleanly and the game was confirmed unaffected -
  proving the attach/detach round-trip itself, with NO breakpoint involved, is safe.
- Module-relative addressing gotcha: `bp BBCF+0xA5450` is WRONG - cdb's MASM evaluator reads
  "BBCF" as the hex literal `0xBBCF`, not the module name, giving a garbage address. Always
  compute the explicit numeric base first via `lm m BBCF` (confirmed `00860000` in this session -
  matches the ASLR finding from the previous RE pass) and use `00860000+0xA5450` instead.
- With that fixed, `u 00860000+0xA5450` and `u 00860000+0x261060` disassembled EXACTLY matching
  the known decompiled signatures of `FUN_004a5450` and `FUN_00661060` respectively - strong
  independent confirmation the RE work's addressing has been correct all along.

**What went wrong, twice, both times ONLY when an actual breakpoint was set and hit (never
during plain attach/read/detach):**
1. First attempt: script got the process attached but appeared to stall long before reaching the
   scripted commands (large ModLoad enumeration for ~150 DLLs takes noticeably longer under full
   invasive attach than it did during the `-pv` smoke test). An impatient external Bash-level
   `timeout` killed the wrapper process, orphaning the elevated `cdb.exe` (Start-Process -Verb
   RunAs launches a detached process tree the timeout couldn't reach) - still holding the game
   suspended. Force-killing that orphaned `cdb.exe` via `taskkill` (without `-pd` set at the time)
   caused the game to terminate - the exact "kill on debugger exit" default described above.
2. Second attempt (this session): fixed both issues above (`-pd` set, and used a `Monitor`-based
   wait instead of an external timeout, patiently waiting ~8+ minutes without touching anything).
   The attach itself proceeded fine and the one-shot breakpoint (`bp /1 00860000+0xA5450 "..."`)
   WAS successfully hit (`g` resumed execution, the breakpoint fired) - but shortly after, the
   game's own crash log shows `Critical error detected c0000374` (`STATUS_HEAP_CORRUPTION`)
   followed shortly by `verifier.dll` loading and then an actual access violation
   (`c0000005`) inside `ntdll!RtlpAllocateHeap`. The debugger itself detached cleanly via `qd`
   this time (confirmed in the log) - but the game had already fatally corrupted its own heap by
   that point and was gone from the process list moments later.

**Conclusion: setting a software breakpoint (INT3 write) on `FUN_004a5450` and letting it actually
fire reliably crashes this game via heap corruption**, independent of the debugger's own
cleanup/detach behavior (which worked correctly the second time). This looks like the game (or
Steam's anti-tamper/crash-handling layer - `steamclient.dll`'s `crashhandler.dll` and
`Breakpad_SteamWriteMiniDumpUsingExceptionInfoWithBuildId` appeared in nearby stack context) reacts
badly to code-section modification or to a live INT3 hit in this specific function, triggering
its own heap-corruption detection. **Do not set a breakpoint on this function (or likely any
function in this call chain) again** - the risk is confirmed, not hypothetical, 1-for-1 across the
only real attempt that got far enough to actually hit the breakpoint. Live CDB debugging of this
specific chain is now considered CLOSED, not just paused - any future identity-field
investigation must be done statically or via the mod's own existing safe call chain (see next
section), not via an attached breakpoint.

## Safer alternative found: get the ENTRY's vtable RVA from the mod's own already-safe call chain (2026-07-12)

Key realization: the mod's OWN code has been calling this exact walk (`FUN_004a5450` + the
entry's vtable slot 7 tier getter) across many prior test sessions with ZERO crashes - only the
external CDB breakpoint ever destabilized the game. This means the entry's own vtable pointer
(already read as `entryVtable` inside `DiagnosticLogRankedListMgrSlot`'s per-row loop, previously
computed only to validate the `+0x1c` slot and then discarded) can be safely logged, giving a
live vtable address with **zero live-debugger risk** - the RVA (`entryVtable - moduleBase`) can
then be looked up directly in the static Ghidra project to identify ENTRY's concrete class and
examine its other fields/vtable slots for an identity value (steamId or similar), entirely via
the user's preferred static-analysis-plus-DEBUG.txt workflow.

**Implemented this session:** re-enabled the per-row loop in `DiagnosticLogRankedListMgrSlot` for
a small, bounded row count (`kIdentityProbeRowCount = 3` - NOT the full `kMaxDiagnosticRows`, to
keep this identity-focused probe minimal) and added `entryVtableRva` to the per-row log output:
`[row]=tier(Name,vtableRva=0xXXXXXX)`. Build verified clean (Debug|Win32). The full name/gameTier
correlation loop used by `PollGameTiers` for actual sorting remains disabled (per the "rotation is
a mod-side mapping bug" finding) - this re-enabled loop is ONLY for identity investigation via the
logged RVA, not for reviving the broken sort-by-tier feature.

**Next step:** get a fresh `DEBUG.txt` from a session with a populated ranked list, grep for
`rankedListMgrDiag`, and take the `vtableRva=0x...` value from any row. Feed that RVA into a
Ghidra decompile pass (`0x00400000 + RVA` = Ghidra address) to identify the entry's concrete class
and thoroughly examine ALL its fields/other vtable slots (not just slot 7) for anything
identity-shaped (a steamId, lobbyId, or similar) - this is the same investigation the previous RE
agent attempted but couldn't complete, because it never had a live vtable address to anchor the
search (only a stale, ASLR-invalidated one from an old log). This directly answers whether the
tag-based filter/sort feature is truly a dead end or has one more viable path.

## ENTRY vtable resolved via static Ghidra analysis - class identified, and a genuinely promising identity-key field found (2026-07-12, new session)

Per the standing next-step ("get the ENTRY's vtable RVA from the mod's own already-safe call
chain" - the CDB-breakpoint path is CLOSED, see the two sections above, do not revisit), this
session took the freshly-logged, stable `entryVtableRva=0x49cc34` value from `DiagnosticLogRankedListMgrSlot`'s
per-row loop and resolved it statically. **Pure Ghidra static analysis this whole session - no
live debugger, no CDB, zero crash risk.**

### Arithmetic correction before anything else: the RVA-to-Ghidra-address math in the task
hand-off was wrong

The task briefing computed `0x00400000 + 0x49cc34 = 0x0049CC34` - **that arithmetic is simply
incorrect** (adding a positive number to `0x00400000` cannot yield a result smaller than the
larger addend's own leading digits unless there's a typo). The correct sum, confirmed via
`python3 -c "print(hex(0x00400000 + 0x0049CC34))"`, is **`0x0089CC34`** (the `4+8=C` nibble
carries). This was caught immediately by inspecting what actually lives at each address:
`0x0049CC34` sits mid-function inside `.text` (`FUN_0049cb90`, real disassembled instructions,
not a vtable), while `0x0089CC34` sits inside `.rdata` (`0x0084a000-0x009d2600`, the same section
every other confirmed vtable in this doc lives in) and resolves cleanly via RTTI. **Whoever reads
a future `vtableRva=0x...` value out of `DEBUG.txt` for this or any similar diagnostic: always
recompute `0x00400000 + rva` with an actual calculator/Python one-liner before treating the sum as
known-good - do not eyeball hex addition, this session almost repeated the exact mistake the task
briefing made.**

### ENTRY's concrete class: `GAMESTEAM_SearchResultNode` (RTTI-confirmed)

At Ghidra address `0x0089CC34`:

- Symbol: `GAMESTEAM_SearchResultNode::vftable`.
- RTTI Complete Object Locator at `vtable-4` (`0x0089CC30`, standard MSVC layout) resolves to
  `GAMESTEAM_SearchResultNode::RTTI_Complete_Object_Locator` /
  `GAMESTEAM_SearchResultNode::RTTI_Type_Descriptor` - a real, non-guessed, demangler-recovered
  name, not a keyword-search inference.
- Constructor `FUN_0046f680`: `operator_new(0x118)` (**object size 0x118 = 280 bytes**), then
  `*puVar2 = GAME_SearchResultNode::vftable;` followed immediately by
  `*puVar2 = GAMESTEAM_SearchResultNode::vftable;` (the classic MSVC two-stage vtable assignment
  for a class with a base - `GAME_SearchResultNode` is the abstract base, `GAMESTEAM_SearchResultNode`
  the concrete override, same pattern already seen for `CNetworkLobbyData`/`CSTEAMNetworkLobbyData`
  and `GAME_CNetworkServer`/`GAMESTEAM_CNetworkServer` in earlier sessions - this game's whole
  network layer follows this `GAME_X` abstract / `GAMESTEAM_X` concrete naming convention).
- Field-initializer `FUN_0046dc00` (called right after vtable assignment) zeroes every field from
  `+0xc` through `+0x114` explicitly, one store per line - this **is** a full, high-confidence
  memory-layout map of the object (reproduced in full below), not a guess from partial vtable
  coverage.

### Full vtable dump (24 slots, ALL real implementations - no `__purecall`, unlike the earlier
`CNetworkLobbyData` abstract base)

| slot | off | fn | body | field meaning (cross-checked against `FUN_00661060`'s known call sites) |
|---|---|---|---|---|
| 0 | 0x00 | `FUN_0046cf80` | scalar-deleting destructor | - |
| 1 | 0x04 | `FUN_0070e400` | `return *(int*)(this+0xc);` | **validity/type flag** - confirmed, this is the exact call `FUN_00661060` uses as `iVar8 = (**(*piVar7+4))()` to decide "no room"/invalid slot |
| 2 | 0x08 | `FUN_0046ea70` | `return CONCAT44(_DAT_00000004,_DAT_00000000);` | reads absolute addr 0/4 - degenerate/unimplemented-looking, not identity-shaped |
| 3 | 0x0c | `FUN_007587d0` | `return this+0x10;` | returns the ADDRESS of `+0x10`, not a value - likely a sub-object/name-buffer accessor |
| **4** | **0x10** | **`FUN_0046e860`** | **`iVar1=*(int*)(this+0x114); out[0]=*(int*)(iVar1+0xc); out[1]=*(int*)(iVar1+0x10);`** | **THE IDENTITY-KEY CANDIDATE - see below** |
| 5 | 0x14 | `FUN_0046e9b0` | `return *(byte*)(this+0x60);` | small state byte |
| 6 | 0x18 | `FUN_00668180` | `return 0;` | constant stub |
| 7 | 0x1c | `FUN_0046e880` | `return *(byte*)(this+0x74);` | **CONFIRMED tier getter** (already live-verified across many prior sessions - this is the exact byte read by the game's own `net_col_def/A-G.hip` icon selection) |
| 8 | 0x20 | `FUN_0046e930` | `return *(byte*)(this+0x6c);` | raw member count - matches `FUN_00661060`'s slot-8 call |
| 9 | 0x24 | `FUN_0046e940` | `return *(byte*)(this+0x6d);` | member-count-format companion (max/capacity) - matches slot-9 call |
| 10 | 0x28 | `FUN_0046e9c0` | `return *(byte*)(this+0x6e);` | byte, adjacent to member-count fields |
| 11 | 0x2c | `FUN_0046e9d0` | `return *(byte*)(this+0x6f);` | byte, adjacent to member-count fields |
| 12 | 0x30 | `FUN_0046e890` | `return *(int*)(this+0x78);` | **written by `FUN_0046db40`** (the identity-key resolver, see below) - an RTT/connection-table-slot index once resolved, `0xffffffff` sentinel until then (matches `FUN_0046dc00`'s `*(this+0x78)=0xffffffff` init) |
| 13 | 0x34 | `FUN_00439970` | `return *(int*)(this+0x64);` | dword |
| 14 | 0x38 | `FUN_00439980` | `return *(int*)(this+0x68);` | dword |
| 15 | 0x3c | `FUN_0046e9a0` | `return *(short*)(this+0x7c);` | word |
| 16 | 0x40 | `FUN_0046e970` | `return *(short*)(this+0x7e);` | word |
| 17 | 0x44 | `FUN_0046e960` | `return *(short*)(this+0x80);` | word |
| 18 | 0x48 | `FUN_0046e950` | `return *(short*)(this+0x82);` | word - `+0x7c/0x7e/0x80/0x82` is a word-quad, plausible per-team/per-slot level or ping values (candidate for the "closest/furthest level" sort mode's own field, not yet cross-checked) |
| 19 | 0x4c | `FUN_0046e980` | `return *(int*)(this+0x84+idx*4);` | **array accessor**, base `+0x84` |
| 20 | 0x50 | `FUN_0046e8b0` | `return *(short*)(this+0x5a);` | word |
| 21 | 0x54 | `FUN_0046e840` | `return *(short*)(this+0x5c);` | word |
| 22 | 0x58 | `FUN_0046e850` | `return *(short*)(this+0x5e);` | word - `+0x5a/0x5c/0x5e` word-triplet |
| 23 | 0x5c | `FUN_0046e8a0` | `return *(short*)(this+0x10a);` | word, near the very end of the 0x118-byte object |

### The identity-key candidate: slot 4 (`FUN_0046e860`, offset 0x10) and its corroboration

```c
// slot 4 - offset 0x10 in GAMESTEAM_SearchResultNode::vftable
void __thiscall FUN_0046e860(int param_1, undefined4 *param_2)
{
  int iVar1;
  iVar1 = *(int *)(param_1 + 0x114);      // follow ENTRY+0x114 (a pointer, null until resolved)
  *param_2 = *(undefined4 *)(iVar1 + 0xc);      // out[0] = sub-object+0xc
  param_2[1] = *(undefined4 *)(iVar1 + 0x10);   // out[1] = sub-object+0x10
}
```

`ENTRY+0x114` is a **pointer field**, explicitly zero-initialized by the field-initializer
(`FUN_0046dc00`: `*(int*)(param_1+0x114) = 0;` is its very last store) - so it is **null until
some other event populates it**, not always safe to read.

**This exact 2-dword shape is independently corroborated from two other, already-decompiled
functions in this investigation (one from this session, one re-confirming an earlier session's
find):**

1. **`FUN_0046db40`** (this session's re-decompile; this is the SAME function an earlier session
   found as `GAMESTEAM_CNetworkServer`'s own vtable slot 44, `off 0xb0` - it turns out to ALSO be
   the resolver that walks the ranked-list's own row list):
   ```c
   undefined4 __fastcall FUN_0046db40(int param_1)
   {
     ...
     for (iVar4 = 0; iVar4 < *(int *)(param_1 + 0xae8); iVar4++) {   // same +0xae8 row COUNT
       iVar2 = FUN_004a5450(iVar4);                                   // same list walker -> ENTRY ptr
       if (*(int *)(iVar2 + 0x110) != 0) {                            // "needs resolution" flag
         iVar3 = FUN_0046e9e0(*(undefined4 *)(*(int *)(iVar2 + 0x114) + 0xc),
                               *(undefined4 *)(*(int *)(iVar2 + 0x114) + 0x10));
         if (-1 < iVar3) {
           *(int *)(iVar2 + 0x78) = iVar3;         // <-- writes slot 12's field (confirmed above)
           *(undefined4 *)(iVar2 + 0x110) = 0;     // clear "needs resolution"
         }
       }
     }
   }
   ```
   This runs over the exact same row list (`+0xae8` count, `FUN_004a5450` walker) our ranked-list
   tier chain already uses - i.e. it is the **connection-quality resolver for this exact row
   object**, not an unrelated class as an earlier session guessed. It reads ENTRY's `+0x114`
   sub-object's `+0xc`/`+0x10` pair as a 2-dword key into `FUN_0046e9e0`, and on success stores
   the resolved index into ENTRY`+0x78` (slot 12).

2. **`FUN_0046e9e0`** (already found in an earlier session, re-confirmed unchanged this session):
   linear-searches up to 20 rows (stride `0x1a` dwords = 104 bytes) of a per-peer sample table for
   a row whose **first two dwords match the caller's 2-dword key exactly**, then averages 5
   `(low,high)` dword-pairs from that row - a per-peer RTT/ping sample cache, keyed by this exact
   2-dword identity.

3. **`FUN_0046e790`** (a `GAMESTEAM_CNetworkServer` method - the *already-connected* P2P session
   object, not the search-result row) calls the **same** `FUN_0046e9e0` with its **own** fields as
   the key:
   ```c
   uVar1 = *(undefined4 *)(param_1 + 0xc7c);
   uVar2 = *(undefined4 *)(param_1 + 0xc78);
   ...
   uVar5 = FUN_0046e9e0(uVar2, uVar1);
   ```
   i.e. `GAMESTEAM_CNetworkServer+0xc78`/`+0xc7c` hold the **identical 2-dword key shape**, read
   directly off the connection object itself (no indirection through a `+0x114`-style pointer -
   makes sense, since a `CNetworkServer` is a live, already-resolved connection to one specific
   peer, unlike a not-yet-joined search-result row).

4. **`FUN_0046f720`** (the writer of new rows into the same 20-slot table `FUN_0046e9e0` reads)
   is called from **four** places, confirmed via `getReferencesTo`, and **every single call site
   passes the same `(param_1+0xc78, param_1+0xc7c)` pair** off a `GAMESTEAM_CNetworkServer`-shaped
   object as the new row's key (`FUN_0046d0a0`, `FUN_0046d370`, `FUN_0046e790`, `FUN_00470240` -
   all four are P2P-connection state-machine functions, matched via the `param_1+0x25e0` state
   byte already known from earlier sessions to be `GAMESTEAM_CNetworkServer`'s own phase field).

**Conclusion: `+0xc78`/`+0xc7c` on the connection object and `(+0x114)->+0xc`/`+0x10` on the
search-result row are the SAME identity key, used by the game itself to correlate "this
not-yet-joined search-result row" with "this already-established P2P connection" for exactly the
purpose of carrying RTT/connection-quality data between the two subsystems.** This is a real,
game-authored cross-referencing mechanism - not a coincidental shape match. Given (a) this is a
Steamworks-integrated title, (b) `GAMESTEAM_CNetworkServer` objects are explicitly per-remote-peer
P2P sessions (their entire purpose is "my connection to one specific Steam user"), and (c) a
64-bit value split as two adjacent dwords is exactly `CSteamID`'s native in-memory representation,
**the leading hypothesis is that this 2-dword pair IS the peer's Steam64 ID** - though this
specific pass did not trace back far enough to find the literal write site of
`CNetworkServer+0xc78/+0xc7c` (i.e. did not catch it being set directly from a
`P2PSessionRequest_t::m_steamIDRemote` or similar Steamworks callback param) to prove the bit
layout beyond doubt.

**Even if it turns out NOT to be litereally `CSteamID`'s bit pattern**, it is still, on the
strength of the evidence above, a **stable, game-authored, per-peer identity key already used by
the game's own code to correlate the search-result row across subsystems** - which is exactly
what's needed to fix the "rotation" mapping bug (see the "RESOLVED (differently than expected)"
section much earlier in this doc: the mod's positional row-index assumption was proven false, and
this doc's own next-step #1 asked for exactly this kind of directly-readable identity field on
the entry object). Reading this 2-dword value per row instead of relying on position would let
the mod correctly attribute `gameTier` data to the right player regardless of whatever internal
list-node reshuffling causes the positional "rotation."

### Confidence and risk assessment for adding a live read

**Confidence: medium-high that this field is a genuine, usable per-peer identity key; medium
(not yet proven) that it is bit-for-bit the Steam64 ID.** Backed by: RTTI-confirmed concrete
class, a full zero-init field map from the real constructor (not guesswork), and three
independent decompiled call sites (one from this session, two corroborating from earlier
sessions) all converging on the same 2-dword key shape being shared between the not-yet-joined
row object and the already-connected P2P session object specifically for cross-subsystem
identity correlation.

**Risk of adding a live read: very low, LOWER than the already-shipped tier-byte read.** Two ways
to do it, in order of preference:

1. **Safest - direct pointer-chase, no virtual call at all:** `entry+0x114` is a plain pointer
   field (not a vtable call), so reading it needs no calling-convention guessing whatsoever:
   ```cpp
   const void* const idSubObj = *reinterpret_cast<void* const*>(
       reinterpret_cast<const uint8_t*>(entry) + 0x114);
   if (idSubObj != nullptr && !IsBadReadPtr(idSubObj, 0x14)) {
       const uint32_t idLow  = *reinterpret_cast<const uint32_t*>(
           reinterpret_cast<const uint8_t*>(idSubObj) + 0xc);
       const uint32_t idHigh = *reinterpret_cast<const uint32_t*>(
           reinterpret_cast<const uint8_t*>(idSubObj) + 0x10);
   }
   ```
   Must handle `idSubObj == nullptr` gracefully (skip that row this poll cycle) - it is null until
   `FUN_0046db40`'s resolution runs at least once for that row, same "benefit of the doubt while
   unresolved" pattern the filter's reputation model already uses elsewhere.
2. **Alternative, if the direct read above ever looks suspicious:** call vtable slot 4 the proper
   way (`this=entry`, one explicit output-buffer argument of 8 bytes, thiscall) - this is a real
   virtual call with an explicit argument (unlike slot 7's zero-arg case already solved), so if
   this route is taken it deserves the same raw-disassembly double-check slot 7's `uVar10`
   mystery-argument got before trusting it (see the "RESOLVED: `uVar10`..." section above) - but
   given option 1 needs no virtual call and no argument-count guessing at all, it should be tried
   first.

### Concrete next steps, in order

1. **Add the direct pointer-chase read (option 1 above) to `DiagnosticLogRankedListMgrSlot`'s
   per-row loop**, logging the two dwords alongside the already-logged tier/name/vtableRva, e.g.
   `[row]=tier(Name,vtableRva=0x..,idLow=0x..,idHigh=0x..)`. Build, deploy, get a fresh
   `DEBUG.txt` from a populated ranked-list session.
2. **Cross-check `idHigh`** against the individual-account Steam64 ID pattern (`0x0110xxxx`-shaped
   high dword) for at least one row - if it matches for players whose Steam friend/lobby-member ID
   is independently known (e.g. a friend already on the list, or cross-referenced against the
   already-logged `owner=<steamId>` from `SortShownCandidates`'s diagnostic for a peer that
   happens to already be a resolved/reachable candidate), that would be a strong direct
   confirmation this literally is the Steam64 ID, not just an opaque game-internal key.
3. **Whether or not step 2 proves the literal Steam64-ID hypothesis**, if the `(idLow, idHigh)`
   pair is confirmed **stable per on-screen player and distinct between different players** (the
   minimum bar for "usable as an identity key," regardless of its exact semantic meaning), wire it
   into `PollGameTiers()` as the correlation key instead of positional row index - this directly
   fixes the "rotation" mapping bug from the "RESOLVED (differently than expected)" section, and
   would finally make the real `gameTier` data safely attributable to the correct player.
4. **If `idSubObj` is null for most/all rows** even on a populated, fully-loaded list (i.e. the
   resolution event that populates `+0x114` rarely or never fires for a not-yet-probed row), this
   candidate is weaker than hoped - it would only become useful for rows the mod has ALSO already
   established (or is establishing) its own P2P probe connection to, which is a smaller subset than
   "every visible row." Still worth checking - even partial coverage (only entries our own prober
   is already talking to) may be enough to validate the tier-averaging data for the subset that
   matters most (the ones the sort is trying hardest to rank).
5. New Ghidra scripts from this session (kept for reuse), all under
   `docs/Research/ghidra_scripts/`, matching `run_ghidra_*.cmd` / `*GhidraReport.txt` in
   `docs/Research/`: `DecompileRankedListEntryVtable.py` (namespace-aware vtable resolver +
   full-slot decompiler + constructor-xref finder - reusable template for any future "resolve a
   live-logged vtable RVA to a real class" investigation), `InspectEntryVtableAddr.py` /
   `CheckVtableSections.py` (quick section/byte inspectors, useful for sanity-checking an address
   actually looks like a vtable before trusting a decompile of it - would have caught this
   session's arithmetic slip immediately if run first), `DecompileSearchResultNodeIdentity.py`,
   `DecompileNetworkServerIdWriters.py`.

## Identity-key candidate wired into the diagnostic (2026-07-12, same day)

Implemented next-step #1 from the section above. `DiagnosticLogRankedListMgrSlot`'s per-row loop
now also does the direct pointer-chase read (`entry+0x114` -> dereference -> `+0xc`/`+0x10` dwords)
exactly as specified - no virtual call, guarded with `IsBadReadPtr` at each step, gracefully
skipping (logging `id=unresolved`) when the sub-object pointer is still null. Also bumped
`kIdentityProbeRowCount` from 3 to 10 to get better cross-player signal in one read (still a small
bounded subset, not the full list - this is an identity-investigation probe, not the production
path). Log format: `[row]=tier(Name,vtableRva=0x..,id=<16 hex digits, idHigh then idLow>)` or
`[row]=tier(Name,vtableRva=0x..,id=unresolved)`. Build verified clean (Debug|Win32).

**Next step for whoever picks this up:** get a fresh `DEBUG.txt` from a populated ranked-list
session and check, across multiple rows/refreshes: (a) does `id=` ever show as resolved (non-
"unresolved") at all, (b) is the same on-screen player's `id=` value stable across repeated reads
and different from other players' (the minimum bar for a usable identity key, regardless of
whether it's literally the Steam64 ID), (c) does the high dword ever match the `0x0110xxxx`-shaped
individual-account pattern for a player whose real steamId is independently known from the
`sort order` log's `owner=` field (cross-reference by name). If (b) holds, this is ready to wire
into `PollGameTiers()` as the real correlation key, finally fixing the rotation/mismapping bug.

## CONFIRMED: the identity field IS the real Steam64 ID, byte-for-byte (2026-07-12, same day)

Fresh `DEBUG.txt` (md5sum-verified) from a populated ranked-list session, refreshed multiple
times, gave 15 distinct resolved `id=` values. **Every single one shares the exact high dword
`01100001`** - precisely the standard Steam64 encoding (universe=public, account type=individual)
- already a strong signal on its own. Went further and cross-referenced two decoded values
directly against the already-logged `sort order`/`owner=` steamIds by name:

- `id=011000014f146cf4` -> decimal `76561199287004404` -> **exact match** for
  `owner=76561199287004404 name="BrotherHoodBR12"` in the same session's `sort order` log.
- `id=011000010b1d4745` -> decimal `76561198146733893` -> **exact match** for
  `owner=76561198146733893 name="ItJustWorksButDoesn't"`.

**This is conclusive: the field at `entry+0x114 -> +0xc/+0x10` is the peer's literal, real Steam64
ID.** Not a game-internal opaque key merely shaped like one - an exact, verified match against
already-independently-known steamIds.

**Bonus finding that further validates this, and separately reconfirms an old bug:** the SAME
diagnostic line's *name* field (still using the old, already-known-broken positional
`m_reachableLobbies[row] -> candidate.ownerName` lookup) showed `id=011000010f3b0d09` mislabeled
as "ItJustWorksButDoesn't" at one point in the session. Decoded, that ID is `76561198215793929`,
which the `sort order` log shows actually belongs to **"FlaqJak"**, a different player entirely.
This is exactly consistent with everything already established: the position-based name
correlation is unreliable (as proven earlier), while **the new ID field - read directly from the
entry object itself, with no positional assumption at all - is accurate every time.** This mismatch
is further proof the fix works, not evidence against it: it shows precisely why position-based
correlation must be replaced, and that the ID field is immune to the exact failure mode that broke
everything until now.

**This closes out the investigation cleanly:** the tag-based sort/filter feature is confirmed
FEASIBLE. The only remaining work is wiring the ID as the real correlation key in place of
position, in `PollGameTiers()` and `BuildCompactedListAndDeliver`'s tier-attribution logic (see
next-steps #3 in the section above, now unblocked) - implemented in the very next commit after
this one, see below.

## Real fix implemented: PollGameTiers now keys tier data by the real Steam64 ID instead of row position (2026-07-12, same day)

Rewired the whole gameTier collection path to use the confirmed-accurate identity field instead of
positional correlation:

- `PollGameTiers()` no longer assumes `m_reachableLobbies[row]` tells us who's at that row. Instead,
  for each row it does the SAME `entry+0x114 -> +0xc/+0x10` pointer-chase already validated above,
  builds a `uint64_t steamId = (uint64_t)idHigh << 32 | idLow`, and only proceeds if the sub-object
  was resolved (non-null) - skipping unresolved rows entirely rather than guessing.
- Removed the old `rowCount == m_reachableLobbies.size()` gate that was papering over the
  positional mismatch - it's no longer needed since correctness no longer depends on row order
  matching delivery order at all. This also means gameTier collection now works even during the
  brief post-delivery lag window where the count temporarily mismatched (previously skipped
  entirely).
- The tier value is stored the same way as before (`m_verdicts[steamId].gameTier` +
  `gameTierAverage` cumulative averaging) - only the *lookup* changed, not the storage/averaging
  logic, which was never the broken part.
- `DiagnosticLogRankedListMgrSlot`'s per-row loop is UNCHANGED (still logs the old, known-unreliable
  name alongside the new reliable id, specifically so future sessions can keep using the mismatch
  as a live sanity check that the id field remains trustworthy where the name field is not).

Build verified clean (Debug|Win32). **Not yet live-tested** - next step is a fresh `DEBUG.txt` +
visual confirmation that `BestConnection`/`WorstConnection` sort now actually reflects the game's
real tier consistently, since the correlation bug that blocked this for the entire investigation
should now be fixed.

## Trivial but total bug found: PollGameTiers() was never actually being called (2026-07-12, same day)

User tested the identity-key fix and reported `BestConnection` still not ordering correctly -
players known to be tier 4 still showing in random middle positions. Fresh `DEBUG.txt`
(md5sum-verified) showed the real bug immediately: **every single `sort order` line all session
(37 of 37) had a positive `key=` value - the `probeElapsedMs` fallback - never once the negative,
`gameTierAverage`-based key.** Cross-checked that the identity chain itself WAS working fine in
parallel (`DiagnosticLogRankedListMgrSlot`'s `id=` values decoded and matched real, currently-
visible players by name, e.g. `76561198102157404` = "gigantic father", `76561198256402225` =
"Vermi" - both present in the same session's `sort order` log) - so the fix from earlier this
session was structurally correct, but its effect was never reaching the sort at all.

**Root cause: `PollGameTiers()`'s call site in `OnSteamCallbacksPump()` was still commented out**
from the much-earlier isolation test (see "MAJOR NEW FINDING"/isolation-test sections). That test
disabled the call to check whether the mod's own polling was causing the "rotation" bug: the
verdict came back "no, it's a mod-side positional-mapping bug" (see "RESOLVED (differently than
expected)"), and `PollGameTiers()` was subsequently rewritten to use the confirmed-correct
identity-key lookup instead of position - but the call site itself was never uncommented
afterward. So for this whole session (and the identity-verification session before it),
`PollGameTiers()` was dead code - it never ran, `gameTier`/`gameTierAverage` never got populated
for anyone, and `SortShownCandidates` correctly (per its own designed fallback logic) always fell
back to `probeElapsedMs`, which is exactly what looked like "still broken."

**Fixed:** re-enabled the `PollGameTiers();` call in `OnSteamCallbacksPump()`. Build verified
clean (Debug|Win32). This is a simple oversight, not a new design problem - the identity-key logic
underneath was never actually exercised until now. **Still not live-tested** - this is the real
first live test of the complete fix (identity-key correlation + the call site actually running).

## CONFIRMED via exact screenshot-to-log match: delivery/identity pipeline is fully correct; remaining gap was average-vs-instantaneous (2026-07-12, same day)

User provided 3 screenshots (one per real search) plus a matching `DEBUG.txt` (md5sum-verified).
Did a direct, unambiguous, name-by-name comparison between each screenshot's visible row order
and the exact `sort order` log line for that same real delivery (matched via the
`GetLobbyByIndex`-burst method):

- **Search 1**: exact position-for-position match, all 11 visible rows (Eroscode, Vermi, gigantic
  father, Kirtle, ragataga, Currsligga, BradyBackRibs, Rinix, snowwolf125, Diozyne, Momo) - this
  search correctly had no real tier data yet (first search of the session, `probeElapsedMs`
  fallback throughout), so an unsorted-by-tag appearance here is expected/correct, not a bug.
- **Search 2**: 10 of 11 visible rows matched exactly in order; the one difference (last visible
  row) is most likely explained by a small timing gap between the log capture and the screenshot
  moment (the list can gain/lose members between the two).
- **Search 3**: **exact match, all 11 visible rows**, in the precise order shown on screen
  (Currsligga, Dungeon Crawler Mac, KkkK, Eroscode, gigantic father, kusanagyu, Kirtle, Haphazard,
  snowwolf125, じ, slayraptor64).

**This conclusively proves the delivery mechanism and the identity-key correlation fix from
earlier today are both working correctly - what the mod computes and delivers is genuinely what
reaches the screen.** The user's continued "still not right" report is explained by something
different and much simpler: **the sort was ranking by `gameTierAverage` (a running average across
every observation this session), while the number displayed in the game's own delay-dot column is
the CURRENT, single instantaneous reading.** Connection quality can genuinely fluctuate tick to
tick (this was the whole reason averaging was requested in the first place) - so a player whose
averaged rank puts them near the top can still show a lower number at any given instant, and this
is a real, expected disagreement between "smoothed rank" and "momentary displayed value," not a
mapping/delivery bug. Confirmed directly in the screenshots: e.g. search 2 shows visible tags
`2,0,4,2,1,3,2,1,1,2,1` in delivered order - NOT visually monotonic - because the delivered ORDER
was correctly ranked by average while each row's DISPLAYED number is the raw instantaneous value.

**User's explicit choice when asked "average vs. latest-only" (2026-07-12): use only the latest
reading.** Implemented: `SortShownCandidates`'s `BestConnection`/`WorstConnection` cases now key
off `gameTier` (the single latest observation) instead of `gameTierAverage`. `gameTierAverage`/
`gameTierSampleCount` are left in place (still computed by `PollGameTiers`, just no longer used for
the sort key) in case a future session wants to revisit the trade-off. Build verified clean
(Debug|Win32). **Not yet live-tested** - this should make the delivered order track the visibly
displayed number much more closely, at the cost of the order being able to shuffle more often as
connections fluctuate (an expected, accepted trade-off per the user's explicit choice).

## Real, confirmed bug found and fixed: stale cached gameTier survived across brand-new searches (2026-07-12, same day)

User reported a specific, concrete anomaly: a player showing delay tag "0" on screen was sorted at
the very TOP of a `BestConnection` list - impossible if 0 were genuinely their best-ever value.
Fresh screenshot + `DEBUG.txt` (md5sum-verified) traced this precisely, using the player's REAL
steamId (from `sort order`'s `owner=` field, not the diagnostic's still-cosmetically-unreliable
name field - confirmed again this session: the diagnostic mislabeled a DIFFERENT player,
`76561198366918608`, as "KkkK" at one point, while the REAL "KkkK" is `76561199486366201` per the
correctly-attributed `sort order` log - a reminder that the diagnostic's `name=` field is decorative
only, never trust it over `owner=`):

- Search 1 (fresh list, first time this player appeared): `known=0` - correctly unresolved.
- Search 2 (later, same session): `known=1 key=-5` - `PollGameTiers` successfully resolved their
  identity and read tier 5.
- **Search 3 (a brand-new, later search): STILL `known=1 key=-5`** - the exact same cached value,
  reused verbatim, sorted to the TOP - while the game's own on-screen tag for their fresh row in
  THIS new search showed the honest default "0", because that new row's own connection-quality
  resolver hadn't run yet for this fresh instance.

**Root cause: `PeerVerdict::gameTier`/`gameTierAverage`/`gameTierSampleCount` are keyed by steamId
and persisted for the entire session, but the underlying GAME-SIDE entry object is recreated from
scratch on every genuinely new `LobbyMatchList_t` delivery - its real tier byte starts back at a
default/unresolved state each time until the game's own resolver (`FUN_0046db40`) catches up for
THAT fresh instance.** Using an old cached value to sort a brand-new list's not-yet-resolved row
produces exactly this: a stale high value winning the sort while the live, currently-displayed tag
is still whatever a fresh/default read into the ENTRY chain honestly returns.

**Fixed:** `OnLobbyListResultDelivered` (fires exactly once per genuinely new list, confirmed via
the `GetLobbyByIndex`-burst method used throughout this investigation) now resets
`gameTier`/`gameTierAverage`/`gameTierSampleCount` to their unresolved defaults for every entry in
`m_verdicts` before processing the new list. `probeElapsedMs`/reputation data are deliberately
NOT touched - those track reachability across searches by design and remain valid session-wide.
This means `BestConnection`/`WorstConnection` will correctly show `probeElapsedMs` fallback (or
"unknown") for everyone immediately after each new search, until `PollGameTiers` re-resolves real
tiers fresh for that specific instance - a brief, honest gap, not a bug.

Build verified clean (Debug|Win32). **Not yet live-tested.**

## Second real bug found and fixed: real tier and fallback estimate were on incompatible numeric scales (2026-07-12, same day)

User reported another screenshot where the ordering was still visibly wrong - explicitly noted
this wasn't just connection instability, since the same players kept showing tier 0 across
repeated refreshes. Fresh `DEBUG.txt` (md5sum-verified) traced it directly in the `sort order` log
for the exact real delivery matching the screenshot:

```
#0 owner=76561198090996621 name="UMA | Akane" known=1 key=0
#1 owner=76561198136424162 name="faygofiend" known=1 key=1656
#2 ... key=1688
```

**`key=0` for a `known=1` entry with real tier data means `gameTier=0`** (the worst possible real
value - `-static_cast<long long>(0)` = `0`). Every other visible entry that round was still on the
`probeElapsedMs` fallback (positive millisecond values, e.g. `1656`). **Since `0 < 1656`, a
CONFIRMED worst-possible real tier mathematically beat every peer whose real tier simply hadn't
been measured yet** - putting a tier-0 player at the very top of "Best Connection." This is a
distinct bug from the earlier staleness fix (which addressed *stale* cached data winning) - this
one is about *correctly-fresh* worst-tier data winning against unrelated-scale fallback data, a
pure key-scale defect in `SortShownCandidates`, unrelated to identity/delivery/staleness.

**Fixed:** real-tier keys now live in their own dedicated numeric band
(`kRealTierKeyBase = -1000000000LL` and below) that no plausible `probeElapsedMs` value (typically
hundreds to tens of thousands) could ever reach - so ANY resolved real tier, however bad, always
sorts as a block strictly ahead of ANY fallback-only peer. Direction (best-first vs. worst-first)
is now baked directly into the key computation via a local `worst` bool, rather than relying on
the shared `descending` flip used by other sort modes - a flip alone would have inverted the
real-tier-vs-fallback bucket priority specifically for `WorstConnection` mode (fallback's small
positive numbers would otherwise outrank the real-tier bucket's large negative numbers under a
naive reversal). `RankedListSortMode_WorstConnection` was removed from the shared `descending` set
accordingly - it's now fully self-contained within the connection-mode case.

Build verified clean (Debug|Win32). **Not yet live-tested.**

## Third real bug found and fixed: the staleness fix itself regressed real tier data to never reaching the screen at all (2026-07-12, same day)

User provided two screenshots (consecutive refreshes) and a fresh `DEBUG.txt` (md5sum-verified),
reporting a player ("Brullar") who visibly should have been top-ranked (tier 4, no one else
visibly higher) but stayed stuck in the middle on the next refresh - and separately raised a sharp,
independent question: they report only ever having visually seen delay-column values 0 through 4,
never anything they'd recognize as higher, despite this doc logging real reads up to 6/7 - meaning
either the mod is reading the wrong value, or values 5-7 are visually indistinguishable from 4 in
the game's own rendering. A background RE pass was dispatched to check that question specifically
(asset/icon-selection analysis, not yet reported back as of this writing).

**Separately, and more urgently, direct log analysis for this exact session found a serious
regression from the PREVIOUS session's staleness fix:** checked all 3 real deliveries this session
(via the `GetLobbyByIndex`-burst method) for ANY entry using a real-tier-based key
(`kRealTierKeyBase` range) - **zero of 3 deliveries had ANY real tier data at all.** Every single
visible sort this entire session was 100% `probeElapsedMs` fallback, despite `PollGameTiers`
correctly resolving real tier data via the identity chain in the background (confirmed separately
via the diagnostic).

**Root cause:** the previous session's fix (clearing `gameTier`/`gameTierAverage`/
`gameTierSampleCount` for every peer at the start of `OnLobbyListResultDelivered`, to stop stale
data from an earlier search winning the sort) collided with an already-known architectural fact:
**the game only creates its row/entry objects at the exact moment a list is actually delivered to
it** (that's what triggers its own `GetLobbyByIndex` calls) - so during the hold period BEFORE
delivery, there are no fresh entries for the new search to poll yet, and wiping the cache at that
exact moment left nothing at all for the sort to use. Combined with the older, already-documented
fact that only the FIRST delivery of a search ever reaches the screen (later in-place
recomputations are no-ops - see "MAJOR FINDING" section), this meant real tier data could
never inform what the user actually sees, ever, after that fix - a complete (if well-intentioned)
regression of the tier-based sort back to pure fallback-estimate behavior.

**Fixed with a bounded-TTL middle ground instead of an unconditional wipe:** added
`PeerVerdict::gameTierTickMs` (timestamp of the last successful real-tier read) and
`kGameTierTtlMs = 45000` (45s). `SortShownCandidates` now only trusts `gameTier` if it was
refreshed within that window - long enough to survive the gap between a new search's delivery and
its own entries resolving (typically a few seconds, based on prior observations), short enough
that a truly old reading (e.g. a player who left and reappeared minutes later) eventually falls
back to being treated as unknown again. The unconditional wipe in `OnLobbyListResultDelivered` was
removed entirely. Build verified clean (Debug|Win32). **Not yet live-tested.**

**Still open, pending the RE agent's report:** whether the raw 0-7 tier value the mod reads is
genuinely the same thing the game's delay column visually distinguishes, or whether it needs a
transformation (clamp, bucket, or a different meaning for the upper bits) that the mod isn't
currently applying - see the dispatched background investigation referenced above.

## RE agent report: icon-selection code path fully resolved - NO clamp exists, and the tier byte's real source is `HOST_NETCOLOR` Steam lobby metadata, not a live per-viewer ping (2026-07-12, new session)

This is the background RE pass referenced in the section above. Investigated whether the
icon-selection code clamps/buckets the raw 0-7 byte before picking a sprite, and what actually
writes the byte in the first place. Pure static Ghidra analysis this session - **no live CDB,
no process interaction, per the standing CLOSED status on live debugging of this call chain.**
New Ghidra scripts, all under `docs/Research/ghidra_scripts/`, matching
`run_ghidra_netcol_select_fn.cmd` / `run_ghidra_searchresultnode_tier_writer.cmd` /
`run_ghidra_lobby_metadata_key_table.cmd` / `run_ghidra_searchresultnode_populate_caller.cmd`
in `docs/Research/`: `DecompileNetColSelectFn.py`, `DecompileSearchResultNodeTierWriter.py`,
`DecompileLobbyMetadataKeyTable.py`, `DecompileSearchResultNodePopulateCaller.py`. Matching
report files: `NetColSelectFnGhidraReport.txt`, `SearchResultNodeTierWriterGhidraReport.txt`,
`LobbyMetadataKeyTableGhidraReport.txt`, `SearchResultNodePopulateCallerGhidraReport.txt`.

### Part 1: the icon-select helpers use the RAW byte as a direct array index - no clamp, no modulo, no bucketing anywhere

Full decompile + manual raw-disassembly cross-check of both tiered icon-draw helpers
(`FUN_00655260`, used by the confirmed ranked-list row renderer `FUN_00661060`; and its
sibling `FUN_00533d10`, used by the post-match/rival-card screens) - both are trivially
simple, and both index the 8-string array with the tier value completely unmodified:

```c
// FUN_00655260 (ranked list) - full decompile, nothing elided:
void FUN_00655260(undefined4 param_1,undefined4 param_2,undefined4 param_3,undefined4 param_4,
                 int param_5,undefined4 param_6)
{
  char *local_28[4]; char *local_18,*local_14,*local_10,*local_c; undefined4 local_8;
  local_28[0]="net_col_def.hip"; local_28[1]="net_col_A.hip"; local_28[2]="net_col_B.hip";
  local_28[3]="net_col_C.hip"; local_18="net_col_D.hip"; local_14="net_col_E.hip";
  local_10="net_col_F.hip"; local_c="net_col_G.hip"; local_8=0;
  FUN_00643b40();
  FUN_00643ea0(local_28[param_5],&local_8,0,0,0);   // <-- direct index, param_5 = raw tier byte
  FUN_006916b0(local_8,param_1,param_2,param_3,param_4,1,param_6,0,0x3f800000,0x3f800000,0,0,0,0);
}
```

Raw disassembly confirms `param_5` (`[EBP+0x18]`, zero-extended straight from the entry's
tier-getter `AL` return per the earlier "RESOLVED: `uVar10`..." section) is used verbatim:
`MOV ECX,dword ptr [EBP+0x18]` then `PUSH dword ptr [EBP+ECX*0x4+-0x24]` - a plain
`array[index]` load, no `AND`, no `CMP`/`JAE`-style bounds clamp, no lookup/remap table, no
`MOD`. **`FUN_00533d10` (the other, post-match-screen caller) does the exact same thing**
(`local_28[param_1]`, same raw-disasm shape). **Conclusively answers whether a clamp exists:
there is no clamp/bucketing transform anywhere in the render path.** Indices 4/5/6/7
(`net_col_D/E/F/G.hip`) are four fully distinct string literals, loaded and drawn via four
fully independent `FUN_00643ea0`/`FUN_006916b0` calls - the code treats all 8 assets as
completely separate, unaliased sprites. (What was NOT verified this pass: whether the actual
`.hip` texture *pixel content* of D/E/F/G is visually distinguishable at the small on-screen
icon size - the game's `net_col_*.hip` files live packed inside one of the proprietary
`data/ETC/*.pac` archives, and no unpacker for this game's `.pac`/`.hip` format was available
in this session/repo. This remains the one untested part of the "is it a code clamp or an
asset-content issue" question - see next steps below.)

### Part 2: the tier byte is `HOST_NETCOLOR`, a one-shot Steam lobby-metadata string set by the HOST, not a locally-measured live ping

Ran a fast whole-binary instruction scan (1,266,930 instructions) for real object-field stores
to offset `+0x74` (filtering out ~140 `[EBP + -0x74]` stack-local false positives), then
decompiled the register-relative survivors. Found the actual writer:

**`FUN_0046fcc0(ENTRY* param_1, void* param_2)`** - called from exactly **one** place in the
whole binary (a single xref from data at `0089cb90`, i.e. invoked via vtable-style dispatch).
Decompiles to: set `ENTRY+0x114 = param_2` (the identity-key sub-object pointer this doc
already proved, in an earlier session, decodes to the peer's real Steam64 ID), then loop up to
18 times over the lobby's Steam metadata key/value pairs (`GetLobbyData`-by-index-shaped
vtable calls through `SteamInternal_ContextInit`), `strncmp`-matching each key name against a
fixed 18-string table at RVA `0x9D9610`. Dumped that table in full:

```
[0]  NETWORK_VERSION
[1]  MATCHING_FILTER
[2]  HOST_NETCOLOR        <-- writes ENTRY+0x74, our confirmed tier byte, UNMODIFIED
[3]  PLAYER_ROOM_NAME
[4]  PLAYER_ROOM_TYPE
[5]  PLAYER_MEMBER_MAX    -> ENTRY+0x6c
[6]  PLAYER_PRIVATE_NUM   -> ENTRY+0x6f
[7]  PLAYER_PRIVATE_MAX   -> ENTRY+0x6e
[8]  PLAYER_SESSION_FLAG  -> ENTRY+0x64
[9]  PLAYER_SESSION_VALUE -> ENTRY+0x68
[10] RANK_MYAREA
[11] RANK_AREA_FILTER
[12] RANK_HOST_LEVEL     -> ENTRY+0x5c
[13] RANK_LEVEL_MIN      -> ENTRY+0x5e
[14] RANK_LEVEL_MAX      -> ENTRY+0x5a
[15] RANK_RTT_FILTER
[16] RANK_DISCONNECT_RATE
[17] RANK_DISCONNECT     -> ENTRY+0x10a
```

Key `[2]`, `"HOST_NETCOLOR"`, is the **exact** literal string this doc already found (and set
aside as a loose end) in the "User's explicit new direction" section much earlier:
`RANK_RTT_FILTER`/`net_col_A..G.hip`/`HOST_NETCOLOR` strings - it is now conclusively the
confirmed source, not a coincidental keyword hit. Its value is parsed via `FUN_0041c8f0` (a
one-line wrapper around `FUN_0079fa3c` - almost certainly `atoi`/`strtol` given a raw metadata
string goes in and a single byte comes out) and stored **completely unmodified** into
`ENTRY+0x74` - matching the getter (`FUN_0046e880`: `return *(byte*)(this+0x74);`) exactly, so
there is no double-transform hiding anywhere between metadata parse and icon-index use either.

**This means the ranked list's delay-dot tier is not a measurement the viewing client makes of
its own connection to that lobby's host at all - it is the HOST's own self-reported netcolor
value, published once as Steam lobby metadata, read by every client browsing the list.** This
is exactly the same `netcolor` mechanism this doc already fully reverse-engineered in the "RE
follow-up on the `netcolor` lead" section (`NetworkUserData+0x194`, gated by a 30-sample
warm-up counter at `+0x196` via `FUN_004a1110`: returns a hardcoded `0` unless the host's own
sample count has reached 30, otherwise returns the true stored tier byte) - that section had
flagged this exact mechanism as "probably the wrong subsystem" for the search list because it
only writes a *local* `RoomMemberEntry.netcolor` once already in a room. **That conclusion was
right about the write path investigated at the time, but incomplete: the same underlying
per-host tier value also gets independently published as `HOST_NETCOLOR` lobby metadata by
Steamworks' own lobby-data-set mechanism (the literal host-side `SetLobbyData` call was not
traced this pass - it's a different process's code path, not observable from static analysis
of one binary snapshot)**, which is exactly what every *other* client's row-populate function
(`FUN_0046fcc0`, called from `FUN_0046d890`, the list-rebuild/filter function bound to the row
list's `+0xae0/+0xae8/+0xaec/+0xaf0` fields already confirmed as the MGR's own list-struct
layout - decompiled in full this pass, and it also applies the `RANK_*`-filter server-side
matchmaking checks in the same loop) reads back.

**This resolves several previously-mysterious observations already logged earlier in this doc,
retroactively:**
- **"Names/rows are static, only the delay ICON progressively fills in over time"** (the
  isolation-test finding from the "RESOLVED (differently than expected)" section): `FUN_0046d890`
  creates every row's identity/name fields synchronously from the lobby-list enumeration
  itself, but `HOST_NETCOLOR` (like all Steam lobby metadata) is fetched via a separate,
  independently-replicating `GetLobbyData` cache that can lag behind the list enumeration by a
  visible amount - so the icon can legitimately arrive later than the row it belongs to, with
  zero reordering of anything, exactly matching the user's description.
- **The "stale cached `gameTier` survived across brand-new searches" bug** (an earlier section
  today): makes complete sense now - each new search creates entirely new `ENTRY` objects,
  each with its own fresh one-shot `HOST_NETCOLOR` fetch; there was never a "live" value to go
  stale in the first place, only a fresh snapshot per list. This also means the newly-added
  `kGameTierTtlMs=45000` fix (section immediately above) is a reasonable practical compromise,
  but is masking, not measuring, a real refresh cadence - the true refresh moment is "a new
  `ENTRY` was created for this search," not any fixed wall-clock duration.
- **Why the earlier `AASTEAM_CNetworker`/`DAT_00a5d270` candidates read flat zero while this
  one is genuinely live:** those were both trying to find a per-viewer *measured* connection
  quality; the real mechanism is a host-authored, Steam-replicated *lobby metadata string*, a
  completely different kind of data source neither candidate was.

### Answer to the user's "only 5 visually distinct values ever seen" report

Since (a) the render path definitely does not clamp (Part 1) and (b) real diagnostic sessions
earlier in this doc already logged genuine live sightings of tier `5`, `6`, and `7` for real
players (e.g. the `18:47:30` log block: `wokewaifu95)=7`, multiple `=6` and `=5` entries), tiers
above 4 clearly DO occur in the wild and DO reach the same unclamped render call as tiers 0-4.
**This is not a code-side clamp/bucket, full stop.** The most likely remaining explanations for
why the user personally hasn't recognized a 6th/7th/8th distinct icon, roughly in order of
likelihood, **none confirmed this session (would need the `.hip` assets extracted, see next
steps):**
1. The `net_col_D/E/F/G.hip` sprite assets themselves may be visually very similar to
   `net_col_C.hip` or to each other at the small on-screen icon size (e.g. a palette/color-ramp
   design where tiers 4-7 are subtle shade variations on the same base icon) - an asset/content
   design choice, not a code bug.
2. Tiers 5-7 may require the host to have a substantially long-lived/stable connection (30+
   samples per the warm-up gate) reported at a level most real hosts practically don't sustain
   before the searching client stops watching that specific lobby - i.e. tiers 5-7 might be
   rare in practice even though structurally possible and already logged in this doc's own
   diagnostics. This is a claim about the real-world distribution of the value, not the code.
3. `net_col_def.hip` (index 0) is very likely the "no data yet"/gated-default sprite (matches
   the `FUN_004a1110` 30-sample gate returning a hardcoded `0`) rather than "worst connection" -
   worth remembering when interpreting any future value distribution: index 0 conflates
   "genuinely tier 0" and "not enough data yet," both from the same host-side gate.

### Concrete next steps, in order

1. **Find or write a `.pac`/`.hip` unpacker** to actually extract and eyeball-compare
   `net_col_def.hip` through `net_col_G.hip` - the only way to conclusively settle whether
   D-G are visually distinct assets. The archive almost certainly lives under `data/ETC/*.pac`
   (many candidate lobby/network-icon-named pacs exist there) - narrowing down which specific
   `.pac` contains these 8 filenames was not attempted this session.
2. **Cross-reference a live `HOST_NETCOLOR`-sourced tier reading against that same host's own
   in-room `netcolor`** (via the already-shipped `NetworkSquareColorWindow.cpp` mechanism) the
   next time the mod's user actually connects to and plays a match against a specific opponent
   whose search-list tier was logged beforehand - a match would be the final confirmation these
   really are the same underlying value, published early via lobby metadata.
3. **If this insight is ever wired back into the mod:** since `HOST_NETCOLOR` is fetched exactly
   once per `ENTRY` at row-creation time (single caller of `FUN_0046fcc0`, from the list-rebuild
   function `FUN_0046d890`) and never re-polled afterward, the existing per-row diagnostic
   loop's repeated `entry+0x1c` vtable calls (throttled to 2/sec via `PollGameTiers`) are
   unnecessary overhead for tracking *changes* to this specific field - it cannot change without
   an entirely new lobby-list delivery creating new `ENTRY` objects. Not attempted/changed this
   session - purely research, no source edited per this task's scope.
4. **Not attempted this session, lower priority:** trace `FUN_0079fa3c` (the real parser
   `FUN_0041c8f0` wraps) fully to confirm it's a plain string-to-int conversion with no extra
   transform of its own (very likely, given the getter's own byte-for-byte pass-through
   already confirmed, but not independently decompiled this pass).

## MAJOR SIMPLIFICATION: switched to reading `HOST_NETCOLOR` directly as Steam lobby metadata, replacing the entire vtable/entry-walk chain (2026-07-12, same day)

Acting on the RE agent's finding immediately above (the tier is `HOST_NETCOLOR`, a plain Steam
lobby-metadata string, not something requiring the mgr/vtable/`FUN_004a5450`/entry-walk chain at
all): rewired the mod to read it the same simple way `ownerName` and `RANK_HOST_LEVEL` are already
read, directly in `OnLobbyListResultDelivered`'s per-lobby loop
(`raw->GetLobbyData(lobby, "HOST_NETCOLOR")`, parsed via `strtol`, range-checked to 0-7, stored
straight into `m_verdicts[candidate.ownerSteamId]` with a fresh `gameTierTickMs`).

**This eliminates every structural problem the vtable-walk approach had, in one step:**
- **No identity-correlation problem** - the value is read tied to the exact `ownerSteamId` already
  being resolved for that lobby, never via row position at all.
- **No "entries don't exist until delivery" chicken-and-egg** - lobby metadata is available
  synchronously while building `m_candidates`, BEFORE `BuildCompactedListAndDeliver` even runs -
  meaning the very FIRST (and only-ever-screen-reaching) delivery of a brand-new search now has
  real tier data available immediately, with no hold/wait needed at all.
- **No staleness ambiguity** - a fresh value is fetched on every single new delivery; the
  `kGameTierTtlMs` bound from the previous fix is now just a sane backstop, not something papering
  over a real gap.
- **No vtable calls, no crash-risk surface whatsoever** - it's the exact same `ISteamMatchmaking`
  API call pattern already used safely for other metadata in this same function.

`PollGameTiers()`'s call site in `OnSteamCallbacksPump()` is now disabled (function and the
mgr/vtable-walk chain left in place, not deleted, in case a future session wants to cross-check
the two sources against each other - they should read the same underlying value per the RE trace).
The per-row identity/tier diagnostic (`DiagnosticLogRankedListMgrSlot`) is left running unchanged,
also for cross-check purposes.

Build verified clean (Debug|Win32). **Not yet live-tested** - this is a genuinely new, much
simpler code path (direct metadata read, never exercised before), not just a variant of what was
already tested. The user's original "Brullar not sorting correctly" and "only see tags 0-4"
reports should both get a fresh, clean test against this implementation - the second question
(whether 5-7 are visually distinct) remains open per the RE agent's report and is unrelated to
this code change; this change only affects HOW the mod obtains the tier value, not what the game
itself renders for values 5-7.

## RE-VERIFICATION of the HOST_NETCOLOR attribution, per user challenge — the +0x74 mapping is correct, BUT +0x74 is the wrong FIELD: the Delay column is actually entry+0x78 (2026-07-12, new session)

User firmly rejected the previous session's claim that `HOST_NETCOLOR` lobby metadata is the
source of the ranked list's Delay column, suspecting a key-table-index-to-field-offset mixup or
a conflation of two systems. Re-derived everything from scratch, instruction-level, without
trusting the previous session's mapping. **Both the user and the previous session turn out to be
partially right: the previous session's store-level attribution was accurate, but it answered
the wrong question — `+0x74` drives the row's colored ICON, not the numeric Delay column the
user actually reads.** Full chain below, all verified this session. No live debugger used
anywhere (CDB remains permanently CLOSED for this game).

### Part 1 — independent re-verification of FUN_0046fcc0's key-to-offset mapping

Method: extracted the raw switch dispatch from `tools/bbcf_disasm_ascii.txt` and read the actual
jump table + key-string table **directly from `BBCF.exe`'s bytes** (PE section mapping via a
Python one-liner — not from any Ghidra report):

- Dispatch at `0x0046FDC6`: `add esi,-2; cmp esi,0xD; ja skip; jmp [esi*4+0x0046FF8C]` — a
  14-entry jump table indexed by (matched key-table index − 2). `esi` is the loop index over the
  18-entry string table at `0x009D9610` (strncmp per key, verified in the same disasm).
- Jump table contents (read from exe bytes at VA `0x0046FF8C`) and the store each target performs:

  | keyIdx | key | handler | store |
  |---|---|---|---|
  | 2 | `HOST_NETCOLOR` | `0x0046FDD9` | `call 0041C8F0; mov byte ptr [ebx+74h],al` — **+0x74, confirmed** |
  | 3 | `PLAYER_ROOM_NAME` | `0x0046FDEC` | wcscpy into `+0x10` |
  | 4 | `PLAYER_ROOM_TYPE` | `0x0046FF21` | no-op |
  | 5 | `PLAYER_MEMBER_MAX` | `0x0046FE89` | byte `+0x6c` |
  | 6 | `PLAYER_PRIVATE_NUM` | `0x0046FE9C` | byte `+0x6f` |
  | 7 | `PLAYER_PRIVATE_MAX` | `0x0046FEAC` | byte `+0x6e` |
  | 8 | `PLAYER_SESSION_FLAG` | `0x0046FEBC` | dword `+0x64` |
  | 9 | `PLAYER_SESSION_VALUE` | `0x0046FECC` | dword `+0x68` |
  | 10 | `RANK_MYAREA` | `0x0046FEDC` | word `+0x5c` |
  | 11 | `RANK_AREA_FILTER` | `0x0046FEED` | word `+0x5e` |
  | 12 | `RANK_HOST_LEVEL` | `0x0046FEFE` | word `+0x5a` |
  | 13/14 | `RANK_LEVEL_MIN`/`MAX` | `0x0046FF21` | no-op |
  | 15 | `RANK_RTT_FILTER` | `0x0046FF0F` | word `+0x10a` |

- **The previous session's summary table in this doc WAS wrong in four rows** (it attributed
  `+0x5c/+0x5e/+0x5a/+0x10a` to `RANK_HOST_LEVEL`/`RANK_LEVEL_MIN`/`RANK_LEVEL_MAX`/
  `RANK_DISCONNECT` — actually `RANK_MYAREA`/`RANK_AREA_FILTER`/`RANK_HOST_LEVEL`/
  `RANK_RTT_FILTER` per the jump table above) — the user's suspicion of sloppy mapping was
  justified. But the one row that mattered, `HOST_NETCOLOR -> +0x74`, is instruction-level
  correct, and the whole-binary store scan (`SearchResultNodeTierWriterGhidraReport.txt`, 165
  raw `[reg+0x74]` MOV hits, exactly ONE byte-sized store to a real object `+0x74`:
  `0046fde4 MOV byte ptr [EBX+0x74],AL` inside `FUN_0046fcc0`) confirms nothing else ever
  writes it. Confidence: as high as static analysis gets.

### Part 2 — THE ACTUAL FINDING: the Delay column digit is entry+0x78 (viewer-measured RTT), not +0x74

Re-reading the full `FUN_00661060` decompile (`NetColIconCallersGhidraReport.txt` lines
1150-1240) past the icon call, the row renderer's LAST draw block is:

```c
uVar6 = (**(code **)(*piVar7 + 0x30))();       // ENTRY vtable slot 12 -> *(int*)(entry+0x78)
FUN_004a77e0(uVar6);                            // (lazy-init singleton getter, decompiler
                                                //  artifact arg - irrelevant)
iVar5 = FUN_004a6620(uVar6);                    // RTT -> digit bucketing, see below
if (iVar5 != -1) {
  FUN_0068fcb0(&DAT_01463ec8, iVar5 + 0x30, ...); // 0x30 = ASCII '0': draws the DIGIT GLYPH
}
```

`FUN_004a6620` (raw disasm, trivially small): `rtt<0 -> -1` (draw nothing), `<0x3C(60) -> 4`,
`<0x64(100) -> 3`, `<0xC8(200) -> 2`, `<0x12C(300) -> 1`, else `0`. **Range 0-4, higher =
better, exactly matching the user's "only ever seen 0-4" report.** The `-1 -> draw nothing`
path is exactly the "Delay value fills in progressively per row" behavior the user described.

Where `+0x78` comes from (all previously decompiled, now correctly connected):
- `FUN_0046dc00` (field init) sets `+0x78 = 0xFFFFFFFF` (sentinel).
- `FUN_0046db40` (runs continuously) walks the same row list and, per row with the
  "needs resolution" flag (`+0x110`) set, calls `FUN_0046e9e0(steamIdLow, steamIdHigh)` using
  the row's own embedded Steam64 ID (`+0x114 -> +0xc/+0x10`, the identity key this doc already
  proved byte-for-byte) and stores the result into `+0x78`.
- `FUN_0046e9e0` looks up the 20-row RTT sample table by that Steam64 key and returns the
  **average of the best 4 of 5 RTT samples** (sum minus max, >>2) — a per-VIEWER measured
  quantity, resolved asynchronously as the game's own QoS pings accumulate samples.

**So the row shows TWO separate connection indicators: the `net_col_*` colored icon (slot 7,
`+0x74`, host-self-reported `HOST_NETCOLOR` metadata — what the mod has been sorting by), and
the numeric Delay digit (slot 12, `+0x78`, viewer-measured RTT — what the user actually
compares against).** The user's rejection was correct in the way that matters: sorting by
`HOST_NETCOLOR` can never match the Delay column, because they are different measurements of
different things (host's own historical quality vs. my live RTT to that host). This also
retroactively explains, cleanly, every "sorted on paper, wrong on screen" session in this doc:
the paper order was sorted by the icon value while the eyes compared against the digit.

Corroborating log evidence (last session's `DEBUG.txt`, 22:14-22:19): the mod's slot-7 reads
showed tiers up to 7 with a spread the user never saw in the Delay column; the user's
screenshot digits were 0-4; the digit is drawn from a different field with different
range/thresholds. All three observations line up only under this Part-2 model.

### Fix implemented this session

- `PollGameTiers()` re-enabled (its call site had also been left commented out — again) and
  rewritten to read `*(int32_t*)(entry + 0x78)` **directly** — a plain field read, NO virtual
  call at all (strictly safer than the old slot-7 call it replaces), still keyed by the proven
  `entry+0x114 -> +0xc/+0x10` Steam64 identity, skipping unresolved (`< 0`) rows.
- `PeerVerdict` gained `gameRttMs` (latest resolved RTT, -1 = never); `gameTier` now stores the
  0-4 Delay digit derived via `GameDelayDigitFromRtt()` (an exact reproduction of
  `FUN_004a6620`'s thresholds, new helper in `RankedListConnectionFilter.cpp`), kept for
  diagnostics/screenshot cross-checks. `gameTierAverage` now averages RTT (diagnostics only).
- `SortShownCandidates()` connection modes key on **raw RTT** inside the existing negative
  real-data band (`kRealTierKeyBase ± rtt`) — finer than the digit but digit-monotonic, so the
  visible Delay column reads sorted; probe-timing fallback and the band separation (both
  previously-fixed bugs) are preserved unchanged. The sort log now emits `delay=D rtt=R` per
  entry with a real reading, so screenshots can be cross-checked digit-for-digit.
- The `HOST_NETCOLOR` metadata read in `OnLobbyListResultDelivered` is REMOVED (with a comment
  explaining why it was wrong); the per-row diagnostic now logs `[row]=icon:T,delay:D,rtt:R`
  so both values stay observable side by side.
- Help text (`RankedListFilterWindow.cpp` + `Localization.csv`, key/en/es) now says the sort
  matches "the list's own Delay rating (0-4)".
- Build verified clean (Debug|Win32).

### Expectations and test protocol for the next live session

- **First search after boot will still look unsorted** — the game itself has no digits yet
  (all `+0x78` sentinels, nothing drawn); the mod falls back to probe timing. This is inherent:
  the metric literally does not exist yet, for the game or for us.
- From the second refresh onward (the game auto-refreshes every ~8-16s), the order should
  match the visible Delay digits monotonically (best first for mode 1), modulo digits that
  resolve after the delivery instant (they'll be picked up next refresh via the 45s TTL cache).
- Cross-reference using the `sort order` lines' `owner=`/`name=`/`delay=` fields against a
  screenshot taken right after a refresh. As always, never trust the `rankedListMgrDiag` line's
  name field (cosmetic, proven unreliable); its `id=` field is the trustworthy one.
- If the digits STILL don't read sorted: check whether the logged `delay=` values match the
  screenshot's digits for the same names. If they match but the order is wrong, the delivery
  path regressed (was proven exact twice); if they don't match, the `+0x78` read or identity
  keying needs re-examination — those are the only two links left in the chain.

## LIVE IN-PLACE LIST REORDERING via the game's own permutation array, plus removal of the delivery hold (2026-07-12, same session)

User confirmed the Delay-column fix works, then asked for two things: (1) real-time
manipulation of the on-screen list (reorder as measurements arrive, no refresh needed), and
(2) complete removal of the 2s/6s pre-delivery hold ("the refreshes should be responsive as
fast as possible"). Both are now implemented, built on a write-side RE pass of the row-list
structures this doc had already mapped read-side. All static analysis; no live debugger.

### RE findings that make in-place manipulation safe (all verified this session)

1. **Every consumer of the visible row order resolves through the SAME permutation array**
   (`listStruct+0xaf4`, 50 dwords, maps visible slot -> logical list position, walked via
   `FUN_004a5450`):
   - The row RENDERER `FUN_00661060` calls `FUN_004a7b40(visibleRow)` **every frame** and
     draws every cell (name, member count, level, icon, Delay digit) from the resolved ENTRY
     per frame - nothing positional is cached in the UI slots (`FUN_00649100(row)` slots hold
     only active/highlight flags at `+4`/`+8`).
   - The row SELECTION/join paths (`FUN_004a89d0`: `perm[selectedRow]` -> join-request struct;
     `FUN_004ac6c0` case 0x4A and `FUN_004ae6d0` case 0x27, both:
     `FUN_004a5450(listStruct[selectedRow + 0x2bd])` - dword index 0x2bd*4 = 0xAF4) resolve
     the user's selected visible row through the same array. **So a permutation rewrite moves
     whole rows consistently for rendering, cursor selection, and auto-connect - clicking
     visual row N always targets the entry displayed at row N.**
   - Bonus: the auto-connect state machines gate on the entry's slot 0x30 (+0x78 RTT) being
     resolved (`"RMSR_CheckingRTT"` wait) and compare `FUN_004a6620(rtt)` against the
     RANK_RTT_FILTER tier - further confirmation the Delay digit chain from the previous
     section is the game's own operative connection metric.
2. **The game writes the permutation array in exactly one place** (`FUN_004a5430`, identity
   reset, confirmed via BOM-skipped disasm grep: only two call sites, `0x004A44B7` and
   `0x004A950E`, both in search-start/clear functions that also zero the row count at
   `+0xae8` first). It is NOT rewritten per delivery, per frame, or by the row-populate
   function - a mod-written order persists until the next search starts.
3. **Row population (`FUN_0046d890`) reuses pre-existing pooled nodes**: it walks the node
   chain (`node+4` next pointers) while filling from raw result records at `listStruct+0xcf4`
   (stride 0x14, count at `+0xcf0`, up to 100), applies the game's own filters (compacting in
   place), sets `+0xae8` to the kept count, resets the walk cursor cache (`+0xaec`/`+0xaf0`) -
   and never touches the permutation array. Also: `FUN_0046fcc0`'s FIRST store is
   `entry+0x114 = record` - **the Steam64 identity sub-object is attached synchronously at
   populate time**, not "later when resolved" as an earlier section assumed (only the +0x78
   RTT resolves later).
4. **`FUN_004a5450` (the walker) has NO bounds check** - it blindly follows next/prev
   pointers. Mitigations built in: the mod only ever writes permutation values `< count`
   (validated against a live count read in the same pass), and restores the identity mapping
   immediately after every delivery (same call stack as `handler->Run()`, see below). Worst
   remaining case (game-side count shrink inside the 400ms window between passes) walks into
   the still-linked node pool (physically >= 50 nodes, proven by the populate function itself
   walking up to raw-count nodes blindly) -> a stale-but-valid node -> at most a one-pass
   ghost row, self-corrected within 400ms. No crash surface identified.
5. **Threading: the Steam callback pump, the renderer, and the selection state machines all
   run on the game's main thread** (single thread ID across pump logs and frame logs in every
   DEBUG.txt session) - permutation writes are frame-atomic by construction.

### Implementation (all in `RankedListConnectionFilter`, build verified clean Debug|Win32)

- **`PollGameListAndApplyOrder()`** (new; replaces both `PollGameTiers()` and the proven-no-op
  `TryLiveResort()`/`Run()`-replay mechanism, both now deleted): every 400ms while a list is
  delivered and the pipeline is active - walks the game's rows by logical index, harvests
  Steam64 ID (+0x114 -> +0xc/+0x10) and Delay RTT (+0x78) into `m_verdicts` (subsuming the old
  PollGameTiers), computes the desired visible order (existing `SortShownCandidates`
  comparator on matched candidates + unmatched rows after them + hidden-pending peers sunk to
  the tail when the filter is on), and rewrites `perm[]` in place only when it actually
  changed (logged as `live order applied`, with per-row name + delay digit). When the
  features get turned off mid-session it restores the identity mapping once.
- **`WriteIdentityGamePermutation()`** (new): the exact write the game's own search-start
  reset performs (perm[i]=i, all 50 slots). Called right after every `handler->Run()` in
  `BuildCompactedListAndDeliver` - the game only resets perm at search START, so without this
  a custom order from list N would scramble the freshly rebuilt list N+1. Same call stack as
  the delivery, so nothing can observe the stale state. Also arms the live pass to run on the
  very next pump (throttle reset).
- **Hold removed entirely**: `OnLobbyListResultDelivered` now delivers immediately, always -
  `kHoldDeadlineMs`/`kConnectionSortHoldDeadlineMs`, the `Holding` pipeline state,
  `IsPeerUnresolved()`, and the hold logic in `OnSteamCallbacksPump` are deleted. Initial
  order uses whatever the caches already know; everything that matures afterward (probes,
  Delay RTTs - which only START measuring once the list exists anyway) reaches the screen via
  the live permutation rewrites within ~400ms instead of a pre-delivery wait. The searching
  popup delay is now whatever Steam itself takes, full stop.
- Deleted with the old mechanism: `m_lastGameLobbyListHandler`, `forceDeliver` parameter and
  the unchanged-skip logic in `BuildCompactedListAndDeliver`, `m_lastLiveResortTickMs`
  (replaced by `m_lastLiveOrderTickMs`), `m_holdDeadlineTickMs`.
- `SortShownCandidates` gained a `logOrder` flag so the 2.5x/sec live path doesn't spam the
  `sort order` log (real deliveries still log it; the live path logs only actual changes).

### What was deliberately NOT done (and why)

- **True live row REMOVAL (count manipulation)**: shrinking `+0xae8` after sinking hidden
  rows to the tail would remove them visually, but the game's RTT resolver (`FUN_0046db40`)
  and populate function iterate logical positions `< count` - a shrunk count would stop RTT
  resolution for whatever nodes physically sit at the tail positions, which are NOT
  necessarily the hidden rows (node order is delivery order, not visible order). v1 therefore
  only sinks hidden-pending peers to the tail instantly; their true removal still happens on
  the next real delivery (~8-16s). If this proves insufficient, the follow-up experiment is
  node-pointer surgery (relink `+4`/`+8` around hidden nodes + fix the cursor cache) - higher
  risk, not attempted.
- **Live row CREATION**: rows are populated from the raw records at `+0xcf4` delivered by
  Steam; injecting synthetic records is possible in principle but has no current use case
  (the mod already controls delivery membership).

### Test protocol for the next session

1. Search ranked with Best Connection sort on. The list should appear **immediately** (no 6s
   wait - log line `lobby list received: N lobbies, delivering immediately`).
2. Sit on the list without touching anything: rows should visibly REORDER in place over the
   first several seconds as Delay digits resolve (grep `live order applied` and compare its
   name/delay sequence against the screen at that moment). This is the acid test - if rows
   visibly jump into sorted position without a refresh, the whole mechanism works.
3. Verify row selection targets the right lobby after reordering (click a row, confirm the
   name on the connect/confirmation flow matches the row clicked).
4. If the filter is on and a probe fails while the list is up, the peer should visibly drop
   to the bottom within ~1s (`live order applied` with `hidden-pending` count > 0).
5. Watch for any visual anomaly right after the game's own auto-refresh (~8-16s cadence) -
   a scrambled list that fixes itself within half a second would indicate the identity-reset-
   after-Run ordering needs revisiting; a persistent scramble would be a real bug.

## LIVE ROW DELETION/RESTORE (payload-partition + count control), and config-window disappearance root-caused and fixed (2026-07-12, same day)

User confirmed live reordering works. Two follow-ups this session: (1) the config window
disappears after idling on the list a while, (2) implement live row creation/deletion so the
hide filter works in real time.

### Config window disappearance: the 50s "recent list activity" window was wrong

`DEBUG.txt` (session 01:48-01:57) shows three long stretches of `onList=0` with
`state1=39 state=4` (still on the results screen): starting 01:52:13, 01:53:48, 01:56:12 -
each beginning EXACTLY 50 seconds after the previous lobby-list request (deliveries at
01:51:23 -> 01:52:58 -> 01:55:22, i.e. request gaps of 95s and 144s). The old
`IsLobbyListLikelyOpen()` required a `RequestLobbyList` within the last 50s
(`kListActivityWindowMs`) - but the game's auto-refresh cadence is NOT bounded (the earlier
"every ~8-16s" observation was a small-sample artifact; a stable list can idle for minutes
between refreshes). **Fixed:** replaced the recency window with `gameListHasRows` - the
game's own row list currently holds rows (count at `listStruct+0xae8` > 0, read via the
already-proven mgr chain, OR `m_gameListOrigCount > 0` to cover the all-rows-hidden-live
case). The row list is authoritative: populated per delivery, zeroed by the same
search-start/back-out functions that reset the permutation array, so it is true exactly while
the list screen has content, indefinitely. All other conditions (gstate==27, state==4,
state1 in {36,38,39}, not-in-functional-room-with-opponent) unchanged.

### Live row deletion/restore: how it works

The permutation array alone cannot remove rows (the renderer's bound is the visible-row index
vs count, and the RTT resolver iterates logical positions, so perm games either leave ghost
rows or starve tail rows of Delay resolution - see the previous section's "What was
deliberately NOT done"). The clean mechanism, now implemented in
`PollGameListAndApplyOrder()`:

1. **Payload partition**: rows whose peer should be hidden get their node payloads (bytes
   `0xC..0x117` of the 0x118-byte `GAMESTEAM_SearchResultNode` - everything except the vtable
   at +0 and the intrusive list links at +4/+8) swapped to the logical tail via a two-pointer
   partition. The node chain, walker cursor cache, and all node addresses stay untouched -
   only contents move. The identity sub-object pointer (+0x114) moves with its payload, so
   identity/RTT attribution stays correct.
2. **Count shrink**: the game's row count (`+0xae8`) is then set to `orig - hiddenCount`.
   Positions `0..shown-1` now form exactly the shape the game's own populate pass
   (`FUN_0046d890`) produces when a refresh returns fewer lobbies - count shrink is a
   vanilla-exercised path (renderer stops drawing, selection can't reach, RTT resolver skips,
   "not matching" text appears at count 0). Hidden rows physically persist past the count,
   parked in the pool.
3. **Restore = the same thing in reverse**: a peer who stops being hidden (TTL expiry, manual
   Restore in the config window) simply stops being partitioned out on the next pass - their
   parked payload swaps back into the visible region and the count grows back. Fully
   symmetric, no refresh needed, works because nothing overwrites parked payloads between
   populates.
4. **Count resync protocol** (the game also writes this field): after every delivery
   (`handler->Run()`), `m_gameListResyncPending` is set - the populate pass runs on a
   subsequent tick, so the live pass waits until the count visibly changes from the last
   value we wrote (or 1.5s passes, covering the kept-count-coincides case) and re-learns the
   game-authored count (`m_gameListOrigCount`). Outside deliveries, any count read that
   differs from what we last wrote means the game repopulated on its own (e.g. metadata
   update re-running populate from the cached records at `+0xcf4` - which also resurrects
   hidden rows for <=400ms until the pass re-hides them; cosmetic only). Spontaneous
   repopulates always restore the game-authored count (computed from the same records), so
   they cannot be confused with our shrunken value while any row is hidden.
5. **Ordering** (the existing permutation rewrite) then operates purely within the shown
   region `0..shown-1`, stable against the pre-partition visible order. If the partition ever
   aborts mid-way (bad pointer - never expected), the count write is skipped entirely so a
   half-partitioned region can never be clipped.

Safety recap: all writes are plain memory on the game's main thread (frame-atomic vs
renderer/selection - same-thread), no vtable calls beyond the long-proven mgr slot-7 getter,
walking parked positions past the count is fine (physical node pool >= raw record count, the
game's own populate walks it blindly the same way), and every consumer of rows resolves
through perm/count which are kept mutually consistent within a single pass.

**Deliberately unchanged:** delivery-time compaction still removes already-known-bad peers
before the game ever sees them (no visible blip). Consequence: a peer hidden AT DELIVERY has
no game row to restore live - manual Restore still needs the next refresh for those, as the
config window already says. Peers hidden LIVE (verdict landed while the list was up) now
disappear within ~400ms and can reappear within ~400ms of being restored. True row CREATION
(injecting rows the game never delivered) remains out of scope - no use case, since the mod
controls delivery membership.

Build verified clean (Debug|Win32). **Not yet live-tested.**

### Test protocol for the next session

1. Idle on a populated list for 3+ minutes without touching anything: the config window must
   stay up the entire time (the old bug killed it at exactly +50s after the last refresh).
   Back out of the list: window must still close promptly.
2. With the filter ON, wait for a probe failure while the list is up (or force one): the row
   should VANISH (not just sink) within ~1s - grep `live row count N -> M`. Row count text/
   scroll behavior should look exactly like a smaller natural list.
3. Restore the peer from the config window: if their row was live-hidden, it should reappear
   within ~1s at its sorted position.
4. Cross-check no ghost/duplicate rows right after the game's own auto-refresh (count resync
   window) and after backing out + re-searching (search-start clear).

## VERIFIED: live deletion works; full rework to all-live filtering, periodic re-probe, new Network Filter + requirement filter, and config-window UI overhaul (2026-07-12, same day)

### Live-deletion verification (fresh DEBUG.txt, md5-verified deployed build)

96 `live row count` / `live order applied` events across the session, all consistent:
initial bulk hide (`28 -> 19 (9 hidden of 28)`), incremental hides as verdicts landed
(`23 -> 22`, `22 -> 21`), correct partition swap counts throughout, no anomalies after
auto-refreshes or re-searches. Also **zero** occurrences of `onList=0` while `state1=39` -
the window-disappearance fix from the previous section held. Both mechanisms are
confirmed working in real gameplay.

### Rework: filtering is now fully live (user-directed redesign)

Since rows can now be hidden AND restored in place, delivery-time compaction was removed
entirely:

- **Delivery serves the full list, always** (`BuildCompactedListAndDeliver` no longer
  consults `ShouldHidePeer`) - the list appears instantly and complete, then "polishes
  itself" live. This also kills the old asymmetry where a peer hidden at delivery had no
  game row to restore without a refresh - now EVERY hide is live-reversible.
- **All hide criteria live in `PollGameListAndApplyOrder()`** (~2.5x/sec), evaluated per
  row from the game's own live data:
  1. Reputation (unreachable/failed peers) - gated by the existing
     `enableRankedListConnectionFilter` checkbox. Newly-hidden announcements moved here
     (same once-per-episode dedup via `m_announcedHidden`).
  2. **Network Filter** (new setting `rankedListNetworkFilter`, 0-4): hides rows whose
     current Delay digit is below the floor (0=All/off, 1-3="N and above", 4="4 only").
     Unresolved digits stay visible until they resolve.
  3. **Unmet-requirement filter** (new setting `hideUnmetRequirementRooms`): replicates
     the game's own join gate (`FUN_004ae6d0` case 0x27, the source of "The room's
     connectivity requirements are not met"): required tier = row's RANK_RTT_FILTER word
     (`entry+0x10a`, values 1-4, other nonzero = 4), else RANK_AREA_FILTER (`entry+0x5e`,
     1 -> requires digit 2, 2 -> requires digit 3); hide when my measured Delay digit to
     that host (`GameDelayDigitFromRtt(entry+0x78)`) is below it. Unresolved RTT = shown
     (the game itself waits in "RMSR_CheckingRTT" rather than rejecting).
  `IsPipelineActive()` now includes both new settings.
- **Periodic whole-list re-probe** (`kListRecheckIntervalMs = 15s`): every listed
  candidate - hidden included - gets a forced P2P re-probe (`StartProbeIfNeeded(id, force
  =true)` bypasses verdict-freshness TTLs; session-blocked peers still excluded). A
  recovered peer flips to Reachable when the probe lands and their row pops back within
  ~400ms; still-dead peers re-confirm (Steam error typically within ~20s). The cycle
  timer resets per search (`m_lastListRecheckTickMs`). Log line: `periodic list recheck:
  re-probing N of M candidates`.
- **`probeElapsedMs` is now first-measurement-only** in `PollProbes()` - re-probes of
  still-open sessions resolve in one pump (~16ms) and would otherwise flatten the
  fallback sort metric for every reachable peer.

### Config window UI overhaul (RankedListFilterWindow)

- Layout now: Sort combo | separator | **Network Filter combo** (All / 1 and above /
  2 and above / 3 and above / 4 only) | Hide-unreachable checkbox | **"Hide rooms that
  would reject your connection"** checkbox | status line.
- The hidden-players list is GONE from the main window. The status line is split:
  `Last search: N shown,` + **`M hidden` as a clickable link** (underlined on hover -
  note: this repo's old ImGui has no `ImGuiMouseCursor_Hand`, so the underline is the
  affordance) that toggles a separate **"Hidden players" window**: fixed default size
  (380x260, user-resizable), "Restore all" pinned at top, and the per-player
  Restore+name+reason list inside a **scrollable child region** so it never grows off
  screen. Note shown when empty: filter-hidden (tier/requirement) players are not
  listed there - those are rule-based and un-hide automatically; only reputation-hidden
  peers are individually restorable (RestorePeer erases the verdict -> row returns live
  within ~400ms).
- New settings rows in `settings.def`: `RankedListNetworkFilter` (int, "0"),
  `HideUnmetRequirementRooms` (bool, "0"). New/updated localization rows (key/en/es) for
  all new labels and help texts; the split status-line strings are
  `"Last search: %d shown,"` and `"%d hidden"`.
- Fixed while in there: the old code's `reasonText = L(...).c_str()` dangling-pointer
  pattern (temporary std::string destroyed at end of expression) - now holds the
  std::string.

Build verified clean (Debug|Win32). **Not yet live-tested.**

### Test protocol for the next session

1. Fresh search: full list appears instantly (nobody pre-hidden), then known-bad peers
   vanish within ~1s.
2. `periodic list recheck` lines every 15s; a hidden peer whose connection recovered
   should reappear without any user action (the acid test for the rework).
3. Network Filter at "2 and above": all rows showing Delay 0/1 vanish once measured;
   switching back to "All" restores them all within ~1s.
4. Requirement checkbox: rows that would show "connectivity requirements not met" vanish
   once your Delay to them resolves. Verify against a room you know rejects you.
5. Click "M hidden" -> Hidden players window opens, scrolls when long, Restore works
   live (row returns without refresh), closing via X works, link toggles.
6. Watch for count flapping: a peer on the hide boundary (e.g. Delay exactly at the
   floor as it fluctuates) will hide/restore repeatedly - if that looks bad in practice,
   add hysteresis (require the digit to differ from the threshold for 2+ consecutive
   passes before flipping).

## Post-test fixes: unified hidden-players list with reasons, session-block system removed, and the scrollbar/cursor overflow bug fixed via the list WIDGET (2026-07-12, same day)

User live-tested the all-live rework: everything works except three issues, all fixed this
session.

### 1. Hidden-players window now lists EVERY hidden row, with its reason, all restorable

Previously only reputation-hidden peers appeared (GetHiddenPeers scanned m_verdicts).
Now `PollGameListAndApplyOrder()` rebuilds `m_liveHiddenPeers` (steamId -> name + reason)
every pass from the actual hide decisions, and `GetHiddenPeers` serves that snapshot:
- Reasons shown: "unreachable", "connection failed", "below network filter",
  "network requirements not met" (`HiddenReason` enum replaces the old flag fields on
  `HiddenPeerInfo`).
- **Entries with no known display name are skipped** (user: "??? - unreachable" rows are
  noise; if we can't name them, don't list them).
- **Restore now works for rule-based hides too**: `RestorePeer`/`RestoreAllPeers` clear the
  reputation verdict AND add the peer to `m_restoreExemptions`, which suppresses the
  network-tier and requirement filters for that peer until the next periodic recheck
  (15s) - so a restore visibly sticks instead of being undone 400ms later, and then fresh
  data decides again (user explicitly OK'd re-hiding on next probe). Exemptions also clear
  at search start.
- Checkbox renamed: "Hide rooms that would reject your connection" -> **"Hide rooms with
  unmet network requirements"** (user found the old name horrible). CSV updated (en/es).

### 2. Session-block / repeat-offender system REMOVED

With the 15s clean-slate recheck, permanent per-session penalties only create stale hides.
Removed: `PeerVerdict::sessionBlocked`, `kSessionBlockFailCount`, the escalation in
`MarkUnreachable`, the probe exclusion, and the "blocked (repeated failures)" UI reason.
Kept: the brief `kReactiveFailHideMs` (2min) hide for a real failed join - that is direct
evidence, but it now expires on its own and the peer gets re-tested like everyone else.

### 3. Scrollbar/cursor reaching into hidden territory: root cause = the list WIDGET keeps its own delivery-time count

The row list (count at listStruct+0xae8) is only the DATA side. The scrollbar, cursor
movement, and row selection are owned by a separate UI **widget** object that captures the
row count when the game builds it and never re-reads ours. RE'd this session (raw disasm,
no live debugger):

- Builder: `FUN_0064bfb0` ("NetworkRankMatchSearchResultWindow"). A lazy-init UI context
  singleton (`FUN_00643b40`, static RVA `0xEF1ED0`, guard bit RVA `0xEF4898`) holds a pool
  pointer at `+0x29C4` -> 4 widget containers of stride `0x15D90` (in-use flag `+0x8C`,
  config-string pointer `+0x90`). The ranked list's container is identified by its config
  pointer == the static "Rank Match Search Result" string (RVA `0x566238`); its sibling
  `FUN_0064c160` builds the casual "Room Search Result" widget the same way (config RVA
  `0x56611C`) - do not confuse them.
- Widget struct at container+0x68: `+0x3C` scroll top, `+0x40` slot array (50 x 0x6FC,
  slot+4 = active flag), `+0x15D78` cursor, `+0x15D80` bottom visible row, `+0x15D84`
  page-size-minus-1 (builder clamps to 10 -> 11 visible rows), `+0x15D88` item count
  (bounds cursor wrap - see cursor-next `0x00648DF0` / cursor-prev `0x00648ED0` - and the
  scrollbar). Builder tail (`0x0064C126`) sets `15D84`/`15D80` = min(count-1, 10).
- Fix: new `FixupRankedResultWidget(shownCount)` (called from the live pass right after
  the row-count write, and from the features-off cleanup): resolves the widget via the
  chain above (guard-checked, IsBadReadPtr/WritePtr everywhere), then - write-on-change
  only - sets count, pageM1 = min(count-1,10), clamps cursor and scroll top, recomputes
  bottom = top + pageM1, and sets slot active flags to exactly rows 0..count-1. Log line:
  `widget fixup: count N -> M`.
- The features-off cleanup branch now also restores the game-authored row count
  (`m_gameListOrigCount`) and re-fixes the widget, so turning everything off brings the
  full list back instantly (consistent with the all-live philosophy).

Build verified clean (Debug|Win32). **Not yet live-tested.**

### Test protocol for the next session

1. Hide some rows via each mechanism (unreachable, network filter, requirement) - ALL of
   them should appear in the Hidden players window with the right reason; no "???" rows.
2. Restore a network-filter-hidden peer: row returns within ~1s and STAYS until the next
   15s recheck (then re-hides if still below the floor).
3. The scroll bar should now match the visible list exactly, and the cursor must not be
   able to move past the last visible row (this was the main visual bug).
4. Turn all filter features off mid-list: every hidden row should return immediately,
   scrollbar included.
5. Grep `widget fixup:` lines and sanity-check the counts track `live row count` lines.

## Testing protocol reminder

User builds/deploys manually via Visual Studio ("Release Deploy" config) — Claude never
deploys automatically. Before trusting any `DEBUG.txt` analysis, verify the deployed
`dinput8.dll` md5sum matches the freshly built one for the config the user actually used
(check `bin/Release/dinput8.dll`, not just `bin/Debug/`) to avoid diagnosing stale-build
behavior as if it were current.
