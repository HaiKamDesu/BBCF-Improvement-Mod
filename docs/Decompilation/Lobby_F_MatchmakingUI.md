# F - UI that drives matchmaking

## Summary

The matchmaking UI is two layers, and only the bottom layer is in the F function list:

1. **Widgets** (`CNetworkUI*`, `CRematch*`, `CPlayerRoomMember`): singleton menu objects that draw, animate
   and track a cursor. They raise two flags (`+0x14` decided, `+0x10` cancelled) and nothing more. None of
   them issues a network request.
2. **The menu controller.** This is the network main-menu scene object, not in the list: dispatcher
   `FUN_0070C120` plus one handler per screen in 0x0071xxxx. It reads the widget flags and talks to the
   network task `GAME_CNetworkTask` (singleton `DAT_00CF7958`, getter `FUN_004A77E0`) through a 32-slot
   **request ring** (`FUN_004A88D0` push, `FUN_004A8150` pop).

Key findings:

- **The task decides which screen is shown, not the UI.** The dispatcher picks the handler from
  `task+0xD8` ("UI screen id"). Only the task state functions write that field. A request is pushed
  as `task+0x1C = 0; push(id)`. Every handler returns early while `task+0x1C == 0`, so all input is
  ignored until the task re-arms `+0x1C = 1`, which happens when it enters its next idle/menu state.
- **No in-flight request can be cancelled from the UI.** While a create, search or join is pending,
  the screen is id 0xF (no menu) or the old list with input blocked. The waits in the task (create
  0x44/0x45, search 0x49 / ranked 0x26, join 0x4D / ranked 0x2C) have **no timeout**, and part D
  confirms the Steam layer has none either. If a Steam result never arrives, the UI is stuck until a
  global network error (outer state 0xD/0xE).
- **Request ids** (player match): 0x19 open Create, 0x1A open Search, 0x1C Steam overlay, 1
  confirm/execute, 2 back, 0x1B re-search. Ranked: 0x15 open the search list, 0x14 toggle background
  queue entry, 0x17 search settings, 0x18 character select, 0x16 refresh list, 1 join/commit, 2 back.
  Lobby: 0x28 enter a lobby, 4 lobby scene ready.
- **Settings and filters are written straight into task fields** before the push:
  - room settings into `task+0x10C..+0x188`, which is the RoomSettingsStatic block the mod already
    knows (0x8F7A64 = 0x8F7958 + 0x10C);
  - player-search filters into `+0x1CC..+0x1E4`;
  - ranked filters into `+0xF4..+0x104` (also saved to OptionData);
  - ranked character into `+0x294..+0x2A8`.
- **Joins are by list index (`task+0x1EC`), not by lobby id.** The task re-reads the row from the live
  list on its next tick, and both lists auto-refresh every 600 idle frames. A refresh landing between
  the click and the pop joins whatever row is now at that index. This is the root of the "clicked one
  player, joined another" class the mod already patches around.
- **Ranked browsing hosts a room.** After 300 frames on the ranked list with no room, the task creates
  its own ranked room (`FUN_004A41D0`). If anyone joins it, the task force-jumps from the list (or a
  pending join) to the match-found state 0x2E. The user's own click is flushed by `FUN_004A9D20`.
- **Online rematch is host-arbitrated:**
  - cursor moves are broadcast to the match peers (packet 0x2F);
  - the decision goes only to the match member with flag 4 (packet 0x30);
  - that host writes the per-side block `big+0x22CEC` and pings the others (0x31);
  - the others pull room state.

  The outcome rule is: either side choosing Exit ends it, both Rematch replays, otherwise character
  select. There is a 1200-frame (20 s) timeout from the start of the result screen that force-sends
  Exit for the local side, so the screen can never wait forever. But a lost host broadcast can split
  the two sides (one rematches, one leaves).
- **Error and notice surface:** the task shows dialogs by index into the string table at `0x009DE870`
  (e.g. 0xB `NTER_RankMatch_SearchSelectError`, 0xC `NTER_RankMatch_ConnectError`, 0x18
  `NTER_PlayerMatch_MemberFull`). Status lines come from `task+0xB4` (0x48 Creating, 0x49 Searching,
  0x4A Connecting). Inline notices come from `FUN_006983E0` (`RMSR_CheckingRTT`, `NTER_Pc_UGCLimited`,
  `NM_LobbySelectLimitTips`, `RankMatchLeaveMyself/Enemy`, `NEWK_RankMatchEntry`).

## Functions

Conventions were checked against disasm.txt. "fastcall(this)" means ecx = this with no stack args and
a plain `ret`. Slot numbers are vtable slots. Widget vtable layout for `CNetworkUIEasyItemMenu` /
`CNetworkUISWItemMenu` descendants:

- 0 scalar dtor (ret 4)
- 1 Decide
- 2 Cancel
- 3 Update(int unused) (ret 4)
- 4 Draw
- 5 GetItemCount
- 6 setter
- 7 Reset

| VA | Name | Size | Level | Notes |
|---|---|---|---|---|
| 0x00523B40 | CPlayerRoomMember_ctor | 135 | L1 | fastcall(this), ret; sets vtable 0x94C928, base ctor FUN_00407940. Callers 0x005244B0, 0x005259E0 (room member window). |
| 0x00523D50 | CPlayerRoomMember_scalar_dtor | 95 | L2 | thiscall(this, byte flags), ret 4; resets vtable to CWindowItemObj, frees if flags&1. |
| 0x00524DF0 | CPlayerRoomMember_Draw | 2926 | L1 | slot 2, fastcall(this). Draws "Member  List", net-colour icons (`net_col_`+A..G.hip), per-member submenu `NM_ViewIDCard`/`NM_AddGoodPlayerList`/`NM_AddBadPlayerList`/`NM_Kick`. No network calls. |
| 0x00526920 | CPlayerRoomMember_Update | 1205 | L1 | slot 1, fastcall(this). Pad input on room member list (+0x4C8 cursor, +0x4AC count, +0xB48 submenu open, +0xB50 sub-cursor). Sub 0: view ID card (FUN_005239E0 / Steam overlay FUN_0064E800). Sub 1/2: good/bad-player notice via FUN_0070ECF0("NM_PlayerListGoodPlayer"/"...Bad..."). Kick confirm (`PMRI_AskKick`) is in the room controllers 0x00713390.. (not this function). Sets +0xB58=1 on action. |
| 0x0052E1F0 | CNetworkUILobbyRegionSelect_scalar_dtor | 108 | L2 | thiscall(this, flags), ret 4. vtable chain LobbyRegionSelect -> CNetworkUISWItemMenu -> CNetworkUIBase. |
| 0x0052E2C0 | CNetworkUISWItemMenu_Cancel | 19 | L2 | slot 2 (LobbyRegionSelect, PlayerMatchCreate). fastcall(this): `[ecx+14h]=0; [ecx+10h]=1`; tail-jmp FUN_00692210 (cancel SE). |
| 0x0052EEB0 | CNetworkUILobbyRegionSelect_Draw | 3445 | L1 | slot 4, fastcall(this), ret. "World Select" lobby/region list. |
| 0x005302F0 | CNetworkUIMenu_Decide | 19 | L2 | slot 1 shared by LobbyRegionSelect, PlayerMatchSearch, PlayerMatchTopMenu, RankMatchTopMenu. fastcall(this): `[ecx+14h]=1; [ecx+10h]=0`; tail-jmp FUN_00692440 ("400_menu_ok.wav"). |
| 0x005303D0 | GetLobbyRegionSelectUI | 136 | L2 | cdecl(), ret; function-static singleton at DAT_011A63B8 (guard DAT_011A7B1C), constructed with vtable 0x94CE28 + FUN_00530590, atexit dtor. Returns object ptr in eax. 18 callers (lobby world code + menu controller). |
| 0x00530590 | CNetworkUISWItemMenu_Reset | 246 | L2 | slot 7, fastcall(this), falls through (no ret in scan: tail). Clears +4 (visible), +8/+0xC/+0x20/+0x24 layout (0x5CD,0xFA,0x172,0x32), 32 items x 0xB8 at +0x2C, and +0x1730..+0x175C anim/cursor fields. |
| 0x00531450 | CNetworkUILobbyRegionSelect_Update | 503 | L2 | slot 3, thiscall(this, int unused), ret 4. If visible: open anim (+0x1730==1, 8 frames, +0x173C alpha, +0x1744 y) / close anim (==2, then Hide FUN_00624B10); when idle, page +0x1734: 0 -> FUN_005317C0 (list input), 1 -> FUN_00531690. +0x1754 cursor bounce. |
| 0x00535A60 | CNetworkUIHotCard_Update/Draw | 797 | L1 | slot 3. Player hot card (profile card) shown in lists; reads big net struct (FUN_004A0FE0) and card getters 0x0070CEE0..0x0070D1C0. No requests. |
| 0x0053BA50 | CNetworkUIPlayerMatchCreate_scalar_dtor | 108 | L2 | thiscall(this, flags), ret 4. |
| 0x0053BAC0 | CNetworkUIPlayerMatchSearch_scalar_dtor | 108 | L2 | thiscall(this, flags), ret 4. |
| 0x0053BB30 | CNetworkUIPlayerMatchSearch_Cancel | 56 | L2 | slot 2, fastcall(this). +0x1730=2 (close anim), +0x1740=0, +0x173C=1.0f, +0x1C=1 (closing), +0x14=0, +0x10=1; cancel SE. |
| 0x0053BBA0 | CNetworkUIPlayerMatchCreate_Draw | 4232 | L1 | slot 4. "Player Match Settings", 15 rows, scrollbar. |
| 0x0053CC30 | CNetworkUIPlayerMatchSearch_Draw | 3373 | L1 | slot 4. "Player Match Settings" (search variant), 6 rows. |
| 0x0053D970 | CNetworkUIPlayerMatchCreate_Decide | 30 | L2 | slot 1, fastcall(this): calls FUN_00542140(2) (item index lookup, result discarded), +0x14=1, +0x10=0, OK SE. |
| 0x0053DAB0 | GetPlayerMatchCreateUI | 146 | L2 | cdecl(), ret; singleton DAT_011A7C40 (vtable 0x94D4DC, FUN_00530590 reset). |
| 0x0053DB50 | GetPlayerMatchSearchUI | 146 | L2 | cdecl(), ret; singleton DAT_011A9428 (vtable 0x94D500). |
| 0x0053DBF0 | CNetworkUISWItemMenu_GetItemCount | 7 | L2 | slot 5, fastcall(this) -> `[ecx+172Ch]`. |
| 0x0053EC60 | CNetworkUISWItemMenu_SetField174C | 16 | L2 | slot 6, thiscall(this, int v), ret 4: +0x174C = v. |
| 0x0053ECF0 | CNetworkUIPlayerMatchCreate_Update | 654 | L2 | slot 3, thiscall(this, int), ret 4. Open/close anim on +0x1730/+0x1740 (8 frames; close ends in Hide). Idle: page +0x1734 -> 0 FUN_0053F790 (main list: up/down, Decide on item 0 or Start button, room-name text entry gated by Steam UGC privilege -> notice `NTER_Pc_UGCLimited`), 2..0x12 option pickers (FUN_0053F5A0/F600/F3D0/F240). |
| 0x0053EFE0 | CNetworkUIPlayerMatchSearch_Update | 563 | L2 | slot 3, thiscall(this, int), ret 4. Same anim pattern; pages via FUN_0053FC80 / FUN_00540030. |
| 0x00540150 | CNetworkUIPlayerMatchTopMenu_scalar_dtor | 108 | L2 | thiscall(this, flags), ret 4. |
| 0x005401C0 | CNetworkUIEasyItemMenu_Draw (PlayerMatchTop) | 771 | L1 | slot 4. "Player Match" panel, 4 rows from +0x30 label table. |
| 0x005404D0 | GetPlayerMatchTopMenuUI | 136 | L2 | cdecl(), ret; singleton DAT_011AAC58 (vtable 0x94DAE4, reset FUN_00540560). |
| 0x00540560 | CNetworkUIEasyItemMenu_Reset | 143 | L2 | slot 7 (PlayerMatchTop), fastcall(this): layout, clears 32 items x 0x14 at +0x2C, +0x10/+0x14 flags, anim (+0x2B4 alpha, +0x2B8 frame, +0x2BC y=32.0f, +0x2C4 anim state), +0x2AC count, +0x2CC. |
| 0x005406A0 | CNetworkUIEasyItemMenu_Update | 950 | L2 | slot 3, thiscall(this,int), ret 4. Anim: +0x2C4 1 open/2 close (8 frames; close -> Hide). When visible and idle: up/down (with wrap only on repeat-press) on +0x28, +0x2CC bounce=5; button 3 -> vtbl slot1 Decide, button 4 -> slot2 Cancel. |
| 0x00540A60 | CNetworkUIRankMatchTopMenu_scalar_dtor | 108 | L2 | thiscall(this, flags), ret 4. |
| 0x00540AD0 | CNetworkUIRankParam_scalar_dtor | 108 | L2 | thiscall(this, flags), ret 4. |
| 0x00540B40 | CNetworkUIEasyItemMenu_Cancel | 56 | L2 | slot 2 (PlayerMatchTop, RankTop), fastcall(this): +0x2C4=2, +0x2B8=0, +0x2B4=1.0f, +0x1C=1, +0x14=0, +0x10=1; tail-jmp cancel SE. |
| 0x00540BA0 | CNetworkUIRankMatchTopMenu_Draw | 696 | L1 | slot 4. "Rank Match"; item 1 label is `MM_Item_RankEntryCancel` when ranked-entry object `DAT_00CF7758+0 == 1`, else `MM_Item_RankEntry`. |
| 0x00540E60 | CNetworkUIRankParam_Draw | 2555 | L1 | slot 4. "Search Settings". |
| 0x005418E0 | GetRankMatchTopMenuUI | 136 | L2 | cdecl(), ret; singleton DAT_011AAF70 (vtable 0x94DB40, reset FUN_00541A80). |
| 0x00541970 | GetRankParamUI | 136 | L2 | cdecl(), ret; singleton DAT_011AB240 (vtable 0x94DB64, reset FUN_00541B10). |
| 0x00541A00 | Generic_GetField2AC | 7 | L2 | slot 5 for EasyItemMenus (`[ecx+2ACh]` = item count); 9 callers, also used on other objects (SCENE_CLobby 0x00749680 calls it on FUN_00530350's object). |
| 0x00541A80 | CNetworkUIRankMatchTopMenu_Reset | 143 | L2 | slot 7, fastcall(this). Same as 0x00540560 but clears +0x2C8 instead of +0x2CC. |
| 0x00541B10 | CNetworkUIRankParam_Reset | 241 | L2 | slot 7, fastcall(this). SWItemMenu-style clear of 32x0xB8 items, +0x1730..+0x174C. |
| 0x005421A0 | CNetworkUIRankMatchTopMenu_Update | 950 | L2 | slot 3, thiscall(this,int), ret 4. Same logic as 0x005406A0. |
| 0x00542560 | CNetworkUIRankParam_Update | 872 | L2 | slot 3, thiscall(this,int), ret 4. Anim on +0x1740/+0x1734; idle page +0x1748: 0 -> FUN_00542AD0 (row list), 1..7 -> FUN_005428F0 (left/right change option +0x5C of row, clamp to +0x60; button 3/4 closes picker). No Cancel path that restores old values. |
| 0x00542DB0 | CNetworkUIRankMatchCharSele_scalar_dtor | 108 | L2 | thiscall(this, flags), ret 4. |
| 0x00542E50 | CNetworkUIRankMatchCharSele_Draw | 6903 | L1 | slot 4. "RankMatch  Character Select": char/colour, Stylish/Technical, MySet, per-char NM_PlayCount/NM_Win/NM_WinRate/NM_WinChainMax/SkillRank from the D-Code profile rows (FUN_004A1410 row = base+0xD4+i*0x180; FUN_004BE030 row+10==0). |
| 0x00544970 | GetRankMatchCharSeleUI | 136 | L2 | cdecl(), ret; singleton DAT_011AC9D8 (vtable 0x94DCCC, reset FUN_00544A40). |
| 0x00544A40 | CNetworkUIRankMatchCharSele_Reset | 259 | L2 | slot 7, fastcall(this). Clears items, 64 char slots +0x175C/+0x1760 (pairs), +0x1964[64] colours, +0x1A68[80]=-1, +0x1960 char cursor, +0x1BAC/+0x1BB0. |
| 0x00544EC0 | CNetworkUIRankMatchCharSele_Update | 550 | L2 | slot 3, thiscall(this,int), ret 4. Anim +0x1730/+0x1740; page +0x1734: 0 FUN_00545470, 1 FUN_00545110, 2/3/6 FUN_00545450, 4 FUN_005452B0; FUN_00534200. |
| 0x0054F9B0 | SCENE_CMatchResult_Step | 745 | L1 | fastcall(this), ret. Per-step dispatcher of the result scene (+0x2C step). Unless char-select mode (+0x10C) is one of 6,0xB,0xF,0x17,0x19,0x1B..0x1D it sets ranked-entry `DAT_00CF7758+0x1C=1`, which suppresses the background ranked queue's match pop-up while on result screens. Step 9 also sets `+0x18=1` on that object. |
| 0x00624830 | CRematchItemMenu_ctor | 186 | L2 | fastcall(this): CRematchUIBase vtable, +4 visible=0, +8/+0xC layout (0x3C0,0x180), +0x1C/+0x20=1, 32 items x 0x18 at +0x28 (value field +0x34 init -1), +0x328 count=0. |
| 0x006248F0 | CRematchWindow_ctor | 91 | L1 | offline rematch window; FUN_00624830 + vtable 0x95E69C + FUN_006252E0 init. |
| 0x00624950 | CRematchItemMenu_dtor | 72 | L1 | vtable reset. |
| 0x006249A0 | CRematchUIBase_dtor | 7 | L2 | fastcall(this): `[ecx]=0x95E674`. |
| 0x006249B0 | CRematchWindow_dtor | 85 | L1 | vtable reset chain. |
| 0x00624A10 | CRematchItemMenu_scalar_dtor | 95 | L2 | thiscall(this, flags), ret 4. |
| 0x00624A70 | CRematchUIBase_scalar_dtor | 34 | L2 | thiscall(this, flags), ret 4. |
| 0x00624AA0 | CRematchWindow_scalar_dtor | 108 | L2 | thiscall(this, flags), ret 4. |
| 0x00624B20 | CRematchWindow_Draw | 61 | L1 | slot 4; picks x from +0x340 side. |
| 0x00625400 | CRematchWindow_Update | 397 | L1 | slot 3, ret 4. Offline: anim +0x330/+0x338, then local input FUN_00625590. |
| 0x006257A0 | CRematchWindowOnline_ctor | 91 | L2 | fastcall(this), ret. FUN_00624830, vtable 0x95E708, then FUN_006264A0: hide, +0x364/+0x368 msg ids=4, clear +0x32C..+0x348, +0x34C local side from FUN_0049A340 (room flags: in-match bit1 -> side 0/1 from bit0, else 2 = spectator), anim fields. Single caller 0x0062A150 (result scene holder, `FUN_00643B40()+0x2994`). |
| 0x00625800 | CRematchWindowOnline_dtor | 85 | L1 | vtable reset chain. |
| 0x00625860 | CRematchWindowOnline_scalar_dtor | 108 | L2 | thiscall(this, flags), ret 4. |
| 0x006258E0 | CRematchWindowOnline_Draw | 38 | L2 | slot 4, fastcall(this): if visible FUN_00625910(+0x35C + 0x5FA, 0x35C). |
| 0x00626470 | CRematchItemMenu_GetItemCount | 7 | L2 | slot 5, `[ecx+328h]`. |
| 0x006266E0 | CRematchWindowOnline_Update | 439 | L2 | slot 3, thiscall(this,int), ret 4. If visible: anim +0x350 (1 open, 2 close -> Hide after 8 frames), then FUN_006268A0(side) for side 0 and 1 (per-side state + input + network send, see Flows). |
| 0x006411F0 | ETCObjectContainer_ctor (Lobby_CBase owner) | 779 | L1 | Constructs the scene's ETC object table: Profile, VSInfo, Winner, StageInfo, Ending, Ranking, ModeSelect, StaffRoll, Result, MainMenu, StorySelect, Lobby_CBase, MyRoomTest, DCC... No network. |
| 0x006B5990 | SCENE_CMatchResult_ExitInput | 129 | L1 | fastcall(this): clears pad repeat state for P1/P2/sys (FUN_004C0C20), +0x14=2. |
| 0x00740EE0 | Lobby_CBase_Update | 205 | L1 | slot 3, fastcall(this). Dispatches on lobby object `(*(this+4))+0x10` state 4..0x16 to the 3D-lobby UI handlers (0x00745800..0x00749200). |
| 0x00741000 | Lobby_CBase_Draw | 233 | L1 | slot 2. Same dispatch for draw handlers 0x00743620..0x007455C0. |
| 0x00749680 | SCENE_CLobby_Init | 1653 | L1 | Loads lobby assets (fullimg/fhd_ads/fhd_hds, avatar acc bins), builds avatars, then **pushes request 4 to the task without clearing task+0x1C** (lobby state 0x6F/0x70 waits for it). If FUN_00541A00 of the lobby object ==3, may call FUN_006C4330 (network-ready latch). |
| 0x0074B400 | SCENE_CLobby_Step | 271 | L1 | Per-step: camera/scene setup; when FUN_0073BE40 ready: fade 10, +0x2C++. |
| 0x0074B510 | SCENE_CLobby_StartBgm | 178 | L1 | Builds a sound cue (0x38C) and fades in (FUN_004BFFB0(100,0)). |
| 0x0079E05C | __purecall | 42 | L1 | library: CRT pure-virtual handler. |

Controller functions outside the list but needed to answer the questions. These were read; the
level shown is the reading depth.

| VA | Role |
|---|---|
| 0x0070C120 | MainMenuNet_Dispatch, fastcall(menu). Picks handler by `task+0xD8`. Resets menu `+0x6B9C` (init flag) when the screen id changes, except when leaving screen 6. If `task+0x270` is set, fades 0x3C frames. |
| 0x00718490 | Screen 1 PlayerMatchTop handler |
| 0x00713150 / 0x00712CE0 | Screen 2 CreateSetting handler / commit settings |
| 0x007182A0 / 0x00718180 | Screen 3 SearchSetting handler / commit filters |
| 0x00717D90 | Screen 4 room list |
| 0x00714960 / 0x007144D0 | Screen 6 in-room settings change |
| 0x00719210 | Screen 7 RankTop |
| 0x00719060 | Screen 8 RankCharSelect |
| 0x00719580 | Screen 9 RankParam |
| 0x00719920 | Screen 10 ranked list |
| 0x007109B0 / 0x00710A80 / 0x00711090 | Screens 0xB/0xC lobby select |
| 0x0062AC30, 0x0062A390, 0x00626680, 0x006268A0, 0x00628760 | Online rematch logic |

## Data

### GAME_CNetworkTask (`DAT_00CF7958`, getter `FUN_004A77E0`)

Fields the UI reads or writes:

| Off | Meaning |
|---|---|
| +0x00 | outer state: 1 net main menu (FUN_004B01B0), 4 ranked (FUN_004AE6D0), 5 match, 6 player match (FUN_004AC6C0), 7 player room (FUN_004ACED0), 8 lobby select (FUN_004ABF50), 9 lobby (FUN_004AAAD0), 0xD/0xE disconnect/error (FUN_004AA890). Switch = FUN_004A7C30(id). |
| +0x04 | sub-state; +0x08 "just entered" (1 on the first tick); +0x0C enter-pending; +0x10 previous outer |
| +0x14 / +0x18 | per-state frame counters (list refresh 600, ranked host delay 300, leave label 30) |
| +0x1C | **UI ready flag.** The UI clears it when pushing; the task sets it to 1 when an idle/menu state is entered (or when a join is refused with `RMSR_CheckingRTT`). |
| +0x24..+0xA0 | request ring, 32 dwords; +0xA4 read idx, +0xA8 write idx. Push FUN_004A88D0 returns 0 when the slot is occupied (callers ignore it). Pop FUN_004A8150. **FUN_004A9D20 zeroes the ring and both indices on every sub-state change.** |
| +0xAC/+0xB8 | dialog busy (FUN_004A7F90). +0xB0 dialog answer (1 = yes). +0xB4 status-line message id. +0xB8 dialog message id, +0xBC type, +0xC0 buttons (0 OK, 2 yes/no), +0xC4, +0xC8. |
| +0xD8 | **UI screen id** (table below) |
| +0xF4..+0x104 | ranked search filters: EntryType (2,3), Area (4,5,6), Grade (7,8,9), ConnSpeed (0xA..0xE), MatchCompleteRate (0xF,0x10) |
| +0x10C..+0x188 | room creation settings = RoomSettingsStatic (mod: 0x8F7A64). +0x10C name[64]; +0x14C RoomType (0..3); +0x150 MemberCount (5..0xB); +0x154 InviteCount (0xC..0x13); +0x158 ConnSpeed (0x14..0x18); +0x15C NetworkColor (0x19,0x1A); +0x160 ContinueCount (0x1B..0x25); +0x164 AutoPass (0x26,0x27); +0x168 SkipTime (0x28..0x30); +0x16C TextChat (0x31,0x32); +0x174 Rotation (0x35..0x38); +0x178 WinRound (0x39..0x3B); +0x17C RoundTime (0x3E..0x42); +0x180=0x44, +0x184=0x45 fixed; +0x188 Rematch (0x47..0x4C, matches mod enum RoomSettingsRematchStatic); then a block copied to +0x17A0. |
| +0x1CC..+0x1E4 | player-match search filters: Area (+0x1CC = option index), RoomType (+0x1D0 = idx+6), MemberCount (+0x1D4 = idx+9), WinRound (+0x1D8 = idx+0xD, forced 0xD when Area is 3/4), +0x1DC=0x15, +0x1E0=0x16 fixed, TextChat (+0x1E4 = idx+0x18) |
| +0x1EC | selected list row **index** for join (player list and ranked list) |
| +0x270 / +0x278 | in-network-match flag / match kind (3 = ranked-style, char change disabled) used by rematch helpers FUN_004A7EB0/7EE0/7E50/7F00 |
| +0x294..+0x2A8 | ranked character choice (FUN_004D20D0, thiscall(task, 6 args), ret 0x18) |
| +0x2B4..+0x2D0 | lobby-enter parameters (FUN_004D2080) |

### Request ids

| id | Pushed by | Consumed in (task) |
|---|---|---|
| 1 | confirm/commit: create (0x00713150), search (0x00718180), join row (0x00717D90, 0x00719920), RankParam (0x00719580), RankCharSel (0x00719060) | 6/0x43, 6/0x48, 6/0x4A, 4/0x27, 4/0x23, 4/0x22 |
| 2 | back from any screen | 6/0x42 -> outer 1; 6/0x43, 6/0x48, 6/0x4A -> 0x42; 4/0x1E -> outer 1; 4/0x27 -> 0x2A; 8 -> back |
| 4 | SCENE_CLobby init (0x00749680) | 9/0x6F, 9/0x70 |
| 6/7/8 | network main menu (outside F) | 1/1 -> outer 4/6/8 |
| 0x14 | RankTop "RankEntry" | 4/0x1E -> 0x1F (entry obj state 0 or 3) else 0x20 |
| 0x15 | RankTop "RankSearch" | 4/0x1E -> 0x24 |
| 0x16 | ranked list refresh button | 4/0x27 -> 0x26 |
| 0x17 | RankTop "SearchParam" | 4/0x1E -> 0x23 if no entry active, else 0x21 |
| 0x18 | RankTop "RankCharSelect" | 4/0x1E -> 0x22 if no entry active, else 0x21 |
| 0x19 | PlayerMatchTop "Create" (also in-room settings change 0x007144D0) | 6/0x42 -> 0x43; 7/0x5E (apply room settings, FUN_004B1240) |
| 0x1A | PlayerMatchTop "Search" | 6/0x42 -> 0x48 |
| 0x1B | room list refresh button | 6/0x4A -> 0x49 |
| 0x1C | PlayerMatchTop "mail" | 6/0x42 -> 0x4F (ISteamFriends vfunc+0x70, overlay; 9 frames later back to 0x42) |
| 0x28 | lobby select (0x00711090) | 8/0x69 -> 0x6B |

### UI screen ids (`task+0xD8`)

| id | Handler | Screen |
|---|---|---|
| 0 | per menu `+0x64` | network main menu |
| 1 | 0x00718490 | Player Match top |
| 2 | 0x00713150 | Create settings |
| 3 | 0x007182A0 | Search settings |
| 4 | 0x00717D90 | Room list |
| 5 | 0x007154F0 | In player room |
| 6 | 0x00714960 | Room settings change |
| 7 | 0x00719210 | Ranked top |
| 8 | 0x00719060 | Ranked char select |
| 9 | 0x00719580 | Ranked search params |
| 10 | 0x00719920 | Ranked list |
| 0xB | 0x007109B0 | lobby select (leave) |
| 0xC | 0x00710A80 | lobby region / world select |
| 0xD | (inline) | jump to main menu network item |
| 0xE | (inline) | leave the menu scene |
| 0xF | (none) | **no menu shown (busy)** |

### Main-menu scene object (controller `this`)

| Off | Meaning |
|---|---|
| +0x10 | frame delay before the handler acts (set to 1..10 on transitions) |
| +0x3C | `CTRL_*` help-name pointer |
| +0x60 | tips id |
| +0x64 | "return" item id |
| +0x6B74 | item index |
| +0x6B84..+0x6BA0 | panel flags |
| +0x6B9C | per-screen init flag |
| +0x824C | last seen screen id |

The list popups live in a widget pool at `FUN_00643B40()+0x29C4`: 4 slots of 0x15D90, in-use at
+0x68. The active slot comes from FUN_00682D50; FUN_00672B10 closes all four.

### Widgets

**EasyItemMenu (PlayerMatchTop / RankTop)**

| Off | Meaning |
|---|---|
| +0x04 | visible |
| +0x10 | cancelled |
| +0x14 | decided |
| +0x1C | closing |
| +0x28 | cursor |
| +0x2C + i*0x14 | item {enabled, label, id} |
| +0x2AC | count |
| +0x2C4 | anim (1 open / 2 close) |

**SWItemMenu (Create / Search / Region / RankParam / CharSele)**

| Off | Meaning |
|---|---|
| +0x2C + i*0xB8 | item {enabled +0, label +4, id +8, current option +0x30, option count +0x34, option labels +0x38[16], flags +0x78} |
| +0x1730 / +0x1740 | anim |
| +0x1734 | page |
| +0x174C | set by slot 6 |
| +0x1750 | cursor (index into items) |

FUN_00542140(id) returns the item index for an id.

Item ids:

- **PlayerMatchTop:** 0 Create, 1 Search, 2 BattleLog, 3 mail.
- **RankTop:** 0 RankSearch, 1 RankEntry, 2 SearchParam, 3 RankCharSelect, 4 BattleLog.
- **Create:** 0 CreateRoom, 1 RoomName, 2 RoomType, 3 MemberCount, 4 InviteCount, 5 ConnectionSpeed,
  6 NetworkColor, 7 ContinueCount, 8 Rematch, 9 AutoPass, 10 SkipTime, 11 TextChat, 12 RotationType,
  13 WinRound, 14 RoundTime.
- **Search:** 0 SearchRoom, 1 RoomType (ANY + 4), 2 Area (Global/Wide/+1), 3 MemberCount, 4 WinRound,
  6 TextChat.
- **RankParam:** 0 EntryType, 1 Area, 2 GradeSetting, 3 ConnectionSpeed, 4 MatchCompleteRate,
  5 and 6 (hidden, saved to OptionData +0x548B4/+0x548B8), 7 Close. Initial values are loaded from
  OptionData +0x51420..+0x51430 (-1 = default).

### Online rematch

**Window** (`CRematchWindowOnline`):

| Off | Meaning |
|---|---|
| +0x28 + i*0x18 | items: {enabled, label, value}. Values: Rematch=0, BackCharSelect=1 (disabled when +0x360 i.e. `task+0x278==3`, its value becomes 2), Exit=2 |
| +0x32C[2] | side decided (window copy) |
| +0x334[2] | cursor per side |
| +0x33C[2] | local forced-exit sent |
| +0x34C | local side |
| +0x350 | anim |
| +0x360 | char change disabled |
| +0x364[2] | per-side label id (0xE rematch, 6 char, 0x1B exit) |

**Network block:** `big(FUN_004A0FE0)+0x22CEC + side*8` = {byte decided, byte choice, pad, dword
forced}. The same memory is also reached as `big+0x6A1C+0x1C2D0`. The host copy is broadcast inside
the 0x30088 match-state packet (FUN_0049D8D0). Clients copy it from room state (FUN_0049B770,
+0x78..+0x84).

### Dialog table

Message id N -> string at `0x009DE870 + 4*N` (base verified against 6 call sites):

| id | String |
|---|---|
| 0x0A | NTER_RankMatch_SearchError |
| 0x0B | NTER_RankMatch_SearchSelectError |
| 0x0C | NTER_RankMatch_ConnectError |
| 0x12 | NEER_RankMatch_SettingDesable |
| 0x13 | NTER_PlayerMatch_SessionNameError |
| 0x14 | NTER_PlayerMatch_CreateError |
| 0x15 | NTER_PlayerMatch_SearchError |
| 0x16 | NTER_PlayerMatch_SearchSelectError |
| 0x17 | NTER_PlayerMatch_ConnectError |
| 0x18 | NTER_PlayerMatch_MemberFull |
| 0x20 | NTER_PlayerMatch_AppVerDisagrError |
| 0x21 | NTER_PlayerMatch_CatalogVerUnError |
| 0x38 | NETTSK_RankAskEntry |
| 0x39 | NETTSK_RankAskEntryCancel |
| 0x3A | NETTSK_RankAskMatchingCancel |
| 0x47 | NTTSK_Synchronizing |
| 0x48 | NTTSK_Creating |
| 0x49 | NTTSK_Searching |
| 0x4A | NTTSK_Connecting |

## Flows

### Handshake common to every screen handler
1. `if task+0x1C == 0 -> return` (input dead while the task works).
2. `if menu+0x10 > 0 -> menu+0x10--, return` (settle delay).
3. On the first tick of a screen (`+0x6B9C == 0` and no list popup open): open/reset the widget, set
   tips, set `+0x6B9C = 1`. If a popup from another screen is still open: close all popups, wait 10
   frames.
4. Read widget flags once it is hidden (close anim done) or decided. To issue a request:
   `task+0x1C = 0; FUN_004A88D0(id)` (return value ignored).
5. The task pops on its next tick, changes sub-state, and sets `+0xD8` to a new screen (often 0xF
   while working). Re-arming `+0x1C = 1` happens when it reaches the next menu state.

### Player match (task outer 6)

- **0x42 Top (screen 1)**
  - Create -> 0x19 -> 0x43.
  - Search -> 0x1A -> 0x48.
  - mail -> 0x1C -> 0x4F (Steam overlay, auto-return after 9 frames).
  - BattleLog -> local player-list overlay (`DAT_00CF7E30+0x464 = 1`; the handler then defers to
    FUN_007127B0).
  - Back -> 2 -> outer 1.
- **0x43 Create settings (screen 2)**
  - The CreateRoom item (or Start) closes the widget; then push 1 and copy the settings into
    `task+0x10C..` (FUN_00712CE0). This runs after the push, in the same frame, before the task tick.
    Cancel -> 2 -> 0x42.
  - 0x44: screen 0xF, FUN_004A97E0 (start CreateLobby). Immediate failure -> 0x47.
    - Flag 0x100 -> 0x45.
    - Flag 0x200 -> 0x46 if net result `+0xAB0 == 8` (dialog 0x13 SessionNameError), else 0x47
      (dialog 0x14 CreateError).
    - Both dialogs return to 0x43.
  - 0x45: FUN_004A8D50, then wait for flag 0x400 -> outer 7 (room) or flag 0x800 -> 0x47.
  - Status line 0x48 "Creating". **No timeout in 0x44/0x45.**
- **0x48 Search settings (screen 3)**
  - Execute: push 1 and copy the filters to `+0x1CC..` (FUN_00718180). Back -> 2 -> 0x42.
  - 0x49: screen 0xF, FUN_004A94C0 (RequestLobbyList). Flag 0x4000 -> 0x4A; flag 0x8000 or immediate
    failure -> 0x4C (dialog 0x15 SearchError) -> 0x48. Status 0x49 "Searching". **No timeout.**
- **0x4A Room list (screen 4)**
  - Every 600 idle frames: net vfunc +0xAC (refresh), but only while vfunc +0xB0 (busy) is 0.
  - Row validity is checked UI-side via the list entry's vfunc+4. An invalid row is silently ignored.
  - Confirm -> `+0x1EC = row index`, push 1. The task re-reads row `net+0xAF4[idx]`:
    - required tier = (vfunc+0x34 & 0xF000) >> 12;
    - RTT (vfunc+0x30) < 0 -> notice `RMSR_CheckingRTT`, re-arm +0x1C, stay;
    - tier(RTT) (FUN_004A6620) < required -> 0x4B (dialog 0x16 SearchSelectError) -> 0x4A;
    - else 0x4D: FUN_004A89D0 (JoinLobby). Flag 0x10000 -> outer 7. Flag 0x20000 -> 0x4E, dialog by
      net code `+0xAB0`: 2/3/4 -> 0x18 MemberFull, 6 -> 0x20 AppVer, 7 -> 0x21 CatalogVer,
      10 -> silently to 0x42, else 0x17 ConnectError. Then back to 0x4A.
  - Refresh button -> 0x1B -> 0x49 (new search, same filters). Back -> 2 -> 0x42 (skips search
    settings).
  - Status 0x4A "Connecting". **No timeout in 0x4D.**

### Ranked (task outer 4)

- **0x00:** if no ranked character yet (`+0x294 < 0`) -> 0x22 (forced char select), else 0x1E.
- **0x1E Rank top (screen 7)**
  - RankSearch (after close anim) -> 0x15.
  - RankEntry -> 0x14: if entry object `DAT_00CF7758` state is 0/3 -> 0x1F, dialog 0x38 "enter
    queue?". Yes -> entry state=1, notice `NEWK_RankMatchEntry`. Otherwise -> 0x20, dialog 0x39 "cancel
    entry?". Yes -> entry state=3. Both return to 0x1E.
  - SearchParam -> 0x17 -> 0x23 (screen 9) or, while entered, 0x21 (dialog 0x12 SettingDesable).
  - CharSelect -> 0x18 -> 0x22 (screen 8) or 0x21.
  - Back -> 2 -> CharSelectConfig reset, outer 1.
  - Leaving 0x1E sets entry `+0x1C = 1` (background queue suspended while browsing).
- **0x22 CharSelect:** the widget has no cancel; when it hides, the controller writes char/colour/flag/
  slot/... to `task+0x294..` (FUN_004D20D0) and OptionData (+0x51314..+0x5141C, +0x54A0C), calls
  FUN_006C4330, pushes 1 -> 0x1E.
- **0x23 SearchParam:** the widget has no cancel. On hide (via `NM_Close` or Cancel) it **always
  commits** the filters to `task+0xF4..+0x104` and OptionData, pushes 1 -> 0x1E.
- **0x24:** screen 0xF; waits until entry `+8` is 0 or 1 (FUN_004A7E10), then 0x26. **No timeout.**
- **0x26:** FUN_004A4440 (ranked RequestLobbyList with the `+0xF4` filters). Flag 0x4000 -> 0x27;
  flag 0x8000 -> 0x29 (FUN_0049DF90 leave + dialog 0x0A SearchError) -> 0x2A.
- **0x27 Ranked list (screen 10)**
  - `+0x18 = 300`: after 300 frames, if not in a room, FUN_004A41D0 creates our own ranked room from
    the filters.
  - Auto-refresh as in 0x4A (600 frames).
  - Requests: 1 join (requirement check RTT tier vs `vfunc+0x5C` / area vs `+0x58`; unknown RTT ->
    `RMSR_CheckingRTT`; fail -> 0x28 dialog 0x0B SearchSelectError -> 0x27; pass -> 0x2B), 2 -> 0x2A
    leave, 0x16 -> 0x26.
- **0x2B:** screen 0xF, FUN_0049DF90 (leave our hosted room) every frame until FUN_0049A920() == 0
  (no room), then 0x2C.
- **0x2C:** FUN_004A4110(+0x1EC) join. Flag 0x10000 -> 0x2E; flag 0x20000 -> 0x2D (dialog 0x0C
  ConnectError) -> leave -> **0x1E (back to ranked top, not the list)**. **No timeout.**
- **Challenge override:** in 0x26/0x27/0x28, if our room's member count (`room+0x38`) >= 2, jump to
  0x2E. This flushes any queued click.
- **0x2E Match found (screen 0xF, game's confirm UI)**
  - Opponent left (member count < 2) or room lost -> FUN_0049DF90, notice `RankMatchLeaveEnemy`,
    0x2F.
  - Room flags bit 12 (both ready) -> outer 5 (match).
  - Confirm button 3 -> FUN_0049DAD0(1) (send ready).
  - Button 2 before our ready is registered -> dialog 0x3A "cancel matching?". Yes -> notice
    `RankMatchLeaveMyself`, 0x30. After ready -> FUN_0049DAD0(0) (unready).
  - Every tick: FUN_004A3440(+0x14 frames, 2) draws "AUTOMATCHMSG_ASKBATTLE".
  - **No timeout in 0x2E itself.** The ~34 s / 2 min kicks the mod logs come from below (part B/C).
- **0x2A / 0x2F / 0x30:** leave and wait for no room. After 30 frames in 0x2A the status shows 0x47.
  Then 0x1E.

### Lobby (outer 8 and 9)

- Screen 0xC world/region select: the decided entry is checked by FUN_00530720. If refused ->
  notice `NM_LobbySelectLimitTips`. Else -> FUN_004D2080 (`task+0x2B4..`), push 0x28.
- Screen 0xB: when the task is in sub 0x68 with no dialog, auto-push 2.
- SCENE_CLobby init pushes 4 once its assets are loaded.

### Online rematch (result scene `FUN_00643B40()+0x2994`, update 0x0062AC30, per-side 0x006268A0)

1. The result scene counts frames in `holder+0x28`. At frame > 160 (0xA0), if the window is hidden,
   FUN_00626600 opens it with items Rematch / BackCharSelect / Exit. BackCharSelect is disabled when
   `task+0x278 == 3`.
2. **Each frame, for each side:**
   - If the block says decided (or forced): mirror the choice into cursor/label and mark the window
     side decided (+OK SE).
   - For the local side only: up/down -> FUN_0049D750(side, value). This sends packet 0x2F000C
     {matchId, side, value} to every other member in the same match (flag 0x1000). Receivers write
     the choice only if not decided (FUN_0049C890).
   - Confirm -> FUN_0049D6B0(side, value, 0). This sends packet 0x300010 to the match member with
     flag 4 (FUN_0049D4B0). **If no such member, nothing is sent.**
   - The local window does **not** mark itself decided on confirm. It waits for the block.
3. **Host side (FUN_0049C760):** if the matchId matches and the side is not yet decided (or the
   packet is forced): write choice + forced, decided=1, and send 0x310008. Clients receiving 0x31
   (FUN_0049C810) request state (9/0x30). The host's 0x30088 broadcast (FUN_0049D8D0) carries the
   block, and clients copy it in FUN_0049B770.
4. **Finish (0x0062AC30):** any decided side whose label is 0x1B (Exit) -> finish now. Both decided ->
   finish. Finish = close anim, fade 0x1E, then next scene 7.
5. **Timeout:** FUN_0062A390 (from FUN_0062F7D0) checks `holder+0x28 > 1200`. Then FUN_00626680 sends
   a forced Exit for the local side, if it has not already sent one. It also marks the local side
   decided **locally** and starts the close anim. The window therefore always ends at most about
   1200 frames (~20 s) after the result screen starts.
6. **Resolution (0x00628760 mode 7):**
   - FUN_004A7D90: both decided, both 0 -> rematch.
   - FUN_004A7D20: both decided, one chose 1, nobody 2 -> character select.
   - Otherwise FUN_0049ECE0(0xE) (own member byte +0x5B = 0xE, dirty bit) = leave the match.
   - Rematch or char select -> FUN_004A7C60(0 / 1 / 2 / 3) by match kind.
   - FUN_004A7D60 (any Exit) is used by the task states 5/7 to end the session.

## Failure paths

| Where | Condition | Cause side | User sees | Recovery |
|---|---|---|---|---|
| 6/0x44 | CreateLobby refused immediately (FUN_004A97E0 != 0) or flag 0x200 | Steam / local | dialog 0x14 CreateError (0x13 SessionNameError if net code 8) | back to Create settings |
| 6/0x45 | flag 0x800 after create | Steam | dialog 0x14 | back to settings |
| 6/0x44-0x45 | no flag ever | Steam call result lost / backend | "Creating" forever, no menu (screen 0xF), input dead | only a global net error (outer 0xD/0xE) |
| 6/0x49 | flag 0x8000 / FUN_004A94C0 fails | Steam | dialog 0x15 SearchError | back to Search settings |
| 6/0x49 | no flag | Steam | "Searching" forever | stuck |
| 6/0x4A | row RTT not measured (<0) | local (ping pending) | notice `RMSR_CheckingRTT`, nothing joins | retry by clicking again |
| 6/0x4A | row tier requirement > our RTT tier | peer requirements | dialog 0x16 | back to list |
| 6/0x4A | invalid row (entry vfunc+4 == 0) or null entry | local list state | nothing at all (no sound, no message) | user clicks again |
| 6/0x4D | flag 0x20000 | Steam enter response / game checks | dialog 0x18 MemberFull (codes 2,3,4), 0x20 AppVer (6), 0x21 CatalogVer (7), silently back to Top (10), else 0x17 ConnectError | back to list |
| 6/0x4D | no flag | Steam / host never answers | "Connecting" forever over the old list | stuck |
| Create item RoomName | Steam UGC privilege denied (net vfunc+0x30(8) == 0) | Steam account | notice `NTER_Pc_UGCLimited` | name cannot be set |
| 4/0x1E | SearchParam/CharSelect while queue-entered | local rule | dialog 0x12 SettingDesable | back to top |
| 4/0x24 | entry object `+8` not 0/1 | local (ranked entry object busy) | screen 0xF, "Searching" | waits until the entry object settles; no timeout |
| 4/0x26 | flag 0x8000 | Steam | dialog 0x0A SearchError, leaves our room | back to ranked top |
| 4/0x27 | RTT unknown | local | `RMSR_CheckingRTT` | click again |
| 4/0x28 | requirement not met (RTT tier or area) | peer | dialog 0x0B SearchSelectError ("connectivity requirements are not met") | back to list |
| 4/0x2C | flag 0x20000 | Steam (closed/full room) | dialog 0x0C ConnectError ("Failed to connect to room") | **back to ranked top**, list lost; user must search again |
| 4/0x2C | no flag | Steam | "Connecting" forever | stuck |
| 4/0x2E | opponent leaves / room lost | peer / Steam | notice `RankMatchLeaveEnemy` | leave room, ranked top |
| 4/0x2E | user cancels (dialog 0x3A yes) | local | notice `RankMatchLeaveMyself` | leave room, ranked top |
| Lobby select | FUN_00530720 refuses | local rule (lobby limits) | notice `NM_LobbySelectLimitTips` + error SE | stay |
| Rematch | no decision within 1200 frames | either player idle | window closes as Exit | leave match |
| Rematch | decide packet target missing (no member with flag 4) | room state | local choice never registers; remains until the 20 s timeout -> Exit | leaves |

## Suspected bugs

1. **Join by index, not by id (ranked and player lists).**
   - **Symptom:** "I clicked X, it tried to join Y" or "Failed to connect to room" right after the list
     refreshed.
   - **Evidence:** the UI stores only `task+0x1EC = row index` (0x00717D90, 0x00719920). The task
     re-reads `net+0xAF4[idx]` on its next tick (6/0x4A, 4/0x27). The list is rebuilt by the 600-frame
     auto-refresh (vfunc +0xAC) and by Steam callbacks independently of the cursor.
   - **Confidence:** high for the mechanism. The window is short (one task tick), but the mod's own
     click-snapshot logs show the effect.
   - **Fixable:** client-side, in the mod: latch the lobby id at click time (the mod already does a
     "CORRECTED" pass in RankedListConnectionFilter).
2. **User click flushed by the challenge override.**
   - **Symptom:** clicking a ranked row produces "match found" with a different person.
   - **Evidence:** 0x004AE6D0 tail. In states 0x26/0x27/0x28, member count >= 2 forces 0x2E with
     FUN_004A9D20, which zeroes the request ring. It is by design (someone joined the room we host
     while browsing, created by FUN_004A41D0 after 300 frames), but nothing tells the user.
   - **Confidence:** high.
   - **Fixable:** in the mod, a notice or toggle; or don't host while a join is in flight.
3. **No timeouts and no user cancel for create/search/join.**
   - **Symptom:** the menu disappears with "Creating/Searching/Connecting" and never returns.
   - **Evidence:**
     - Every handler returns while `task+0x1C == 0`. The waiting sub-states (6/0x44, 0x45, 0x49, 0x4D;
       4/0x24, 0x26, 0x2B, 0x2C) poll net flags with no counter.
     - Part D: the Steam layer polls "pending -> keep waiting (no timeout)".
     - The only exits are a net error forcing outer 0xD/0xE (`vfunc+0x2C > 99` or net-manager flag
       0x8000000).
   - **Confidence:** medium-high that the stall is reachable whenever a Steam call result is dropped.
     Whether Steam ever drops them is the open question. Note the mod's own CCallResult proxy holds
     lobby-list results; if it ever failed to forward one, this stall is exactly what users would
     see.
   - **Fixable:** client-side, by a mod watchdog that pushes the failure flag after N seconds.
4. **Ranked join failure throws the user back to Rank top.**
   - **Symptom:** after "Failed to connect to room" you must re-search, losing the list and your
     place.
   - **Evidence:** 4/0x2D falls to the shared `break`, which sets sub 0x1E (0x004AE6D0, case 0x2D).
     Player match returns to the list instead (0x4E -> 0x4A).
   - **Confidence:** high.
   - **Fixable:** in the mod (redirect 0x2D to 0x26).
5. **RankParam and CharSelect have no cancel.**
   - **Evidence:** backing out commits whatever the cursor left. 0x00719580 commits whenever the
     widget is hidden, whatever the close reason. The widget slot-2 cancel just closes.
   - **Confidence:** high. It is a UX bug, not a network inconsistency.
6. **Invalid rows ignored silently.**
   - **Evidence:** 0x00717D90 / 0x00719920 return without feedback when the entry is null or entry
     vfunc+4 returns 0. To the user this looks like an unresponsive list.
   - **Confidence:** high.
7. **Rematch split brain.**
   - **Evidence:**
     - The local window only learns its own decision from the host's echo.
     - The 20 s timeout overrides it with a forced Exit (FUN_00626680 sends `forced=1`, which the host
       accepts even when already decided, 0x0049C760).
   - **Symptom:** if both chose Rematch but the host's state broadcast to one side is late or lost,
     that side force-exits at 20 s while the host proceeds. Also, if the authority member
     (`flag 4`) is gone, FUN_0049D4B0 returns 0 and confirms are silently dropped.
   - **Confidence:** low-medium. It depends on packet reliability (part E).
8. **Request push result ignored.**
   - **Evidence:** FUN_004A88D0 returns 0 when the ring slot is occupied. Every caller has already
     cleared `task+0x1C`, so a dropped push would freeze input until the task re-enters a menu state.
     SCENE_CLobby pushes 4 without the handshake at all.
   - **Confidence:** low. The ring is 32 deep and flushed on every sub-state change.
9. **Possible blank list screen.**
   - **Evidence:** screens 4 and 10 only take input when the pool popup exists. If the popup is
     closed by something else while `+0x6B9C == 1` and the screen id does not change, the handler
     does nothing. There is no re-init and no input.
   - **Confidence:** low (not observed; needs an in-game check).

## Cross-part edges

- **Network task (B):** every request id above, the `+0x1C` / `+0xD8` / `+0xB4` / `+0xB8` contract,
  sub-state machines 0x004AC6C0, 0x004AE6D0, 0x004ACED0, 0x004ABF50, 0x004AAAD0, 0x004B01B0, and the
  ranked-entry object `DAT_00CF7758` (FUN_004A40A0: +0 state, +8 sub, +0x18, +0x1C suspend).
  FUN_004A41D0 hosts the ranked room.
- **Steam lobby (D):** net interface `FUN_00486070()->vtbl+0x1C` object:
  - +0xAE8 list count, +0xAF4 entries;
  - vfunc +0xAC refresh, +0xB0 busy, +0x30 UGC privilege;
  - entry vfunc +0x30 RTT, +0x34 req tier, +0x58 area req, +0x5C RTT req, +4 valid;
  - +0xAB0 last result code -> dialog choice.

  The net-manager flag word (FUN_0049A400) bits 0x100/0x200/0x400/0x800 (create),
  0x4000/0x8000 (search), 0x10000/0x20000 (join).
- **Network server / room (C):** room struct `big+0x22D10`, `+0x23218` (own member, flags +0x2C:
  bit1 in-match, bit0 side, bit 0x1000, bit 4 authority), member array stride 0x98 from +0x48 (mod's
  RoomMemberEntry), matchId +0x30; FUN_0049DF90 leave; FUN_0049A920 "in a room"; FUN_0049DAD0 ready.
- **Connection/netcode (E):** rematch packets 0x2F (cursor), 0x30 (decide), 0x31 (notify), 0x30088
  (state) sent with FUN_0040A3C0 + FUN_0040A7E0/0040A890/0040A9E0 to member +0x78 connections.
- **Scenes:** FUN_00645640 (scene change from dispatcher screen 0xE), result scene holder
  `FUN_00643B40()+0x2994`, OptionData (FUN_004B9700) persistence of ranked char/filters.

## Open questions

- Is `task+0x1C` ever re-armed by an outer-state error path (0xD/0xE)? If not, after an error the menu
  might come back dead. This needs part B's reading of FUN_004AA890.
- Do the stale completion flags in the net-manager word (0x4000 / 0x10000 / 0x20000) survive into a
  later wait? An example is the list auto-refresh finishing after the user pressed Back and then
  Search again. This needs part D: are the flags cleared on request start (FUN_004A94C0 /
  FUN_004A4440 / FUN_004A89D0 / FUN_004A4110)?
- Semantics of the gating flags in FUN_0062F7D0 (holder +0x48/+0x4C/+0x50/+0x54 and `+0x40`): they
  decide whether the 20 s rematch timeout is evaluated at all. If they are not all set, the timeout
  path is skipped and only the per-side decisions end the window.
- The return value of FUN_0049D4B0 for the local player when the local member has flag 4 (host
  sending a decide to itself): does the self-connection at +0x78 loop back?
- Net code 10 in 6/0x4E silently returns to Player Match top. Which Steam/game condition produces it?
  Part D lists EChatRoomEnterResponse 10 = "member blocked you", which would explain the silent path.
- Who sets the ranked entry object `+8` (waited on in 4/0x24)? A stuck value there freezes "Rank
  Search" behind screen 0xF.
