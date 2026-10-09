# B - GAME_CNetworkTask (network mode state machine)

## Summary
GAME_CNetworkTask is a tiny task node (vtable 0x008A9C4C, 0x18 bytes, created by GAME_CNetworkTaskFactory id 0x01020000) whose update (0x004A73D0) drives one static object, the **network task** at **DAT_00CF7958** (ctor 0x004A5860). That object holds an **outer task id** (+0x00) and an **inner state** (+0x04); the dispatcher 0x004A6F70 runs one outer task function per frame plus four always-on helpers: the Steam invite machine (0x004A6670), the netMgr frame update (0x0049AEA0: packet dispatch, keepalive, and conversion of every async netServer result into one-frame flags), the **ranked automatch** machine (0x004A47C0, object DAT_00CF7758) and the dialog/toast renderer (0x004A6D60).

Outer tasks: 0 idle, 1 network top menu, 2 network entry (netServer init + uei login), 3 D-code/profile sync (already rewritten by the mod), 4 ranked menu, 5 ranked match, 6 player-match menu (create/search/join), 7 inside a player room, 8 lobby entry (uei GetLobby + Steam lobby join), 9 inside the 2D lobby, 0xD forced exit (error/sign-out), 0xE normal exit.

Most important findings:
1. **Match start never checks connectivity.** The host starts a match (0x00497D40) as soon as both seated members have the READY bit (msg 8). The task then waits for in-match handshakes with timeouts of 600 frames (D-code), 1200 frames (pre-battle sync, CommonError) and the 5 s member-silence drop. For player rooms of type 0x2x it waits up to 600 frames for P2P (`NEER_P2PBattle_Desable`); type 0x10 (ranked and normal player rooms) has no such wait at all.
2. **Liveness is a 300-frame silence counter per member** (member+0x70), reset by any packet from that peer (0x00499E90). The host drops a member after more than 300 frames (0x00496FF0). The client leaves after 301 frames without a host packet (0x00497320), which gives "HostLost" or "LostBattler". These counters only run while netServer+0xAAC (in-session flag) is set.
3. **Leave can silently do nothing.** `Room_Leave` (0x0049DF90) returns 0x66 without leaving when the netServer request slot is busy or the join/create is not complete. It also only resets the local room when we have no member entry yet, without cancelling the netServer join. Several states call it once and then wait for Room state 0 with no timeout (task 5 0x3F, task 7 0x5D, task 0xD), so the UI can get stuck. A join that completes after such a local reset leaves us as a ghost member of the host's room (suspected).
4. **Ranked manual-list joins have no auto-cancel.** Task 4 state 0x2E waits forever for the host's 0x1000. Only the automatch path has the 30 s `RankAutoCancel`. This matches the stale-room logs: two minutes, then `RankMatchLeaveMyself`, which only appears after the user confirms the cancel dialog.
5. **The host's seating logic freezes itself.** It returns early while any member entry has 0x1000 and a stage other than 0x11/0x12. A stale finished-match entry would block seating and match start for every later joiner (suspected cause of the stale room).
6. Smaller bugs. A failed client-leave poll sets the *join failed* flag (0x20000). Task 5's pre-match profile upload decrements +0x14 but tests +0x20, so its 3000-frame timeout never fires. Task 4 state 0x27 retries ranked-room creation every frame while create keeps failing.

## Functions
| VA | Name | Size | Level | Notes |
|---|---|---|---|---|
| 0x00405B20 | IntrusiveList_PushBack | 89 | L1 | thiscall(list, node), ret 4. Appends node to a doubly linked list (+4 tail, +8 count). Used by the receive-log node pool (0x004A1890). |
| 0x00405CB0 | IntrusiveList_Advance | 15 | L1 | fastcall(list): cursor(+4) = cursor->next. Advances the receive-message debug log ring (netData+0x23264) in 0x0049AEA0. |
| 0x00407E40 | SteamCtx_GetFriends | 18 | L1 | Returns SteamInternal_ContextInit(0x9D3210)+8 = ISteamFriends. Task 6 state 0x4F calls its vt+0x70 (ActivateGameOverlay) - open the Steam overlay (friends/invite). |
| 0x0040A430 | RingBuffer_Init | 299 | L1 | library: allocates a send/recv byte ring (sizes 0x80000/0x80000) for the packet layer; called once from NetSubsystem_Init. |
| 0x0040A560 | PacketChannelTable_Init | 222 | L1 | library: allocates 10 channel records of 0x28 bytes each with 0x400-byte buffers (the 10 packet channels cleared by FUN_0040A9E0 on room reset). |
| 0x0040A650 | PacketQueue_Pop | 81 | L1 | thiscall(queue, &msgPtr, &senderInfo16): pops the next received packet (read pos +0x1C < write pos +0x18); copies 16-byte sender id (steamId64 + extra) and returns the message pointer. Source of every message dispatched by 0x0049AEA0. |
| 0x0040A910 | PacketQueue_Clear | 20 | L1 | fastcall(queue): +0x18 = +0x1C = 0 after the frame's packets are consumed. |
| 0x0040BF90 | ProfileStampSource | 143 | L1 | Already named by the mod. Returns a Mersenne-Twister value used as the profile blob stamp. Not lobby logic. |
| 0x0040DEC0 | ProfileChecksumSeal | 68 | L1 | Already named (see docs/Decompilation/ProfileSync.md). Writes stamp + 16-bit checksum of the profile blob. |
| 0x00413280 | Obj_ClearField18 | 8 | L1 | fastcall(obj): obj+0x18 = 0. Called on network exit / room leave on a UI object. |
| 0x0041F0E0 | ReplayCatalog_Init | 87 | L1 | thiscall(obj, n): allocates n*0x18 entries + 0x2408 zero bytes (replay/ranking id table, 0x3E entries from 0x0089D268). |
| 0x004276A0 | UeiWorkMgr_Ctor | 179 | L1 | fastcall(mgr): uei web work manager (DAT_00A5A050 area) defaults: +0x1C=7, +0x58=0xF, +0xB8=7. Constructed lazily by 0x00428050. |
| 0x00427EC0 | Uei_StartCreateUser | 34 | L1 | fastcall(mgr): replaces strategy +0xE0 with uei strategy 2 (ThinkLogicStrategyCreateUser). uei server: create account. |
| 0x00427FD0 | Uei_StartGetLobby | 76 | L1 | fastcall(mgr): sets option flag, strategy +0xE0 = 3 (ThinkLogicStrategyGetLobby). uei server: lobby list. |
| 0x00428050 | Uei_StartLogin | 163 | L1 | fastcall(mgr): lazily constructs the work manager; if login latch DAT_00A5A070 == 0, strategy +0xE0 = 1 (ThinkLogicStrategyLogin). Already used by the mod's forced re-login. |
| 0x004281E0 | Uei_ResetToIdle | 109 | L1 | fastcall(mgr): sets option flag, FUN_004282C0 reset, strategies +0xE0 and +0xE4 = 0 (ThinkLogicStrategyIdle). |
| 0x00434A30 | Obj_SetField2C | 13 | L1 | thiscall(obj, v), ret 4: obj+0x2C = v (option manager flag set before uei login/get-lobby). |
| 0x004528F0 | MT_NextU32 | 74 | L1 | library: Mersenne Twister genrand (tempering); refills via 0x00452990 every 624 draws. |
| 0x00452990 | MT_Refill | 193 | L1 | library: Mersenne Twister state regeneration (seed 0x1571 if never seeded). |
| 0x00465C60 | CCallback_DlcInstalled_Ctor | 167 | L1 | library: Steam CCallback<GAMESTEAM_CDLCManager, DlcInstalled_t> constructor. |
| 0x00465D10 | GAMESTEAM_CDLCManager_Ctor | 140 | L1 | DLC manager constructor (registers DlcInstalled_t). Reached through 0x004949A0. |
| 0x0046A390 | Uei_PollLogin | 54 | L2 | cdecl(), ret 0. Reads uei work manager (FUN_00427CD0): if +0x20 (logged-in latch) != 0 -> 0; else mgr+0 status: 0 -> -100 (pending), 0xB -> 100 (failed), 2 -> 0x6E (account not registered), anything else -> 0. Used by tasks 2/8 state 100. |
| 0x0046A3D0 | Uei_PollGeneric | 31 | L2 | cdecl(), ret 0. mgr+0: 0 -> -100 pending, 0xB -> 100 failed, else 0. Used after create-user (state 0x66) and get-lobby (state 0x67). NO timeout anywhere above it except the uei strategy's own. |
| 0x0046A9B0 | Uei_IsLoggedIn | 9 | L1 | Returns uei mgr byte +0x20 (login latch). Task 8 state 0 skips login when set. |
| 0x0046BE70 | Uei_GetLobbyThunk | 15 | L1 | FUN_00427CD0 then 0x00427FD0 (start GetLobby). |
| 0x0046BF10 | Uei_LoginThunk | 15 | L1 | FUN_00427CD0 then 0x00428050 (start Login), returns 0. |
| 0x0046BF20 | Uei_CreateUserThunk | 15 | L1 | FUN_00427CD0 then 0x00427EC0 (start CreateUser), returns 0. |
| 0x0046BF80 | Uei_IdleThunk | 12 | L1 | FUN_00427CD0 then 0x004281E0 (reset strategies to Idle). Called right before every login attempt. |
| 0x0046E630 | Peer_MatchesSender | 47 | L1 | thiscall(peer, senderId12), ret 4: compares peer+0x78/+0x7C/+0x80 with the 12-byte sender id. Used by message handlers 5/0x12/0x13 to accept data only from the expected opponent. |
| 0x0046E8C0 | NetServer_Get | 108 | L1 | Returns the GAMESTEAM_CNetworkServer singleton DAT_00A62730 (ctor FUN_0046C6E0). Same object as holder vt+0x1C. |
| 0x0046EAA0 | NetServer_AllocLists | 213 | L1 | fastcall(netServer): allocates +0xBF0 (0x12 x 0x44 entries) and +0xCEC (7 x 0x4C) result arrays. |
| 0x00471380 | ReplayRanking_InitTables | 102 | L1 | Sets 'bbrd.dat' file name and copies the RANK_ALL leaderboard table. Not lobby logic. |
| 0x004725B0 | NetSubsystem_Init | 252 | L1 | Registered init: packet rings, room data, netServer, then registers task factories AA_CNetworkSenderFactory (id 0x30000000), AA_CNetworkReceiverFactory (0x01010000) and GAME_CNetworkTaskFactory (0x01020000). Returns 1, or 0 if the Steam networker init fails. |
| 0x004742F0 | GAME_CNetworkTaskFactory_Create | 112 | L1 | Factory vt slot 0: operator_new(0x18) + 0x004A5C90. The task node itself is tiny; all state lives in the static DAT_00CF7958. |
| 0x00474980 | GAMESTEAM_CVoiceChatManager_Ctor | 84 | L1 | Voice chat manager ctor (P2PSessionRequest/ConnectFail callbacks). Part E/C edge. |
| 0x00476360 | PickRandomStage | 141 | L1 | (charA, charB, mode): picks a stage id (10 for some mirror cases, else random). Used when the host chooses the stage (task 5 state 0x34). |
| 0x0047B4C0 | Catalog_ResetAll | 217 | L1 | fastcall(catalog): clears 10 catalog slots (0x0047C7B0) and counters. |
| 0x0047B6A0 | Catalog_ParseLine | 398 | L1 | library: line tokenizer for tmsenv.info / catalog text files. |
| 0x0047B840 | Catalog_ParseTmsEnv | 877 | L2 | fastcall(catalog), ret 0. Reads 'tmsenv.info' from the downloaded catalog (BCSM blob) and fills server-tunable values: +0x200088.. NetSetsudan*Pt (disconnect penalty), +0x200098 IsSetRankMatchParam, +0x20009C RankFirstWaitMin, +0x2000A0 RankFirstWaitRange, +0x2000A4 RankIntervalWaitMin, +0x2000A8 RankIntervalWaitRange, +0x2000AC RankAutoCancel (all seconds), +0x200270/71 IsDisableOnlineSU(+Message), +0x20026C GetPingInfoTimeOutTime_PS4. Sets +0x200008 = 1 (parsed). |
| 0x0047BCB0 | Catalog_DefaultEnv | 151 | L1 | fastcall(catalog): defaults +0x20001C=30, +0x200020=30, +0x200024=20, +0x200028=120, +0x20002C=240, +0x200030=1, +0x200034=5. |
| 0x0047BF30 | Catalog_ClearSlots | 362 | L1 | library: clears catalog/DLC slot arrays. |
| 0x0047C0A0 | Catalog_FindFile | 284 | L1 | thiscall(catalog, name, &size): finds a named file inside the downloaded BCSM catalog blob (tmsenv.info, _appendidlist.txt...). |
| 0x0047C1D0 | Catalog_Stub | 6 | L1 | Returns constant; used by DLC query start 0x004B9100. |
| 0x0047C2C0 | Catalog_IsLoaded | 7 | L1 | fastcall(catalog): returns +0x200004. |
| 0x0047C4A0 | Catalog_LoadBlob | 315 | L1 | thiscall(catalog, size): validates 'BCSM' magic, decodes, then 0x0047B840. Called from DLC query poll 0x004B9070. |
| 0x0047C5E0 | Catalog_Reset | 137 | L1 | fastcall(catalog): memset 2 MB + defaults. |
| 0x0047C7B0 | CatalogSlot_Reset | 256 | L1 | library: one catalog slot reset. |
| 0x0047D910 | CharSelectConfigCommit | 233 | L1 | Already named by the mod. Commits char-select config; used by ranked entry flag setters 0x004A2BB0/0x004A2BD0. |
| 0x0047FD20 | CharSelect_CopyPendingConfig | 264 | L1 | Copies +0x16B8 -> +0x118 and +0x1DC8 -> +0x828 (pending ranked char config into active). Called when returning from a ranked match (0x004A46F0). |
| 0x0048DB00 | SaveData_OnlineBlock | 7 | L1 | Returns saveMgr+0x9F90 (online settings block copied back in task 1 state 0x15). |
| 0x004949A0 | DLCManager_Get | 212 | L1 | Lazily creates the GAMESTEAM_CDLCManager singleton DAT_00CACF28. |
| 0x00496040 | InstallManager_Get | 126 | L1 | Returns CInstallManager_NULL singleton; its vt+0x1C 'installing' blocks the invite prompt in 0x004A6670. |
| 0x004963A0 | Flags_Test | 13 | L1 | thiscall(obj, mask), ret 4: obj+0x28 & mask. |
| 0x00496B50 | RoomSnapshotMsg_Init | 281 | L1 | Builds the host room-state packet header (0x203A8 = type 2, size 0x3A8) used by 0x0049DFF0. |
| 0x00496C70 | SpectatorInfoMsg_Init | 192 | L1 | Builds packet 0x180094 (type 0x18) header, used by 0x0049EA80. |
| 0x00496D30 | NetMgr_Ctor | 259 | L1 | fastcall(netMgr): constructor of DAT_00CAD000 (flag words, per-slot records). Called lazily by NetMgr_Get 0x0049A400. |
| 0x00496E40 | Room_ClearMatchState | 212 | L1 | fastcall(netMgr): if my member entry is in a match (0x1000) and its stage is not 0x0E/0x10/0x0F, resets the match bookkeeping; used on entering task 5/7 and after rematch decline. |
| 0x00496FF0 | NetMgr_HostKeepalive | 816 | L2 | fastcall(netMgr), ret 0. Host only (called from 0x0049AEA0 when my entry has +0x2E bit0). Host entry (+0x2321C)+0x70 += 1; > 60 frames -> netMgr[0] /= 0xC0000000 (broadcast room + heartbeat) and reset. If netServer+0xAAC != 0: for each of 8 members with status==2 and not host: member+0x70 (frames since last packet) += 1; when > 300 (5 s @60fps) -> netServer vt+0x34(memberIndex) (drop member) and netMgr[0] /= 0x10000000. Also raises 0x40000000 when member steamIds or my stage change. In-match: if my entry has 0x1000, stage == 0x0C, not seated (bit1 clear) and netData+0x22C9A > 0x21C (540 frames) -> netMgr[1] /= 0x20000000 and netMgr[0] /= 0x10000000. |
| 0x00497320 | NetMgr_ClientKeepalive | 326 | L2 | fastcall(netMgr), ret 0. Client side (my entry is not host and has 0x20000). Host entry +0x70 += 1 and own +0x70 += 1. While host counter < 0x12D: own counter > 60 -> netMgr[0] /= 0xC0000000 (send heartbeat). If host counter reaches 0x12D (301 frames ~5 s with no packet from the host) and netServer+0xAAC != 0 -> FUN_004A5740 (leave as client) and netMgr[0] /= 0x20000000 ('host lost'). Spectator stall: same 0x21C rule as host. |
| 0x004974E0 | NetMgr_RoomTick | 1276 | L2 | fastcall(netMgr), ret 0. Runs only when Room state != 0 and my entry exists. Host: picks up pending broadcast (+0x23254), clears member 0x800000 unless room flag 0x8000000; idle auto-start timer: if room+0x36 (seconds) != 0 and >2 (>1 for type 0x40) members are active, my entry seated (bit1) and not in match/OK, +0x23258 counts frames; 300 frames before room+0x36*60 sets entry+0x50 bit1 and broadcasts; after the limit sends msg 7 (0x7000C) every 20 frames. Then per room type (Room+0x30 & 0xF0): 0x10/0x30 -> 0x00497D40, 0x20 -> 0x004979E0, 0x40 -> 0x00498480, 0x50 -> 0x004989E0 (host only), then 0x00499720 for everyone. Raises 0x40000000 if any member's +0x34 byte changed. |
| 0x004979E0 | RoomHost_TickType20 | 847 | L1 | fastcall(netMgr): host seating/match-start logic for room type 0x2x (FFA/lobby-slot style), uses 0x004A1E70/0x004A23D0. Same role as 0x00497D40 for another room type. |
| 0x00497D40 | RoomHost_TickPlayerMatch | 1843 | L2 | fastcall(netMgr), ret 0. HOST seating + match start for room types 0x10 (ranked / player match) and 0x30. (1) If any member (status 2) has 0x1000 (in match) and stage byte +0x5B is not 0x11/0x12 -> return (match running; nothing else runs). (2) For members that finished (0x1000): clear flags (mask 0xFFBFE000), apply rotation rule Room+0x30&0xF00 (0x100/0x200/0x400) and win-streak byte +0x35, unseat losers (0x004A2350), reseat by side (bit0) -> broadcast. (3) If memberCount < 2 or Room+0x3A bit 0x20 (settings being edited): unseat everyone. (4) Else apply win-limit (Room+0x34 high nibble) and opt-outs (0x400000/0x800000), fill empty seats 0/1 from the queue (0x004A2280; random pick for rule 0x400). (5) MATCH START: if memberCount >= 2, not editing, both seated entries have bit1 (seated) and bit3 (OK pressed) and neither has 0x1000 -> matchCounter (+0x2324C)++, every member with status 2 gets 0x1000 and matchId(+0x30) = counter, seat-0 entry gets bit2, broadcast. NO connectivity / P2P check is made here. |
| 0x00498480 | RoomHost_TickType40 | 1361 | L1 | fastcall(netMgr): host logic for room type 0x40 (replay theater / spectating). |
| 0x004989E0 | RoomHost_TickType50 | 3367 | L1 | host logic for room type 0x50 (team/lobby-slot variant: uses 0x004A1630/0x004A1730/0x004A1980/0x004A1FA0). |
| 0x00499720 | Room_ResolveSeatPointers | 577 | L2 | fastcall(netMgr), ret 0. If my entry is not in a match: when my stage byte (+0x5B) != 0 reset it to 0 and broadcast; clear +0x23244/+0x23248 (side 0/1 player entry pointers) and my +0x73. If in a match: on stage 0 -> stage 2, broadcast, reset match data (0x0049D3A0) and fill +0x23244/+0x23248 from the seated entries (by type: 0x20 via FUN_004A0F50, 0x40 single, else FUN_004A1580(0/1)); on stage 9 refresh pointers via 0x004A1630. |
| 0x00499B00 | Room_AbortIfSessionGone | 63 | L1 | Invite flow: if Room state != 0 and netServer vt+0xCC() -> 0x0049EF20 (local room reset). vt+0xCC is FUN_0041DB90 = always 1 on Steam. |
| 0x00499CB0 | Room_Reset | 264 | L2 | fastcall(room = netData+0x22D10), ret 0. room+0 (state) = 0, zeroes header +4..+0x47, 8 member entries (0x98 each from +0x48): status short 0, ids/names cleared, name = L"USER_ERR_ULIMIT", seat/side shorts = 0xFFFF, +0x60 = 0xFF000000, memberIndex = i; clears +0x508..+0x548 (my/host entry pointers, seat pointers), +0x53C = 1, then 0x004A1830 (slot table reset). |
| 0x00499DC0 | MatchData_Reset | 37 | L1 | fastcall(p): clears 0x4C bytes, +2 = 0x02000200, +0x3C = 1. |
| 0x00499DF0 | Room_ResetBothLoaders | 72 | L1 | For both opponent D-code rows: if loader poll != -100 reset it (FUN_004A0CF0). Used before re-fetching opponent profiles. |
| 0x00499E40 | MatchFlags_Reset | 27 | L1 | fastcall(p): clears +0x4C..+0x6B, +0x5D = 1. |
| 0x00499E60 | Room_ResetLoaderIfIdle | 38 | L1 | Reset the opponent D-code loader if it is not pending (task 5 state 0x36 entry). |
| 0x00499E90 | Room_TouchSender | 379 | L2 | stdcall(senderId12*), ret 4. For each member whose peer id (member+0x78..+0x80) equals the sender: member+0x70 (silence counter) = 0. Ignores loopback ids (0x7F000001 / 0). Called for every received message except types 0x2B-0x2E: this is the only liveness refresh. |
| 0x0049A230 | OwnProfileFetchStart | 179 | L1 | Already named by the mod (ADDR_OwnProfileFetchStart). Arms the own D-code download. |
| 0x0049A2F0 | Profile_ClampOpponentField | 71 | L1 | Helper of msg 0x13 handler: sanitises a received byte field (< 0xC). |
| 0x0049A470 | NetData_CopyOwnId | 24 | L1 | Copies own id into netData; passthrough of param. |
| 0x0049A4E0 | NetData_GetSlotByte | 25 | L1 | Returns netData+0x6A69+i*0x10 (per-side char/config byte). |
| 0x0049A510 | NetData_GetSlotDword | 27 | L1 | Returns netData+(i+0x6A7)*0x10. |
| 0x0049A770 | Room_NoPlayerBusy | 57 | L1 | Returns 1 if no member (status 2) has 0x1000 or +0x5B != 0 - i.e. room settings may be edited (event 0x21). |
| 0x0049A810 | Room_OpponentPresent | 215 | L2 | cdecl(int checkType50), ret 0. False if my entry is null. For type 0x50 with stage < 10 -> true. If I am seated: true when the OTHER side pointer (+0x23248 for side 0, +0x23244 for side 1) is non-null. If spectating: true when both side pointers are set. Used as 'opponent still here' in tasks 5/7 (false -> LeaveBattler / LostBattler). |
| 0x0049A8F0 | Room_SpectateTargetPresent | 48 | L1 | True if my entry exists and +0x23244 (side-0 player) is set; spectator variant of 0x0049A810. |
| 0x0049A940 | OwnProfileFetchPoll | 32 | L1 | Already named by the mod. |
| 0x0049A960 | NetMgr_TestFlag20000000 | 24 | L1 | fastcall(netMgr): netMgr[1] & 0x20000000 (match/spectate stall) -> SpectateOver error. |
| 0x0049A980 | Room_AmIHost | 24 | L2 | cdecl(), ret 0. Returns (myEntry+0x2E) & 1, or 0 when no entry. Host = owner of the room. |
| 0x0049A9A0 | Room_AmISeatedSide0 | 36 | L1 | True if my entry has bit1 (seated) and bit0 == 0. |
| 0x0049A9D0 | P2P_IsConnected | 19 | L1 | fastcall(obj): if obj+0x1C238 bit0 (attempted) return bit1 (connected). Waited on by task 7 state 0x61 (600 frames). |
| 0x0049A9F0 | Room_IsType1 | 23 | L1 | (Room+0x30 & 0x0F) == 1. |
| 0x0049AA30 | Room_OpponentReturned | 175 | L1 | Player variant: true if a seated opponent's stage is 0x0F/0x12 (returned to room). Drives NTER_PlayerMatch_ReturnBattler (0x26). |
| 0x0049AAE0 | Room_HostReturned | 85 | L1 | True if the host is not me, room type != 0x10, and the host's stage is 0x0F/0x12 -> NTER_PLayerMatch_ReturnHost (0x28). |
| 0x0049AB40 | Room_ReplayOwnerReturned | 72 | L1 | True if side-0 entry is not me and its stage is 0x0F/0x12 -> ReturnReplayOwner (0x27). |
| 0x0049ABB0 | RoomHeader_Equal | 70 | L1 | thiscall(a, b), ret 4: compares two room headers (+0x2C, +0x30 and 0x14 shorts). Client uses it to detect host room-setting changes. |
| 0x0049AC20 | Msg20_Apply | 193 | L1 | Applies a msg 0x20 payload to the room (helper for 0x0049CD60). |
| 0x0049ACF0 | Msg21_Apply | 234 | L1 | Applies a msg 0x21 payload (helper for 0x0049CF30). |
| 0x0049ADE0 | Msg23_ApplyInputs | 179 | L1 | Copies replay/training input frames into battle data (helper for 0x0049D110). |
| 0x0049AEA0 | NetMgr_FrameUpdate | 2047 | L2 | fastcall(netMgr = DAT_00CAD000), ret 0. Every frame except task 0xD/0xE. Steps: tick voice chat; host mirrors 'pad connected' into my entry 0x100000; 0x0049F750; clear netMgr[0], netMgr[1], netMgr[5]; 0x0049F990; drain PacketQueue_Pop and dispatch by msg type (1 member status, 2 room snapshot, 3/4 match settings, 5, 7 opt-out, 8 READY/OK, 9, 0xB room closed -> if netServer+0xAAC leave and netMgr[1] /= 0x10000000, 0xF, 0x12-0x17, 0x18, 0x19, 0x1D-0x23, 0x26, 0x27, 0x2B-0x2E -> lobbyData vt+0x7C..+0x88, 0x2F-0x31, default -> netServer vt+0x90); keepalive (host 0x00496FF0 / client 0x00497320 + header compare); netServer vt+0x94 >= 100 -> netMgr[1] /= 0x8000000. Then converts every netServer async poll into one-frame flags (see Data). Reactions: 0x100 copy own user; 0x400 -> 0x0049EE00 (host room built); 0x10000/0x40000 -> 0x0049ED30 (joined); 0x800/0x1000/0x2000/0x100000/0x20000/0x80000 -> Room_Reset + clear 10 packet channels. Then 0x004974E0; netMgr[0] 0x40000000 -> host broadcast 0x0049DFF0 / client 0x0049E820; 0x80000000 -> heartbeat 0x0049D8D0. |
| 0x0049B770 | Msg3_4_MatchSettings | 260 | L1 | Message 3/4: only if my entry has 0x1000 and matchId matches; copies match settings (19 dwords) into netData+0x6A1C, round/time data; type 4 also copies 9 dwords to +0x22CC8. |
| 0x0049B880 | Msg8_ReadyOk | 341 | L2 | thiscall(netMgr, msg, sender12), ret 8. HOST receives a member's READY/OK: ignored while Room+0x3A bit 0x20. Finds the sender entry. byte8 -> clear 0x800. If entry not in match and stage 0: byte6==0 (seated request): requires bit1 and side(bit0)==byte4, then byte5==0 sets bit3 (OK) else clears it; byte6!=0: byte5==0 sets 0x800 (wants seat, only if not seated) else clears 0x800. Broadcast (0x40000000). This is how 'OK' in the ranked match-found popup reaches the host. |
| 0x0049B9E0 | Msg7_OptOut | 243 | L1 | Message 7 (host): sets/clears 0x400000 (rotate out) or 0x800000 (opt-out of play, only when Room flag 0x8000000) on the sender's entry. Not for types 0x20/0x50. |
| 0x0049BAE0 | Msg1D_Handler | 84 | L1 | Message 0x1D: room-level update gated on my entry not in a match. |
| 0x0049BB40 | Msg5_OpponentData | 100 | L1 | Message 5: accepted only from the opponent peer (0x0046E630) with the current matchId -> my entry +0x73 = 1 (opponent ready for battle). |
| 0x0049BBB0 | Msg13_OpponentProfile | 469 | L1 | Message 0x13: in match (0x1000 + bit2) and my stage 0 -> stores opponent char/profile summary, ProfileStampSource, 0x0049A2F0. |
| 0x0049BD90 | Msg14_Handler | 118 | L1 | Message 0x14: in-match data copy. |
| 0x0049BE10 | Msg16_Handler | 198 | L1 | Message 0x16: in-match stage/music selection from host (matchId check via 0x0049A3A0). |
| 0x0049BEE0 | Msg17_Handler | 118 | L1 | Message 0x17: in-match data. |
| 0x0049BF60 | Msg9_Handler | 160 | L1 | Message 9: per-member counter/flag update (my entry +0x36/+0x38). |
| 0x0049C000 | Msg2_RoomSnapshot | 105 | L2 | stdcall(msg, sender), ret 8. Client applies the host's room snapshot only if both my entry and host entry exist and netServer+0xAAC != 0: 0x004A01E0(header, rev, ids, names, sender) then 0x004A0400 (member details). |
| 0x0049C070 | MsgF_SlotAssign | 256 | L1 | Message 0xF: lobby-slot assignment (uses 0x004A1B30/0x004A1B40/0x004A1BF0) when my entry is not in a match but has 0x10000. |
| 0x0049C170 | Msg1E_Handler | 347 | L1 | Message 0x1E: room-level request handled when not in a match; replies via packet channel. |
| 0x0049C2D0 | Msg1F_Handler | 387 | L1 | Message 0x1F: room-level update when not in a match. |
| 0x0049C460 | Msg12_Handler | 279 | L1 | Message 0x12: in-match message accepted only from the opponent with matching matchId. |
| 0x0049C580 | Msg15_Handler | 128 | L1 | Message 0x15: in-match (rematch decision) - writes netData+0x6A66 flags. |
| 0x0049C600 | Msg1_MemberStatus | 352 | L1 | Message 1: member status update; for the sender entry: if in my match sets netData+0x22CC0 bits (0x80000000 heartbeat request); host mirrors bit 0x100000; then 0x004A0400 (member data). |
| 0x0049C760 | Msg30_Handler | 172 | L1 | Message 0x30: in-match per-side data into netData+0x6A1C+side*8. |
| 0x0049C810 | Msg31_Handler | 118 | L1 | Message 0x31: in-match data. |
| 0x0049C890 | Msg2F_Handler | 130 | L1 | Message 0x2F: in-match data. |
| 0x0049C920 | Msg18_Handler | 484 | L1 | Message 0x18: spectator info request; replies with 0x190008. |
| 0x0049CB10 | Msg19_Handler | 118 | L1 | Message 0x19: in-match data. |
| 0x0049CB90 | Msg22_Handler | 449 | L1 | Message 0x22: room-level update when not in a match. |
| 0x0049CD60 | Msg20_Handler | 459 | L1 | Message 0x20 -> 0x0049AC20. |
| 0x0049CF30 | Msg21_Handler | 478 | L1 | Message 0x21 -> 0x0049ACF0. |
| 0x0049D110 | Msg23_Handler | 373 | L1 | Message 0x23 -> 0x0049ADE0 (input frames). |
| 0x0049D290 | Msg26_Handler | 141 | L1 | Message 0x26: in-match, newer sequence (+0x6A58) only. |
| 0x0049D320 | Msg27_Handler | 113 | L1 | Message 0x27: in-match data. |
| 0x0049D3A0 | MatchData_ResetFull | 158 | L1 | fastcall(p): 0x00499DC0 + 0x00499E40 combined reset. |
| 0x0049D440 | DCodeFetchPump | 98 | L1 | Already named. Per-frame opponent D-code loader pump (dispatcher calls it for tasks other than 0/2/3/0xE). See DCodeNetworkStallBug.md. |
| 0x0049D520 | Room_OpponentLoader | 51 | L1 | fastcall(room): returns the opponent's loader slot (+0x538 if I am side 0, +0x534 if side 1). |
| 0x0049D8D0 | NetMgr_SendHeartbeat | 503 | L1 | Sends my periodic status packet (msg 1) to the room; triggered by netMgr[0] 0x80000000 (every ~60 frames from the keepalive functions). |
| 0x0049DAD0 | Room_SendReady | 149 | L2 | stdcall(ok, seatRequest, a3, a4), ret 0x10. Needs my entry (+0x23218) seated (bit1) or seatRequest != 0, and a host entry (+0x2321C). Sends msg 8 (header 0x0008000A) to the host peer (hostEntry+0x78): byte4 = my side (or 0xFF), byte5 = !ok, byte6 = seatRequest, byte7 = a3, byte8 = a4. Returns 1 if sent. Used by the ranked popup OK (automatch step 9, task 4 state 0x2E) and room events 0x1D/0x1E/0x22. |
| 0x0049DB70 | Room_SendOptOut | 113 | L1 | stdcall(a, b), ret 8: sends msg 7 (0x0007000C) to the host (rotate/opt-out). Events 0x1F/0x20. |
| 0x0049DD90 | Room_SendCharSelect | 248 | L1 | stdcall(11 args), ret 0x2C: sends msg 0x13 (0x00130018) with my character/colour/rank data to the room. Task 5 state 0x33, rematch. |
| 0x0049DE90 | Room_SendStageMusic | 158 | L1 | stdcall(stage, music), ret 8: sends msg 0x16 (0x0016000C). Host only (task 5 states 0x34/0x35). |
| 0x0049DF30 | Room_SendKickOrMisc | 87 | L1 | Sends a one-byte request to the host (event 0x27). |
| 0x0049DFF0 | Room_HostBroadcast | 653 | L1 | Host: Room+0x3C (revision) += 1, my entry +0x4C += 1, builds msg 2 (0x00496B50) with header, revision, member ids/names and sends it to every member. Triggered by netMgr[0] 0x40000000. |
| 0x0049E630 | Room_KickMember | 159 | L1 | Host: asks netServer to kick member index (event 0x26 -> PMRI_AskKick OK, task 7 state 0x53). |
| 0x0049E820 | Room_ClientSendStatus | 595 | L1 | Client: sends my status (msg 1) to the host; uses 0x0049D520 and 0x004C32D0 (voice). |
| 0x0049EA80 | Room_SendSpectatorInfo | 598 | L1 | Sends msg 0x18 (spectator/replay info) - task 7 state 0x5F. |
| 0x0049ED30 | Room_OnJoined | 206 | L2 | stdcall(joinResult), ret 4. On netMgr[0] 0x10000/0x40000: netServer+0xAAC = 1 (in session), applies the host's initial room data via 0x004A01E0(result+0x10, +0x44, +0x54, +0x194, result), copies per-member shorts, Room state = 2. |
| 0x0049EE00 | Room_OnCreated | 282 | L2 | stdcall(createParams), ret 4. On netMgr[0] 0x400: netServer+0xAAC = 1, Room_Reset, copies room header from createParams (+8 x10 dwords, +0x108 type -> Room+0x30, +0x10C -> Room+0x34, +0xFC/+0xFD/+0x104), memberCount += 1 and inserts myself into the first free member slot (host flag), then Room state = 2 (rest of function). |
| 0x0049EF20 | Room_ResetLocal | 42 | L1 | Room_Reset + clear the 10 packet channels. No network traffic. |
| 0x0049F0F0 | NetData_StoreMatchChars | 313 | L1 | Stores chosen characters for the match (+0x25F0 -> +0x25F4) when seated. |
| 0x0049F2A0 | Match_PrepareBattle | 346 | L1 | Prepares online battle data (pads, training-online flags via 0x006D3B70/0x006D3EE0/0x006D4460/0x006D4480), ProfileStampSource. |
| 0x0049F400 | Room_RequestLobbySlot | 386 | L1 | thiscall(netMgr, pts), ret 4: type 0x50 room: if host not in match, pick a free side (0x004A1730) and 0x004A2560. |
| 0x0049F750 | Room_SyncMemberCounters | 562 | L1 | Copies per-member counters (+0x8E/+0x90 shorts) host -> side tables. |
| 0x0049F990 | NetMgr_UpdateOwnRankInfo | 501 | L1 | Updates my member entry rank/profile fields from the own profile (0x004A1410, 0x004A1480, 0x004BE030) and options. |
| 0x004A01E0 | Room_ApplySnapshot | 539 | L2 | stdcall(header13*, rev4*, ids*, names*, sender*), ret 0x14. Only if Room+0x3C (revision) <= incoming rev: copies header (13 dwords to Room+4), memberCount/revision/+0x40/+0x44, netServer vt+0xA4(member array, ids, names), rebuilds 8 entries (status 2 if present else cleared), recomputes my entry pointer (+0x23218, matched on own steamId via FUN_00407A60) and host entry pointer (+0x2321C = entry with +0x2E bit0, gets the sender address), and drops stale side pointers whose matchId/side changed. The joiner therefore takes the host's member table verbatim - including any stale entries the host still holds. |
| 0x004A0400 | Room_ApplyMemberDetail | 500 | L1 | stdcall(detail, flag), ret 8: copies per-member detail into entries other than mine. |
| 0x004A0B80 | ProfileLoaderPoll | 42 | L1 | Already documented in ProfileSync.md: cc 3/4 -> 0, 6 -> 100, else -100. |
| 0x004A0BB0 | NetData_ResetOwnUser | 149 | L1 | Resets own user record (+0x697A, +0xCA, +0x6970 buffer). |
| 0x004A0C50 | NetData_ResetAll | 151 | L1 | Full network data reset on network entry (task 2 state 0): own user, match data, Room_Reset, loaders. |
| 0x004A0D50 | DCodeBlobReset | 105 | L1 | Already named by the mod. |
| 0x004A0DC0 | Profile_ApplyDisconnectPenalty | 22 | L1 | profile+0xC0 -= catalog+0x20008C (NetSetsudanAddPt). Task 5 state 0x37 before the pre-match upload: the disconnect counter is charged up front. |
| 0x004A0DE0 | SlotTable_Reset | 366 | L1 | Clears lobby-slot assignment bits in 8 entries (type 0x50 rooms). |
| 0x004A1410 | Profile_RankRow | 24 | L1 | Already documented: blob+0xD4+char*0x180. |
| 0x004A1480 | Profile_RankValue | 15 | L1 | FUN_004BE150(profile+0xC8). |
| 0x004A1630 | Slot_FindEntry | 127 | L1 | thiscall(room, side), ret 4: finds the member assigned to a lobby slot side. |
| 0x004A1730 | Slot_FindEntryAlt | 159 | L1 | thiscall(room, side), ret 4: as above, other slot class. |
| 0x004A1810 | Battle_Get | 21 | L1 | Thin wrapper of FUN_0069D2D0 (battle data object). |
| 0x004A1830 | SlotTable_ResetFromRoom | 84 | L1 | Clears slot bits in member entries (part of Room_Reset). |
| 0x004A1890 | ReceiveLogPool_Init | 160 | L1 | Allocates n (0x40) GAME_CReceiveMsgLogNode (0x28 bytes) into a list. |
| 0x004A1980 | Slot_BothFilled | 44 | L1 | Type 0x50: true unless both slot sides are filled. |
| 0x004A1A00 | Loader_IsArmed | 19 | L1 | loader cc == 1. |
| 0x004A1AB0 | Loader_IsBusy | 25 | L1 | loader cc != 0 && != 3. |
| 0x004A1B30 | Slot_IsEmpty | 9 | L1 | room+0x40 byte == 0. |
| 0x004A1B40 | Slot_GetAssignment | 164 | L1 | Reads the 0x30000000 slot bits of a member. |
| 0x004A1BF0 | Slot_ClearAssignment | 24 | L1 | Clears slot bits of member i. |
| 0x004A1C10 | ProfileBlobSealBlob | 15 | L1 | Already named. |
| 0x004A1DD0 | DCodePayloadValidator | 150 | L1 | Already named. |
| 0x004A1E70 | RoomHostT20_PickNext | 250 | L1 | Picks the next member with 0x800 (wants seat) for type 0x2x rooms. |
| 0x004A1F70 | ProfileBuildDefault | 31 | L1 | Already named. |
| 0x004A1FA0 | Room_SetByte49 | 20 | L1 | room+0x49 = hi<<4 / lo. |
| 0x004A2050 | Profile_FixupRankRow | 98 | L1 | Profile row helper used by the D-code validator. |
| 0x004A2280 | Room_SeatMember | 160 | L2 | thiscall(room, memberIndex, seat), ret 8. If seat < memberCount: moves the member with queue position == seat out, assigns memberIndex to that queue position / seat (+0x34 byte, bit1/bit0 set by caller). Host only. |
| 0x004A2350 | Room_UnseatMember | 117 | L2 | thiscall(room, memberIndex), ret 4: moves member to the end of the queue (shifting +0x34 queue positions), clearing its seat. Host only. |
| 0x004A23D0 | RoomHostT20_CheckStart | 391 | L1 | Type 0x2x start check (no member in match on either slot). |
| 0x004A2560 | Slot_Init | 83 | L1 | thiscall(slot, v), ret 4: slot defaults (+0x40 = 0x64640101). |
| 0x004A25C0 | DCodeFetchTick | 210 | L1 | Already named. |
| 0x004A26A0 | Loader_Arm | 145 | L1 | Arms a D-code loader (cc = 1) with a steamId. |
| 0x004A2740 | Profile_SealWithSaveTime | 133 | L1 | Already documented in ProfileSync.md. |
| 0x004A2870 | Profile_PostLoadFixups | 235 | L1 | Profile rank-row fixups after load. |
| 0x004A2BB0 | Ranked_ClearEntryFlag | 27 | L1 | charSelect+0x16A8 = 0 + commit (ranked entry cancelled; task 4 event 2). |
| 0x004A2BD0 | Ranked_SetEntryFlag | 27 | L1 | charSelect+0x16A8 = 1 + commit (entering ranked menu, task 4 state 0x1E entry). |
| 0x004A2C20 | Ranked_EntryWasPaused | 37 | L1 | True if charSelect+0x16A8 is not 0/1 (entry paused/other mode). |
| 0x004A2C50 | Automatch_UiTick | 72 | L1 | fastcall(automatch): +0x2C/+0x30 counters; draws the automatch status (0x004A3440 when +0x28 == 2, 0x004A2CA0 when +0x28 == 1). |
| 0x004A2CA0 | Automatch_DrawSearching | 1941 | L1 | UI: 'AUTOMATCHMSG_FINDBATTLER' banner, rank icons (SkillRank_%02d). |
| 0x004A3440 | Automatch_DrawMatchFound | 1608 | L1 | UI: match-found popup: AUTOMATCHMSG_ASKBATTLE / FINDBATTLER / NON, connection quality LineS..LineD, both players via 0x004A3A90. |
| 0x004A3A90 | Automatch_DrawPlayerCard | 1462 | L1 | UI: one player card in the popup ('Ready' mark when that entry has the OK bit, rank icon, net colour hip). |
| 0x004A4050 | Settings_RankFilterLevel | 23 | L1 | fastcall(settings+0xF8): 6 -> 2, 5 -> 1, else 0 (minimum connection class). |
| 0x004A4070 | Settings_PingFilterLevel | 46 | L1 | fastcall(settings+0x100): 0xB..0xE -> 1..4, else 0. |
| 0x004A4110 | Ranked_JoinRow | 177 | L2 | stdcall(byte row), ret 4. Fills netServer join block (+0x3D8, 0x408 bytes cleared): own id, +0x745 = row, +0x744 = pad, +0x747 = app version (0x004A7850), +0x748 = catalog version (0x004A7500), +0x74C = 0, then 0x004A56B0 -> netServer vt+0x10 (Join); r == 0 -> Room state = 1. Results arrive as netMgr 0x10000 / 0x20000. |
| 0x004A41D0 | Ranked_CreateRoom | 619 | L2 | cdecl(), ret 0. Fills netServer create block (+0xD8, 0x1F8 bytes): own id, pad, rank value (+0x1D8), +0x1D4 = 2 (capacity), type flags +0x1E0 /= 0x110 (+0x1000000 if profile flag), +0x1E4 /= 0x20006302, ping/rank filters from settings (+0xF8, +0x100, +0xFC: 8 -> rank+-3, 9 -> rank..rank+10, else 0..99), versions; then 0x004A56E0 -> netServer vt+0x04 (Create); r == 0 -> Room state = 1. Result netMgr 0x400 / 0x800. |
| 0x004A4440 | Ranked_SearchRooms | 398 | L2 | cdecl(), ret 0. netServer result count +0xAE8 = 0, clears result index table, fills search block (+0x2D0, 0x108 bytes) with own id, pad, versions, rank range and ping filter, then 0x004A57D0 -> netServer vt+0x0C (Search). Result netMgr 0x4000 / 0x8000. |
| 0x004A45D0 | Automatch_Reset | 105 | L1 | Clears automatch fields 0..0x44 (not +0x1C/+0x24). Called on network exit and when entry state 3 lasts > 240 frames. |
| 0x004A46F0 | Ranked_ReturnFromMatch | 200 | L1 | thiscall(automatch, apply), ret 4: step = 0; restores char config and returns to task 1 (or 4 when entry flag == 1) with UI page 7/0. |
| 0x004A47C0 | Automatch_Tick | 2998 | L2 | fastcall(automatch = DAT_00CF7758), ret 0. Background ranked matchmaking ('RankedStep %d'), runs every frame from the dispatcher. See Flows: Ranked automatch. Timers from tmsenv (defaults: first wait 180 + rand%180 frames, interval 600 + rand%300, auto-cancel 1800 frames). |
| 0x004A5430 | NetServer_ClearResultIndex | 20 | L1 | fastcall(netServer): +0xAF4[0..49] = 0..49. |
| 0x004A54D0 | NetServer_PreallocResults | 50 | L1 | thiscall(netServer, n), ret 4: n times netServer vt+0x70 (allocate search result node) and pool it. |
| 0x004A5510 | NetServer_Poll58 | 8 | L1 | netServer vt+0x58(0) -> netMgr[1] 0x10/0x20. |
| 0x004A5520 | NetServer_ShutdownPoll | 5 | L1 | jmp netServer vt+0x7C: shutdown poll used by task 0xD/0xE state 0x8C (< 0 = pending). |
| 0x004A5530 | NetServer_PollInviteJoin | 13 | L1 | netServer vt+0x4C(+0x3D8) -> netMgr[0] 0x40000/0x80000. |
| 0x004A5540 | NetServer_PollJoin | 13 | L1 | netServer vt+0x48(+0x3D8) -> netMgr[0] 0x10000/0x20000. |
| 0x004A5550 | NetServer_PollCreate | 13 | L1 | netServer vt+0x3C(+0xD8) -> netMgr[0] 0x400/0x800. |
| 0x004A5560 | NetServer_PollDestroy | 13 | L1 | netServer vt+0x40(+0xD8) -> netMgr[0] 0x1000/0x2000. |
| 0x004A5570 | NetServer_PollLeave | 13 | L1 | netServer vt+0x50(+0x3D8) -> netMgr[0] 0x100000 ok / **0x20000** fail (same bit as join failure, see Suspected bugs). |
| 0x004A5580 | NetServer_Poll5C | 8 | L1 | netServer vt+0x5C(0) -> netMgr[1] 4/8. |
| 0x004A5590 | NetServer_PollMemberA | 9 | L1 | jmp netServer vt+0x68(i) -> netMgr[0] 0x1000000/0x2000000. |
| 0x004A55A0 | NetServer_PollMemberB | 9 | L1 | jmp netServer vt+0x6C(i) -> netMgr[0] 0x4000000/0x8000000. |
| 0x004A55B0 | NetServer_PollInit | 29 | L1 | netServer vt+0x38(+8); 0x6B -> netServer+0xAB0 = 9 (rating/age). -> netMgr[0] 0x100/0x200. |
| 0x004A55D0 | NetServer_Poll64 | 13 | L1 | netServer vt+0x64(+0x9E0) -> netMgr[1] 0x1000/0x2000 (room info update result, event 99). |
| 0x004A55E0 | NetServer_PollSearch | 13 | L1 | netServer vt+0x44(+0x2D0) -> netMgr[0] 0x4000/0x8000. |
| 0x004A55F0 | NetServer_Poll54 | 8 | L1 | netServer vt+0x54(0) -> netMgr[1] 1/2. |
| 0x004A5600 | NetServer_PollNameCheck | 34 | L1 | netServer vt+0x60(+0x7E0); 0xB4 -> +0xAB0 = 8 (session name rejected). -> netMgr[1] 0x100/0x200. |
| 0x004A5640 | NetServer_Shutdown | 5 | L1 | jmp netServer vt+0x78: leave network mode (task 0xD/0xE state 0x8C entry). |
| 0x004A5680 | NetServer_JoinLobbySlot | 48 | L1 | netServer vt+0x14(+0x3D8); 0 -> Room state 1. Lobby cabinet join (0x004A8AB0). |
| 0x004A56B0 | NetServer_Join | 48 | L2 | fastcall(netServer), ret 0: +0xAB0 = 0; r = vt+0x10(+0x3D8); r == 0 -> Room state (netData+0x22D10) = 1; returns r. |
| 0x004A56E0 | NetServer_Create | 48 | L2 | fastcall(netServer), ret 0: +0xAB0 = 0; r = vt+0x04(+0xD8); r == 0 -> Room state = 1; returns r. |
| 0x004A5770 | NetServer_DropMember | 9 | L1 | jmp netServer vt+0x34 (member index on stack): host drops a silent member. |
| 0x004A5780 | NetServer_Initialize | 34 | L1 | fastcall(netServer): +0xAB0 = 0; vt+0x00(+8) (start init); r == 0 -> netData reset. Task 2 state 0. |
| 0x004A57B0 | NetServer_RoomInfoUpdate | 23 | L1 | +0xAB0 = 0; vt+0x30(+0x9E0). |
| 0x004A57D0 | NetServer_Search | 23 | L1 | +0xAB0 = 0; vt+0x0C(+0x2D0). |
| 0x004A5800 | NetServer_NameCheck | 23 | L1 | +0xAB0 = 0; vt+0x2C(+0x7E0). |
| 0x004A5C90 | GAME_CNetworkTask_Ctor | 97 | L1 | fastcall(node): AA_TaskNode base + GAME_CNetworkTask vtable (0x008A9C4C). |
| 0x004A5D20 | GAME_CNetworkTask_Dtor | 94 | L1 | Scalar deleting destructor (vt slot 0). |
| 0x004A5D80 | NetServer_PushRoomInfoChange | 59 | L1 | thiscall(netServer, entry8), ret 4: appends to the +0xABC change list (+0xAB8 count). |
| 0x004A5DC0 | Room_HostSyncSettings | 175 | L1 | Host in a player room, when not in a match: pushes room setting changes (0x004B1240) to netServer. |
| 0x004A5E70 | Invite_ClearState | 47 | L1 | fastcall(task): +0xDC/+0xE0/+0xE4 = 0, close message window. Ends the invite sub-machine. |
| 0x004A6670 | Invite_Tick | 1679 | L2 | fastcall(task), ret 0. Steam invite / join-request sub-machine on task+0xE0, active only when task+0xE8 != 0 (set by the GameLobbyJoinRequested path). See Flows: Invite. |
| 0x004A6D60 | NetTask_DialogTick | 525 | L2 | fastcall(task), ret 0. Posts the dialog requested in +0xB8 (1 = close; else message table 0x009DE870[id] with up to 4 buttons +0xBC..+0xC8 from table 0x009DE9CC: 1 OK, 2 NG/back, 3 CreateNew, 4 EntryPause, 5 EntryCancel, 6 Consent, 7 Cancel), keeps it in +0xAC, writes the chosen button's value to +0xB0 (2 on B/back). +0xB4 = one-shot toast id (shown unless charSelect+0x2600 == 2). |
| 0x004A6F70 | NetTask_Update | 941 | L2 | fastcall(task = DAT_00CF7958), ret 0. The outer state machine. See Flows: Outer task dispatcher. |
| 0x004A7390 | NetTask_CloseDialog | 51 | L1 | fastcall(task): closes the active dialog (+0xAC, +0xB8 = 0). |
| 0x004A73D0 | GAME_CNetworkTask_Update | 134 | L2 | vt slot 1 of GAME_CNetworkTask, called by the task system each frame; ecx ignored, ret 0. Ticks the Steam networker (holder vt+0x18 -> AASTEAM_CNetworker vt+0x14 = uei + Steam pumps), lazily constructs DAT_00CF7958 (0x004A5860), then 0x004A6F70. |
| 0x004A7500 | Version_Catalog | 16 | L1 | Catalog/DLC version byte (FUN_0047C210) sent in join/create blocks. |
| 0x004A7850 | Version_App | 38 | L1 | Application version byte (FUN_0047C1E0) sent in join/create/search blocks; mismatch -> AppVerDisagr error on the host. |
| 0x004A7880 | RoomSettings_Rounds | 155 | L1 | Maps a room-settings menu value to rounds-to-win code. |
| 0x004A7990 | RoomSettings_Flag | 84 | L1 | Maps a menu value (0x48..) to room flag bits (0x10000000...). |
| 0x004A7A90 | RoomSettings_Type | 83 | L1 | Maps menu values to room type / rule. |
| 0x004A7AF0 | RoomSettings_Time | 76 | L1 | Maps a menu value (0x3E..) to round time. |
| 0x004A7BB0 | RoomSettings_Capacity | 122 | L1 | Maps a menu value to capacity (0x2D default). |
| 0x004A7C60 | MatchCtx_Reset | 180 | L1 | thiscall(ctx, mode), ret 4: clears the per-match context, +0x4C = -1. Mode 1 player, 2 spectator/replay. |
| 0x004A7D20 | Rematch_SomeoneWants | 59 | L1 | Both sides answered and at least one wants rematch, none declined. |
| 0x004A7D60 | Rematch_Declined | 45 | L1 | Either side answered 2 (decline). |
| 0x004A7D90 | Rematch_BothAccept | 45 | L1 | Both answered 0 (accept). |
| 0x004A7E10 | Automatch_IsIdle | 21 | L1 | fastcall(automatch): step (+8) is 0 or 1. |
| 0x004A7F00 | Match_CanRematch | 79 | L1 | Rematch possible: in match (+0x270), counters allow, and role 0/3/4. |
| 0x004A8910 | Invite_Join | 240 | L1 | Fills the join block (+0x3D8) for an invite (+0x747 version, +0x74C = 0xF) and calls netServer vt+0x18; 0 -> Room state 1. Result netMgr 0x40000 / 0x80000. |
| 0x004A89D0 | PlayerMatch_JoinRow | 221 | L1 | Fills join block for the selected player-room row (task+0x1EC) and 0x004A56B0 (Join). Result 0x10000/0x20000. |
| 0x004A8AB0 | Lobby_JoinCabinet | 217 | L1 | Fills join block from a lobby cabinet slot and 0x004A5680. |
| 0x004A8B90 | Room_SendInfoUpdate | 50 | L1 | Copies task+0x418 room info and 0x004A57B0 (event 99). |
| 0x004A8BD0 | Lobby_CreateCabinetRoom | 373 | L1 | Fills create block for a lobby cabinet match and 0x004A56E0. |
| 0x004A8D50 | PlayerMatch_CreateRoom | 1782 | L1 | Fills create block from the room-creation menu (rules via 0x004A7880/0x004A7A90/0x004A7BB0, 0x00519980/0x00519A10 options) and 0x004A56E0. 'NM_MyRoom_Download_Error' path. |
| 0x004A94C0 | PlayerMatch_Search | 516 | L1 | Fills the search block from the search menu and 0x004A57D0. |
| 0x004A96D0 | UploadOwnProfile | 162 | L1 | Already named. |
| 0x004A9780 | Settings_UploadOnlineBlock | 83 | L1 | Copies task+0x40C settings and 0x004A5800 (task 1 state 0x15). |
| 0x004A97E0 | PlayerMatch_NameCheck | 44 | L1 | Copies room name (task+0x10C) to netServer+0x7E0 and 0x004A5800 (task 6 state 0x44). |
| 0x004A9B60 | Match_SendRandomStage | 242 | L1 | Host picks stage/music and sends them (0x0049DE90). |
| 0x004AA700 | Sfx_MenuError | 67 | L1 | Plays 400_menu_error.wav. |
| 0x004AA7F0 | NetTask_SetUiPage | 16 | L1 | thiscall(task, page), ret 4: +0xD8 = page. |
| 0x004AA800 | Menu_ReturnToNetworkScene | 20 | L1 | charSelect+0x110 = 0x1B and scene change. |
| 0x004AA820 | Sfx_MatchFound | 104 | L1 | Plays 401_credit.wav twice + vibration request (match found / member joined). |
| 0x004AA890 | Task_ExitNetwork | 571 | L2 | fastcall(task), ret 0. Outer tasks 0xD (forced) and 0xE (normal). See Flows. |
| 0x004AAAD0 | Task9_InLobby | 3654 | L1 | fastcall(task): outer task 9, the 2D lobby (CSTEAMNetworkLobbyData) - cabinet seats, lobby chat, cabinet matches; states 0x6F..0x8D; error codes in +0x430 ('NetworkErr %d' only shown when catalog+0x200298 == 1). See Flows. |
| 0x004ABA50 | Task9_CabinetMatch | 1238 | L1 | Lobby cabinet match in-room handler (called by task 9), analogue of task 7's room logic. |
| 0x004ABF50 | Task8_EnterLobby | 1742 | L1 | fastcall(task): outer task 8: uei login/register, GetLobby list, choose lobby, join the Steam lobby (lobbyData vt+0x24, poll vt+0x2C). See Flows. |
| 0x004AC6C0 | Task6_PlayerMatchMenu | 1836 | L2 | fastcall(task), ret 0. Outer task 6: player match create / search / join. See Flows. |
| 0x004ACED0 | Task7_PlayerRoom | 3859 | L2 | fastcall(task), ret 0 (disasm.txt is misaligned at this address because of a preceding jump table; convention from the dispatcher call site and Ghidra). Outer task 7: inside a player room. See Flows. |
| 0x004ADE90 | Task7_RoomEvents | 1986 | L2 | fastcall(task), ret 0. Room UI events and match-start detection for task 7 (called every frame from state 0x51). See Flows. |
| 0x004AE6D0 | Task4_RankedMenu | 2590 | L2 | fastcall(task), ret 0. Outer task 4: ranked menu (entry, manual room list, joining). See Flows. |
| 0x004AF1A0 | Task5_RankedMatch | 2647 | L2 | fastcall(task), ret 0. Outer task 5: ranked match from 'both OK' to battle and back. See Flows. |
| 0x004AFC90 | Task2_NetworkEntry | 1173 | L2 | fastcall(task), ret 0. Outer task 2: netServer init + uei login/register. See Flows. |
| 0x004B01B0 | Task1_NetworkTop | 1840 | L2 | fastcall(task), ret 0 (alloca probe preserves ecx). Outer task 1: network top menu. See Flows. |
| 0x004B0970 | ProfileSyncTask | 1975 | L1 | Already named (outer task 3), fully documented in docs/Decompilation/ProfileSync.md; replaced by the mod. |
| 0x004B1240 | Room_BuildSettingsChange | 871 | L1 | Builds the room settings change list from the menu (0x004A7880.. mappers). |
| 0x004B4280 | Obj_Get464 | 7 | L1 | Returns obj+0x464 (replay/download busy check in task 7). |
| 0x004B8CE0 | Transfer_Poll | 16 | L1 | Already covered by DCodeFetchTick research (CUMSTask poll). |
| 0x004B8D30 | OwnProfileUploadPoll | 31 | L1 | Already named. |
| 0x004B8E00 | DlcQuery_Request | 176 | L1 | DLC/catalog query internals. |
| 0x004B8EB0 | Transfer_Submit | 170 | L1 | Submits a profile download (CUMSTask). |
| 0x004B9070 | DlcQueryPoll | 104 | L1 | Already named (ADDR_DlcQueryPoll). On completion loads the catalog (tmsenv.info). |
| 0x004B9100 | DlcQueryStart | 126 | L1 | Already named. |
| 0x004B9210 | Transfer_SubmitUpload | 141 | L1 | Submits the own profile upload. |
| 0x004BDFF0 | Rank_Pack | 20 | L1 | library: swaps two shorts into a packed rank value. |
| 0x004BE010 | Rank_RowDefault | 30 | L1 | library: rank row default (LP 0x7FFF). |
| 0x004BE030 | Rank_IsUnranked | 9 | L1 | row+10 == 0. |
| 0x004BE150 | Rank_ToDisplayValue | 110 | L1 | library: rank math. |
| 0x004BE760 | Rank_Recursive | 116 | L1 | library: rank math helper. |
| 0x004BF4F0 | Sound_PlayVoice | 184 | L1 | Plays a system voice (VS screen). |
| 0x004C0CA0 | Scene_RequestBattle | 126 | L1 | Requests the battle scene (mode 2, 6). |
| 0x004C1060 | Scene_ModeMap | 227 | L1 | Maps scene request ids. |
| 0x004C3280 | VoiceChat_Base | 9 | L1 | Voice chat manager base ctor. |
| 0x004C32D0 | VoiceChat_Get | 208 | L1 | Lazily creates the voice chat manager singleton DAT_00EC1580; ticked first thing in NetMgr_FrameUpdate. |
| 0x004E9220 | CharSelect_RandomizeHelper | 204 | L1 | Character select helper used for random stage/char. |
| 0x00511380 | Obj_Get_D4 | 7 | L1 | getter +0xD4. |
| 0x005183C0 | Obj_Get_B4 | 7 | L1 | getter +0xB4. |
| 0x00519980 | Option_MapA | 129 | L1 | Room creation option mapper. |
| 0x00519A10 | Option_MapB | 404 | L1 | Room creation option mapper. |
| 0x00550560 | Scene_IsTransitioning | 50 | L1 | True while a scene change is pending; tasks wait for it before showing errors / leaving. |
| 0x0060A1B0 | CharSelect_Random | 11 | L1 | thunk to 0x004E9220. |
| 0x0060A1C0 | CharSelect_RandomChar | 89 | L1 | Random character id (0x18 default). |
| 0x0062B840 | Obj_Get_54 | 4 | L1 | getter +0x54. |
| 0x0062B850 | Obj_Get_50 | 4 | L1 | getter +0x50. |
| 0x00647C60 | Scene_BreakIntoMain | 53 | L1 | Requests the 'BreakIntoMain' scene (VS screen start, task 5 state 0x31). |
| 0x00647CC0 | Scene_ReturnToMenu | 22 | L1 | Scene change back to the menu (task 0xD, task 5 0x38). |
| 0x00648210 | ProfileSync_Helper | 2201 | L1 | Large helper of the profile sync task (see ProfileSync.md). |
| 0x0068A7E0 | MsgWindow_Close | 115 | L1 | Closes a message window. |
| 0x006910B0 | Toast_Show | 614 | L1 | Shows a notice/toast with a message id (used by +0xB4 and 'InviteSelectPad'). |
| 0x00691F30 | Toast_Draw | 256 | L1 | Draws the toast (bow_1.hip). |
| 0x00695AF0 | MsgWindow_Name | 14 | L1 | Returns the message window name string. |
| 0x006973D0 | MsgWindow_IsIdle | 99 | L1 | Message window idle check. |
| 0x00698220 | MsgWindow_Update | 80 | L1 | Message window per-frame update. |
| 0x0069C3B0 | Battle_SetupOnline | 729 | L1 | Fills battle setup from netData (+0x22CC8) before an online battle. |
| 0x0069C930 | Battle_CopyInputBuffer | 216 | L1 | Copies replay input buffers. |
| 0x0069CA10 | Battle_CopyInputs | 16 | L1 | thunk to 0x0069C930. |
| 0x0069D350 | Battle_Ref | 29 | L1 | thunk to FUN_0069D2D0. |
| 0x0069D380 | Battle_ClearReplayCounters | 21 | L1 | +0x64ED8/+0x64EDC = 0. |
| 0x0069DE70 | Battle_ApplyMatchSettings | 671 | L1 | Applies match settings (rounds/time) for online battle. |
| 0x006A1C40 | Stub_Noop | 15 | L1 | Empty function. |
| 0x006AF620 | Localize_AppendText | 193 | L1 | Localization append (DLC append idlist). |
| 0x006AF6F0 | Localize_LoadAppendIdList | 135 | L1 | Loads '_appendidlist.txt' from the catalog after the DLC query (task 1 state 0x1C). |
| 0x006B2070 | Localize_Helper | 240 | L1 | Localization helper. |
| 0x006B2FE0 | Localize_AddId | 517 | L1 | Localization id insert ('Localize ID overflow'). |
| 0x006C3D40 | SaveSeq_State | 95 | L1 | Save sequence state getter. |
| 0x006C4130 | SaveSeq_Start | 185 | L1 | Starts a save sequence ('SaveSequence'). |
| 0x006D3B70 | Training_ResetOnline | 664 | L1 | Training settings reset for online (TRI_*). |
| 0x006D3EE0 | Training_ResetOnline2 | 471 | L1 | Training settings reset (TRI_ReadHelp). |
| 0x006D4460 | Training_ClearA | 26 | L1 | Training online flags clear. |
| 0x006D4480 | Training_ClearB | 31 | L1 | Training online flags clear. |
| 0x006D70B0 | Training_SetDefault | 399 | L1 | Training default value setter. |
| 0x00702750 | Obj_Get_E8 | 7 | L1 | getter +0xE8 (replay busy). |
| 0x0070E410 | Obj_Get_F0 | 7 | L1 | getter +0xF0 (type 0x40 room playback flag). |
| 0x0073C3B0 | Lobby_SortList | 247 | L1 | Sorts lobby list entries (qsort) for task 9. |
| 0x00778FE0 | File_Exists | 51 | L1 | Already documented in ProfileSync.md. |
| 0x00779020 | BbdcDat_Read | 395 | L1 | Already documented in ProfileSync.md. |
| 0x00779390 | BbdcDat_Buffer | 6 | L1 | Returns &DAT_0182A764. |
| 0x007836A0 | GGPO_PeerDisconnected | 46 | L1 | fastcall(session): true when the netcode session reports the peer gone (count at +8->+0x210 > 5 or flag +0x112). Used by tasks 5/7/9 as 'LeaveBattler'. Part E edge. |

## Data

### Singletons
| Address | What | Getter |
|---|---|---|
| 0x00CF7958 | Network task object (this part's state). The mod's `RoomSettingsStatic` (0xCF7A64) is task+0x10C | 0x004A77E0 (ctor 0x004A5860, guard DAT_00CF7D98) |
| 0x00CF7758 | Ranked automatch object | 0x004A40A0 (reset 0x004A45D0) |
| 0x00CAD000 | netMgr: per-frame event flags + keepalive caches | 0x0049A400 (ctor 0x00496D30) |
| 0x00CAD0C0 | netData: own user, match data, **Room at +0x22D10** | 0x004A0FE0 (ctor 0x004A06D0) |
| 0x00A62730 | GAMESTEAM_CNetworkServer ("netServer") | 0x0046E8C0; also holder vt+0x1C |
| DAT_00C97E3C | Network interface holder: vt+0x18 -> AASTEAM_CNetworker (Steam/uei pump; vt+0x2C(pad) = ISteamUser::BLoggedOn ? 0 : 100, vt+0x14 tick, vt+0x50/+0x58 invite state), vt+0x1C -> netServer, vt+0x20 -> text-input object | 0x00486070 |
| thunk 0x004A29F0 -> 0x0046A820 | CSTEAMNetworkLobbyData ("lobbyData", Steam lobby layer used by the 2D lobby and invites) | - |
| DAT_00A5A050 / latch DAT_00A5A070 | uei work manager / login latch | FUN_00427CD0 |
| DAT_00CF77A8 | TUS latch (set when uei login fails: `LOBBY_CannotLogin`) | - |
| catalog (FUN_0047C8C0) +0x200000.. | tmsenv.info values (see 0x0047B840); +0x20026C copied to netServer+0x1170 (ping timeout) | - |

### Network task object (DAT_00CF7958)
| Off | Meaning |
|---|---|
| +0x00 | outer task id (0,1..9,0xD,0xE) |
| +0x04 | inner state of the current task |
| +0x08 | "first frame in this state" (set by the dispatcher from +0x0C) |
| +0x0C | state-changed request (set by every transition) |
| +0x10 | previous outer task |
| +0x14 / +0x18 | generic frame counters (timeouts, minimum display time) |
| +0x1C | UI refresh request / automatch suppression mirror |
| +0x20 | 3000-frame profile upload timer (tasks 1, 3, 9) |
| +0x24..+0xA0 | event ring, 32 dwords (push 0x004A88D0 from UI code, pop 0x004A8150) |
| +0xA4 / +0xA8 | event ring read / write index |
| +0xAC | active dialog message id |
| +0xB0 | dialog result (button value; 2 = back) |
| +0xB4 | one-shot toast message id |
| +0xB8 | dialog request message id (1 = close) |
| +0xBC..+0xC8 | dialog button label ids |
| +0xCC | room/session error message id (task 5/7) |
| +0xD0 | in-match error message id (task 5/7) |
| +0xD8 | UI page id |
| +0xDC/+0xE0/+0xE4/+0xE8/+0xEC | invite: already-online flag / invite sub-state / prev / pending / pad chosen |
| +0x1EC | selected search-result row |
| +0x270 | in-match flag; +0x274 match aborted; +0x278 role (0 player, 1 type 0x30, 2 spectator/replay) |
| +0x280/+0x284 | kick target steamId |
| +0x294 | ranked character slot (-1 forces the settings page, state 0x22) |
| +0x298..+0x2A8 | char-select parameters sent with msg 0x13 |
| +0x2B4/+0x2B8 | selected 2D-lobby id; +0x2BC..+0x2D0 lobby join params; +0x2D8 cabinet id |
| +0x2E0 / +0x328 | submitted chat/comment text / text-input buffer (0x40 bytes each); +0x368 text submitted; +0x438 text input open |
| +0x36C | ranked "cancel" dialog shown flag (task 4 state 0x2E) |
| +0x380 | uploader object (task 1 states 0x12-0x14) |
| +0x410 | "show NTER_Signout toast on exit" flag (set in task 1 state 0) |
| +0x420 | cabinet join retry budget; +0x430 lobby error code ('NetworkErr %d') |

### netMgr flags (DAT_00CAD000), rebuilt every frame by 0x0049AEA0
Each async netServer request is polled every frame. The poll returns -100 while pending, 0 on success and 100 or more on failure. The result shows up as one bit for exactly one frame. Any state that is not looking at that frame misses it.

| word | bit | meaning (poll) |
|---|---|---|
| [0] | 0x100 / 0x200 | Init ok / failed (vt+0x38; 0x6B -> +0xAB0 = 9 rating) |
| [0] | 0x400 / 0x800 | Create session ok / failed (vt+0x3C) |
| [0] | 0x1000 / 0x2000 | Destroy (host leave) ok / failed (vt+0x40) |
| [0] | 0x4000 / 0x8000 | Search ok / failed (vt+0x44) |
| [0] | 0x10000 / 0x20000 | Join ok / failed (vt+0x48) - **0x20000 is also set by a failed client leave (vt+0x50)** |
| [0] | 0x40000 / 0x80000 | Invite join ok / failed (vt+0x4C) |
| [0] | 0x100000 | Client leave ok (vt+0x50) |
| [0] | 0x1000000.. 0x8000000 | per-member ops (vt+0x68/0x6C) |
| [0] | 0x10000000 | host dropped a silent member (also client 'kicked' path) |
| [0] | 0x20000000 | client: host silent for 301 frames, left the session |
| [0] | 0x40000000 | room changed, broadcast (host) / send status (client) |
| [0] | 0x80000000 | heartbeat due |
| [1] | 1/2, 4/8, 0x10/0x20 | vt+0x54 / vt+0x5C / vt+0x58 results |
| [1] | 0x100 / 0x200 | name check ok / failed (vt+0x60; 0xB4 -> +0xAB0 = 8) |
| [1] | 0x1000 / 0x2000 | room info update ok / failed (vt+0x64) |
| [1] | 0x8000000 | netServer tick (vt+0x94) >= 100: fatal; the dispatcher forces task 0xD |
| [1] | 0x10000000 | msg 0xB "room closed" received while in session (we left) |
| [1] | 0x20000000 | spectator/match stall (> 0x21C frames) |

netServer+0xAB0 = last error code (2/3/4 member full, 6 app version, 7 catalog version, 8 session name, 9 rating/age, 10 silent cancel). netServer+0xAAC = in-session flag (set by OnCreated/OnJoined; gates keepalive and snapshot handling). netServer+0xAE8 = search result count; +0xAEC/+0xAF0 = result list.

### Room (netData+0x22D10) and member entries (as used here; complements src/Game/Room/Room.h)
- Room+0x00 state: 0 none, 1 create/join requested, 2 in room, 3 leaving. **`IsInRoom` (0x0049A920) = state != 0 is what every "wait until left" loop polls.**
- Room+0x30 type: high nibble class (0x10 ranked + player rooms, 0x20, 0x30 training, 0x40 replay, 0x50 lobby slot), bits 0xF00 = rotation rule, 0x4000000 = text allowed, 0x8000000 = opt-out allowed. Room+0x34 high nibble = win limit; +0x36 idle auto-start seconds; +0x38 member count; +0x3A flags (0x10 in ranked match, 0x20 settings being edited); +0x3C **revision** (the mod calls it secondsElapsed; it is incremented per host broadcast and a client ignores snapshots older than it).
- Room+0x508 (netData+0x23218) = **my member entry**; +0x50C = host entry; +0x534/+0x538 = side 0/1 player entries; +0x53C match counter.
- Member entry (0x98): +2 status (2 = present); +4 steamId; +0x2C flags: bit0 side, bit1 seated, bit2 seat-0 starter, **bit3 READY/OK**, 0x800 wants seat, **0x1000 in match**, 0x20000 joined, 0x100000 pad ok, 0x400000 rotate out, 0x800000 opt out, +0x2E bit0 host; +0x30 matchId; +0x34 queue/seat position (the mod's matchPlayerIndex); +0x35 win streak; +0x50 local flags; **+0x5B match stage** (3 char sent, 4 stage, 5 music, 0xA battle, 0xC in battle, 0xD rematch yes, 0xE rematch no, 0xF/0x12 returned, 0x10 error, 0x11 finished); **+0x70 silence counter**; +0x73 opponent-data flag; +0x78 peer address (send target).

### Packet types handled here (short at msg+2; first dword of a send = size | type<<16)
1 member status, 2 room snapshot (host), 3/4 match settings, 5 opponent ready-for-battle, 7 rotate/opt-out (0x7000C), 8 READY/OK (0x8000A), 9 counters, 0xB room closed, 0xF slot assign, 0x12-0x17 in-match data (0x13 char select 0x130018, 0x16 stage/music 0x16000C), 0x18 spectator info, 0x19, 0x1D-0x23, 0x26/0x27, 0x2B-0x2E lobby (lobbyData vt+0x7C..+0x88), 0x2F-0x31, 0x36 client leave notice (x3), 0x37 host destroy notice (x3); others -> netServer vt+0x90.

### Message table (dialog id -> string), base 0x009DE870
3 NTER_Unconnected, 4 NTER_CommonError, 5 NTER_InitError, 6 NTER_RatingAge, 7 NTER_VerifyError, 8 RankMatch_NotMatching, 9 RankMatch_CreateError, 0xA RankMatch_SearchError, 0xB RankMatch_SearchSelectError, 0xC RankMatch_ConnectError, 0xD RankMatch_DeleteError, 0xE RankMatch_MemberFull, 0xF RankMatch_Started, 0x10 RankMatch_LostBattler, 0x11 RankMatch_LeaveBattler, 0x12 NEER_RankMatch_SettingDesable, 0x13 PlayerMatch_SessionNameError, 0x14 PlayerMatch_CreateError, 0x15 PlayerMatch_SearchError, 0x16 PlayerMatch_SearchSelectError, 0x17 PlayerMatch_ConnectError, 0x18 PlayerMatch_MemberFull, 0x19 PlayerMatch_Started, 0x1A PlayerMatch_BreakUp, 0x1B PlayerMatch_HostLost, 0x1C PlayerMatch_Kicked, 0x1D PlayerMatch_LeaveBattler, 0x1E PlayerMatch_SpectateOver, 0x1F PlayerMatch_FinishedBattle, 0x20 PlayerMatch_AppVerDisagrError, 0x21 PlayerMatch_CatalogVerUnError, 0x22 TeamBtlContinueError, 0x23 ReplayLoadError, 0x24 RPT2_ReplayVersionError, 0x25 LeaveReplayOwner, 0x26 ReturnBattler, 0x27 ReturnReplayOwner, 0x28 ReturnHost, 0x29 LobbyMatch_SlotJoinError, 0x2A ForceSlotOut, 0x2B LobbyMatch_LostBattler, 0x2C LobbyMatch_LobbyDestroy, 0x2D LobbyMatch_ForceReturn, 0x2E NEER_P2PBattle_Desable, 0x2F NTER_Signout, 0x30-0x34 NTTSK_UpdateDCodeAsk*, 0x35 PMRI_RoomExitYesNo, 0x36 PMRI_RoomReleaseYesNo, 0x37 PMRI_AskKick, 0x38 NETTSK_RankAskEntry, 0x39 RankAskEntryCancel, 0x3A RankAskMatchingCancel, 0x3B RankAskReturnMode, 0x3C RankAskReturnWarning, 0x3D-0x3F NETTSK_LobbyAsk*, 0x40-0x43 NETTSK_MainAsk_Entry*, toasts 0x44 Initializing, 0x45 UpdateDCodeDown, 0x46 UpdateDCodeUp, 0x47 Synchronizing, 0x48 Creating, 0x49 Searching, 0x4A Connecting, 0x4B Disconnecting, 0x4C Checking, 0x4D ReplayLoading, 0x4E LobbySlotJoin, 0x4F-0x51 NETWORK_APPEND_*, 0x52 LOBBY_LobbyLogin, 0x53 LOBBY_Register, 0x54 LOBBY_LobbyGet, 0x55 LOBBY_CannotGetLobby. Buttons (0x009DE9CC): 1 MSGW_OK, 2 MSGW_NG, 3 SelectMsgCreateNew, 4 SelectMsgEntryPause, 5 SelectMsgEntryCancel, 6 MSGW_CENSENT, 7 MSGW_CANCEL.

### Automatch object (DAT_00CF7758)
+0 entry state (0 none, 1 entered, 2 paused, 3 cancelling), +4 frame counter, **+8 step ("RankedStep")**, +0xC previous step, +0x10 active this frame, +0x14 timer, +0x18 "allowed" (set each frame by the game mode the player waits in), +0x1C "suppressed" (set each frame by network menus/tasks), +0x20 popup visible, +0x24 auto-accept (settings+0xF4 == 3), +0x28 popup sub-state, +0x34 input lock (30 f), +0x38 auto-cancel timer, +0x3C left flag, +0x40 "I joined someone" flag, +0x44 chosen result row. Timers (frames): first wait `RankFirstWaitMin*60 + rand % (RankFirstWaitRange*60)` (default 180 + rand%180), interval `RankIntervalWaitMin*60 + rand % (Range*60)` (default 600 + rand%300), auto-cancel `RankAutoCancel*60` (default 1800). Server values apply only when tmsenv `IsSetRankMatchParam` != 0.

## Flows
All counts are frames (the game runs at 60 fps, so 60 frames = 1 s). "Entry" means the first frame in a state (+0x08 set). Every transition clears the request block with `NetTaskClearRequests` (0x004A9D20), which also **drops all queued UI events**.

### Outer task dispatcher (0x004A6F70, every frame)
1. If task is not 0, 2, 0xD or 0xE: `networker->vt+0x2C(pad)` (ISteamUser::BLoggedOn) >= 100 sends task 1 to **0xE** and any other task to **0xD**. Separately, netMgr[1] & 0x8000000 (netServer fatal) sends the task to **0xD**.
2. `Invite_Tick` (0x004A6670).
3. Task 0 (idle): pop one event. Event 5 enters network mode (task 2). If still idle: +0x1C = 1, +0x410 = 0, then skip to step 8.
4. Unless task is 0xD/0xE: `NetMgr_FrameUpdate` (0x0049AEA0).
5. Copy +0x0C into +0x08 (entry flag). Run the task function. While an invite is being processed (+0xE0 != 0), tasks 4 and 6 are not run.
6. `Automatch_Tick`, then `Automatch_UiTick`. If automatch step == 10 and task != 5, switch to **task 5** (ranked match found by the background search).
7. For tasks other than 0, 2, 3 and 0xE: `DCodeFetchPump`.
8. Text-input results: chat/comment submission through lobbyData vt+0x6C (task 9) or netServer vt+0xC4 (task 7); event 10 in task 1 state 1. Then lobbyData vt+0x74 tick and `NetTask_DialogTick`.

### Task 2 - network entry (0x004AFC90)
- **0**: reset all net data (0x004A0C50) and `NetServer_Initialize` (vt+0x00). If it fails synchronously, go to 2.
- **3**: toast Initializing. Wait for netMgr 0x100 (go to 100) or 0x200 (+0xAB0 == 9 goes to 4, otherwise 2). **No timeout here.** The only exits are the Steam BLoggedOn check and the netServer result.
- **100**: uei idle, then uei Login. Waits at least 60 frames, then polls `Uei_PollLogin`, then shows the toast for 16 more frames.
  - 0: go to **task 3** (profile sync).
  - 100: `LOBBY_CannotLogin` message, TUS latch DAT_00CF77A8 = 1, go to **task 1** (online without uei: no D-codes, no saves).
  - 0x6E: go to 0x65.
- **0x65**: `LOBBY_RegistConfirm` yes/no. Yes goes to 0x66, no goes to task 1.
- **0x66**: uei CreateUser. 0 goes back to 100, 100 shows `LOBBY_CannotRegister` and goes to task 1.
- **2 / 4**: dialog `NTER_InitError` (5) or `NTER_RatingAge` (6) with OK, then **task 0xE**.

### Task 3 - profile sync
Documented and replaced: docs/Decompilation/ProfileSync.md. It ends in task 1.

### Task 1 - network top menu (0x004B01B0)
- **0**: netMgr init(1), +0x410 = 1, go to 1.
- **1**: pop an event. 6 goes to **task 4** (ranked), 7 to **task 6** (player match), 8 to **task 8** (lobby). The other events start sub-flows:
  - 10 goes to 0x15: settings upload (0x004A9780). netMgr[1] 0x100 copies settings back. 0x200 with error 8 shows dialog 0x16 (VerifyError 7), otherwise dialog 0x17 (CommonError 4).
  - 0xB goes to 0x11: profile upload, 3000-frame budget on +0x20.
  - 0xC/0xD/0xE go to 0x18/0x19/0x1A: ranked entry cancel/pause dialogs 0x40-0x43, which set automatch +0 to 3/1/2.
  - 0xF/0x10/0x11 go to 0x12/0x14/0x13: uploader object +0x380.
  - 0x13 goes to 0x1C: DLC/catalog query (0x004B9100/0x004B9070). This loads tmsenv.info and copies the ping timeout to netServer+0x1170.

### Task 4 - ranked menu (0x004AE6D0)
Locals computed every frame: `notInRoom = !IsInRoom()`, `fewMembers = Room.memberCount < 2`, `full = memberCount >= 2`.
- **0**: netMgr init(2,0). If +0x294 < 0 go to 0x22 (choose character), otherwise 0x1E.
- **0x1E (ranked top)**: entry sets charSelect+0x16A8 = 1 and UI page 7. Events:
  - 2: clear the flag, go to task 1.
  - 0x14: automatch idle goes to 0x1F, otherwise 0x20.
  - 0x15: go to 0x24 (manual room list).
  - 0x17: idle goes to 0x23, otherwise 0x21.
  - 0x18: idle goes to 0x22, otherwise 0x21.
- **0x1F**: dialog `RankAskEntry` (0x38). OK sets automatch entry = 1 and toast `NEWK_RankMatchEntry`; the background search starts.
- **0x20**: dialog `RankAskEntryCancel` (0x39). OK sets automatch entry = 3.
- **0x21**: dialog `SettingDesable` (0x12), then back. **0x22/0x23**: settings pages (event 1 returns).
- **0x24**: UI page 0xF. Waits until the automatch step is 0 or 1, then goes to 0x26. If the automatch is wedged in another step this waits forever.
- **0x26 (search)**: entry runs `Ranked_SearchRooms` (vt+0x0C). netMgr 0x4000 goes to 0x27, 0x8000 to 0x29. Toast Searching.
- **0x27 (room list)**:
  - Entry: +0x14 = 0, +0x18 = 300, UI page 10.
  - Each frame: +0x18 counts down. Once it is below 1 and we are not in a room, **`Ranked_CreateRoom`** runs, so we also host a ranked room while browsing.
  - Every 600 frames: netServer vt+0xAC (refresh the list). The countdown only advances while vt+0xB0 reports not busy.
  - Event 2 goes to 0x2A, 0x16 to 0x26.
  - Event 1 (row clicked): look up the result node for row +0x1EC. If the room has a rank or connection filter (node vt+0x5C / vt+0x58) and the measured RTT (node vt+0x30) is below 0, show toast `RMSR_CheckingRTT` and stay. If `Ping_ToClass(rtt)` is below the room's required class, go to **0x28**. Otherwise go to **0x2B**.
- **0x28**: dialog `RankMatch_SearchSelectError` (0xB), then back to 0x27.
- **0x29**: `Room_Leave` and dialog `RankMatch_SearchError` (0xA), then 0x2A.
- **0x2A / 0x2F / 0x30**: call `Room_Leave` every frame until not in a room, then 0x1E. In 0x2A, after 30 frames the toast is Synchronizing.
- **0x2B**: entry UI page 0xF. Call `Room_Leave` (our own hosted room) every frame until not in a room, then 0x2C.
- **0x2C (join)**: entry runs `Ranked_JoinRow(+0x1EC)` (vt+0x10, Room state 1). netMgr 0x10000 goes to 0x2E, 0x20000 to 0x2D. Toast Connecting. **No timeout of its own.**
- **0x2D**: dialog `RankMatch_ConnectError` (0xC), then `Room_Leave` once and 0x1E.
- **0x2E (matched: popup / waiting for OK)**: entry UI page 0xF.
  - Each frame read my entry's flags: `ok = bit3`, `go = 0x1000`.
  - Button 3 with !ok: `Room_SendReady(1)`.
  - Button 2: if ok, `Room_SendReady(0)` (un-ready). Otherwise dialog `RankAskMatchingCancel` (0x3A) and +0x36C = 1.
  - `go`: **task 5**.
  - `notInRoom`, or the cancel dialog answered OK, or `fewMembers`: `Room_Leave`. Toast `RankMatchLeaveMyself` (only when the user confirmed the cancel), otherwise `RankMatchLeaveEnemy`, then 0x30/0x2F.
  - **No timeout: if the host never sets 0x1000 and nobody leaves, this state lasts forever.**
- **Post-switch rule**: in states 0x26/0x27/0x28, `memberCount >= 2` (someone joined *our* hosted room) jumps to **0x2E**. This is how the host side reaches "match found".

### Ranked automatch, background (0x004A47C0, every frame, object DAT_00CF7758)
`active` requires entry state 1, the "allowed" flag (+0x18) set by the current game mode, no suppression (+0x1C) from network menus, no save in progress, and no pending invite. Both flags are cleared every tick.
- **0 idle**: if active, go to 2. If entry == 3 for more than 240 frames, reset.
- **1**: like 0, but active goes to 3.
- **2 first wait**: timer = FirstWaitMin + rand%Range. Then 3.
- **3 host**: entry runs `Ranked_CreateRoom` (fail goes to 0xB). netMgr 0x400 goes to 4, 0x800 to 0xB.
- **4 interval wait**: timer = IntervalWaitMin + rand%Range. Then 5.
- **5 search**: entry runs `Ranked_SearchRooms`. 0x4000 goes to 6, 0x8000 to 0xB.
- **6 choose**:
  - Entry: netServer vt+0xAC (refresh). Then wait until vt+0xB0 is not busy.
  - For every result (count netServer[0x2BA]), skip rooms whose connection or rank filter excludes us, and rooms we filter out by ping class.
  - Build a weighted list: one ticket, plus one per ping class above 1, plus up to three more for rank distance < 3 / < 2 / < 1. Pick at random.
  - Nothing picked goes back to 4. A pick, while nobody has joined our own room, goes to 7 with +0x40 = 1.
- **7**: `Room_Leave` every frame until not in a room (drops our own hosted room), then 8.
- **8 join**: entry runs `Ranked_JoinRow(pick)` (fail goes to 0xB). 0x10000 goes to 9, 0x20000 to 0xB.
- **9 match found (popup)**:
  - Entry: input lock 30 frames, auto-cancel = RankAutoCancel (1800). Auto-accept users get sub-state 3. Others get sub-state 1 and the match-found sound.
  - Per frame:
    - **Leave** when any of these holds: (auto-accept and not active); not in a room; the popup's "cancel" answered yes; memberCount < 2; entry == 3; blocked; **auto-cancel expired**. Leaving runs `Room_Leave`, sets step 0xC, and shows a toast unless auto-accept: `RankMatchLeaveMyself` if the user cancelled, otherwise `RankMatchLeaveEnemy` (including the 30 s timeout).
    - My entry has 0x1000: step **10**, and charSelect+0x2558 = 0xC (scene).
    - Sub-state 1: button 5 shows the popup (sub-state 2).
    - Sub-state 2: button 3 sends `Room_SendReady(1)`. Button 2 un-readies if already OK, otherwise asks `RankMatchAskCancel`.
    - Sub-state 3: auto `Room_SendReady(1)`.
- **10**: match accepted. The dispatcher switches to task 5.
- **0xB / 0xC**: `Room_Leave` every frame until not in a room. Then step 0 (0xC, or entry 0/3), otherwise step 1.
- **Global rules after the switch**:
  - Not active (or left) and step not in {0, 1, 9, 10, 0xB, 0xC}: step 0xB.
  - Our own room has 2 or more members and we did not join someone (+0x40 == 0), in any step other than those: step **9** (host side match found).

### Room host logic (0x00497D40) - when a match starts
A match starts when both seated members (seat 0/1) have bit1 and **bit3**, neither is already in a match, the room has 2 or more members, and settings are not being edited.

The host then sets 0x1000 and matchId on every present member and broadcasts the room (msg 2, 0x0049DFF0). Clients see 0x1000 on their own entry in the next snapshot, which moves them to task 5 (ranked) or task 7 state 0x55 (player room).

Nothing validates that the two players can reach each other. Validation happens only afterwards, through timeouts (see Failure paths).

### Keepalive / liveness (0x00496FF0 host, 0x00497320 client, 0x00499E90 refresh)
- Every received packet resets the sender entry's +0x70.
- **Host**: own counter above 60 sends a status broadcast and heartbeat. Each other present member's counter above 300 calls `netServer vt+0x34(member)` (drop) and raises 0x10000000.
- **Client**: own counter above 60 sends a heartbeat. If the host counter reaches 301, it leaves (vt+0x1C) and raises 0x20000000.
- All of this runs only while netServer+0xAAC != 0. A spectator or match that stalls past 0x21C frames of netData+0x22C9A (stage 0xC) raises netMgr[1] 0x20000000.

### Task 5 - ranked match (0x004AF1A0)
Error sampling every frame:
- +0xCC = 0x10 (`RankMatch_LostBattler`) if netMgr[0] 0x20000000 (host silent) or not in a room.
- +0xCC = 4 (CommonError) if netData+0x22CC4 (pre-battle sync wait counter, incremented in FUN_0056BCB0) > 1200.
- +0xD0 = 0x10 if in a match and the opponent is gone (`Room_OpponentPresent(1)` == 0) or `GGPO_PeerDisconnected`.

The errors are only acted on in states 0x33-0x35 and 0x39-0x3C, which go to 0x3D. **States 0x36 and 0x37 ignore them.**

States:
- **0**: clear match state, Room+0x3A |= 0x10, go to 0x31.
- **0x31**: VS screen (`ns072.wav`, BreakIntoMain), 60 frames.
- **0x32**: charSelect+0x2558 = 6 (char select scene). When it returns 0: prepare battle, role 3, +0x270 = 1, go to 0x33.
- **0x33**: entry sends msg 0x13 (my char) and stage 3. Wait until both char-select flags (netData+0x6A1C+0x4C and +0x5C) are set.
- **0x34**: host picks the stage (msg 0x16), stage 4. Wait for netData+0x6A1C[0].
- **0x35**: host picks the music, stage 5. Wait for [0x4A], then battle setup.
- **0x36**: wait until the opponent D-code is ready (`FUN_0049D560`) or `FUN_0049A7F0`. **Timeout 601 frames, after which it proceeds anyway.**
- **0x37**: disconnect-penalty pre-charge (0x004A0DC0) and `UploadOwnProfile`. Poll until done. The intended 3000-frame budget is broken (bug 2).
- **0x38**: request the battle scene, stage 10, go to 0x39.
- **0x39 (battle/after)**: opponent stage 0xE (declined) goes to 0x3F after the scene. Error goes to 0x3D. Rematch possible and opponent 0xD goes to 0x3A.
- **0x3A-0x3C**: rematch handshake (`Rematch_*` predicates), resend char select, back to 0x39.
- **0x3D/0x3E**: wait for the scene, then show the +0xCC or +0xD0 dialog. Stage 0x10.
- **0x3F**: `Room_Leave` **once on entry**, then wait until not in a room (**no timeout**).
- **0x40**: if this came from the automatch (step 10): if the entry was paused, ask `RankAskReturnMode` (0x3B), otherwise return via 0x004A46F0. Otherwise **task 4**.
- **0x41**: `RankAskReturnWarning` (0x3C).

### Task 6 - player match menu (0x004AC6C0)
- **0x42**: page 1. Event 2 goes to task 1, 0x19 to create (0x43), 0x1A to search (0x48), 0x1C to 0x4F (Steam overlay, 9 frames).
- **0x43**: page 2. Event 1 goes to 0x44.
- **0x44**: `PlayerMatch_NameCheck` (vt+0x2C). netMgr[1] 0x100 goes to 0x45. 0x200 with error 8 goes to 0x46 (SessionNameError 0x13), otherwise 0x47 (CreateError 0x14).
- **0x45**: `PlayerMatch_CreateRoom` (vt+0x04). 0x400 goes to **task 7** with toast Creating. 0x800 goes to 0x47.
- **0x48**: page 3. Event 1 goes to 0x49.
- **0x49**: `PlayerMatch_Search` (vt+0x0C). 0x4000 goes to 0x4A, 0x8000 to 0x4C (SearchError 0x15).
- **0x4A**: list, refreshed every 600 frames.
  - Event 1 checks the room's connection class (node vt+0x34 & 0xF000) against `Ping_ToClass(rtt)`. RTT unknown shows `RMSR_CheckingRTT`. Too low goes to 0x4B (SearchSelectError 0x16). OK goes to 0x4D.
  - Event 0x1B goes to 0x49, event 2 to 0x42.
- **0x4D**: `PlayerMatch_JoinRow` (vt+0x10). 0x10000 goes to **task 7**. 0x20000 goes to 0x4E.
- **0x4E/0x50**: dialog chosen by netServer+0xAB0. 2/3/4 MemberFull (0x18), 6 AppVerDisagr (0x20), 7 CatalogVerUn (0x21), 10 none, anything else ConnectError (0x17). Then back to 0x4A (or 0x42 for 0x50).

### Task 7 - in a player room (0x004ACED0 + 0x004ADE90)
Error sampling every frame:
- +0xCC: 0x1C Kicked (netMgr[1] 0x10000000), 0x1B HostLost (netMgr[0] 0x20000000), 0x1A BreakUp (not in a room).
- +0xD0 for spectators: 0x25 (spectate target gone), 0x27, 0x28, 0x1E (stall), 4 (sync > 1200).
- +0xD0 for players: 0x1D LeaveBattler (opponent gone or GGPO disconnected), 0x26, 0x28, 0x1E, 4.

States:
- **0**: init, go to 0x51.
- **0x51 (room)**: each frame runs `Task7_RoomEvents` until a room error appears, then goes to 0x54.
  - Events: 2 asks to exit (0x52: `RoomExitYesNo` 0x35 for a client, `RoomReleaseYesNo` 0x36 for the host; OK goes to 0x5D).
  - 0x1D/0x1E ready / un-ready (`Room_SendReady`); 0x1F/0x20 opt-out (msg 7); 0x21 settings edit (0x5E); 0x22; 0x23; 0x24 info update (state 99); 0x25; 0x26 kick (0x53 `AskKick` 0x37, host); 0x27; 0x31 chat text; 0x34.
- **Match start** (inside RoomEvents): my entry has 0x1000 and stage 2.
  - Type 0x40 (replay): go to 0x5F.
  - Type 0x2x: go to 0x61. This waits for `P2P_IsConnected` for **600 frames**. On timeout it goes to 0x62: dialog `NEER_P2PBattle_Desable` (0x2E), auto-closed after 300 frames, then back to the room (or leave).
  - Every other type, **including 0x10/0x11 normal player rooms**, goes straight to 0x55 (in match) with **no connectivity wait**.
- **0x55-0x5C**: match, rematch, return to room.
- **0x54**: room error dialog (+0xCC), then 0x5D.
- **0x5D**: `Room_Leave` **once on entry**, page 0xF. Waits until not in a room (**no timeout**), then task 6.

### Task 8 - enter the 2D lobby (0x004ABF50)
- **0**: if uei is logged in go to 0x67, otherwise 100 (uei Idle, then Login).
- **100/0x65/0x66**: login/register as in task 2. A login failure shows `LOBBY_CannotLogin`, sets the TUS latch and goes to task 1.
- **0x67**: uei GetLobby. 16-frame minimum. 0 goes to 0x69. 100 goes to dialog `LOBBY_CannotGetLobby` (0x55), then 0x68.
- **0x69**: list, page 0xC. Event 2 goes to 0x6A, event 0x28 to 0x6B.
- **0x6A/0x6B**: wait until lobbyData vt+0x14 == 0 (idle).
  - 0x6A goes to task 1.
  - 0x6B: if this lobby is already joined (0x004A2A10) go to 0x6C. Otherwise lobbyData vt+0x10(&lobbyId) once.
- **0x6C**: entry runs lobbyData vt+0x24(join params); nonzero goes to 0x6D. Then poll vt+0x2C: 0 goes to **task 9**, 0x6C to 0x6E (MemberFull 0x18), anything else to 0x6D (ConnectError 0x17). **No timeout.**

### Task 9 - inside the 2D lobby (0x004AAAD0)
- **0x6F/0x70/0x75**: lobby UI.
- Cabinet seats:
  - Join a seat: lobbyData vt+0x3C, then 0x79. Polling vt+0x40 >= 100 shows `NTER_Blocking` for 0x6D, then 0x7A (SlotJoinError 0x29).
  - Leave a seat: vt+0x38 or 0x77/0x78 (ask dialogs 0x3D/0x3E).
  - Leave the lobby: 0x7C (`LobbyAskLeaveLobby` 0x3F).
- **Cabinet match** (0x7D-0x82): host creates (0x004A8BD0) or joins (0x004A8AB0) a room.
- Errors go to 0x88 with code +0x430:

  | Code | Cause |
  |---|---|
  | 1 | opponent seat vanished |
  | 2 | waited longer than catalog+0x200294 |
  | 3/7 | app version mismatch |
  | 4 | create start failed |
  | 5 | create failed (0x800) |
  | 6 | seat lost |
  | 8 | join start failed |
  | 9 | join failed after retries (+0x420) |

  0x88 then shows `NTER_LobbyMatch_LostBattler` (0x2B) and leaves the room. The code is shown as a 'NetworkErr %d' toast only when catalog+0x200298 == 1.
- Lobby gone (lobbyData vt+0x60 == 0): 0x73/0x74 show `LobbyDestroy` (0x2C) or 0x72 `ForceReturn` (0x2D), lobbyData vt+0x30, wait vt+0x34, then task 1.
- 0x8A: profile upload, 3000-frame budget.

### Task 0xD / 0xE - leave network mode (0x004AA890)
- **0**: `Room_Leave` (**fire and forget**), automatch reset, invite cleared, networker vt+0x58(pad).
  - 0xD: scene back to menu, page 0xE, lobbyData vt+0x20/+0x30 (leave Steam lobby), go to 0x8B.
  - 0xE: toast `NTER_Signout` if +0x410, go to 0x8C.
- **0x8B**: wait until lobbyData vt+0x34 >= 0 and the scene is ready.
- **0x8C**: entry (0xD only) shows dialog `NTER_Signout` (0x2F). Then `NetServer_Shutdown` (vt+0x78). Poll vt+0x7C until >= 0 and the dialog is closed, then **task 0**.

### Invite (0x004A6670, state in +0xE0, active while +0xE8)
- **0**: if the networker invite state (vt+0x50) is 3 (and not saving), or 1 (already online, +0xDC), then:
  - Allowed (not installing, no DLC busy, inner state not 0x11-0x14): `MSGW_AskGotoInvite` yes/no, go to 3.
  - Otherwise: `MSGW_InviteError`, go to 2.
- **3**: yes goes to 4, no goes to 0xC.
- **4**: force automatch entry = 3, `Room_Leave`, leave the Steam lobby (lobbyData vt+0x34 / +0x20 / +0x30). When the lobby is left, we are not in a room and the automatch is idle, go to 5.
- **5/6**: scene change, wait for event 3, go to 7.
- **7**: `InviteSelectPad`.
- **8**: if not online, go to task 2 (login first). Otherwise go to task 1, then 9.
- **9**: `Invite_Join` (vt+0x18). netMgr 0x40000 goes to 0xE (**task 7**). 0x80000 or a synchronous failure goes to 10. No own timeout.
- **10**: dialog by +0xAB0 (MemberFull / AppVer / CatalogVer / ConnectError), then 0xD (**task 6**).
- **0xC**: done.

## Failure paths
"Side" says who causes the failure: server (ASW uei or Steam backend), Steam result, peer (the other player / P2P), or local logic.

| # | Where | Exact condition | Side | User sees | Recovery |
|---|---|---|---|---|---|
| F1 | dispatcher 0x004A6FB1 | ISteamUser::BLoggedOn false (networker vt+0x2C >= 100) while in task 1, 3..9 | Steam | Task 1: exit (0xE). Others: 0xD and dialog `NTER_Signout` | Leaves network mode |
| F2 | dispatcher | netServer tick vt+0x94 >= 100 (netMgr[1] 0x8000000) | Steam/local (part C) | Forced exit 0xD and `NTER_Signout` | Leaves network mode |
| F3 | task 2 state 3 | init poll >= 100: +0xAB0 == 9 (poll 0x6B) gives RatingAge, else InitError | Steam/server | `NTER_RatingAge` / `NTER_InitError` | Exits (0xE). **Pending init has no timeout** |
| F4 | task 2/8 state 100 | uei login status 0xB | uei server | `LOBBY_CannotLogin`; TUS latch = 1 | Continues to task 1 without uei. D-codes and saves are off for the session |
| F5 | task 2/8 state 0x66 | uei create user 0xB | uei server | `LOBBY_CannotRegister` | Task 1 |
| F6 | task 8 state 0x67 | uei GetLobby 0xB | uei server | `LOBBY_CannotGetLobby` | Back to task 1 |
| F7 | task 8 state 0x6C | lobbyData join start != 0, or poll result not 0 (0x6C = full) | Steam lobby | `PlayerMatch_ConnectError` / `MemberFull` | Back to the lobby list. **Pending poll has no timeout** |
| F8 | task 4 0x26 / automatch 5 | search failed (0x8000) | Steam | Manual: `RankMatch_SearchError`. Automatch: silent, step 0xB | Retries next interval |
| F9 | automatch 3 / task 4 0x27 | create failed (0x800, or a synchronous failure) | Steam | Silent | Automatch: 0xB. Task 4 0x27: **create is retried every frame** while it keeps failing |
| F10 | task 4 0x27 | room's ping/rank class above ours | local (RTT) | `RankMatch_SearchSelectError`; RTT unknown gives toast `RMSR_CheckingRTT` | Back to the list |
| F11 | task 4 0x2C / automatch 8 | join failed (0x20000) | Steam/host reject (+0xAB0) | Manual: `RankMatch_ConnectError` (no reason shown). Automatch: silent | Room reset by NetMgr_FrameUpdate |
| F12 | task 6 0x4D / invite 9 | join failed. +0xAB0: 2/3/4 MemberFull, 6 AppVerDisagr, 7 CatalogVerUn, 10 nothing, other ConnectError | host reply / Steam | Matching dialog | Back to the list (task 6) |
| F13 | task 6 0x44/0x45/0x49 | name check / create / search failed | server | SessionNameError / CreateError / SearchError | Back to the menu |
| F14 | host keepalive 0x00496FF0 | a member sends nothing for more than 300 frames while netServer+0xAAC | peer | That member disappears. Host sees `PlayerMatch_LeaveBattler` / `RankMatchLeaveEnemy` | Member dropped through vt+0x34 |
| F15 | client keepalive 0x00497320 | no packet from the host for 301 frames while +0xAAC | peer/host | Task 7: `PlayerMatch_HostLost` (0x1B). Task 5: `RankMatch_LostBattler` (0x10) | Leaves as client |
| F16 | msg 0xB | host sent "room closed" while +0xAAC | host | Task 7: `PlayerMatch_Kicked` (0x1C) | Left |
| F17 | task 7 sampling | not in a room any more (Room state 0) | any | `PlayerMatch_BreakUp` (0x1A) | Task 6 |
| F18 | task 5/7 | opponent pointer gone (`Room_OpponentPresent` false) or `GGPO_PeerDisconnected` | peer | `RankMatch_LostBattler` (ranked) / `PlayerMatch_LeaveBattler` | Leave and return to the menu |
| F19 | task 5/7 | netData+0x22CC4 > 1200 (pre-battle sync waited 20 s) | peer/P2P | `NTER_CommonError` | Leave |
| F20 | task 7 0x61 | type 0x2x room: P2P not connected within 600 frames | P2P | `NEER_P2PBattle_Desable` (auto-closes after 300 frames) | Back to the room |
| F21 | task 5 0x36 | opponent D-code not loaded in 601 frames | server (CUMSTask) | Nothing; proceeds | Continues |
| F22 | automatch step 9 | popup open longer than `RankAutoCancel` (1800 frames), or memberCount < 2, or not in a room | peer / local | `RankMatchLeaveEnemy` (timeout included) | Leaves and resumes searching |
| F23 | task 4 0x2E | memberCount < 2 or not in a room | peer | `RankMatchLeaveEnemy` | Back to the ranked top |
| F24 | task 4 0x2E | user confirms `RankAskMatchingCancel` | user | `RankMatchLeaveMyself` | Back to the ranked top |
| F25 | task 9 0x7D.. | cabinet match codes 1-9 (+0x430) | various | `NTER_LobbyMatch_LostBattler`; 'NetworkErr %d' only with the debug flag | Back to the lobby |
| F26 | task 9 | lobby gone (lobbyData vt+0x60 == 0) | Steam lobby | `LobbyDestroy` / `ForceReturn` | Task 1 |
| F27 | invite 0 | not allowed now (installing, DLC busy, inner state 0x11-0x14) or the networker invite state is bad | local | `MSGW_InviteError` | Invite dropped |
| F28 | invite 9/10 | invite join failed | Steam/host | MemberFull / AppVer / CatalogVer / ConnectError | Task 6 |

States that wait **without any timeout** (they rely entirely on the layer below, or on the user):

| Where | Waits for |
|---|---|
| task 2 state 3 | netServer init |
| task 4 0x2C, task 6 0x45/0x49/0x4D, automatch 3/5/8, invite 9 | netServer create / search / join poll |
| task 8 0x6C, task 9 seat polls | lobbyData join poll |
| task 4 0x2E | READY / match start |
| task 4 0x24 | automatch to go idle |
| task 5 0x37 | profile upload (bug 2) |
| task 5 0x3F, task 7 0x5D | Room state 0 after a single `Room_Leave` |
| task 4 0x2A/0x2B/0x2F/0x30, automatch 7/0xB/0xC | Room state 0; these call `Room_Leave` every frame, which effectively retries |

## Suspected bugs
1. **Single-shot leave followed by an unbounded wait.**
   - Symptom: "Disconnecting..." or a dead UI after a match or room error. It clears only when Steam/netServer completes something by itself or the player is signed out.
   - Evidence: `Room_Leave` (0x0049DF90) returns 0x66 without leaving. For a client this happens when netServer+0xC54 != 0 (a request in flight) or +0x25E0 != 7 (0x0046E540). For the host it happens when +0xBC4 != 0 or +0x25DD != 3 (0x0046E440). Room state then stays 1 or 2. Task 5 state 0x3F (0x004AF1A0) and task 7 state 0x5D call it only on entry, then wait `IsInRoom() == 0` with no timeout. Task 0xD calls it once and never checks.
   - Confidence: medium. The 0x66 return is confirmed in asm at 0x0046E602. Whether the busy states really coincide with these exits in practice needs logs.
   - Client-side and fixable: re-call `Room_Leave` every frame (as tasks 4 and automatch already do) or add a timeout that forces Room_ResetLocal.
2. **Task 5 pre-match upload timeout uses the wrong counter.**
   - Symptom: the ranked match can hang after the D-code phase if the profile upload poll stays pending (-100).
   - Evidence: entry writes [edi+14h] = 3000 (0x004AF76E), and the poll path decrements [edi+14h] (0x004AF77A). The give-up test is `cmp [edi+20h],0 / jge` (0x004AF78F). +0x20 belongs to tasks 1/3/9 and keeps whatever they left, which is normally >= 0, so the budget never expires. Error sampling is also not acted on in 0x36/0x37.
   - Confidence: high for the code fact, low-medium for frequency (the CUMSTask worker normally finishes).
   - Client-side, trivially fixable (test +0x14, or set +0x20).
3. **Client leave failure raises the join-failed flag.**
   - Evidence: 0x0049B3F1 `or [ebx],20000h` after `NetServer_PollLeave`. The 0x200000 that should probably be used is never set or read.
   - Symptom: any state waiting on 0x10000/0x20000 in the same frame as a failed leave sees a bogus join failure (`RankMatch_ConnectError`, or automatch 0xB). Room_Reset also runs.
   - Confidence: high on the code, low impact (leave and join are normally serialised by the IsInRoom waits).
   - Fixable.
4. **Ghost join after a local-only leave.**
   - Evidence: while Room state is 1 (join requested, my entry not yet created), `Room_Leave` takes the "no entry" branch. That branch calls vt+0xCC (FUN_0041DB90, always 1) and `Room_ResetLocal`, and **sends nothing and does not cancel the netServer join**. When the join later succeeds, NetMgr_FrameUpdate still runs `Room_OnJoined` (0x0049ED30) unconditionally. Room state becomes 2 and +0xAAC = 1, so we sit in the host's room although no task expects it. The host sees a member who never presses OK. In automatch that member is us at steps 0-4, and the "own room has 2 or more members" rule can jump us to step 9.
   - Symptom: "match found" with a player who never answers; random 30 s `RankMatchLeaveEnemy`.
   - Confidence: low-medium. It needs part C to confirm that the netServer join keeps running after a UI cancel.
   - Fixable client-side: ignore or leave an unexpected 0x10000 when Room state was reset.
5. **The host's seating logic freezes while any member is "in match".**
   - Evidence: 0x00497D40 returns immediately if any present member has 0x1000 and stage +0x5B is not 0x11/0x12. Entries are only cleared by Room_Reset, by the member-drop path, or by finishing the match (stage 0x11/0x12). Dropping a silent member requires netServer+0xAAC.
   - Symptom: stale entries from the host's previous match stay seated with their OK bits. A new joiner is queued behind them (seat index 4/6 as in RankedStaleRoom.md), sees two strangers in the popup, and nothing ever starts.
   - Confidence: low-medium. This is a mechanism that fits all the logged facts, but the exact stale stage values are unverified.
   - Fixable only on the host (the mod can repair its own room when hosting).
6. **Manual ranked list has no auto-cancel.** Task 4 state 0x2E has no timer, unlike automatch step 9 (RankAutoCancel). This is the observed two-minute wait followed by `RankMatchLeaveMyself`. Client-side, fixable with a mod-side timer.
7. **Create spam on persistent failure.** In task 4 state 0x27, once +0x18 expires, `Ranked_CreateRoom` runs on every frame where Room state is 0. A failing create resets the room the same frame (0x800 leads to Room_Reset), so create is re-issued roughly every frame or two while Steam keeps failing. Medium confidence. Client-side.
8. **Missed one-frame results.** All netServer results are one-frame flags. A state that is not polling on that exact frame loses them. Example: task 4 state 0x24 waits for the automatch while a join result for the automatch arrives, which is harmless. But any transition (which clears requests) between issuing a request and its result leaves the netServer result unobserved, and Room state may stay 1. Low confidence; structural.
9. **Invite and network menu races.** While +0xE0 != 0, tasks 4 and 6 are frozen (not run) but their pending netServer requests continue. Invite state 4 force-leaves the room. Results that arrive for the frozen task are lost (see 8). Low confidence.

## Cross-part edges
- **netServer (part C)**: vt+0x00 Init, +0x04 Create, +0x08 Destroy(host leave), +0x0C Search, +0x10 Join, +0x14 Join (cabinet), +0x18 Join (invite), +0x1C Leave(client), +0x2C name check, +0x30 room info update, +0x34(i) drop member, +0x38..+0x6C async polls (see Data), +0x70 alloc result node, +0x78 shutdown, +0x7C shutdown poll, +0x90 default message handler, +0x94 tick (>= 100 fatal), +0xA4 apply member list, +0xAC refresh list, +0xB0 list busy, +0xB4/+0xB8/+0xC0/+0xC4 room ops (chat/comment), +0xCC (always 1). Fields read: +0xAAC in-session, +0xAB0 error, +0xAE8 count, +0x2BA*4 count, +0x2BD+i result handles, +0x1170 ping timeout.
- **Search results**: GAME_SearchResultNode / GAMESTEAM_SearchResultNode (via 0x004A5450): vt+0x30 RTT ms, vt+0x34 connection class bits, vt+0x50 rank, vt+0x58/+0x5C filter levels.
- **Steam lobby layer (part D)**: CSTEAMNetworkLobbyData through thunk 0x004A29F0. Used in tasks 8/9 (vt+0x10, +0x14, +0x24, +0x2C, +0x30, +0x34, +0x38..+0x64), invite state 4 (vt+0x20/+0x30/+0x34), exit (vt+0x20/+0x30/+0x34), lobby chat (vt+0x6C), and messages 0x2B-0x2E (vt+0x7C..+0x88). AASTEAM_CNetworker: vt+0x2C BLoggedOn, +0x30(1), +0x50/+0x58 invite (GameLobbyJoinRequested) state, +0x14 tick.
- **uei client (part A)**: work manager (FUN_00427CD0) strategies via 0x004291D0: 0 Idle, 1 Login, 2 CreateUser, 3 GetLobby, 4 EnterLobby, 5 LeaveLobby, 6 InLobby/UpdateLobbyUser, 7 DownloadTUS, 8 UploadTUS. Status mgr+0: 0 pending, 1 ok, 2 not registered, 0xB failed. Login latch +0x20 / DAT_00A5A070. Profile up/download via CUMSTask (0x004B8D30, 0x004B8CE0, 0x004B9210).
- **Connection / netcode (part E)**: packet ring 0x0040A650/0x0040A7E0/0x0040A3C0, peer address member+0x78, P2P state obj+0x1C238 (0x0049A9D0), GGPO disconnect 0x007836A0, pre-battle sync counter netData+0x22CC4 (FUN_0056BCB0), voice chat manager (0x004C32D0).
- **UI (part F)**: event pushes 0x004A88D0 come from 0x0070xxxx/0x0071xxxx menu code. Dialog/message window FUN_00695830/0x006959A0/0x006959F0/0x00697A30. charSelect struct (0x0047E860) +0x2558 scene request, +0x25F0 pad, +0x16A8 ranked entry mode, +0x110 menu mode. Automatch popup UI 0x004A2CA0/0x004A3440/0x004A3A90.
- **Mod**: `src/Game/Room` (Room+0x30 type, +0x38 count, member +0x30/+0x34) matches this map; ranked stale-room detector `InspectRoomAfterJoin`; ProfileSync rewrite of task 3; GhidraDefs names ADDR_NetTaskSwitch / ADDR_NetTaskDialogBusy / ADDR_NetTaskClearRequests.

## Open questions
1. Part C: does netServer cancel an in-flight Join/Create when the UI calls `Room_Leave` during Room state 1, and what do +0xC54, +0x25DD and +0x25E0 encode? This decides suspected bug 4 and part of bug 1.
2. What exactly sets a member's 0x1000 back to 0 in a host's room when the opponent disconnects mid-match (stage stuck at 0xA/0xC)? This confirms or kills suspected bug 5. A host-side mod dump after a disconnected ranked match would settle it.
3. Where is the UI path that ends network mode normally (writes task 0xE / pushes an exit event)? It is not in this cluster; it is likely written directly by menu code.
4. netServer vt+0x94: which conditions make it return >= 100 (the global forced exit)?
5. Catalog +0x200294 (cabinet wait limit) and +0x200298 (debug error toast) have no static writer. They are probably filled from the catalog blob; confirm the values.
6. Message types 0x1D-0x23, 0x2F-0x31 were only mapped at L1 (in-match/room sync payloads). Their exact layouts are part E work.
7. Who sets automatch +0x18 ("allowed") each frame (training / replay theater / arcade scenes)? This decides where background ranked search actually runs.
