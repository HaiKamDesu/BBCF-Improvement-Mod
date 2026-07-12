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

## Testing protocol reminder

User builds/deploys manually via Visual Studio ("Release Deploy" config) — Claude never
deploys automatically. Before trusting any `DEBUG.txt` analysis, verify the deployed
`dinput8.dll` md5sum matches the freshly built one for the config the user actually used
(check `bin/Release/dinput8.dll`, not just `bin/Debug/`) to avoid diagnosing stale-build
behavior as if it were current.
