# Character palettes: selection, storage, rendering, online sync

Status 2026-10-19: **mapped, and the mod side rewritten** (see [Rewrite status](#rewrite-status-2026-10-19)). Boundary is complete at L1, and the functions the mod hooks or
would call are at L2. Mapped because of a cluster of field reports: "my palette doesn't load",
"it goes back to default on rematch", "we're both on default", "one round colour 1, the next
colour 7", "default until a bit later". The verdict for each report is in
[Symptoms -> causes](#symptoms---causes). Most of them are bugs in the mod, not the game. The
game has one bug of its own: the online mirror-colour bump.

All addresses are absolute VAs (image base 0x00400000). `GhidraDefs.h` stores them as RVAs
(VA - 0x400000).

## The model in one paragraph

A player's colour is one dword, `select+0x1650` (P1) / `+0x1670` (P2), inside the **live slot
block** of the static select singleton `0x00C903B0`. Character select (offline) or the network
records (online) write it before the battle loads. The battle scene loads one **palette
container** per player (an `AADX_CDynamicTextureList` of `AA_CPalette` objects, sub index
`colour*8 + file`, files 0..7 = body + 7 effect palettes). Every time an entity is drawn,
`EntityPaletteSource` reads the live colour dword and asks the container to `Bind(colour*8+file)`.
`Bind` re-uploads the palette to the GPU only when the sub index differs from the last one
uploaded, or when the palette object's dirty flag is set. The game never re-reads `pal.pac`
during a battle and never sends the live dword over the network. Characters and their effects
are destroyed and recreated every round, and on each recreation they read the live dword again.

## Boundary

Inside: colour selection (CSS widget, slot builders, mirror resolution), the live slot block and
its resume snapshot, the online char-select message and records, the match descriptor, battle
palette container load / lookup / upload / teardown, the per-entity palette slots that copy the
player palette, and the round/rematch lifecycle where it touches any of these.

Stopped at:
- **Lobby/room transport.** Covered in `Lobby_*.md`. Only message 0x13 and the match-state
  broadcast are mapped here.
- **The big scene inits** (0x0061BBB0 VS, 0x0072DD10 main menu, 0x00613150 CSS) and the
  character command dispatcher 0x005AE5C0. Only their palette lines are mapped.
- **Character select dot-art bake** (0x00610470). Already mapped in `GhidraDefs.h`; it reads
  `pal.pac` for the CSS preview only.
- **Effect palette files and `setPalette` (entity+0x354).** Covered by
  `tools/build_effect_palette_map.py` and `src/Palette/EffectSheets.cpp`.

## Functions

| VA | Name | Size | Level | Notes |
|---|---|---|---|---|
| 0x0047E860 | GetCharSelectStruct | 108 | L2 | no args, returns static 0x00C903B0 (`select`) |
| 0x0047E6D0 | CharSel_GetSlotBlock | 31 | L2 | thiscall(select, slot) ret 4 -> select+0x1648+slot*0x20, 0 if slot>1 |
| 0x0047EBB0 | CharSel_GetCssPlayer | 34 | L2 | thiscall(select, i<4) -> select+0x118+i*0x710 |
| 0x0047D910 | CharSel_SaveResumeSnapshot (mod: CharSelectConfigCommit) | 233 | L2 | fastcall ecx=select. Copies CssPlayer 0/1 and both slot blocks to +0x16B8.. / +0x24D8.. **Resume snapshot for returning from a ranked match, not the in-match config.** Never runs online. Mod hook GetPaletteIndexPointers at 0x0047D92D |
| 0x0047FD20 | CharSel_RestoreResumeSnapshot | 264 | L1 | reverse copy snapshot -> live; only caller 0x004A46F0 (return from ranked) |
| 0x004A46F0 | Ranked_ReturnToOfflineMode | 200 | L1 | calls 0x0047FD20, sets scene = saved +0x16B0 |
| 0x004A2B40 | Resume_Save(kind 2) | 48 | L1 | +0x16A8=kind, +0x16B0=scene, then 0x0047D910 |
| 0x004A2B70 | Resume_Save(kind 4) | 48 | L1 | same |
| 0x004A2BF0 | Resume_Save(kind 3) | 48 | L1 | same |
| 0x004A2BB0 | Resume_Clear | 27 | L1 | +0x16A8=0, jmp 0x0047D910 (main menu init: fires the mod hook every main-menu visit) |
| 0x004A2BD0 | Resume_SetRanked | 27 | L1 | +0x16A8=1, jmp 0x0047D910 |
| 0x00613150 | SCENE_CCharSelect::Init | 4135 | L1 | scene id 6 (mod hook GetGameStateCharacterSelect 0x006131F2); offline resume snapshot; restores local CssPlayer char/colour from +0x700/+0x708 |
| 0x0061BBB0 | SCENE_CVSInfo::Init | 10258 | L1 | scene id 0x0E (mod hook GetGameStateVersusScreen 0x0061BBDE); offline resume snapshot |
| 0x0072DD10 | SCENE_CMainMenuEX::Init | 8635 | L1 | scene id 0x1B, mode 0xD, Resume_Clear |
| 0x00628F70 | SCENE_CWinner::Init | 4568 | L1 | scene id 0x10 (mod hook GetGameStateVictoryScreen 0x00628FA9) |
| 0x0074B7F0 | SCENE_CLobby::Init | 4247 | L1 | scene id 0x1F (mod hook GetGameStateLobby 0x0074B811) |
| 0x006C3520 | SCENE_CReplayTheater::Init | 1383 | L1 | scene id 0x1A (mod hook 0x006C3550) |
| 0x0060CA20 | CssUi_ColourCursor | 803 | L1 | L/R cycles CssUi+0xA14[side] over 0..23, skipping locked 12..23 (0x004B9C90) |
| 0x0060BCD0 | CssUi_SideTick | 3401 | L1 | colour cursor, remembers colour within the visit (+0x2A5C), offline mirror fix 0x0060A040, confirm |
| 0x00609B40 | CssUi_InitSideColour | 216 | L1 | offline: CssPlayer+0x52C if same char else 0. Online: OptionData+0x53AC4[char], 0 if locked |
| 0x0060A040 | CssUi_ResolveMirrorColour | 205 | L1 | offline only: while another decided side has same char+colour, bump (0->1->2->3->0, or +1 wrap at 12) |
| 0x0060D930 | CssUi_ConfirmSide | 1206 | L1 | stores OptionData+0x53AC4[char]; online sends msg 0x13 with colour = CssUi+0xA14[side] |
| 0x0060DDF0 | CssUi_AutoConfirmSide | 913 | L1 | timeout variant of the above |
| 0x0060E9F0 | CssUi_OnlineTick | 3123 | L1 | copies the opponent's char/options from the net record but **not their colour** (the CSS never shows it). Timeout: locked colour -> 0 |
| 0x00612530 | CSS_FinishToMatch | 1911 | L1 | offline: CssUi -> CssPlayer+0x52C -> slot builders, replay header. Online: 0x0069C3B0 + 0x0069DE70 |
| 0x004815A0 | Slot_BuildFromCss | 333 | L1 | thiscall(select, slot): {char, 0, CssPlayer+0x52C, +0x530, 1, opt, 0, 100} |
| 0x00480020 | Slot_BuildFromCss_NoHuman | 172 | L1 | same, +0x10 = 0 |
| 0x00480220 | Slot_BuildCpuOpponent | 979 | L1 | arcade CPU: colour 0, or 1 on a mirror of colour 0 |
| 0x0047E500 | Slot_MirrorBumpOther | 62 | L1 | other slot colour 1 on a mirror (arcade boss path) |
| 0x0047E570 | Css_MirrorBumpChallenge | 72 | L1 | challenge mode mirror bump |
| 0x0047ECB0 | Slot_BuildSinglePlayer | 541 | L1 | arcade / score attack / "continue" rebuild |
| 0x004818D0 | Slot_RollVariantAndStage | 219 | L1 | slot+0x04 = SharedSelectRng & 1, flipped on a mirror (the Platinum Sena/Luna bit) |
| 0x004800D0 | Slot_BuildRandomPair | 319 | L1 | attract demo: colour 0, P2 = 1 on a mirror |
| 0x006DE980 | Slot_BuildScripted | 632 | L1 | scripted battles: colour 0, P2 = 1 on a mirror |
| 0x004807E0 | Slot_BuildRolledOpponent (mod: PlatinumPersonalityRoll) | 1109 | L1 | Abyss / quick-change opponent: colour = SharedRng % 10, (c+1)%10 on a mirror; also writes the +0x04 variant bit. The mod's name only describes the +0x164C write |
| 0x00568E90 | SCENE_CBattle::ExitRouter | 2742 | L1 | routes the next scene. Training quick change: colour 0 for a changed character, 1 to break a mirror, **written while the scene id is still 0x0F** |
| 0x00625590 | ResultMenu_Select | 520 | L1 | offline result menu: rematch -> scene 0x0E with no slot rebuild |
| 0x00628760 | Result_Resolve | 1060 | L1 | Winner menu: CSS / arcade continue (0x0047ECB0) / main menu / online request |
| 0x0069C690 | Desc_CaptureFromLiveSlots | 668 | L1 | offline: replay header colour <- live slot +8 (called at battle load too) |
| 0x0069C3B0 | Desc_FillFromNetRecords | 729 | L2 | fastcall(desc): desc+0x3C4 = rec0[2], desc+0x430 = rec1[2] |
| 0x0069DE70 | Desc_ApplyToLiveSlots | 671 | L2 | fastcall(desc), online or replay theater only: writes **both** live slot blocks from the descriptor; colour > 23 -> 0 |
| 0x0049DD90 | Room_SendCharSelect | 248 | L2 | stdcall 11 args ret 0x2C, msg 0x13 (layout below) to the room authority |
| 0x0049BBB0 | Msg13_OnCharSelect | 469 | L1 | authority only: stores the side record, mirror check 0x0049A2F0, marks the broadcast dirty |
| 0x0049A2F0 | Record_ResolveMirror | 71 | L1 | **same char + colour -> colour+1, 0 if >= 12** on the record that arrived second; flips the variant bit. Convention not checked in asm |
| 0x004989E0 | Room_AssembleMatchFromMembers | 3367 | L1 | room type 0x5x: same mirror bump, always on side 0 |
| 0x0049D8D0 | Room_BroadcastMatchState | 503 | L1 | authority sends settings + both records (msg 0x30088) to every match member, spectators included |
| 0x0049B770 | Msg3_4_OnMatchState | 260 | L1 | clients copy the records + settings block |
| 0x0049C920 | Msg18_OnReplayTheaterInfo | 484 | L1 | room replay theater: both records from the replay owner |
| 0x004AF1A0 | NetTask_Ranked | 2647 | L1 | state 0x33 sends msg 0x13 (colour = netTask+0x298); **rematch 0x3A clears both records and re-sends**; 0x35/0x3C desc + apply |
| 0x004ACED0 | NetTask_Room | 3859 | L1 | rematch 0x57->0x58: records reused, host re-sends stage/BGM only, then desc + apply |
| 0x0054C620 | NetMatch_EnterBattle | 186 | L1 | player or spectator: desc + apply, scene 0x0E |
| 0x004D20D0 | NetTask_SetRankedParams | 61 | L1 | netTask+0x294 char, +0x298 colour from the ranked lobby widget |
| 0x00550CF0 | SceneFactory_Create | 403 | L1 | builds the scene named by select+0x110 |
| 0x0054F9B0 | SCENE_CBase::RunLoadPhase | 745 | L1 | step machine at this+0x2C. Step 3 = Init (vtable[2], writes the scene id), step 5 = vtable[9], step 8 = vtable[4] + vtable[7], step 0xB = vtable[11]. Init runs >= 3 frames after the scene is created |
| 0x0056BFE0 | SCENE_CBattle::PreloadSprites | 12486 | L1 | step 5: preloads sprite lists, then zeroes the three palette owners' loaded flags |
| 0x00567D30 | SCENE_CBattle::LoadCharResources | 4014 | L1 | step 8: for owner 0,1,2: sprite list, **PaletteContainerLoad (call 0x00567F35)**, add every HPAL entry, story overrides, CreateGpu; then 40 custom slots, GetCharObjPointers; offline: match init 0x0055F6F0 at 0x00568BF4 |
| 0x0056BCB0 | SCENE_CBattle::OnlineSyncStep | 803 | L1 | step 0xB, online: returns each frame until the netplay sync passes, then match init 0x0055F6F0 at 0x0056BDDE. Skipped if net+0x274 (aborted) |
| 0x00567A30 | SCENE_CBattle::Destroy | 325 | L1 | -> 0x0054FEC0 -> 0x0054FCF0 |
| 0x0054FCF0 | SceneUnloadAll | 305 | L1 | 0x0055C9E0 then 0x0040E880(0) |
| 0x0055C9E0 | BattleScene_ResetLoadedChars | 937 | L1 | zeroes owner+0x830 / +0x82C and the loaded flags of all three owners |
| 0x0040E880 | ResourceRegistry_FreeFromLevel | 66 | L1 | deletes every registry resource >= mark: **frees the palette containers** |
| 0x0055C540 | BattleSceneGet | 108 | L2 | returns static 0x011B6AE0 |
| 0x00557A20 | PlayerResSlot ctor | 249 | L1 | 3 owners of 0x20BDC bytes at BattleScene+0x2B8 |
| 0x005B6310 | PaletteContainerLoad | 122 | L2 | thiscall(ecx=owner, capacity) ret 4. Creates a type-2 resource into owner+0x824, stores its pointer at owner+0x830 (**0x005B6372, mod hook GetPalBaseAddresses; ECX = owner there**), allocates an empty sub array |
| 0x005B6420 | PaletteContainerAddEntry | 440 | L1 | copies one HPAL into sub owner+0x18, then owner+0x18++ (sub index = pal.pac entry order) |
| 0x005B6830 | PaletteContainerStoryOverride | 270 | L1 | story mode: replaces sub colour*8+7 with story0X_palYY |
| 0x005B6940 | PaletteContainerCreateGpu (mod: PaletteTextureRefresh_Main) | 53 | L2 | container->Create(w,h,bpp,1) then Bind(0, 0x300). **Creates textures; not a refresh.** Never call it |
| 0x005B6390 | SpriteListLoad | 143 | L1 | owner+0x82C, sprite images (not palettes) |
| 0x005B6980 | SpriteListCreateGpu (mod: PaletteTextureRefresh_Alt) | 65 | L1 | same as 0x005B6940 for the sprite list |
| 0x005B6AB0 | PlayerResSlot_ResolvePalette | 95 | L2 | thiscall(owner, int* handle, sub, int* outSub) ret 0xC. handle = owner+0x824; **sub >= owner+0x18 -> sub 0** |
| 0x0057C8B0 | EntityPaletteSource | 220 | L2 | thiscall(ecx=root CharData, colour(-1=player), file, int* handle, int* sub) ret 0x10. Reads `select + CharData+0x30*0x20 + 0x1650` (0x0057C91A); sub = colour*8+file (file<0: 7-file*8); colour 0x15 runs 0x00581340 |
| 0x0057C990 | EntityPaletteSourceFixed | 232 | L1 | variant that can force sub file*8+7 regardless of colour |
| 0x005ABD80 | CheckColour22 | 141 | L1 | per frame: live colour == 0x15 -> 0x00581340 |
| 0x00581340 | ApplyColour22Mask | 1914 | L1 | per-character mask palette for the 22nd colour (index 21) |
| 0x00592F70 | CharData::EnableColour22Mask | 173 | L1 | +0x9DC=1, reserves a custom slot into +0x360, copies the player palette |
| 0x005A6C80 | RenderPaletteChooser | 1485 | L1 | tint modes (+0x1258 = 1/2) copy per draw, else own slot +0x358, else link +0x35C, else frame override / +0x356 fixed / EntityPaletteSource |
| 0x0059F630 | Entity_BuildDrawList | 11682 | L1 | asks RenderPaletteChooser for (handle, sub) per sprite |
| 0x004498E0 | Shader_SetTexture | 161 | L1 | dynamic lists: list->Bind(sub, flags), then effect->SetTexture |
| 0x0044C880 | AADX_CDynamicTextureList::Bind | 485 | L2 | thiscall(list, sub, flags) ret 8. **Skips the upload iff `list+0x40 == sub && pal+0xC == 0`.** Otherwise uploads pal+0x81C through staging + ring texture, sets list+0x40 = sub |
| 0x0044B960 | AADX_CDynamicTextureList::Create | 350 | L2 | (w,h,bpp,flag) ret 0x10; D3DXCreateTexture x2, +0x40 = -1 |
| 0x0044C360 | AADX_CDynamicTextureList::OnResetDevice | 142 | L2 | recreate; writes +0x40 = -1 itself (precedent for the mod doing the same) |
| 0x00452E40 | AA_CPalette::GetData | 302 | L2 | vtable[2]; copies the working copy into the locked rect, **clears +0xC** |
| 0x00452D60 | AA_CPalette::LockWorking | 14 | L2 | vtable[10]; sets +0xC = 1, returns +0x81C |
| 0x00452C60 | PalObjCopy | 173 | L2 | thiscall(ecx=dst, src) ret 4; writes dst +0x1C and +0x81C, sets dst dirty, **clears src dirty** |
| 0x00452DE0 | PalObjSetEntry | 87 | L2 | thiscall(pal, index, colour) ret 8; working copy only, dirty if changed |
| 0x00452D10 | PalObjRestoreWorking | 52 | L2 | thiscall(pal) ret; +0x1C -> +0x81C, dirty = 1 |
| 0x004B1620 | PaletteCopyByHandle | 75 | L2 | cdecl(int* dstHandle, dstSub, int* srcHandle, srcSub) |
| 0x0057CAA0 | PaletteIdResolve | 61 | L2 | stdcall(id, int* handle, int* sub) ret 0xC; custom slot id -> (scene+0x178[id], 0) |
| 0x0057E0F0 | EntityGetPaletteColour | 169 | L1 | reads an entry through LockWorking, so it **sets dirty as a side effect** |
| 0x005787E0 | CopyPlayerPalToOwnSlot | 130 | L1 | one-shot copy into the entity's custom slot (+0x358) |
| 0x00578870 | CopyPlayerPalToMaskSlot | 130 | L1 | one-shot copy into the colour-22 mask slot (+0x360) |
| 0x005919D0 | CacheEntityPaletteHandle | 63 | L1 | caches (handle, sub) in entity+0x12C8/+0x12CC; the sub index is computed from the colour at that moment |
| 0x005AE5C0 | CharCommandDispatch | 27231 | L1 | only the palette commands are mapped (see "Writers of palette data") |
| 0x0055F5C0 | MatchState_Reset | 292 | L1 | mod hook GetMatchVariables at 0x0055F67F |
| 0x0055F6F0 | MatchState_Init | 68 | L1 | reset + 0x00561E20. Callers: 0x00568BF4 (offline, step 8), 0x0056BDDE (online, step 0xB), BattleScene ctor. **Once per battle load** |
| 0x00558330 | CreatePlayerObjects | 680 | L1 | mod hook GetCharObjPointers 0x005584EF; at load **and every round** |
| 0x005585F0 | RoundStart | 615 | L1 | mod hook MatchIntroStartsPlaying 0x00558846; every round |
| 0x0055BC90 | BattleEvents_Drain | 599 | L1 | event 2/3: kill all entities + 0x00558330 + 0x005585F0 |
| 0x00562B40 | Round_Next | 2086 | L1 | round counter++, timer reset; also training position reset and replay round skip |
| 0x00561720 | MatchPhase_Dispatch | 1489 | L1 | offline frame and netplay frame (incl. rollback resimulation) |
| 0x0049AEA0 | P2P_Dispatch | 2047 | L1 | game P2P dispatch; mod hook PacketProcessingFunc 0x0049B234 delivers mod palette packets from here |

## Data

### Live slot block (`select+0x1648 + slot*0x20`, 8 dwords; P1 0x00C919F8, P2 0x00C91A18)

| Off | Field |
|---|---|
| +0x00 | character index |
| +0x04 | variant bit (0/1; Platinum Sena/Luna; SharedRng & 1, flipped on a mirror) |
| +0x08 | **colour index** 0..23 (12..23 need unlocking, 0x004B9A00). P1 0x00C91A00, P2 0x00C91A20 |
| +0x0C | per-character 2-bit sub-option |
| +0x10 | 1 = built from a player's pick (human / online), 0 = CPU / scripted |
| +0x14 | character select option |
| +0x18 | English voice flag |
| +0x1C | 100 (probably handicap) |

The block is static and never freed. The resume snapshot at `+0x24D8 / +0x24F8` (colour at
0x00C92890 / 0x00C928B0) is **never read in battle**.

### Other select-singleton fields
`+0x108` game mode (1 arcade, 4 story, 5 versus, 6 training, 0xB replay theater, 0xD menu, 0xF
online, 0x10 abyss), `+0x10C` current scene id (what the mod calls GameState), `+0x110` requested
next scene, `+0x118 + i*0x710` CssPlayer (colour at +0x52C), `+0x1688` local side, `+0x16A8`
resume kind.

### Message 0x13 "char select" (0x18 bytes)
`+0` header 0x00130018, `+4` matchId, `+8` side, `+9` char, **`+0xA` colour (byte)**, `+0xB`
option, `+0xC` flags (bit 1 option A, bit 2 Type-B, bits 6-7 sub-option), `+0xD` voice language,
`+0xE` rank. The authority stores it as the side record at `netData(0x00CAD0C0)+0x6A68 +
side*0x10` (`[2]` = colour) and broadcasts both records.

### Match descriptor 0x0155B470 (also the replay header)
Colour P1 `+0x3C4`, P2 `+0x430`; character `+0x3B8` / `+0x424`.

### Battle scene owners (`BattleScene 0x011B6AE0 + 0x2B8 + slot*0x20BDC`)
Slot 0 = P1, 1 = P2, 2 = shared/common effects (not a 24-colour character container: never write
to it). Live container pointers: `0x011B6AE0+0xAE8` (P1), `+0x216C4` (P2), `+0x422A0` (slot 2).

| Owner off | Field |
|---|---|
| +0x0 | loaded flag |
| +0x4 / +0x8 / +0xC | max width / height / bpp of its palettes |
| +0x18 | palettes in the container (lookup clamps to sub 0 at or beyond this) |
| +0x824 | container registry handle |
| +0x82C | sprite list |
| +0x830 | **palette container** (the mod's `m_pPalBaseAddr`) |

### `AADX_CDynamicTextureList` (vtable 0x00898C2C)
`+0x4` sub array, `+0xC` sub count, `+0x18` texture, `+0x30` kind (1 = dynamic), `+0x3C` staging
texture, **`+0x40` last uploaded sub (-1 = none)**, `+0x48..+0x58` ring of textures. One GPU
texture ring per container, so one per player.

### `AA_CPalette` (vtables 0x00899384 / 0x008993B8)
`+0xC` dirty, `+0x1C` **base copy** (256 x 4 bytes), `+0x81C` **working copy** (uploaded, and
what per-entry writers edit), `+0x101C` width 256, `+0x1020` bpp 32. The mod's file address
`[[container+4] + colour*0x20 + file*4] + 0x1C` is sub `colour*8+file`'s base copy, and the
"+0x800 duplicate" is the working copy.

## Flows

### Colour selection
- **Offline:** CSS widget `+0xA14` (0x0060CA20) -> confirm (offline mirror fix 0x0060A040) ->
  0x00612530 copies it to CssPlayer+0x52C and builds the live blocks (0x004815A0 / 0x00480020 /
  0x0047ECB0 / 0x00480220).
- **Online:** confirm sends msg 0x13 (ranked sends netTask+0x298). The authority stores the
  record, mirror-bumps the record that arrived second (0x0049A2F0), and broadcasts. Every member,
  spectators included, runs 0x0069C3B0 -> 0x0069DE70, which writes **both** live blocks from the
  records before the VS screen.

### Battle load (every mode, each a new SCENE_CBattle)
1. Step 3, Init: scene id 0x0F.
2. Step 5: owner loaded flags := 0.
3. Step 8 (0x00567D30): **PaletteContainerLoad for owner 0, 1, 2 in that order** (the mod's
   GetPalBaseAddresses fires 3 times), fill subs, create the GPU texture, GetCharObjPointers.
   Offline then runs match init (GetMatchVariables -> `MatchState::OnMatchInit`).
4. Online only, step 0xB (0x0056BCB0): waits frames for the netplay sync, **then** match init.
   Frames are drawn, and the mod's per-frame code runs, between step 8 and match init.

Match init runs **once per battle load**: never per round, never on a training position reset,
replay round skip or rollback.

### Rounds
Round end posts event 2/3. 0x0055BC90 kills every entity and recreates both characters
(GetCharObjPointers + MatchIntroStartsPlaying fire every round). **Every round re-resolves every
palette from the live colour dword**, so whatever value it holds at that moment is what the round
shows. The containers are not reloaded.

### Rendering
Per draw: RenderPaletteChooser -> EntityPaletteSource (live colour, file) -> sub `colour*8+file`
-> Shader_SetTexture -> `Bind(sub)`. Bind uploads only on a sub change or a dirty palette, and the
upload clears dirty.

Why the mod's colour-index toggle ever worked: changing the dword changes the sub, so Bind
misses its cache. Why some characters "never redraw" and others do: they only redraw when
something else happens to bind another sub or set dirty (an effect file in between,
EntityGetPaletteColour, a palette command).

### Rematch and the next match
| Flow | Scenes | Live colour before the next battle |
|---|---|---|
| Offline result-menu rematch | 0x10 -> 0x0E -> 0x0F | **reused as-is**, including any value the mod left in it |
| Arcade continue / next stage | 0x10 -> 0x0E -> 0x0F | rebuilt from CssPlayer (0x0047ECB0) |
| Training quick change | 0x0F -> 0x0E -> 0x0F | game writes 0 (char changed) or 1 (mirror) in the exit router, while the scene is still 0x0F |
| Ranked rematch | 0x10 -> 0x0E -> 0x0F | records cleared, msg 0x13 re-sent, re-applied (the mirror bump can switch sides) |
| Player room rematch | 0x10 -> 0x0E -> 0x0F | records reused, re-applied |
| Back to CSS / lobby | -> 6 / 0x1F | new pick |

### Teardown
The SCENE_CBattle destructor frees all three containers (0x0055C9E0 zeroes owner+0x830, then
0x0040E880(0) deletes the resources). That happens at least 3 frames **before** the next
scene's Init hook fires (Winner / VS / CSS / Lobby).

## Copies of palette data that go stale

When the mod writes the player container, these do not follow:
- **Own custom slot** (entity+0x358, 40 scene slots): copied once at reservation (0x005787E0).
  The mod already re-syncs these for Platinum only (`PaletteManager::SyncOwnedCustomPaletteSlots`).
- **Platinum item link** (+0x35C): PaletteControlObj palettes, built once.
- **Colour-22 mask** (+0x360): copied once (0x00578870), then masked.
- **Entity handle cache** (+0x12C8): caches a sub index computed from the colour at that moment.
- **Story mode**: file 7 of every colour is replaced at load.
- **Fixed-colour lookups** (file < 0, +0x356, 0x0057C990): use sub `k*8+7` whatever the colour.

### Writers of palette data
Character commands in 0x005AE5C0 edit the working copy and/or restore it from the base copy:
VH_Frame (Valkenhayn, every frame), RG_EyeSwitch, NY_Light, TM_AST_Light, PT_Light,
IzLightSaberSwitch, PH_Light, PH_Cloak, NT_RagnaSwitch, NT_HairSwitch, SU_Light. **So a custom
palette must be written to both the base and the working copy**, as the mod already does,
or these revert it to native.

## The mod against this map

| Mod hook / code | Fires | Problem |
|---|---|---|
| GetPalBaseAddresses (0x005B6372) | 3x per battle load, owner 0,1,2 | P1/P2 picked by a static counter. Correct while the count stays 3, but ECX (owner) gives the slot exactly |
| `m_pPalBaseAddr` | captured at load, nulled at the next Winner/VS hook | **dangles** from the battle destructor until then |
| GetPaletteIndexPointers (0x0047D92D) | main menu, offline CSS/VS, ranked menu; never online | harmless (the pointers are static), but the comment that it is a per-match commit is wrong |
| GetMatchVariables -> OnMatchInit | once per battle load; online after the sync wait | everything palette-related is one-shot here, and the whole function is skipped when the overlay is not initialized |
| `UpdatePalette()` toggle | every palette write | **writes the live colour dword the game itself uses**: entities recreated each round pick up whichever value it holds; index 21 triggers the colour-22 mask code |
| `RestoreNativePalIndex` | Winner, CSS, the frame after leaving InMatch | can overwrite colours the game itself wrote (training quick change) |
| `OnlinePaletteManager::TryApplyReceived` | from packet receipt and every frame | applies as soon as the handle "looks ready", which includes the window between container load and our OnMatchInit, with the previous match's slot indices |

## Symptoms -> causes

### 1. Opponent's colour rewritten / "colour 1 then colour 7" / opponent default on rematch (mod bug, confirmed in a log)
Online, the opponent sends its palette at **its** match init. When we are the side still waiting
for the step-0xB sync, or still loading, the packets arrive first. They are buffered until our
handle is "ready". The handle becomes ready the moment GetPalBaseAddresses gives it the new
container, but it still holds the **previous match's** `m_switchPalIndex1/2`. At least one frame
runs before our match init. In that frame `TryApplyReceived -> ShowReceived -> UpdatePalette()`
writes the opponent's palette into the old slot pair and **sets their live colour dword to the
old slot**. Our OnMatchInit then reads that as their native colour. It backs up their custom
palette as "Default", looks up the wrong palettes.ini slot, and marks the received palette
decided, so it is never re-applied. At match exit the wrong value is restored as their "native"
colour.

Evidence, `DEBUG_DCodeIncident_20261014_231909_slot4.txt` on the dev machine:
- The local player was Kokonoe on colour slot 5 as P2 until 22:29.
- From 22:33 on, the opponent's palette packets arrive 2-6 s before our container load
  (22:33:20 vs 22:33:26). A frame is logged between the load (26.006) and OnMatchInit (26.038).
- **Every opponent in every later match reads as native slot 5**: Ragna, Jin and Naoto.
- A Jin vs Ragna match we were spectating reads both sides as 5.

### 2. "Default until a bit later" (mod design)
Palettes are exchanged only at each sender's match init. The receiver shows native colours until
the sender has loaded, passed the sync and its packets have arrived. Nothing is sent during
CSS/VS, although online the live colours are already final before the VS screen (0x0069DE70).

### 3. "We're both on default" (mod, several paths)
- `MatchState::OnMatchInit` returns before any palette work when the overlay is not initialized
  (the cold-boot / wrong-window / Optimus no-GUI cases): nothing is applied and nothing is sent.
- Spectators who join after the players' match init get nothing (no resend), and their own
  OnMatchInit restores both players to default.
- A one-shot apply that finds the handle not ready, or a palettes.ini name not in the loaded set
  yet (background reload pending), is never retried.

### 4. Colour changes between rounds or between games (mod + one game bug)
- The toggle leaves the live dword on N or N+1. Characters and effects are recreated every round
  from that dword, and .impl files with empty effect blocks leave slot N and N+1 on **different**
  native effect colours.
- The offline result-menu rematch reuses the live dword, including a toggled value.
- `Random` / comma lists in palettes.ini re-roll every battle load, so every game of a set.
- **Game bug:** in an online mirror, the side whose msg 0x13 reaches the host second gets
  colour+1, and anything at or above 12 wraps to **0**. Which side gets bumped depends on packet
  order and can change between ranked rematches (records are re-sent). A mirror on colour 12 or
  above therefore puts the bumped player on default.

### 5. Wrong colour after training quick change (mod bug)
The game writes 0 or 1 into the live dword while the scene is still 0x0F. The mod's
"left the match" restore then writes the previous colour over it, so the new character keeps the
old character's colour number and a mirror loses the game's de-dupe.

### 6. Silent heap corruption / unrelated crashes (mod bug)
Writes through a stale `m_pPalBaseAddr` (received packets, block-list reapply, Platinum sync,
palette editor) can land in the freed containers between the battle destructor and the next
Init hook. `TryGetPalFileAddr`'s SEH probe does not catch a read of freed-but-mapped heap.

## Rewrite design

Goal: the mod never writes the game's colour dword, only palette data, and every write is gated
on a container that is alive and belongs to the current battle. `[mod]` marks behaviour that
deliberately differs from what the mod does today; none of it changes vanilla game behaviour.

1. **Owner-based slot capture.** At 0x005B6372, `slot = (ecx - (0x011B6AE0+0x2B8)) / 0x20BDC`.
   Accept slots 0 and 1 only; drop the counter. Store the owner and resolve the container live
   (`owner+0x830`, null after teardown) on every access, with a vtable check (0x00898C2C) and
   `sub < container count && sub < owner+0x18`. This closes the dangling-pointer window.
2. **Redraw by dirty flag, not by index.** `[mod]` Write sub `colour*8+file`'s base and working
   copies, set `pal+0xC = 1` on each written sub, and set `container+0x40 = -1`; the engine
   writes that value itself at 0x0044C360. The dirty flag alone is not enough: any PalObjCopy
   from the player sub clears it before the GPU pass. Then delete `UpdatePalette()`, the N+1
   slot write, `RestoreNativePalIndex` and its three callers (CSS, Winner, the match-exit edge).
   This removes causes 1 (the index part), 4 (toggle part), 5, the colour-21 side effect, and
   the old Okammunist / Taokaka bugs at the root.
3. **A per-battle generation.** Each GetPalBaseAddresses for slot 0 starts a new battle
   generation. A handle is "ready" only once OnMatchInit has run for the current generation.
   Received palettes are applied only to a ready handle, so they always land after our own init
   and on the right slot.
4. **Online exchange.** `[mod]` Apply palettes.ini only to the local side online (no apply-then-
   restore on the opponent). Re-send our palette once when the opponent's first palette for this
   battle arrives, which covers a late receiver; spectators can be covered the same way. Send
   at the VS screen too, so a faster receiver has it before its match init. The wire format is
   unchanged, so older mod versions still interoperate.
5. **Decouple palette work from overlay init.** Run PaletteManager / OnlinePaletteManager match
   init even when WindowManager is not initialized.
6. **Retry instead of one-shot.** If the handle or the palette set is not ready at match init,
   retry each frame for the rest of the battle.
7. **Stale copies.** Generalise the Platinum own-slot sync to every character's own slots (+0x358)
   and the colour-22 mask slot.
8. **Game fix, optional, host-side only** `[mod]`: patch the online mirror bump (0x0049A2F0 /
   0x004989E0) to keep the colour within the player's unlocked range instead of wrapping to 0.
   Only the authority runs it, so every member, spectators included, still gets the same records.
   Needs the asm convention of 0x0049A2F0 checked first.
9. A `UseLegacyPaletteApply` setting that restores today's path, per the rewrite rule
   (vanilla-equivalent when off).

Verification: offline (versus colour change, result-menu rematch, training quick change into a
mirror, colour 21/22, Valkenhayn / Nu / Izanami / Platinum commands, story) and online with two
instances (ranked set with rematches, room rematch, mirror at colour 12+, a spectator joining
mid-set, a slow loader on each side).

## Rewrite status (2026-10-19)

Implemented: rewrite design steps 1-6 and 8-9, with step 7 limited to Platinum (the existing
sync). Code is in `CharPaletteHandle.*`, `hooks_palette.cpp` (GetPalBaseAddresses),
`PaletteManager.*`, `MatchState.cpp`, `OnlinePaletteManager.cpp` (apply log line only). New
settings: `PaletteLegacyIndexToggle` (default 0) and `FixOnlineMirrorColour` (default 1).

Verified:
- **Online, 15 battles on the dev machine (2026-10-18 22:57-23:45, ranked and rooms, including
  spectated matches).** Every opponent was read on their own colour (12, 10, 1, 9, 15...),
  where the old build pinned them all to the local player's slot. Palettes that arrived during
  loading were shown right after our match init. A mid-match palette change from the opponent
  was received and shown. The player saw the custom palettes on screen.
- **Offline, automated (bbcf-agentctl, training).** The palette was checked by counting pixels of
  a pure-magenta test palette (~27,600 px when shown, 0 when not):
  - the palettes.ini palette is shown at match start;
  - it survives position resets, which recreate the characters;
  - switching to native and back from the palette editor mid-match shows immediately;
  - training quick change into a Ragna mirror: the game's own bump of the dummy to colour 2 is
    kept, the mod writes nothing back on the way out, and both Ragnas draw their own palette;
  - `PaletteLegacyIndexToggle = 1` (the old flip) still works.

Not yet seen: the mirror-wrap patch acting as room host on colour 12+, a spectator joining
mid-set, story mode file-7 overrides.

## Open questions

- **0x0049A2F0 calling convention** (Ghidra shows 3 stack args, no `ret N` seen). Blocks rewrite
  step 8.
- **Third owner (slot 2) contents** in non-story modes, and whether its count can ever differ.
  Not blocking: the owner-based capture ignores it.
- **Room type 0x5x** (the 0x004989E0 path): which room it is. Read the room type byte in a 2D
  lobby cabinet match.
- **Late spectator records**: can the 0x30088 sequence check leave a spectator on the previous
  game's records? Needs a capture with a spectator joining mid-set.
- **Replay-file load path** that fills descriptor colours (mode 0xB, 0x006C2960 -> 0x0069DE70):
  not traced past the clamp.
- **Thread proof**: EndScene, P2P dispatch and match init are believed to share the main thread
  (one message loop, 0x0044F1E0). Settle with a thread id in each log line.
- **FPAC order**: `sub = colour*8 + file` is assumed for every character. Dump owner+0x18 per
  character to confirm.
- **Rollback**: whether the character palette commands re-run during resimulation (visual only).
