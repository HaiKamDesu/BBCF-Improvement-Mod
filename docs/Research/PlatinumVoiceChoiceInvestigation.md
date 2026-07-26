# Platinum Voice Choice (Sena/Luna) Investigation (2026-07-15)

> **SHIPPED STATE (2026-07-25): OFFLINE-ONLY by default.** `PlatinumVoiceChoice` applies the
> voice override offline (training / vs CPU / replay / local versus) via a client-side load+play
> in-register bias of the personality read; there it is safe. ONLINE it is DISABLED by default
> because it is CONFIRMED to desync (mounting the other voice bank diverges checksummed battle
> state). A bool setting `PlatinumVoiceOnlineDesyncUnsafe` (default 0, with loud DESYNC warnings
> in the tooltip) gates the online path for anyone continuing the work. A desync-safe online
> path is viable via "mod-private audio" (load the other .pac ourselves + XACT RegistBank +
> register cues into a spare registry slot + redirect playback, leaving all checksummed state
> byte-identical to vanilla) — the recipe is RE'd in this doc but NOT yet implemented
> (large/crash-prone; the core cue-container assumption is unproven). The chronological history
> of every attempt (flag-write desync, suffix-only silence, load+play bias online desync,
> mid-match reload crash) is below.

**Verdict: flag identified and confirmed by two independent methods. Sync-across-network status is unresolved (no evidence either way) — treat the settings.ini override as a local-only cosmetic force unless/until proven otherwise.**

## Background

Platinum the Trinity randomly picks one of two "personalities" (fan-named Sena/Luna) each
time she's selected, affecting her voice lines (and, per the memory-diff evidence below,
possibly some move data too). Goal: find the flag controlling this and add a
`PlatinumVoiceChoice` settings.ini override.

## Method 1: empirical memory diffing (DEBUG.txt)

Added a temporary hook dumping Platinum's `CharData` block to `DEBUG.txt` on load, then ran
labeled training-mode sessions ("voice 1"/"voice 2", user-identified as Sena/Luna) and
diffed byte-for-byte across sessions.

- No difference in any documented `CharData` field.
- Found a large (~12KB, offsets ~0x2278-0x5170) undocumented padding region containing what
  looks like per-move active-frame debug strings (`pt417`, `a9c7c8`-style pairs). Content
  differed consistently between Sena/Luna sessions (208 offsets, 0 exceptions across 9
  independent loads), including *before* any inputs were thrown — ruling out "just an
  action-dependent cache coincidence." This suggests personality may affect more than just
  voice (frame data?), but it's a downstream effect, not the root flag, so it wasn't
  pursued further.
- Extended the dump to the small per-player "select struct" that the existing
  `GetPaletteIndexPointers` hook already points into (used for palette-index tracking).
  This struct sits at a **fixed, static address** (not heap-allocated), one slot per player,
  0x20 bytes apart. Diffed across 5 labeled sessions (voice 1,1,2,2,2... etc across two
  separate test runs, 14 samples total): a 4-byte field at **struct+0x4** matched the
  reported voice with **zero exceptions**: `1 = Sena, 0 = Luna`. struct+0x0 = character
  index (confirmed 0x11 = 17 = Platinum in every sample).

## Method 2: static disassembly (BBCF.exe v8.0 dumpbin dump, `tools/bbcf_disasm_ascii.txt`)

Note: the disasm file has a stray UTF-16 BOM (`FF FE`) despite being plain ASCII after that
— `file` misdetects it as UTF-16 and breaks naive `grep`. Strip the first 2 bytes
(`tail -c +2`) before grepping, or tools will silently return zero matches.

Located the copy function that commits the per-player select struct into its battle-time
copy (`edx+0x1648..0x1668` → `edx+0x24D8..0x24F8`, 0x20 bytes/player, matches the 0x20
player stride found empirically):

```
0047D910: mov edx,ecx           ; edx = select-struct singleton base (from 0047E860())
  ...
0047D927: lea esi,[edx+1648h]   ; source = "live" struct (what our hook captures)
0047D92D: lea edi,[edx+24D8h]   ; dest = "committed" struct (battle-time copy)
0047D933: mov ecx,8
0047D938: rep movs dword ptr es:[edi],dword ptr [esi]   ; 0x20 bytes = 8 dwords
```

Traced backward to the write site for `+0x164C` (== struct+0x4, our confirmed flag),
inside character-select confirm handling:

```
0047DA02-0480AA2: cmp dword ptr [edi+108h],10h   ; mode/context gate
0047DA02-0480B1A: jne  <skip the randomization block entirely>
  ...
0047DA02-0480A18: call 0040BF00       ; per-stream RNG, used just above for rand()%10 (palette 0-9)
0047DA02-0480B5A: call 0040BF00       ; same RNG, this draw feeds the personality flag
0047DA02-0480B64: and edx,80000001h   ; classic MSVC signed-mod-2 idiom
0047DA02-0480B71: mov esi,[ebp+8]     ; player index (0/1)
0047DA02-0480B74: shl esi,5           ; * 0x20 stride
0047DA02-0480B7E: mov dword ptr [esi+edi+164Ch],edx   ; <-- writes our flag: rand()%2
```

`0x0040BF00` resolves to `GetRandomStream(streamIndex)->Next()`-style wrapper: indexes into
an array of RNG-state objects (`streamIndex*0x9CC + [0xA135C8]`, 0x9CC-byte stride per
stream — consistent with each stream being an independent PRNG instance, e.g. Mersenne
Twister-sized state). This is a **different** RNG entity than the battle-scoped
`AA_CRandomManager` referenced elsewhere in `GhidraDefs.h` (that one lives inside the
in-battle object-manager struct; this one is indexed from a standalone global array used at
character-select time).

Three read sites for the flag were also found (generic engine predicates, likely
script-callable), each checking one specific literal:
```
cmp dword ptr [esi+ecx+164Ch],1  ; sete al   (used elsewhere by 1+ characters, generic reuse)
cmp dword ptr [esi+ecx+164Ch],2  ; sete al
cmp dword ptr [esi+ecx+164Ch],3  ; sete al
```
Empirically we only ever observed 0 or 1 for Platinum; the `==2`/`==3` checks are presumably
for other characters reusing the same generic slot for unrelated purposes (BBCF's engine
reuses generic per-character "variant" fields across the cast — same pattern as the `SLOT_`
fields already documented in `CharData.h`).

## Network sync: unresolved

Grepped for all other references to the "committed" destination offset (`+24D8h`/`+24DCh`)
in the whole binary: exactly one other hit, a mirror-image **restore** function
(`0047FD20`, committed → live, likely used when returning to character-select from a match
to repopulate the editable UI state). No socket/send/recv calls appear anywhere near either
copy function or their callers.

Per `SpectatorDesyncInvestigation.md`, BBCF's netcode is a GGPO rollback port: **no state
crosses the wire during a match** — correctness relies entirely on deterministic replay of
synced inputs from frame 0. That guarantee applies to the *in-match simulation*; the
personality roll happens during the pre-match character-select phase, which is a distinct
(and not necessarily deterministic-lockstep) phase. Found no evidence this flag is
explicitly transmitted in a select/ready packet, but also found no evidence it *isn't* —
this would need either a live two-client packet capture or locating the
character-select-state serialization code (not identified in this pass).

**Working assumption until proven otherwise: treat this as a local-only cosmetic pick.**
Each client may roll and hear a different personality; there is no confirmed guarantee the
opponent hears the same choice you force locally.

## Implementation

- `PlatinumVoiceChoice` setting added (`src/Core/settings.def`): `0` = vanilla RNG (default),
  `1` = force Sena, `2` = force Luna.
- Enforced in `src/Hooks/hooks_palette.cpp`, `ForcePlatinumVoiceChoice()`, called from the
  existing `GetPaletteIndexPointers` hook for both players. Overwrites struct+0x4 right
  before the commit copy (`0047D910`) runs, so both the live and committed copies end up
  forced consistently.

---

# Resolved: online sync mechanism + why the override fails online (2026-07-22)

Follow-up after Cuack's two-session bug report ("Platinum voice not working online"). With
the richer `[PlatVoice]` diagnostic logging (setting value, both char indices, the flag the
game rolled *before* our write, online state, local match slot), the mechanism is now
**confirmed**, and the original v8.0 "sync unresolved" is closed.

## Field evidence (Cuack, `Bug Reports/Cuack Platinum voice/Test 2`)

Single-Platinum online matches (local player = match slot 0 = P1; opponent not Platinum):

| Setting | game roll (`origFlag`) | our override | heard |
|---|---|---|---|
| Luna | 0 (Luna) | `0 -> 0` no-op | Luna ✓ (luck) |
| Luna | 0 (Luna) | `0 -> 0` no-op | Luna ✓ (luck) |
| **Luna** | **1 (Sena)** | **`1 -> 0` forced Luna** | **Sena ✗** |

The last row is the smoking gun: the hook fired, detected Platinum online, wrote 0 (Luna) to
the live struct — yet **Sena (the original roll) played**. The voice online always equals the
pre-override roll; our write is ignored. Both players always heard the *same* voice even on
vanilla RNG → the game synchronizes it natively.

## Sync mechanism

The roll is **not** transmitted in a packet. It is a **deterministic shared-seed PRNG draw**:
`FUN_004807E0` (personality randomize) runs at character-select *confirm* on both clients
(lockstep flow, driven by synced inputs), and draws stream 0 of the global PRNG
`FUN_0040BF00` (`streamIndex*0x9CC + [0xA135C8]`, `Next()` = `FUN_004528A0`). That stream is
seeded once per match from a value both clients agree on, so both roll **identically** →
identical voice. This is the same class of RNG the engine uses for other select-time picks.

The RNG is **MT19937** (Mersenne Twister): `FUN_00452990` regenerates the 624-word block,
`FUN_00452940` = `init_genrand(seed)` (`0x6C078965` multiplier). Two seed paths were found:

- **Offline** (training / arcade / vs-CPU): `FUN_00471F00` seeds streams 0/1 (`0x00472262` /
  `0x00472284`) from a **high-resolution timer** (QueryPerformanceCounter-style, imports
  `[0x84A37C]`+`[0x84A378]`). Non-deterministic, per-boot.
- **Online + replay**: `FUN_0069D001` seeds streams 0/1 (`0x0069D7BA` / `0x0069D7CE`) from a
  **stored per-match value**, copied earlier (`0x0069D54C..0x0069D563`) out of
  `FUN_0055C540()+0x83E48` / `+0x83E4C` into the per-slot match/replay struct at
  `0x0155B908` / `0x0155B90C` (stride `0xA0`). Both clients seed from the **same** value →
  identical MT sequence → identical roll. (The exact wire handshake that agrees this seed at
  session init isn't a distinct char-select packet; per `SpectatorDesyncInvestigation.md` the
  netcode is a GGPO rollback port where per-match state is agreed at session init and only
  inputs cross the wire mid-match. That the seed is deterministic-shared is proven both by the
  code path and by the field evidence that vanilla always produced the same voice on both ends.)

**There is no native channel carrying the voice flag** — nothing to piggyback / intercept.
A modded client therefore cannot "tell" a vanilla opponent which voice to use; the opponent
recomputes the roll from the shared seed regardless. Any online force must be a MOD-level
exchange (both peers modded), applied identically on both sides — see the design options below.

## Why our current hook is ignored online (the ordering bug)

- `FUN_004807E0` writes the rolled value directly into the **live** select-struct at
  `[playerIndex*0x20 + base + 0x164C]` (base = `FUN_0047E860()` singleton).
- The battle-time reads of the flag are script-VM condition opcodes
  (`0x0057D3F3 / 0x0057D416 / 0x0057D439`, `ret 8`, `sete al`) that read from that **same
  live struct via `FUN_0047E860`** — *not* from the `+0x24D8` committed copy.
- Our `ForcePlatinumVoiceChoice` runs from the `GetPaletteIndexPointers` hook, which sits on
  the per-frame commit copy (`0047D92D`, `lea edi,[edx+24D8h]`). Online, the confirm-time
  roll (`FUN_004807E0`) lands **after** the last time our commit-copy hook runs, so the roll
  overwrites our value in the live struct, and the battle reads the roll. Our write is dead.

## Correct hook point

Force the value **at the roll**, `FUN_004807E0` (base-rel `0x000807E0`), substituting the
rolled register right before it is stored:
- P1 write `0x00480B7E mov [esi+edi+164Ch],edx` → force `edx`
- P2 write `0x00480BAC mov [ebx+164Ch],eax`     → force `eax`

Forcing here puts our value into the live struct at the authoritative moment; all downstream
battle reads honor it. Offsets recorded in `GhidraDefs.h`
(`ADDR_PlatinumPersonalityRoll*`).

## DESYNC CAVEAT — must be designed around

The battle-time reads are **script-VM opcodes**, i.e. the personality flag feeds the
**deterministic simulation** (not purely cosmetic audio). Since `FUN_004807E0` is part of the
lockstep-deterministic select flow, if the two clients force **different** values the
simulations diverge → desync. Therefore forcing at the roll is only safe if both clients end
up with the same per-slot value. Two viable designs:

1. **Network-synced force (correct, matches user expectation "each controls own voice").**
   Exchange each player's `PlatinumVoiceChoice` over the existing packet path
   (`OnlinePaletteManager` is the natural home), then on BOTH clients force each Platinum slot
   to *that slot owner's* choice. Deterministic on both sides → no desync.
2. **Offline/local-only (safe interim).** Only force in offline / training / replay contexts
   (no peer to desync); leave online on vanilla shared RNG. Effectively reverts the online
   promise but removes the desync risk immediately.

Naïvely moving the current local-only force to the roll site **without** one of the above
would risk desyncs whenever the two players pick different voices — do not ship that.

## Implementation (2026-07-22, option 1: network-synced force)

Shipped the mod-level exchange. Files:

- **`Packet.h`** — new `PacketType_PlatinumVoiceChoice` (1-byte payload: the settings enum
  0/1/2).
- **`NetworkManager.cpp`** — dispatch case → `OnlinePaletteManager::RecvPlatinumVoiceChoicePacket`.
- **`OnlinePaletteManager.{h,cpp}`** — `SendPlatinumVoiceChoicePacket` (sent in the existing
  match-init `SendPalettePackets` flow, which goes only to IMPlayers that completed the
  Announce/Acknowledge handshake → **never reaches a mod-less opponent**, so no interference),
  `RecvPlatinumVoiceChoicePacket`, `m_playerVoiceChoices[2]` (per-slot, -1 = none),
  `GetPlayerVoiceChoice()`, reset in `OnMatchInit`/`ClearSavedPalettePacketQueues`.
- **`hooks_palette.cpp`** — removed the old commit-copy-time force. New
  `ApplyPlatinumVoiceForce()` called every frame from `MatchState::OnUpdate`; writes the flag
  directly into the live select-struct (`FUN_0047E860()` singleton + `slot*0x20 + 0x164C`).
  Slot mapping: our own match slot → our setting; opponent slot → their received choice (else
  vanilla RNG). Offline → P1 (local-player slot; P2-side/local-2P not distinguished).
- **`GhidraDefs.h`** — `ADDR_GetCharSelectStruct` etc.; address resolved via
  `GetBbcfBaseAdress() + RVA`.

Two modded clients converge on identical per-slot flags → no divergence. A mod-less opponent
hears you via their own RNG (accepted cosmetic tradeoff). **Still requires the live
mismatch test** (see caveat above) before wide release to rule out any sim divergence.

## Resolved: the flag IS synced state — mismatch desyncs (Test 3, 2026-07-22)

The live mismatch test (Cuack modded vs a mod-less opponent) settled the caveat:

| Test | Setting | you heard | opp heard | forced value vs opp RNG | result |
|---|---|---|---|---|---|
| 1 | Default | sena | sena | (no force) | ok |
| 2 | Luna | luna | luna | match (opp RNG rolled Luna) | ok |
| 3 | Luna | luna | sena | **mismatch** (opp RNG rolled Sena) | **DESYNC → connection lost** |

Clean signal: **matching value → no desync; differing value → desync.** The personality flag
is therefore part of the synced/checksummed match state (gameplay may be identical, but it is
hashed). The async per-frame write itself is fine (Test 2 forced a value via the same write
with no desync — because it happened to match). **Forcing a value that differs from what the
opponent's client holds desyncs.**

Consequence: **you cannot override the voice against a mod-less opponent** — their client
recomputes the flag from the shared-seed RNG, and any override diverges from it. The
"cosmetic-only, safe against mod-less" assumption is disproven.

### Fix applied

Online forcing is now gated on the opponent being confirmed modded:
`OnlinePaletteManager::HasReceivedVoiceChoice(oppSlot)` (a mod-less client never sends the
voice packet). `ApplyPlatinumVoiceForce()`:
- **offline** → force P1 (unchanged).
- **online, opponent modded** → force both slots to their converged owner choices (both
  clients compute identical values → in sync).
- **online, opponent mod-less / not-yet-received / spectator** → do NOT touch the flag; leave
  vanilla shared RNG (you hear the game's pick for your own Platinum too, but no desync).

Remaining risk to validate: the modded-vs-modded arrival-order race (we start forcing when we
receive the peer's packet; if they haven't applied ours yet there's a brief window). Test 2
shows a matched async write doesn't desync, and pre-packet both sides share identical RNG, so
convergence should be clean — but confirm with a two-modded-client match.

## Mid-match voice swap + real-time mod-vs-mod (2026-07-22, implemented)

Once the load+play override worked, mid-match swapping was added. RE of the XACT loader found
it runs on a dedicated WORKER thread (`CreateThread` @0x4506AD, request queue under a critical
section; `CreateFileA`/`ReadFile` on the worker) — the game thread only enqueues a load. So
re-triggering a bank load mid-match does NOT stall the game thread (no freeze -> no netplay
disconnect); the only cost is latency (first post-swap line may be briefly silent until the
worker finishes).

Design (on-demand reload, "Option A"): the voice-bank load routine `FUN_00555A20`
(`__thiscall`, ecx = voice manager) is PURE LOAD — it contains no play calls and takes no
stack args — so re-invoking it just remounts banks (our load-bias hooks pick the chosen
personality) without replaying anything. Implementation in `hooks_palette.cpp`:
- The 6 load hooks capture `ebx` (= the routine's `this`, unchanged across it) into
  `g_platVoiceMgrThis`.
- `PollPlatinumVoiceReload()` runs every frame from `MatchState::OnUpdate`, gated to
  `GameState_InMatch` (15). It computes each Platinum slot's effective choice and, on a change
  vs the last mount, re-invokes `FUN_00555A20(this)` (async enqueue → freeze-free).
- `ADDR_PlatinumVoiceLoadRoutine = 0x00155A20` in `GhidraDefs.h`.

Real-time mod-vs-mod: `OnlinePaletteManager::OnUpdate` re-sends `PacketType_PlatinumVoiceChoice`
whenever the local setting changes after the match-init flush (`m_lastSentVoiceChoice` tracks
the last sent value; set in `SendPlatinumVoiceChoicePacket`, reset on match init/clear). The
receiving modded client stores it, and its own per-frame poller remounts the opponent slot's
bank — so a compatible-mod opponent hears the change live.

### v1 (reload only) was still silent — the cue table wasn't re-registered

Testing showed mid-match swap still went silent. Diagnosis: `FUN_00555A20` loads the bank but
does NOT register the game-side cue-name table (`0xEBFF68`) that the play resolver searches —
that is a SEPARATE routine `FUN_0056B850`. So the remount loaded the alternate bank into XACT,
but the resolver kept the original personality's cue names (e.g. Sena's `pt###b`); requesting
the swapped suffix (`pt###a`) found nothing → silence.

`FUN_0056B850` (addr `ADDR_PlatinumVoiceCueTableRegister = 0x0016B850`): `__cdecl`, no args,
self-bootstraps its context (`FUN_0047CA90` → `this`, `FUN_0047E860`), reads the currently
loaded banks by tag (`FUN_0047CA50`, a generic 0x3D4-entry tag→handle table) and rebuilds the
cue-name tables into registry slots 6/7/8 (`FUN_004C07C0`/`FUN_004C0A70`).

Preload-both feasibility (checked, rejected): the tags (`0x48…`) and registry slots (6/7/8) are
hardcoded throughout the mount; making a second personality coexist would need a hand-built
parallel load-submit under spare tags + register into spare slots 2–5 + a redirect hook on
`FUN_004C1060`. High effort and a manual-resource crash surface — not the easy path.

### FINAL (2026-07-23): mid-match abandoned — start-of-match only, via a latch

v2 (below) CRASHED immediately offline: calling `FUN_0056B850` standalone to re-register the
cue tables is not safe outside the game's own load sequence. Preload-both was assessed as not
worth the effort/risk. So mid-match live swapping was dropped entirely. Final design:

- The LOAD hooks latch the mounted personality per slot into `g_platLatchedForced[2]`.
- The PLAY hooks ECHO that latch instead of re-reading the live setting. So toggling the
  setting mid-match cannot make PLAY request a suffix the mounted bank lacks — no silent-voice
  bug. The change simply takes effect at the next match load, when LOAD re-latches.
- The latch is intentionally NOT reset between matches: it always mirrors the currently mounted
  bank (written at mount-decision time), and PLAY only reads it for a Platinum slot after LOAD
  has run, so a prior match's value is never mis-applied and is never wiped mid-match.
- Removed: `PollPlatinumVoiceReload`, the mid-match reload, the cue-table re-register, and the
  real-time packet resend. Mod-vs-mod still applies the opponent's pick at match start via the
  existing match-init `PacketType_PlatinumVoiceChoice` send (not live mid-match).

Everything below is retained as the RE record of the abandoned mid-match attempts.

### v2 (reload + re-register), implemented — REVERTED (crashed)

`PollPlatinumVoiceReload()` on a detected choice change now: (1) re-invokes `FUN_00555A20`
(async bank reload, biased to the chosen personality by our load hooks), then (2) opens a
90-frame window and calls `FUN_0056B850()` every ~15 frames. The async bank load lands after a
variable disk-latency delay; whichever re-register call runs after it lands picks up the new
cues (idempotent — earlier/later calls just rebuild the current tables). Spacing avoids
per-frame churn.

RESIDUAL RISK (could not be settled by static analysis; validate offline FIRST):
- `FUN_0056B850` is normally invoked via the load-sequencing callback; calling it standalone
  mid-match is judged safe (pure cue-table rebuild, self-contained, game-thread only), but is
  unverified in that context.
- `FUN_004C07C0` frees a slot's cue table before rebuilding. Resolve and rebuild both run on
  the game thread (never concurrent), and playing audio holds an engine handle rather than the
  cue-name table, so a free-in-use is judged benign — but unverified.
Validation plan: offline, swap the voice mid-match repeatedly and during active voice lines —
confirm no crash/hitch and the voice changes within ~1.5s; then online (both modded) watching
for desync and confirming the opponent hears the change.

## CORRECTION: the suffix-only hook is silent; the LOAD read must also be biased (2026-07-22)

The first audio approach (below) hooked only the PLAY resolvers' suffix read. Live test: forcing
Sena gave TOTAL silence. Root cause, traced and verified against the shipped game files:

- Platinum's two personalities ship as SEPARATE XACT banks: `data/Sound/Voice/vbtl_pt_0.pac`
  (cues `pt000a..pt417a` = Luna) and `vbtl_pt_1.pac` (`pt000b..pt417b` = Sena). Plus tiny
  `vbtldb_pt_0/1.pac` (dialogue) and a subvoice set. **Only the rolled personality's bank is
  mounted per match.**
- The voice-bank LOAD routine `FUN_00555A20` (battle load; `edi` = select-struct base =
  `FUN_0047E860()`) reads the personality and uses it to index a `.pac` filename format table
  (`0x9DC8D0` `vbtl_%s_%d`, `0x9DC8F0` `vbtldb_%s_%d`, `%s`=`pt`) and mount that bank. Reads:
  slot-0 at `+0x164C` (`0x5560D0` vbtl, `0x55621D` subvoice, `0x55640C` vbtldb), slot-1 at
  `+0x166C` (`0x556176`, `0x556319`, `0x556488`; last one uses `eax` as base, not `edi`). All
  bases are the select-struct singleton. Loader call targets: `0x4763F0/0x476480/0x4764F0`.
- So forcing only the PLAY suffix to "b" requests `pt###b` from a bank that mounted only
  `pt###a` → name lookup (`FUN_004BF730`→`FUN_004C1060` slot, `FUN_00405190` name) fails →
  playback skipped → silence. Exactly the observed bug.
- NOTE: an earlier RE pass mislabeled `0x5560D0` etc. as "UI label" reads. They are the voice
  BANK LOADER. That mislabel is why the suffix-only approach was attempted. Corrected here.
- The audio system is XACT (`AA_CWaveBankDataBase_XACT::RegistBank`), not CriWare. Mounted
  banks + play handles (`char+0x1E9E4`) are per-client heap → never in the GGPO checksum.

### Fix (implemented): bias BOTH the load and play reads, in-register, gated to Platinum
`hooks_palette.cpp` now hooks all 8 personality reads (6 load + 2 play) by direct address and
substitutes the chosen personality IN-REGISTER (`esi` at load sites, `eax` at play sites),
NEVER writing `+0x164C`. Shared `BiasPlatinumPersonality(base, slot)` reads char index at
`base+slot*0x20+0x1648` (Platinum-gated) and returns the chosen value or the original rolled
value. Per-slot choice: own slot → our setting (offline = P1); opponent slot → their sent
choice via `OnlinePaletteManager::GetPlayerVoiceChoice` else the original RNG value. Forcing
the load mounts the chosen bank; forcing the play requests its cues → audible, no silence.
Nothing synced changes → desync-safe offline, online vs modded, and online vs mod-less.
Addresses recorded in `GhidraDefs.h` (`ADDR_PlatVoiceLoad_*`, `ADDR_PlatinumVoiceResolver*`).

Blast-radius note: the load reads are shared by ALL characters (for non-Platinum `+0x164C`
is a voice-set half index, not personality), so every hook is gated on
`char index == Platinum`; non-Platinum returns the original value unchanged. NEEDS in-game
validation: (1) offline Platinum both voices audible; (2) other characters' voices unaffected;
(3) online no desync + correct local voice vs modded and mod-less opponents.

## (superseded) FIRST client-side audio-only attempt (2026-07-22)

The flag-forcing approach was abandoned entirely: since the flag feeds the deterministic
script VM, there is no way to force it against a mod-less/RNG opponent without desyncing, and
gating to modded-only would drop the feature for the common case. Instead we override at the
audio layer, which is purely client-side.

**How the voice file is chosen.** The personality is read in two kinds of sites:
- The query VM (evaluator `FUN_0057D020`, jump table `0x0057DEB8`) — deterministic script
  execution. This is why the flag is checksum-relevant; we never touch it.
- Two voice-file-path resolvers `FUN_005CFC80` / `FUN_005D1440`. Each reads the personality
  (`mov eax,[eax+ecx+164Ch]` at `0x005CFF57` / `0x005D15E2`; `eax = slot<<5`, `ecx` = struct
  base) and appends a filename suffix: `0→"a" 1→"b" 2→"c" 3→"d"` + `.wav` (strings
  `0x958960/68/70/78`, `0x95433C`), producing `data/char/<name><suffix>.wav`. The loaded voice
  handle is stored at char+`0x1E9E4` — a per-client heap pointer, so it cannot be part of the
  GGPO checksum (else every match would desync). **The suffix is the only thing that differs
  between Sena and Luna.**

**The hook.** `hooks_palette.cpp` hooks the personality read in both resolvers by direct
address (`GetBbcfBaseAdress() + RVA`, 7-byte `mov`, jmp-back just past it). The trampoline
computes the local override in `ResolvePlatinumVoiceLocal(base, slot<<5)` and puts it in `eax`
for the following suffix switch — **without writing back to `+0x164C`**. It reads the char
index at `base+slot*0x20+0x1648` and only acts for Platinum. Per-slot decision:
- our own slot (offline = P1; online = `GetThisPlayerMatchPlayerIndex`) → our
  `PlatinumVoiceChoice`;
- other slot → the choice that player sent over `PacketType_PlatinumVoiceChoice`
  (`GetPlayerVoiceChoice`), else the game's RNG value (unchanged);
- spectator → each slot uses that player's sent choice.

**Result:** no synced state ever changes → cannot desync. Works offline, online vs modded, and
online vs mod-less. You always hear your own Platinum in your chosen voice; a modded opponent
hears it too (packet); a mod-less opponent hears their own RNG for your Platinum. The
`OnlinePaletteManager` voice packet is retained purely so modded clients render each other's
picks; it is never required for correctness or safety. The per-frame flag write, the
`GetPaletteIndexPointers` force, and the modded-only desync gate were all removed.
