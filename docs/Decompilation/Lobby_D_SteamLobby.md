# D - Steam lobby layer (AASTEAM_CNetworker, session objects, CSTEAMNetworkLobbyData)

## Summary

This part is the thin layer between the game and Steam's matchmaking: three "overlapped" session objects
(`AASTEAM_CSessionCreateObj`, `AASTEAM_CSessionJoinObj`, `AASTEAM_CSessionSearchObj`) that wrap exactly one
ISteamMatchmaking async call each (CreateLobby / JoinLobby / RequestLobbyList) plus the matching CCallResult,
the `AASTEAM_CNetworker` singleton (0x00A25788) that owns the P2P packet pump, voice chat and the Steam invite
callback, and `CSTEAMNetworkLobbyData` (0x00A5D270), which is NOT a Steam lobby at all but the 64-player avatar
"Network Lobby" (areas, cabinets, seat/duel state machine) running over the uei/SteamWorkMgr server layer and
P2P messages. The policy (when to search/create/join/leave, timeouts, UI) lives in `GAMESTEAM_CNetworkServer`
(part C); this layer has no timers, no retries and no cancel.

Most important findings:
1. Every lobby call result is accepted as current. There is no cancel: the busy flag (+4) of a session object is
   cleared only by its own result handler, and the LeaveLobby wrappers refuse (return 0x66) while busy. So a
   CreateLobby/JoinLobby still in flight when the higher state machine gives up completes later as "success" and
   the client ends up owning or sitting in a Steam lobby nobody tracks (ghost room in the ranked list / phantom
   member). The "stale result -> LeaveLobby" branches in the handlers are unreachable.
2. If CreateLobby/JoinLobby/RequestLobbyList returns k_uAPICallInvalid (0), no call result is registered but busy
   is still set to 1: that session object is busy forever (every later call returns 0x66, polls return -100).
3. All LobbyEnter_t failures (2 doesn't exist, 4 full, 5 error, 6 banned, 7 limited, 10/11 blocked) collapse into
   one result code 100. bIOFailure is never read by any lobby handler. The UI cannot know why a join failed.
4. The game never registers LobbyChatUpdate_t / LobbyKicked_t. Membership changes are only learnt through the
   game's own P2P messages, which are sent unreliable on channel 0 (SendP2PPacket type 0) - "left"/"closed" are
   sent 3 times and never acknowledged. P2PSessionConnectFail_t is registered by the transport with a no-op handler.
5. Ranked search filters: ownerID != me, distance, NETWORK_VERSION ==, MATCHING_FILTER ==, RANK_LEVEL_MIN >=,
   RANK_LEVEL_MAX <=, optional RANK_DISCONNECT != 1. No slots-available or result-count filter, no joinable flag
   change by hosts (SetLobbyJoinable/SetLobbyType/SetLobbyMemberLimit are never called anywhere in the binary).
6. CNetworker_Update has a hard self-loop (`0x0041D1E0: jmp 0041D1E0`) when the inbound packet buffer is full: a
   burst of channel-0 P2P packets freezes the game permanently.
7. Everything here runs on the main thread (callbacks from SteamAPI_RunCallbacks in the main loop 0x0044F1E0);
   the async task threads are used only for leaderboards/replay UGC.

## Functions
| VA | Name | Size | Level | Notes |
|---|---|---|---|---|
| 0x004072E0 | GAME_SearchResultNode_ScalarDtor | 88 | L1 | thiscall(this, byte flags), ret 4. Calls base dtor 0x004059C0, frees if flags&1. Vtable 0x0089C98C slot 0. |
| 0x0040A6B0 | PacketMgr_PopOutgoing | 178 | L1 | thiscall(this=PacketMgr 0x00A129B0, out u16** payload, out 16-byte header), ret 8. Pops next queued outgoing packet (linear buffer: +8 size,+0xC read ofs; or ring at +0x2C, count +0x30, head +0x34, mode +0x38, +0x40). Returns bytes consumed (u16 len + 0x10 hdr) or 0 when empty. Consumer is CNetworker_FlushSendQueue. Owned by packet transport (part E). |
| 0x0040A770 | PacketMgr_PushIncoming | 105 | L2 | thiscall(this=PacketMgr, u16* packet(len-prefixed), 16-byte header{steamID lo,hi,1000,..}), ret 8. Appends hdr+payload to inbound buffer (+0x10 data, +0x14 capacity, +0x18 used). Returns 1, or 0 when capacity < used+len+0x10 (FULL). CNetworker_Update spins forever on 0 (see bugs). |
| 0x0040A9C0 | VoicePacket_SetHeader | 28 | L1 | thiscall(this=u16 hdr, uint len), ret 4. Clamps len to <0xFFFF, stores len at +4 and total size len+0xC at +0. Voice packet builder. |
| 0x00414410 | AA_CNetworker_ctor | 52 | L1 | thiscall(this), ret. Sets vtable AA_CNetworker 0x0084F87C, inits sub-tables (0x00414450), zeroes +4..+0xC and +0x1170. |
| 0x00414450 | AA_CNetworker_InitPlayerTable | 121 | L1 | thiscall(this). Clears 30 entries x 0x94 at +8 (two 0x40-byte strings each), then 0x00414520. |
| 0x004144D0 | AA_CNetworker_dtor | 7 | L1 | thiscall(this). Restores AA_CNetworker vtable (EH unwind helper). |
| 0x004144E0 | AA_CNetworker_ScalarDtor | 34 | L1 | thiscall(this, flags), ret 4. Vtable 0x0084F87C slot 0. |
| 0x00414510 | AA_CNetworker_ReturnMinus1 | 4 | L1 | no args, ret; returns -1. Vtable slot 19 (default stub). |
| 0x00414520 | PlayerTable30_Clear | 83 | L1 | thiscall(this). Zeroes 30 x 0x94 entries (ids + two 0x40 strings + dword). Also used by 0x004AAAD0 / 0x004ACED0. |
| 0x00414580 | AA_CRankingReader_ctorBase | 9 | L1 | thiscall(this): sets AA_CRankingReader vtable. Leaderboard code, not lobby. |
| 0x00414610 | AA_CRankingWriter_ctorBase | 9 | L1 | thiscall(this): sets AA_CRankingWriter vtable. Leaderboard code, not lobby. |
| 0x0041A800 | AASTEAM_CAsyncTaskThread_ctor | 174 | L1 | thiscall(this). ThreadObject base (0x0041ABE0), InitializeCriticalSection(+0x10), task vector +0x28/+0x2C/+0x30, running flag +0x34, manual-reset event +0x38. Used only by AASTEAM_CRankingThreadManager (0x0041F9D0) for leaderboard/replay tasks; NOT used by the lobby layer. |
| 0x0041A8D0 | AASTEAM_CAsyncTaskThread_dtor | 170 | L1 | thiscall(this). Stops thread (0x0041AD90: state=2, wait 3000 ms, "Thread Delete Time Out" message box on timeout), closes event, frees vector, deletes CS. |
| 0x0041A990 | AASTEAM_CAsyncTaskThread_ScalarDtor | 33 | L1 | thiscall(this, flags), ret 4. Vtable 0x0084FDDC slot 3. |
| 0x0041A9C0 | AASTEAM_CAsyncTaskThread_Wake | 21 | L1 | thiscall(this). Sleep(0); PulseEvent(+0x38). Vtable slot 2. |
| 0x0041A9E0 | AASTEAM_CAsyncTaskThread_ThreadProc | 289 | L1 | thiscall(this) worker loop (vtable slot 0). Until state(+4)==2: pop front task under CS +0x10; none -> WaitForSingleObject(event,10000); else call task->vtbl[0], then task+0x1D=0 (queued) and task+0x1C=1 (done) under task CS. Worker thread; ranking/replay only. |
| 0x0041ABE0 | AASTEAM_ThreadObject_ctor | 75 | L1 | thiscall(this): vtable AASTEAM_ThreadObject, handle +8 = -1. |
| 0x0041AC30 | AASTEAM_ThreadObject_dtor | 153 | L1 | thiscall(this). If running(+4==1): state=2, vtbl[2] (wake), WaitForSingleObject(thread,3000), assert box "Thread Delete Time Out" (0x0044EE10) on timeout; CloseHandle. |
| 0x0041AD00 | AASTEAM_ThreadObject_IsStopping | 10 | L1 | thiscall(this) -> (this+4 == 2). |
| 0x0041AD90 | AASTEAM_ThreadObject_Stop | 57 | L1 | thiscall(this). Same stop/wait-3000 ms logic as dtor without closing. |
| 0x0041AEC0 | CCallback_GameLobbyJoinRequested_ctor | 167 | L2 | thiscall(this=CCallback 0x14 bytes, obj, memfn), ret 8. Sets vtable 0x0084FF40, +0xC obj, +0x10 fn, SteamAPI_RegisterCallback(this, 333 = GameLobbyJoinRequested_t). Created by CNetworker_Init with fn = 0x0041CAC0. |
| 0x0041B1A0 | AASTEAM_CNetworker_ctor | 364 | L2 | thiscall(this=0x00A25788), ret. AA ctor, vtable 0x0084FF54, +0x1198 0x80 bytes cleared, +0x2C3A=0, +0x1220 (lobby state)=1, +0x1224=0, +0x1230/34 (my id)=0, +0x1238 member table 0x1A00 cleared, +0x3A48 masked, +0x1194..+0x1218 cleared, +0x3630/34=-1, +0x3638=0, +0x3640 two 64x8 hDCODE tables cleared, +0x3A40 (join-request callback ptr)=0. Lazy-init from 0x0041C900 / 0x0041CAC0, atexit 0x008480B0. |
| 0x0041B310 | RecentPlayerNameRing_ctor | 184 | L1 | thiscall(this=0x00A291E0). Clears 64 entries x 0x30 (CSteamID + 14-wchar name) and write index +0xC00. Global "recent player names" ring filled from search results (0x0041CF40) and avatar lobby (0x0041BE40). |
| 0x0041BC10 | AASTEAM_CNetworker_ScalarDtor | 107 | L1 | thiscall(this, flags), ret 4. Calls Shutdown (0x0041C820) then AA vtable; vtable slot 0. |
| 0x0041BE40 | RecentPlayerNameRing_Add | 91 | L1 | thiscall(this=ring, idLo, idHi, wchar* name), ret 0xC. Writes id + name (truncated to 14 chars, "..." suffix via 0x0041C730) at index +0xC00, index = (index+1) & 63. |
| 0x0041BEA0 | CNetworker_EnterAvatarLobby | 49 | L2 | thiscall(this=CNetworker, region, area, lobbyId, void* memberData, int size=0x40), ret 0x14. Only if state(+0x1220) in {1,7,8,9,10} else returns 100. Resets: state=1, +0x2C3A=0, +0x1224/+0x1230/+0x1234=0, member table cleared; then SteamWorkMgr(0x00A5A050)->SetEnterParams (0x00427F60 -> strategy 4) ; stores +0x2C40=area, +0x2C3C=region, +0x1228=lobbyId; state=4; returns 0. Avatar lobby runs over the uei/SteamWorkMgr layer (part A/B), not ISteamMatchmaking. |
| 0x0041BF70 | CNetworker_LeaveAvatarLobby | 31 | L2 | thiscall(this), ret 4 (arg unused). SteamWorkMgr strategy 5 (0x00428020); state(+0x1220)=6; returns 0. |
| 0x0041C520 | CNetworker_GetSlot1184 | 17 | L1 | thiscall(this, int i), ret 4 -> this+0x1184+i*4. Vtable slot 16. |
| 0x0041C540 | CNetworker_PollEnterAvatarLobby | 74 | L2 | thiscall(this), ret. Reads SteamWorkMgr state dword (0x00A5A050+0): 0 -> -100 (pending); 0xB -> 100 (failed, state left at 4); 0xC -> state=10, 0; 0xD -> state=11, 0; anything else -> state=5 (entered), 0. |
| 0x0041C590 | CNetworker_PollLeaveAvatarLobby | 47 | L2 | thiscall(this), ret. SteamWorkMgr state 0 -> -100 (pending); else state=5 and returns 100 if workmgr state==0xB else 0 (caller then resets to 1). |
| 0x0041C5C0 | CNetworker_IsLoggedOn | 41 | L2 | thiscall(this, unused), ret 4. ISteamUser::BLoggedOn (user+0x04): true -> 0, false -> 100. Vtable slots 10 and 11. |
| 0x0041C670 | CSessionSearchObj_ResetResults | 108 | L2 | thiscall(this=search obj), ret. +0x10..+0x1C=0, +0x20 (distance code)=1, zero filter array (+0x2C, count +0x24 * 0x4C), +0x30 (result count)=0, clears 50 result entries x 0x14 from +0x38. |
| 0x0041C6E0 | CNetworker_ResetAvatarLobby | 74 | L2 | thiscall(this), ret. state(+0x1220)=1, +0x2C3A=0, +0x1224/+0x1230/+0x1234=0, member table (+0x1238, 0x1A00) cleared. Returns 0. |
| 0x0041C730 | CopyNameTruncated14 | 230 | L1 | cdecl(src, wchar* dst, uint maxChars=0xE). Converts via 0x0041E280 (part A) and wcsncpy; appends "..." at dst[14] when truncated. |
| 0x0041C820 | CNetworker_Shutdown | 141 | L2 | thiscall(this), ret. SteamWorkMgr shutdown (0x00427C40); deletes the GameLobbyJoinRequested CCallback at +0x3A40 (SteamAPI_UnregisterCallback). Called from dtor 0x0041BC10 (vtable slot 2 too). |
| 0x0041C8B0 | CNetworker_GetPersonaName | 52 | L1 | thiscall(this, unused), ret 4. ISteamFriends::GetPersonaName (friends+0x00) or "" (0x0084FFD0) if no ISteamFriends. Vtable slot 15. |
| 0x0041C970 | RecentPlayerNameRing_Get | 95 | L1 | cdecl() -> &0x00A291E0 (lazy ctor 0x0041B310, guard 0x00A29DE4). |
| 0x0041C9D0 | CNetworker_GetJoinRequestFlag | 9 | L2 | thiscall(this, unused), ret 4 -> +0x1218 (3 = a Steam invite/join-friend request is pending). Vtable slot 20. |
| 0x0041CA20 | CNetworker_Init | 132 | L2 | thiscall(this, unused), ret 4. new CCallback(0x14) -> 0x0041AEC0(obj=this, fn=0x0041CAC0) stored at +0x3A40 (registers GameLobbyJoinRequested_t 333); SteamWorkMgr init 0x00427E40. Vtable slot 1. |
| 0x0041CAB0 | CNetworker_GetFlag1194 | 10 | L1 | thiscall(this) -> +0x1194 & 1. Vtable slot 17. |
| 0x0041D0E0 | CNetworker_Update | 274 | L2 | thiscall(this), ret. Main-thread per-frame (vtable slot 4). Runs leaderboard reader/writer SMs (0x0041F840, 0x0041FEA0), voice capture (0x00424D40); then drains ISteamNetworking channel 0: while IsP2PPacketAvailable(&size,0) and ReadP2PPacket(this+0x313A, 0x4F2, &size, &remote, 0): voice packets (type 0xFF01) -> 0x004249B0; everything else -> PacketMgr_PushIncoming(this+0x313C, {remote, 1000}). If that returns 0 (buffer full) the code executes `jmp 0041D1E0` (self-loop): permanent freeze. |
| 0x0041D200 | CNetworker_ClearJoinRequestFlag | 13 | L2 | thiscall(this, unused), ret 4. +0x1218 = 0. Vtable slot 22. |
| 0x0041D210 | CCallback_Run_NoArgsThunk | 14 | L1 | thiscall(this) tail-jump to this+0x10 (shared CCallbackBase slot 1 for all CCallback<> in this layer). |
| 0x0041D220 | CCallback_Run | 15 | L2 | thiscall(this, void* param, bool ioFailure, SteamAPICall_t), ret 0x10. Calls vtbl[1](param) i.e. obj->*fn(param). Shared slot 0 of every CCallback<> vtable here (JoinRequest, LobbyDataUpdate x2, P2P x4). |
| 0x0041D230 | CNetworker_SendAvatarLobbyMsg | 45 | L1 | cdecl(a, CSteamID* id), ret 8 (stdcall-like). SteamWorkMgr 0x00428100 -> 0x00428400 (store msg + target id). Used by lobby-data slot 27. |
| 0x0041D260 | CNetworker_FlushSendQueue | 250 | L2 | cdecl/thiscall(this), ret. Vtable slot 3. Pops every queued outgoing packet (0x0040A6B0). Destination 0, 127.0.0.1 or own SteamID -> loop back into inbound buffer (0x0040A770, result ignored). Else copies to this+0x2C48 (u16 len + data) and ISteamNetworking::SendP2PPacket(id, buf, len+2, k_EP2PSendUnreliable(0), channel 0). ALL game messages go out unreliable on channel 0. |
| 0x0041D3C0 | CNetworker_SetJoinRequestFlag | 16 | L2 | thiscall(this, unused, int v), ret 8. +0x1218 = v. Vtable slot 21; called with v=3 by the join-request handler. |
| 0x0041D3D0 | CNetworker_SetMyAvatarMemberData | 27 | L1 | stdcall(void* data, int size=0x40), ret 8. SteamWorkMgr 0x00428110: copies data, starts strategy 6 (member-data update) unless strategy 5 (leaving) is active or 6 already running (then re-arms it). |
| 0x0041D410 | CNetworker_PumpWorkMgr | 12 | L1 | cdecl(). SteamWorkMgr 0x00428260: runs tick (vtbl+0xC) of both active strategies (+0xE0, +0xE4). Vtable slot 5. |
| 0x0041D4C0 | CNetworker_SyncAvatarMembers | 383 | L2 | thiscall(this, unused), ret 4. Only in state 5. Stores own SteamID in +0x1230; +0x2C38=64; +0x2C3A = member count from SteamWorkMgr list (0x00427DE0, stride 0x48). Marks every table entry (+0x1238, 64 x 0x68) whose id is no longer listed with +0x1240=1 (left); adds new ids to the first free entry (+0 used=1, +4 new=1, +8=0, +0x10/14 id, +0x64 ping=-1) and copies 0x40 bytes member data to +0x18. Silently drops members beyond 64. |
| 0x0041DB90 | Stub_ReturnTrue | 6 | L1 | no args, ret; returns 1. Shared stub (13 callers); vtable slot 9/25. |
| 0x0041E320 | AASTEAM_CRankingTask_ctor | 508 | L1 | thiscall(this). Init leaderboard task block (0x2698 bytes) with three CCallResults: LeaderboardFindResult_t (0x450) at +0x2638, LeaderboardScoreUploaded_t (0x452) at +0x2658, LeaderboardScoresDownloaded_t (0x451) at +0x2678. Leaderboards only. |
| 0x0041E720 | Ranking_IsSameUtcDayHour | 134 | L1 | cdecl(t1, t2). gmtime both, returns 0 when hour and day fields match, else 1. Leaderboard weekly/monthly reset logic. |
| 0x0041E7B0 | Ranking_CheckPeriod | 185 | L1 | cdecl(a,b). Period index compare using fixed UTC epochs (mktime UTC). Leaderboards. |
| 0x0041E870 | Ranking_MonthParity | 67 | L1 | cdecl(time) -> 1/2 by month parity, 0 if gmtime fails. Leaderboards. |
| 0x0041E8C0 | Ranking_PeriodIndex | 172 | L1 | cdecl(time). (time - epoch0) % (epoch1-epoch0). Leaderboards. |
| 0x0041E970 | CCallResult_LeaderboardFind_GetSize | 6 | L1 | returns 0x10 (sizeof callback). Shared GetCallbackSizeBytes for 16-byte callbacks (LobbyCreated_t, GameLobbyJoinRequested_t, LeaderboardUGCSet_t...). |
| 0x0041EA70 | RankingTask_IsBusy | 9 | L1 | thiscall(this) -> +4 != 0. |
| 0x0041EC10 | RankingTask_Tick | 259 | L1 | thiscall(this). Leaderboard SM: FindLeaderboard (UserStats+0x5C) -> DownloadLeaderboardEntries (+0x70, range -50..50 around user) etc. via generic CCallResult setter 0x00421EA0. Not lobby. |
| 0x0041ED30 | RankingTask_DownloadForUsers | 221 | L1 | thiscall(this). FindOrCreateLeaderboard (+0x58) / DownloadLeaderboardEntriesForUsers (+0x74). Not lobby. |
| 0x0041EE10 | RankingTask_DownloadRange | 213 | L1 | thiscall(this). FindLeaderboard / DownloadLeaderboardEntries(Global, start, start+count). Not lobby. |
| 0x0041EEF0 | RankingTask_Upload | 261 | L1 | thiscall(this). FindLeaderboard / UploadLeaderboardScore (+0x7C, method KeepBest/ForceUpdate, 16 details). Not lobby. |
| 0x0041F0B0 | RankingTask_StartUpload | 39 | L1 | thiscall(this). If idle: +4=1,+8=1, timeout +0x2624=3600; else -100. |
| 0x0041F140 | MkTimeUtc | 126 | L1 | cdecl(tm*). Temporarily sets TZ=UTC, mktime64, restores TZ. Leaderboards. |
| 0x0041F1C0 | AASTEAM_CRankingReader_ctor | 151 | L1 | thiscall(this). Leaderboards. |
| 0x0041F2E0 | RankingReader_GetInstance | 157 | L1 | cdecl() -> lazy singleton 0x00A29E00 (0x2780 bytes). Leaderboards. |
| 0x0041F840 | RankingReader_Update | 42 | L1 | thiscall(this). Tick + idle detection. Leaderboards. |
| 0x0041FC30 | AASTEAM_CRankingWriter_ctor | 159 | L1 | thiscall(this). 16 ranking tasks of 0x2698. Leaderboards. |
| 0x0041FD80 | RankingWriter_GetInstance | 157 | L1 | cdecl() -> lazy singleton 0x00A29E10 (0x274D0 bytes). Leaderboards. |
| 0x0041FEA0 | RankingWriter_Update | 2124 | L1 | thiscall(this, bool reset), ret 4. 10-state leaderboard upload SM (UGC download/upload via CUMSTask, period rollover, score upload). Called each frame from CNetworker_Update. Not lobby. |
| 0x00420850 | FixedName16_Set | 104 | L1 | thiscall(this=char[16], const char*), ret 4. Copies up to 15 chars (task names "ReplayUploader"/"ReplayDownloader"). |
| 0x004208C0 | AASTEAM_CReplayDownloadTask_ctor | 268 | L1 | thiscall(this). CCallResult RemoteStorageDownloadUGCResult_t (0x525). Replays. |
| 0x00420A10 | AASTEAM_CAsyncTaskInterface_dtor | 76 | L1 | thiscall(this). DeleteCriticalSection(+4). |
| 0x00420A60 | AASTEAM_CReplayDownloadTask_dtor | 247 | L1 | thiscall(this). Unregisters its CCallResult. Replays. |
| 0x00420B60 | AASTEAM_CAsyncTaskInterface_ScalarDtor | 99 | L1 | thiscall(this, flags), ret 4. |
| 0x004215F0 | AASTEAM_CReplayUploadTask_ctor | 329 | L1 | thiscall(this). CCallResults RemoteStorageFileShareResult_t (0x51B), LeaderboardUGCSet_t (0x457). Replays. |
| 0x004217C0 | AASTEAM_CReplayUploadTask_dtor | 245 | L1 | thiscall(this). Replays. |
| 0x00421D70 | CCallResult_Run_NoHandle | 36 | L2 | thiscall(this=CCallResult, void* param), ret 4. Clears handle (+0x10/+0x14) and calls obj(+0x18)->*fn(+0x1C)(param, ioFailure=false). CCallResultBase slot 1 (shared by every CCallResult<> in the game). |
| 0x00421DA0 | CCallResult_Run | 53 | L2 | thiscall(this, void* param, bool ioFailure, SteamAPICall_t h), ret 0x10. If h == stored handle (+0x10/+0x14): clear handle, call obj->*fn(param, ioFailure). Mismatched handles are ignored (stale results cannot reach handlers). Slot 0 of every CCallResult<> vtable (LobbyCreated, LobbyEnter, LobbyMatchList, leaderboards, UGC). |
| 0x00421EA0 | CCallResult_Set | 77 | L1 | thiscall(this, hLo, hHi, obj, fn), ret 0x10. Unregisters old handle, stores new, registers if non-zero. Used by leaderboards; the lobby wrappers inline the same sequence. |
| 0x00422330 | AASTEAM_CUMSTaskOwner_ctor | 210 | L1 | thiscall(this). Allocates CUMSTask (0x110). Replays/leaderboards. |
| 0x00422410 | AASTEAM_CUMSTask_ctor | 327 | L1 | thiscall(this). CCallResults FileShare (0x51B) and DownloadUGC (0x525). |
| 0x00422680 | AASTEAM_CUMSTask_dtor | 233 | L1 | thiscall(this). Unregisters both CCallResults. |
| 0x004230F0 | UMSTask_Tick | 33 | L1 | thiscall(this). Dispatch on mode 1/2/3 to 0x00423490 / 0x004232D0 / 0x00423130. |
| 0x00423130 | UMSTask_TickMode3 | 414 | L1 | UGC upload job: header "URAA"(0x41415255) + owner SteamID, enqueue on async thread (0x0041AB10), poll done flag. Not lobby. |
| 0x004232D0 | UMSTask_TickMode2 | 442 | L1 | UGC download job, same pattern. Not lobby. |
| 0x00423490 | UMSTask_TickMode1 | 374 | L1 | UGC write job, same pattern. Not lobby. |
| 0x00423610 | UMSTask_StartRead | 160 | L1 | thiscall(this). If idle and worker not busy: mode=2, busy. Else -100. |
| 0x004236B0 | UMSTask_StartWrite | 160 | L1 | thiscall(this). mode=1. |
| 0x00423750 | UMSTask_SetJob | 88 | L1 | thiscall(this, src, a, b), ret 0xC. Copies 0x60-byte header into job. |
| 0x00424240 | CCallback_VoiceChat_P2PConnectFail_ctor | 167 | L1 | thiscall(this, obj, fn), ret 8. Registers 1203 (P2PSessionConnectFail_t) for AASTEAM_CVoiceChatManager (fn 0x00424CA0). |
| 0x004242F0 | CCallback_VoiceChat_P2PSessionRequest_ctor | 167 | L1 | thiscall(this, obj, fn), ret 8. Registers 1202 (P2PSessionRequest_t) for voice chat (fn 0x00424CE0). |
| 0x004247F0 | VoiceChat_ReapStalePeers | 150 | L1 | thiscall(this=VoiceChatMgr). Walks peer map (+0x5618) and removes the first peer whose flag +0x34 is 0 (stops its playback channel). |
| 0x004248C0 | CCallback_P2PConnectFail_GetSize | 6 | L1 | returns 9 (sizeof P2PSessionConnectFail_t). |
| 0x004249B0 | VoiceChat_OnPacket | 312 | L1 | thiscall(this=VoiceChatMgr, idLo, idHi, packet*), ret 0xC. Returns true (consumed) for type 0xFF01 voice packets: decompress (ISteamUser::DecompressVoice via 0x00407E80) and play. Called from CNetworker_Update for every P2P packet. |
| 0x00424D40 | VoiceChat_Update | 490 | L1 | thiscall(this). If recording: GetAvailableVoice (user+0x24) / GetVoice (user+0x28), send to peers; every 60 frames enumerate capture devices (DirectSoundCaptureEnumerateA) and Start/StopVoiceRecording (user+0x1C/+0x20). |
| 0x00424F30 | VoiceChat_SendToAllPeers | 191 | L1 | thiscall(this, packet). Queues the voice packet to every peer (0x0040A7E0). |
| 0x00425F20 | RbTree_RotateLeft | 84 | L1 | library: std::map rotate. |
| 0x00426070 | RbTree_Erase | 605 | L1 | library: std::map erase ("invalid map/set<T> iterator"). |
| 0x00427C40 | SteamWorkMgr_Shutdown | 63 | L1 | thiscall(this=0x00A5A050). Deletes both active strategies (+0xE0, +0xE4), 0x00432E10, 0x00434D30. Part A/B object. |
| 0x00427DC0 | SteamWorkMgr_GetMember | 19 | L1 | thiscall(this, i), ret 4 -> +0x30 + i*0x48. |
| 0x00427DE0 | SteamWorkMgr_GetMemberCount | 24 | L1 | thiscall(this) -> (+0x34 - +0x30)/0x48. |
| 0x00427E40 | SteamWorkMgr_Init | 97 | L1 | thiscall(this). Own SteamID, init, both strategies = Idle (strategy 0). |
| 0x00427EB0 | SteamWorkMgr_GetMembersDirty | 4 | L1 | thiscall(this) -> byte +0x3C. |
| 0x00428020 | SteamWorkMgr_StartLeave | 34 | L1 | thiscall(this). Replace strategy +0xE0 with strategy 5. |
| 0x00428100 | SteamWorkMgr_QueueMsg | 9 | L1 | thunk to 0x00428400. |
| 0x00428110 | SteamWorkMgr_SetMyMemberData | 106 | L1 | thiscall(this, data, size), ret 8. See 0x0041D3D0. |
| 0x00428250 | SteamWorkMgr_ClearMembersDirty | 5 | L1 | thiscall(this): +0x3C = 0. |
| 0x00428260 | SteamWorkMgr_Tick | 29 | L1 | thiscall(this): strategies[+0xE0]->vtbl[3](this), strategies[+0xE4]->vtbl[3](this). |
| 0x004282C0 | SteamWorkMgr_Reset | 216 | L1 | thiscall(this). Clears state, strings, member vector. |
| 0x00428400 | SteamWorkMgr_SetPendingMsg | 93 | L1 | thiscall(this, wchar*, CSteamID*), ret 8. Stores msg (0x00468780) and target id +0xC0, flag +0xC8=1. |
| 0x00428460 | SteamWorkMgr_CopyMemberData | 57 | L1 | thiscall(this, src, size), ret 8. memcpy to +0x60, size +0xA0. |
| 0x004293E0 | CCallback_P2PSessionRequest_GetSize | 6 | L1 | returns 8 (sizeof P2PSessionRequest_t). |
| 0x00429DC0 | Stub_Ret4 | 3 | L1 | ret 4, no-op. Used as AA_CNetworker vtable default AND as the P2PSessionConnectFail_t handler of GAMESTEAM_CNetworkTransport (0x0046C7C0) - connect failures are ignored. |
| 0x00432000 | RbTree_Next | 76 | L1 | library: tree iterator ++. |
| 0x00432F50 | SteamWorkMgr_InitSelf | 149 | L1 | thiscall(this, idLo, idHi, 2), ret 0xC. Self record: id, time64, magic "BBCF" (0x46434242), sprintf("%lld", id). |
| 0x00434D30 | SteamWorkMgr_FreeBuffers | 109 | L1 | thiscall(this). Frees 12 buffers at +0x40 and +0x70. |
| 0x00435970 | RbTree_Max | 31 | L1 | library. |
| 0x00435990 | RbTree_Min | 30 | L1 | library. |
| 0x004360F0 | RbTree_RotateRight | 88 | L1 | library. |
| 0x00439970 | SearchResultNode_GetSessionFlag | 4 | L1 | thiscall(this) -> +0x64 (PLAYER_SESSION_FLAG). GAMESTEAM_SearchResultNode slot 13. |
| 0x00439980 | SearchResultNode_GetSessionValue | 4 | L1 | thiscall(this) -> +0x68 (PLAYER_SESSION_VALUE). Slot 14; generic getter shared by 46 callers. |
| 0x0044EE10 | AssertBox | 141 | L1 | cdecl(fmt, ...). vsprintf, appends to assert.txt, MessageBoxA(..., "Continue"). Used for "Thread Delete Time Out". |
| 0x00469BA0 | CCallback_JoinObj_LobbyDataUpdate_ctor | 167 | L2 | thiscall(this=joinObj+0x58, obj, fn), ret 8. Vtable 0x0089C874, registers 505 (LobbyDataUpdate_t), fn = 0x0041CD80. Registered for the whole lifetime of the join object (fires for every lobby data update of any lobby; handler filters by lobby id). |
| 0x00469C50 | AASTEAM_CSessionJoinObj_ctor | 242 | L2 | thiscall(this), ret. Vtable 0x0089C884 (base COverlapedObj 0x0089C85C). +4 busy=0, +8 done=0, +0xC result=0, +0x10=0, +0x16=0, +0x18/+0x1C arrays=0, +0x20 lobby CSteamID=0, +0x28 owner CSteamID=0, +0x30=0; CCallResult<LobbyEnter_t> at +0x38 (vtable 0x0089C864, iCallback +0x40 = 504, handle +0x48 = 0); CCallback<LobbyDataUpdate_t> at +0x58. Size 0x70. |
| 0x00469D50 | CNetworkLobbyData_ClearRows | 81 | L1 | thiscall(this). Clears 64 x 0x68 rows at +0x8... and calls 0x0046A410. |
| 0x00469DB0 | CNetworkLobbyData_ctor | 112 | L1 | thiscall(this). Vtable 0x0089C7CC; clears 32 area entries x 0xA8 from +8, rows. |
| 0x00469E20 | CSTEAMNetworkLobbyData_ctor | 371 | L2 | thiscall(this=0x00A5D270), ret. Base ctor; vtable 0x0089C88C; +0x30A0 (status)=0, +0x30A8..+0x30B8=0, +0x30CC (broadcast countdown)=60, +0x30D8 0x40 bytes cleared, +0x3118=0, 64 member records x 0x70 at +0x3128 (+0x20 ping=-1), +0x4D38 CSteamID=0, then 0x0046A470, 0x0046A4D0. |
| 0x00469FA0 | CCallResult_LobbyEnter_dtor | 49 | L1 | thiscall(this). Unregisters pending handle (EH unwind path). |
| 0x0046A050 | AASTEAM_CSessionJoinObj_dtor | 220 | L2 | thiscall(this), ret. Frees +0x18/+0x1C, unregisters LobbyDataUpdate callback (+0x58) and pending LobbyEnter call result (+0x38). Does NOT call LeaveLobby: destroying a join object while joined leaves the Steam lobby membership alive. |
| 0x0046A130 | CNetworkLobbyData_dtor | 7 | L1 | thiscall(this): restore base vtable. |
| 0x0046A170 | AASTEAM_CSessionJoinObj_ScalarDtor | 33 | L1 | thiscall(this, flags), ret 4. Vtable 0x0089C884 slot 0. |
| 0x0046A1A0 | CNetworkLobbyData_ScalarDtor | 34 | L1 | thiscall(this, flags), ret 4. |
| 0x0046A1D0 | CSTEAMNetworkLobbyData_ScalarDtor | 95 | L1 | thiscall(this, flags), ret 4. Slot 0. |
| 0x0046A230 | LobbyData_IsDuelIdle | 12 | L1 | thiscall(this) -> duel state(+0x3112)==0. Slot 25. |
| 0x0046A240 | LobbyData_DuelIdleResult | 17 | L1 | thiscall(this) -> 0 if duel state 0 else 100. Slot 14. |
| 0x0046A260 | LobbyData_SeatRequestResult | 40 | L1 | thiscall(this) -> -100 while request flag 1 pending or state 1 (seated/waiting); 0 in state 2; else 100. Slot 16. |
| 0x0046A290 | LobbyData_HasOlderSeatedRival | 118 | L2 | thiscall(this, bool compareJoinDate), ret 4. Scans 64 member records (+0x312C, stride 0x70): not self, used, seated(+0x66), same side(+0x65 == +0x3111). Without compare -> 1 on first match; with compare -> 1 if that member joined earlier (u64 at rec+0x50 < my +0x30F8). Else 0. |
| 0x0046A310 | LobbyData_EnterResult | 51 | L1 | thiscall(this). -100 while a command (+0x3090) or pending (+0x3098) is active; 0 if status(+0x30A0)==4; 108 if status 3; else 100. Slot 11. |
| 0x0046A350 | LobbyData_LeaveResult | 42 | L1 | thiscall(this). -100 while busy; 100 if still in lobby (status 4), else 0. Slot 13. |
| 0x0046A380 | LobbyData_IsInLobby | 13 | L1 | thiscall(this) -> status(+0x30A0)==4. Slot 24. |
| 0x0046A3F0 | LobbyData_IsIdle | 27 | L1 | thiscall(this) -> 0 if no command pending else -100. Slot 5. |
| 0x0046A410 | CNetworkLobbyData_ClearTables | 78 | L1 | thiscall(this). +0=0, 64 x 0x68 rows, 32 x 0xC pair table at +0x1A08. |
| 0x0046A470 | LobbyData_ClearPingTable | 87 | L1 | thiscall(this). 64 x 0x10 entries (+0x18, id=-1) and 64 dwords at +0x418. |
| 0x0046A4D0 | LobbyData_Reset | 67 | L2 | thiscall(this). +0x3098=0, +0x3090 (command)=0, clears area table (32 x 0xA8), then 0x0046A550. Slot 8 via 0x0046BE60. |
| 0x0046A520 | LobbyData_ClearDuelTarget | 37 | L1 | thiscall(this=rec+0x30D8 block). Clears target fields (+0x50..+0x73). |
| 0x0046A550 | LobbyData_ResetSession | 302 | L2 | thiscall(this). status(+0x30A0)=0, flags/requests (+0x30A8..+0x30B8)=0, +0x30CC=60, my member data (+0x30D8, 0x40) cleared, 64 member records reset (+0x3128.., id, ping -1, index), +0x309C=0, +0x1508=0, UI rows (+0x1510, 64 x 0x68) cleared, pair table (+0x2F10) cleared. |
| 0x0046A690 | LobbyData_ClearDuelAll | 60 | L1 | thiscall(this=+0x30D8 block). Clears target + join date fields. |
| 0x0046A6D0 | LobbyData_ClearAreaTable | 42 | L1 | thiscall(this). 32 x 0xA8 area entries. Slot 3. |
| 0x0046A700 | LobbyData_DebugPrint | 178 | L1 | thiscall(this). If debug flag 5: prints MyJoinDate, LobbyID (+0x1228), AreaID, RegionID, Slot n/64 via debug text 0x006A9DB0 (stub). Slot 30. |
| 0x0046A810 | CCallback_GetSize24 | 6 | L1 | returns 0x18. GetCallbackSizeBytes for LobbyEnter_t and LobbyDataUpdate_t (pack-8 layout: LobbyEnter_t.m_EChatRoomEnterResponse at +0x10). |
| 0x0046A890 | LobbyData_GetFirstMemberJoinTime | 50 | L1 | thiscall(this) -> u64 at first used member record +0x18, else 0. Slot 26. |
| 0x0046A9A0 | LobbyData_ReturnSize32 | 6 | L1 | returns 0x20. Slot 6 (also LeaderboardScoreUploaded size). |
| 0x0046A9C0 | LobbyData_DuelStateMachine | 1056 | L2 | thiscall(this), ret. Only when status==4. Avatar-lobby cabinet/duel SM on +0x3112 (states 0-7, see Flows); frame counters +0x30C8 (180) and +0x30D0 (300). After the switch: on state change resets +0x30CC and request flags; when countdown <1 and no leave command, publishes my member data (+0x30D8, 0x40 bytes) via 0x0041D3D0 and re-arms countdown=60 frames. |
| 0x0046AE00 | LobbyData_BuildUiRows | 46 | L1 | thiscall(this). status==4 only. Copies 64 member records into UI rows (+0x1510, 0x68 stride, flags: seated/side/duel bits 0x100/0x200/0x400/0x800) and the 32 cabinet pair slots (+0x2F10); debug flag 0x16F fills fake "Player%04d" rows. |
| 0x0046B2F0 | LobbyData_PingTick | 601 | L2 | thiscall(this), ret. status==4 and duel state not 5/6. Every 180 frames (+0x4D30) or when my flag byte changes: collect up to 8 random other members from CNetworker table (shuffle 256 times when >7), send ping msg 0x2B0010 with QPC timestamp via packet queue (0x0040A7E0). |
| 0x0046B560 | LobbyData_SyncMembers | 730 | L2 | thiscall(this), ret. status==4. For CNetworker entries flagged "left": clear matching record and the CNetworker entry. For entries flagged "new": take a free record (0x0046C010), copy id/data, mark self, send greeting msg 0x2D0004 to non-self members. Merge member data updates (keep newest by +0x34 seq). If SteamWorkMgr members-dirty: clear flag and resync (0x0041D4C0). |
| 0x0046B840 | LobbyData_SendChat | 210 | L1 | stdcall(a, CSteamID* to), ret 8. Builds msg 0x2E002E with own SteamID and persona name (ISteamFriends::GetPersonaName) and queues it. Slot 33. |
| 0x0046B920 | LobbyData_OnPlayerName | 32 | L1 | stdcall(msg*), ret 8. Adds (id, name) to the recent-name ring. Slot 34. |
| 0x0046B940 | LobbyData_OnPing | 118 | L1 | thiscall(this, msg*, CSteamID* from), ret 8. status==4: reply 0x2C0010 echoing the timestamp. Slot 31. |
| 0x0046B9C0 | LobbyData_OnPong | 205 | L1 | thiscall(this, msg*, from*), ret 8. RTT = (QPC_now - sent)*100/1e6 stored in CNetworker member entry +0x129C; if it was -1 (first sample) sends a ping back. Slot 32. |
| 0x0046BA90 | LobbyData_CmdSelectArea | 99 | L1 | thiscall(this). Command 1: two-frame handshake, marks area entry (+0x5240/44) selected, then command=0. |
| 0x0046BB00 | LobbyData_CmdEnterLobby | 422 | L2 | thiscall(this), ret. Command 2 (+0x3094 step). Step 0: copy enter params (+0x5248..+0x5268) into my member data, CNetworker_EnterAvatarLobby; ok -> step 1 (else stays step 0, retried next frame). Step 1: CNetworker_PollEnterAvatarLobby: >=100 -> step 3; CNetworker state 4 -> wait; 5 -> sync members, +0x309C=3600, step 2; other (10/11) -> status = 3 (if 10) or 2, command done. Step 2: my id -> +0x30A8, status=4, done. Step 3: status=2, CNetworker reset, session reset, done. No timeout in step 1. |
| 0x0046BCC0 | LobbyData_CmdLeaveLobby | 256 | L2 | thiscall(this), ret. Command 3. Step 0: if a cabinet match room is active (+0x4D28 != 0) builds a temporary CSessionJoinObj on the stack with lobby id +0x4D38 and calls 0x0041C1E0 (ISteamMatchmaking::LeaveLobby), destroys it; then CNetworker_LeaveAvatarLobby -> step 1. Step 1: poll; wait while CNetworker state 6; then reset CNetworker + session, command done. |
| 0x0046BDC0 | LobbyData_RequestBackToReady | 25 | L1 | thiscall(this). Only in duel state 3: set flag 8. Slot 19. |
| 0x0046BDE0 | LobbyData_RequestStandUp | 25 | L1 | thiscall(this). Only in duel state 2: set flag 2. Slot 17. |
| 0x0046BE00 | LobbyData_RequestSit | 43 | L1 | thiscall(this, side), ret 4. Only in duel state 0: flag 1, +0x30B4=side. Slot 15. |
| 0x0046BE30 | LobbyData_RequestChallenge | 34 | L1 | thiscall(this). State 2 and no stand-up pending: flag 4. Slot 18. |
| 0x0046BE60 | LobbyData_ResetThunk | 8 | L1 | calls 0x0046A4D0, returns 0. Slot 8. |
| 0x0046BE80 | CNetworkLobbyData_Return100 | 8 | L1 | returns 100, ret 4. Slot 10 default. |
| 0x0046BE90 | LobbyData_RequestEnter | 78 | L2 | thiscall(this, params[9]), ret 4. If no command: copies 9 dwords to +0x5248, command=2 (enter), step 0; else 200. Slot 9. |
| 0x0046BEE0 | LobbyData_RequestLeave | 47 | L2 | thiscall(this). If no command: command=3 (leave); else 200. Slot 12; also auto-called by Update when CNetworker state==8 and status==4. |
| 0x0046BF30 | LobbyData_RequestSelectArea | 76 | L1 | thiscall(this, id[2]), ret 4. command=1. Slot 4. |
| 0x0046BF90 | LobbyData_FindOpponentOnSide | 114 | L1 | thiscall(this, side), ret 4. Earliest-joined used record on that side with duel state not 0/1 (returns record ptr or 0). |
| 0x0046C010 | LobbyData_AllocMemberRecord | 36 | L1 | thiscall(this) -> first free record at +0x3128 (64 x 0x70) or 0 when full (new member silently ignored). |
| 0x0046C040 | LobbyData_SendToMember | 45 | L1 | thiscall(this, a, id*), ret 0xC. status==4 only, else 100. Slot 27. |
| 0x0046C070 | LobbyData_OnMatchRoomCreated | 52 | L2 | thiscall(this, lobbyLo, lobbyHi), ret 8. Duel state 5 only: flag 0x10, +0x30C0/C4 = Steam lobby id of the cabinet match room. Slot 20; called by CNetworkServer create-poll (0x0046D640) when session type==2. |
| 0x0046C0B0 | LobbyData_OnMatchEnded | 25 | L1 | thiscall(this). Duel state 6 only: flag 0x40. Slot 23. |
| 0x0046C0D0 | LobbyData_SetMatchResult | 43 | L1 | thiscall(this, result), ret 4. Duel state 5 only: flag 0x20, +0x30B8=result. Slot 22. |
| 0x0046C100 | LobbyData_SetMyStatus | 63 | L1 | thiscall(this, src[5]), ret 4. status==4: copies flag/time fields into my member data. Slot 28. |
| 0x0046C140 | LobbyData_Update | 156 | L2 | thiscall(this), ret. Per-frame (slot 29, main thread). If CNetworker state==8 and status==4 -> RequestLeave. Then SyncMembers, PingTick, DuelStateMachine, BuildUiRows; dispatch command +0x3090: 1 select area, 2 enter, 3 leave. (+0x3098 pending -> command 4 is dead code: never set.) |
| 0x0046C1E0 | CCallback_CreateObj_LobbyDataUpdate_ctor | 167 | L2 | thiscall(this=createObj+0x78, obj, fn), ret 8. Vtable 0x0089CAF4; registers 505 (LobbyDataUpdate_t), fn 0x0041CCF0. |
| 0x0046C290 | CCallback_Transport_P2PSessionRequest_ctor | 167 | L1 | thiscall(this, obj, fn), ret 8. Registers 1202 (P2PSessionRequest_t) for GAMESTEAM_CNetworkTransport; fn 0x0046F650 = AcceptP2PSessionWithUser(remote) unconditionally. |
| 0x0046C340 | AASTEAM_CSessionCreateObj_ctor | 276 | L2 | thiscall(this), ret. Vtable 0x0089CB04. +4 busy, +8 done, +0xC result, +0x10..+0x28 params (+0x14 session flags, +0x18 max members), +0x2C data count, +0x30 data array (stride 0x44: key ptr, 0x40 value), +0x34, +0x38, +0x3C lobby CSteamID; CCallResult<LobbyCreated_t> at +0x58 (vtable 0x0089CAE4, iCallback +0x60=513); CCallback<LobbyDataUpdate_t> at +0x78. Size 0x8C. |
| 0x0046C460 | AASTEAM_CSessionSearchObj_ctor | 233 | L2 | thiscall(this), ret. Vtable 0x0089CB1C. 50 result entries x 0x14 at +0x34 (vector ctor), CCallResult<LobbyMatchList_t> at +0x420 (vtable 0x0089CB0C, iCallback +0x428=510, handle +0x430), +0x28/+0x2C filter count/array = 0, then reset 0x0041C670. |
| 0x0046C7C0 | GAMESTEAM_CNetworkTransport_RegisterP2P | 189 | L1 | thiscall(this). Registers P2PSessionRequest_t (fn 0x0046F650, accept) and P2PSessionConnectFail_t (fn 0x00429DC0 = no-op). |
| 0x0046CAE0 | CCallResult_LobbyMatchList_dtor | 49 | L1 | thiscall(this). Unregister pending handle. |
| 0x0046CB20 | CCallResult_LobbyCreated_dtor | 49 | L1 | thiscall(this). Unregister pending handle. |
| 0x0046CBC0 | AASTEAM_CSessionCreateObj_dtor | 220 | L2 | thiscall(this), ret. Frees +0x30/+0x34, unregisters LobbyDataUpdate callback and pending LobbyCreated call result. Does NOT LeaveLobby. |
| 0x0046CCA0 | AASTEAM_CSessionSearchObj_dtor | 202 | L2 | thiscall(this), ret. Frees +0x2C, unregisters pending LobbyMatchList call result, destroys 50 entries. |
| 0x0046CE90 | AASTEAM_CSessionCreateObj_ScalarDtor | 33 | L1 | thiscall(this, flags), ret 4. |
| 0x0046CEC0 | AASTEAM_CSessionSearchObj_ScalarDtor | 33 | L1 | thiscall(this, flags), ret 4. |
| 0x0046CEF0 | AASTEAM_SearchResult_ScalarDtor | 34 | L1 | thiscall(this, flags), ret 4. Vtable 0x0089CADC (one 0x14-byte search result entry). |
| 0x0046CF80 | GAMESTEAM_SearchResultNode_ScalarDtor | 94 | L1 | thiscall(this, flags), ret 4. Vtable 0x0089CC34 slot 0. |
| 0x0046E840 | SearchResultNode_GetRankMyArea | 5 | L1 | thiscall(this) -> u16 +0x5C (RANK_MYAREA). Slot 21. |
| 0x0046E850 | SearchResultNode_GetRankAreaFilter | 5 | L1 | -> u16 +0x5E (RANK_AREA_FILTER). Slot 22. |
| 0x0046E860 | SearchResultNode_GetOwnerId | 27 | L1 | thiscall(this, out CSteamID*), ret 4. Copies owner id (claimed "ownerID" metadata) from search entry (+0x114)->+0xC/+0x10. Slot 4. |
| 0x0046E880 | SearchResultNode_GetNetColor | 4 | L1 | -> byte +0x74 (HOST_NETCOLOR, the connection tier shown as delay dots). Slot 7. |
| 0x0046E890 | SearchResultNode_GetRttSlot | 4 | L1 | -> +0x78 (RTT table index, -1 until resolved). Slot 12. |
| 0x0046E8A0 | SearchResultNode_GetRankRttFilter | 8 | L1 | -> u16 +0x10A (RANK_RTT_FILTER). Slot 23. |
| 0x0046E8B0 | SearchResultNode_GetRankHostLevel | 5 | L1 | -> u16 +0x5A (RANK_HOST_LEVEL). Slot 20. |
| 0x0046E930 | SearchResultNode_GetMemberMax | 4 | L1 | -> byte +0x6C (PLAYER_MEMBER_MAX). Slot 8. |
| 0x0046E940 | SearchResultNode_GetNumMembers | 4 | L1 | -> byte +0x6D (GetNumLobbyMembers at parse time). Slot 9. |
| 0x0046E950 | SearchResultNode_GetU16_82 | 8 | L1 | -> u16 +0x82. Slot 18. |
| 0x0046E960 | SearchResultNode_GetU16_80 | 8 | L1 | -> u16 +0x80. Slot 17. |
| 0x0046E970 | SearchResultNode_GetU16_7E | 5 | L1 | -> u16 +0x7E. Slot 16. |
| 0x0046E980 | SearchResultNode_GetDword84 | 17 | L1 | thiscall(this, i), ret 4 -> +0x84 + i*4. Slot 19. |
| 0x0046E9A0 | SearchResultNode_GetU16_7C | 5 | L1 | -> u16 +0x7C. Slot 15. |
| 0x0046E9B0 | SearchResultNode_GetByte60 | 4 | L1 | -> byte +0x60. Slot 5. |
| 0x0046E9C0 | SearchResultNode_GetPrivateMax | 4 | L1 | -> byte +0x6E (PLAYER_PRIVATE_MAX). Slot 10. |
| 0x0046E9D0 | SearchResultNode_GetPrivateNum | 4 | L1 | -> byte +0x6F (PLAYER_PRIVATE_NUM). Slot 11. |
| 0x0046EA70 | SearchResultNode_Slot2_Broken | 12 | L1 | `mov eax,[0]; mov edx,[4]; ret` - reads absolute address 0 (would AV if ever called). Slot 2; no direct callers found. |
| 0x004A2970 | CNetworkLobbyData_GetAreaValue | 114 | L1 | thiscall(this, region, area, idx), ret 0xC. region>=9 -> 0, -1 = flat index, area>3 -> 0; entry = this+8+(area+region*4)*0xA8; returns -1 if entry unused else entry+0x10+idx*0x10. Slot 2. |
| 0x004A2A00 | CNetworkLobbyData_Return10 | 6 | L1 | returns 10. Slot 7 (entries per area). |
| 0x004A2A10 | CNetworkLobbyData_GetAreaEntry | 51 | L1 | thiscall(this, region, area), ret 8 -> this+8+index*0xA8. |
| 0x004A2A50 | CNetworkLobbyData_SumArea | 75 | L1 | thiscall(this, region, area), ret 8. Sums slot-2 values over 10 indices, -1 on any error. Slot 1. |
| 0x004A2AA0 | CNetworkLobbyData_AreaIndex | 39 | L1 | cdecl(region, area) -> area + region*4 with the same clamping. |
| 0x00506AB0 | Stub_ReturnTrue_Ret4 | 8 | L1 | mov eax,1; ret 4. Shared stub (vtable slot 12). |
| 0x00516A20 | Stub_ReturnFalse_Ret4 | 5 | L1 | xor eax,eax; ret 4. Shared stub. |
| 0x00668180 | Stub_ReturnFalseByte | 3 | L1 | xor al,al; ret. SearchResultNode slot 6. |
| 0x006A9DA0 | Stub_ReturnZero | 3 | L1 | xor eax,eax; ret. Shared vtable default (56 callers). |
| 0x006A9DB0 | Stub_Nop | 1 | L1 | ret. Shared no-op (790 callers) - includes the release-build debug print. |
| 0x0070E400 | SearchResultNode_GetValid | 4 | L1 | mov eax,[ecx+0Ch]; ret. Slot 1 (set to 1 after parse). |
| 0x00751460 | Stub_Ret8 | 3 | L1 | ret 8, no-op. AA_CNetworker slot 21 default. |
| 0x007587D0 | SearchResultNode_GetRoomName | 4 | L1 | lea eax,[ecx+10h]; ret -> wchar room name (PLAYER_ROOM_NAME). Slot 3. |
| 0x007D8B48 | DirectSoundCaptureEnumerateA_thunk | 6 | L1 | import thunk (jmp [IAT]). Used by voice chat device enumeration. |
| 0x008480B0 | AASTEAM_CNetworker_atexit | 163 | L1 | atexit destructor for 0x00A25788: SteamWorkMgr shutdown, deletes join-request CCallback (unregister), restores AA vtable. No LeaveLobby here. |
| 0x00848550 | CSTEAMNetworkLobbyData_atexit | 76 | L1 | atexit destructor for 0x00A5D270 (restores base vtable only). |

### Out-of-list functions read for this report (they are the actual Steam-lobby calls; owned by other clusters or unclustered)

| VA | Name | Size | Level | Notes |
|---|---|---|---|---|
| 0x0041BF90 | CNetworker_CreateLobby (part C list) | 240 | L2 | thiscall(this=CNetworker, CSessionCreateObj* obj), ret 4. obj busy -> 0x66; call result already pending (+0x68/+0x6C != 0) -> 100. Clears busy/done/result, clears hDCODE table #0 (+0x3640). Lobby type: `obj+0x14 & 0xF` == 0 (ranked) or 2 (cabinet match) -> 3 = k_ELobbyTypeInvisible (searchable, hidden from friends); anything else (player room) -> 2 = k_ELobbyTypePublic. ISteamMatchmaking::CreateLobby(type, maxMembers = obj+0x18) (mm+0x34). Unregister old handle, store new at +0x68, obj +0x70, fn 0x0041CB90; registers only if handle != 0. busy=1 ALWAYS. VoiceChat start (0x00425000: ISteamUser::StartVoiceRecording). Returns 0. |
| 0x0041C110 | CNetworker_JoinLobby (part C list) | 208 | L2 | thiscall(this=CNetworker, CSessionJoinObj* obj, int unused), ret 8. busy -> 0x66. Clears hDCODE table #1 (+0x3840). JoinLobby(obj+0x20) (mm+0x38); handle -> +0x48, fn 0x0041CE10; register if != 0; busy=1 always; voice start. Returns 0. |
| 0x0041C270 | CNetworker_RequestLobbyList (part C list) | 625 | L2 | cdecl(CSessionSearchObj* obj), ret 4. busy -> return. AddRequestLobbyListStringFilter("ownerID", "%lld" of own SteamID, 3=NotEqual) (mm+0x14); AddRequestLobbyListDistanceFilter(map obj+0x20: 0->0 Close, 2->2 Far, 3->3 Worldwide, else 1 Default) (mm+0x24); for each of obj+0x28 filters (array obj+0x2C, stride 0x4C: key ptr, value char[0x40] at +4, type +0x44, cmp +0x48): type 0 -> NearValueFilter(key, atoi) (mm+0x1C); type 1 -> NumericalFilter(key, atoi, cmp') (mm+0x18); type 2 -> StringFilter(key, value, cmp') (mm+0x14). cmp' map: 0->-1 LessThan, 1->1 GreaterThan, 2->0 Equal, 3->-2 EqualOrLess, 4->2 EqualOrGreater, 5->3 NotEqual. RequestLobbyList() (mm+0x10); handle -> +0x430, fn 0x0041CF40; busy=1 always. |
| 0x0041CB90 | CSessionCreateObj_OnLobbyCreated | 345 | L2 | thiscall(this=createObj, LobbyCreated_t* p, bool ioFailure (ignored)), ret 8. If busy==0 (stale, unreachable in practice): LeaveLobby(p->lobby), clear data list, clear current-lobby id in CNetworkServer (0x0046E8C0/0x0046FCA0 with 0). Else if p->m_eResult==1 (k_EResultOK): obj+0x3C = lobby id, CNetworkServer current lobby = id, SetLobbyData("ownerID", "%lld" own id), SetLobbyData("ownerName", GetPersonaName()), SetLobbyData for every queued key (0x0041D420), result=0. Else result=100 (EResult discarded, nothing to leave). busy=0, done=1. |
| 0x0041CE10 | CSessionJoinObj_OnLobbyEnter | 252 | L2 | thiscall(this=joinObj, LobbyEnter_t* p, bool ioFailure (ignored)), ret 8. If busy==0 (unreachable): LeaveLobby(p->lobby), clear arrays, clear current lobby. Else if p->m_EChatRoomEnterResponse (+0x10) == 1: obj+0x20 = lobby, obj+0x28 = GetLobbyOwner(lobby) (mm+0x8C), current lobby = id, result 0. Any other response (2..11) -> result 100, response value discarded. busy=0, done=1. m_bLocked / permissions ignored. |
| 0x0041CF40 | CSessionSearchObj_OnLobbyMatchList | 410 | L2 | thiscall(this=searchObj, LobbyMatchList_t* p, bool ioFailure (ignored)), ret 8. busy==0 -> reset results (unreachable). Else for i < p->m_nLobbiesMatching: GetLobbyByIndex(i) (mm+0x30) -> entry[n].lobby (+0x38+n*0x14), GetLobbyData("ownerID") -> _atoi64 -> entry owner (+0x40), GetLobbyData("ownerName") -> recent-name ring. Writes up to m_nLobbiesMatching entries into a 50-entry array with no bound check. result=0 always (even on IO failure). |
| 0x0041CCF0 | CSessionCreateObj_OnLobbyDataUpdate | 136 | L1 | thiscall(this, LobbyDataUpdate_t*), ret 4. If lobby == obj+0x3C, success, member != lobby, member valid: GetLobbyMemberData(lobby, member, "hDCODE") (mm+0x60); non-empty -> record member in CNetworker hDCODE table #0. No call to SetLobbyMemberData exists in the binary, so this never fires usefully. |
| 0x0041CD80 | CSessionJoinObj_OnLobbyDataUpdate | 136 | L1 | Same for join object (obj+0x20), table #1. |
| 0x0041D480 | CNetworker_RecordDcodeMember | 56 | L1 | thiscall(this, kind, idLo, idHi), ret 0xC. Writes the id into EVERY empty slot of the 64-slot table (loop has no break). |
| 0x0041D420 | CSessionCreateObj_ApplyLobbyData | 91 | L1 | thiscall(this=createObj). For each queued (key,value) (count +0x2C, array +0x30 stride 0x44): SetLobbyData(obj+0x3C, key, value) (mm+0x50); return values ignored. Also re-run on room edits by 0x0046FFF0. |
| 0x0041C620 | CSessionCreateObj_ClearData | 80 | L1 | Clears +0x10..+0x28 and the data array. |
| 0x0041C080 | CSessionCreateObj_Leave | 134 | L2 | cdecl(createObj), ret 4. busy -> 0x66 WITHOUT leaving. Else LeaveLobby(obj+0x3C) (mm+0x3C), id=0, current lobby=0, done=1, result 0, voice stop (0x00425070: StopVoiceRecording). Synchronous. |
| 0x0041C1E0 | CSessionJoinObj_Leave | 134 | L2 | cdecl(joinObj, int unused), ret 8. Same as above for obj+0x20. |
| 0x0041C5F0 | CSessionObj_Poll | 43 | L2 | cdecl(obj), ret 4. busy -> -100; done -> result (0 ok / 100 fail); neither -> 100. |
| 0x0041CAC0 | CNetworker_OnGameLobbyJoinRequested | 200 | L2 | member fn(GameLobbyJoinRequested_t* p), ret 4. Stores p->m_steamIDLobby into CNetworker+0x3A44/48 (globals 0x00A291CC/D0) and calls vtable slot 21 (0x0041D3C0) -> +0x1218 = 3. No state check, no validation, overwrites any previous request. m_steamIDFriend ignored. |
| 0x00421DA0 | CCallResult_Run | 53 | L2 | (in list) handle compare -> handlers above. |
| 0x0046F650 | Transport_OnP2PSessionRequest | 37 | L1 | ISteamNetworking::AcceptP2PSessionWithUser(remote) (net+0x0C) for anyone. |
| 0x0044F1E0 | MainLoop | 238 | L1 | PeekMessage loop; when idle runs one game frame then SteamAPI_RunCallbacks() - all lobby callbacks are dispatched here, on the main thread, once per frame. |

## Data

### Steam interface offsets used (CSteamAPIContext from SteamInternal_ContextInit(0x009D3210))
context +0x04 ISteamUser, +0x08 ISteamFriends, +0x0C ISteamUtils, +0x10 ISteamMatchmaking, +0x14 ISteamUserStats,
+0x20 ISteamNetworking.

| Interface | Offset | Method | Call sites |
|---|---|---|---|
| ISteamMatchmaking | 0x10 | RequestLobbyList | 0x0041C270 |
| ISteamMatchmaking | 0x14 | AddRequestLobbyListStringFilter | 0x0041C270 (ownerID != me; type-2 filters) |
| ISteamMatchmaking | 0x18 | AddRequestLobbyListNumericalFilter | 0x0041C270 |
| ISteamMatchmaking | 0x1C | AddRequestLobbyListNearValueFilter | 0x0041C270 (supported, no caller uses type 0) |
| ISteamMatchmaking | 0x24 | AddRequestLobbyListDistanceFilter | 0x0041C270 |
| ISteamMatchmaking | 0x30 | GetLobbyByIndex | 0x0041CF40 |
| ISteamMatchmaking | 0x34 | CreateLobby | 0x0041BF90 |
| ISteamMatchmaking | 0x38 | JoinLobby | 0x0041C110 |
| ISteamMatchmaking | 0x3C | LeaveLobby | 0x0041C080, 0x0041C1E0, 0x0041CB90 (stale), 0x0041CE10 (stale) |
| ISteamMatchmaking | 0x44 | GetNumLobbyMembers | 0x0046FCC0 (search result parse, part C) |
| ISteamMatchmaking | 0x4C | GetLobbyData | 0x0041CF40 (ownerID, ownerName), 0x0046E660 (PLAYER_PRIVATE_NUM) |
| ISteamMatchmaking | 0x50 | SetLobbyData | 0x0041CB90, 0x0041D420, 0x0046E660 |
| ISteamMatchmaking | 0x54/0x58 | GetLobbyDataCount / GetLobbyDataByIndex | 0x0046FCC0 (all keys of each result) |
| ISteamMatchmaking | 0x60 | GetLobbyMemberData("hDCODE") | 0x0041CCF0, 0x0041CD80 |
| ISteamMatchmaking | 0x8C | GetLobbyOwner | 0x0041CE10, 0x0046D0A0, 0x0046D370 |
| ISteamMatchmaking | - | never called: AddRequestLobbyListFilterSlotsAvailable (0x20), ResultCountFilter (0x28), CompatibleMembersFilter (0x2C), InviteUserToLobby (0x40), GetLobbyMemberByIndex (0x48), DeleteLobbyData, SetLobbyMemberData (0x64), chat msgs, RequestLobbyData (0x70), SetLobbyMemberLimit (0x7C), SetLobbyType (0x84), SetLobbyJoinable (0x88), SetLobbyOwner | |
| ISteamUser | 0x04 | BLoggedOn | 0x0041C5C0 |
| ISteamUser | 0x08 | GetSteamID | many |
| ISteamUser | 0x1C/0x20/0x24/0x28 | Start/StopVoiceRecording, GetAvailableVoice, GetVoice | voice chat (0x00425000, 0x00425070, 0x00424D40) |
| ISteamFriends | 0x00 | GetPersonaName | 0x0041CB90 (ownerName), 0x0041C8B0, 0x0046B840 |
| ISteamUtils | - | not used by this layer | |
| ISteamNetworking | 0x00 | SendP2PPacket(id, buf, len, 0=Unreliable, channel 0) | 0x0041D260 |
| ISteamNetworking | 0x04/0x08 | IsP2PPacketAvailable / ReadP2PPacket (channel 0, 0x4F2-byte buffer) | 0x0041D0E0 |
| ISteamNetworking | 0x0C | AcceptP2PSessionWithUser | 0x0046F650 |

Steam callbacks registered by the whole game: 333 GameLobbyJoinRequested_t, 505 LobbyDataUpdate_t (x2: create/join
objects), 1202 P2PSessionRequest_t (x2: transport, voice), 1203 P2PSessionConnectFail_t (x2: transport no-op, voice),
1005 DlcInstalled_t, 1101-1103 UserStats. NOT registered: 506 LobbyChatUpdate_t, 512 LobbyKicked_t, 507 LobbyChatMsg_t,
337 GameRichPresenceJoinRequested_t. Call results: 513 LobbyCreated_t, 504 LobbyEnter_t, 510 LobbyMatchList_t.

Callback struct sizes as compiled (pack 8): LobbyCreated_t 0x10 (EResult +0, lobby +8); LobbyEnter_t 0x18 (lobby +0,
permissions +8, locked +0xC, response +0x10); LobbyDataUpdate_t 0x18 (lobby +0, member +8, success +0x10);
LobbyMatchList_t 4; GameLobbyJoinRequested_t 0x10 (lobby +0, friend +8).

### Lobby metadata keys
Key table 0x009D9610 (18 ptrs, used by host and by result parser 0x0046FCC0): 0 NETWORK_VERSION, 1 MATCHING_FILTER,
2 HOST_NETCOLOR, 3 PLAYER_ROOM_NAME, 4 PLAYER_ROOM_TYPE, 5 PLAYER_MEMBER_MAX, 6 PLAYER_PRIVATE_NUM, 7 PLAYER_PRIVATE_MAX,
8 PLAYER_SESSION_FLAG, 9 PLAYER_SESSION_VALUE, 10 RANK_MYAREA, 11 RANK_AREA_FILTER, 12 RANK_HOST_LEVEL,
13 RANK_LEVEL_MIN, 14 RANK_LEVEL_MAX, 15 RANK_RTT_FILTER, 16 RANK_DISCONNECT_RATE, 17 RANK_DISCONNECT.
Search-side copy at 0x009D9658: NETWORK_VERSION, MATCHING_FILTER, PLAYER_ROOM_TYPE, (0x9664 unused), RANK_LEVEL_MIN,
RANK_LEVEL_MAX, RANK_DISCONNECT. Plain keys "ownerID" (0x0084FE7C), "ownerName" (0x0084FE84); member key "hDCODE".
All values are decimal strings ("%d") except PLAYER_ROOM_NAME ("%s"), ownerID ("%lld").

Written by host (queued by CNetworkServer 0x0046E0A0 from a session-params block P, applied in the LobbyCreated
handler, after ownerID/ownerName):
- all types: NETWORK_VERSION (P+0x110), HOST_NETCOLOR (P+0x100)
- ranked (P+0x108 & 0xF == 0): MATCHING_FILTER (P+0x138), RANK_MYAREA (+0x132), RANK_AREA_FILTER (+0x134),
  RANK_HOST_LEVEL (+0x130), RANK_LEVEL_MIN (+0x144), RANK_LEVEL_MAX (+0x148), RANK_RTT_FILTER (+0x136),
  RANK_DISCONNECT_RATE (+0x150), RANK_DISCONNECT (+0x14C)
- cabinet match (== 2): MATCHING_FILTER (+0x154), RANK_RTT_FILTER, PLAYER_MEMBER_MAX (+0xFC), PLAYER_SESSION_FLAG (+0x108)
- player room (other): MATCHING_FILTER (+0x114), PLAYER_ROOM_NAME, PLAYER_ROOM_TYPE (+0x118), PLAYER_MEMBER_MAX (+0xFC),
  PLAYER_PRIVATE_NUM (+0xFE), PLAYER_PRIVATE_MAX (+0xFD), PLAYER_SESSION_FLAG (+0x108), PLAYER_SESSION_VALUE (+0x10C)
- updated while hosting a room: whole room set re-applied by 0x0046FFF0; PLAYER_PRIVATE_NUM refreshed by 0x0046E660.
Max members = P+0xFC (2 for ranked per mod logs "members=1/2").

### Search filters (CNetworkServer 0x0046F940 builds, 0x0041C270 applies; S = search params block)
- always: ownerID != my SteamID (string NotEqual); distance from S+0xF0 (ranked) / S+0xD8 (room): 0 -> Worldwide,
  1 -> Far, else -> Default; NETWORK_VERSION == S+0xCD.
- ranked (S+0xCC == 0): MATCHING_FILTER == S+0xEC; RANK_LEVEL_MIN >= S+0xF8; RANK_LEVEL_MAX <= S+0xFC;
  if S+0x104 == 1: RANK_DISCONNECT != 1.
- player room: MATCHING_FILTER == S+0xD0; if S+0xD4 != 0: PLAYER_ROOM_TYPE == S+0xD4.
- no slots-available filter, no result-count filter (Steam default cap, 50 = size of the result array), no
  near-value filter. Client-side post filter only for rooms (0x0046D890: netcolor, room type bits, member max,
  private flag); ranked results are all kept.

### AASTEAM_CSessionCreateObj (0x8C, vtable 0x0089CB04; instance at CNetworkServer+0xBC0)
+0x04 busy, +0x08 done, +0x0C result (0 ok, 100 fail), +0x14 session flags (low nibble: 0 ranked, 1 room, 2 cabinet
match), +0x18 max members, +0x2C data count, +0x30 data array (0x44: key ptr + char[0x40]), +0x3C lobby CSteamID,
+0x58 CCallResult<LobbyCreated_t> (+0x60 id 513, +0x68 handle, +0x70 obj, +0x74 fn 0x0041CB90),
+0x78 CCallback<LobbyDataUpdate_t> (+0x84 obj, +0x88 fn 0x0041CCF0).

### AASTEAM_CSessionJoinObj (0x70, vtable 0x0089C884; instance at CNetworkServer+0xC50)
+0x04 busy, +0x08 done, +0x0C result, +0x10 count, +0x16 short n, +0x18/+0x1C arrays, +0x20 lobby CSteamID,
+0x28 owner CSteamID (GetLobbyOwner), +0x30, +0x38 CCallResult<LobbyEnter_t> (+0x40 id 504, +0x48 handle, +0x50 obj,
+0x54 fn 0x0041CE10), +0x58 CCallback<LobbyDataUpdate_t> (+0x64 obj, +0x68 fn 0x0041CD80).
CNetworkServer wraps it with: +0xC70/74 target lobby (copied into obj+0x20 by layout), +0xC80 1 = invite join,
+0x25E0 join state byte.

### AASTEAM_CSessionSearchObj (>=0x440, vtable 0x0089CB1C; instance at CNetworkServer+0xCC0)
+0x04 busy, +0x08 done, +0x0C result, +0x20 distance code, +0x24 filter capacity, +0x28 filter count, +0x2C filter
array (0x4C), +0x30 result count, +0x34 50 x AASTEAM_SearchResult (0x14: vtable, +4 lobby id, +0xC owner id from
metadata), +0x420 CCallResult<LobbyMatchList_t> (+0x428 id 510, +0x430 handle, +0x438 obj, +0x43C fn 0x0041CF40).

### CCallResult / CCallback layout (Steam SDK)
CCallResult: +0 vtable, +4 flags, +8 iCallback, +0x10/+0x14 SteamAPICall_t, +0x18 obj, +0x1C member fn.
CCallback: +0 vtable, +4 flags (bit0 registered), +8 iCallback, +0xC obj, +0x10 member fn.

### AASTEAM_CNetworker (singleton 0x00A25788, vtable 0x0084FF54)
+0x1194 flag, +0x1218 join-request flag (3 = invite pending), +0x1220 avatar-lobby state (1 idle, 4 entering,
5 in lobby, 6 leaving, 10/11 enter variants, 7/8/9 others; 8 forces leave), +0x1228 lobby id, +0x1230/34 my SteamID,
+0x1238 64 x 0x68 member table (+0 used, +4 new, +8 left, +0x10 id, +0x18 0x40 data, +0x64 RTT, -1 unknown),
+0x2C38 slots (64), +0x2C3A members, +0x2C3C region, +0x2C40 area, +0x2C48 send scratch, +0x313A P2P read buffer
(0x4F2), +0x3640/+0x3840 two 64 x CSteamID hDCODE tables, +0x3A40 CCallback<GameLobbyJoinRequested_t>*,
+0x3A44/48 invited lobby id.
Vtable 0x0084FF54: 0 dtor, 1 Init, 2 Shutdown, 3 FlushSendQueue, 4 Update, 5 PumpWorkMgr, 10/11 IsLoggedOn,
12 true, 15 GetPersonaName, 16 GetSlot1184, 17 GetFlag1194, 20 GetJoinRequestFlag, 21 SetJoinRequestFlag,
22 ClearJoinRequestFlag, others stubs.

### CSTEAMNetworkLobbyData (singleton 0x00A5D270, getter 0x0046A820, vtable 0x0089C88C) - avatar Network Lobby
+0x08 32 x 0xA8 area/region table, +0x1510 64 x 0x68 UI rows, +0x2F10 32 x 0xC cabinet pairs, +0x3090 command
(1 select area, 2 enter, 3 leave), +0x3094 command step, +0x3098 pending (never set), +0x309C 3600 timer set on
enter, +0x30A0 status (0 none, 2 failed/left, 3 failed variant, 4 in lobby), +0x30A8 my id, +0x30B0 duel request
bits (1 sit, 2 stand, 4 challenge, 8 back, 0x10 match room id, 0x20 result, 0x40 match ended), +0x30B4 requested side,
+0x30B8 result, +0x30C0 match lobby id param, +0x30C8 seat timer, +0x30CC member-data broadcast countdown (60),
+0x30D0 post-match timer, +0x30D8 my member data (0x40), +0x30F0 opponent id, +0x30F8 my join date,
+0x3100 match lobby id, +0x310C loss counter, +0x3111 my side, +0x3112 duel state, +0x3113 result,
+0x3128 64 x 0x70 member records (+4 used, +8 self, +0x10 id, +0x20 RTT, +0x30 0x40 data mirror: +0x48 target id,
+0x50 join date, +0x65 side, +0x66 seated, +0x6A duel state), +0x4D28 match type, +0x4D30 ping timer (180),
+0x4D38 match Steam lobby id, +0x5240 area select, +0x5248 enter params (9 dwords).
P2P message ids (via packet queue): 0x2B0010 ping, 0x2C0010 pong, 0x2D0004 greeting, 0x2E002E name.

### Globals
0x00A25788 CNetworker, 0x00A291D8 its init guard; 0x00A291E0 recent-name ring (64 x 0x30, index 0x00A29DE0, guard
0x00A29DE4); 0x00A5A050 SteamWorkMgr (parts A/B); 0x00A5D270 CSTEAMNetworkLobbyData (guard 0x00A624E0);
0x00A62730 GAMESTEAM_CNetworkServer (0x0046E8C0); 0x00A129B0 packet manager; 0x00A29E3C voice chat manager;
0x00A29E00/0x00A29E10 ranking reader/writer.

## Flows

### Create (host) - all on main thread
1. CNetworkServer 0x0046E0A0 (only if create state byte +0x25DD == 0): clear data list, queue keys, max members,
   CreateLobby via 0x0041BF90. Return 0 -> +0x25DD = 1.
2. Wait: 0x0046D640 polls 0x0041C5F0 every frame: -100 pending (no timeout); >=100 -> +0x25DD = 0, returns 100
   (UI shows failure); 0 -> state 3 (hosting). For cabinet matches (type 2) also LobbyData slot 20 with the lobby id.
3. LobbyCreated handler sets ownerID, ownerName and all keys. Lobby is Invisible (ranked, cabinet) or Public (room).
4. Hosting: metadata refreshes (0x0046FFF0, 0x0046E660). Close: 0x0046E440 (state 3, not busy) -> LeaveLobby, send
   0x370004 three times unreliable to members, state 4; 0x0046D6C0 waits state 4 -> 0.
5. No path ever marks the lobby non-joinable or changes its type while a match is running.

### Search (ranked list / room list)
1. 0x0046F940 (search state +0x25DF == 0, obj not busy): build filters, 0x0041C270, state 1.
2. 0x0046D890 polls: pending -> keep waiting (no timeout); >=100 -> state 0; 0 -> for each entry build a
   GAMESTEAM_SearchResultNode (0x0046FCC0 reads every lobby key + GetNumLobbyMembers), room post-filter, list count
   +0xAE8, state 0. The ranked list rows are these nodes; RTT/netcolor tiers resolved later (part C).

### Search result -> join
1. UI picks a node; 0x0046DFA0 copies node(+0x114)->lobby id into the join target (+0xC70) (0x0046DF20 = join by a
   given lobby id, 0x0046DE70 = join the invited lobby from CNetworker+0x3A44, flag +0xC80=1).
2. 0x0041C110 JoinLobby; state +0x25E0 = 1 (search/direct) or 6 (invite).
3. 0x0046D370 / 0x0046D0A0 per frame: poll; pending -> wait (no timeout); >=100 -> state 0 (all EChatRoomEnterResponse
   values identical); 0 -> GetLobbyOwner; if valid -> send room-join request (channel 8, msg 10) to owner, state 2;
   if owner invalid -> state 5 WITHOUT LeaveLobby.
4. State 2: waits for reply 0x33; if the request object reports type 0x33 with counter >= 0x709 (1801 frames, 30 s)
   -> send cancel (8, 0x33), LeaveLobby, state 5. State 3 (accepted, set by message handler) -> state 7 (in room).
   State 4 (refused, msg 0x34) -> cancel + LeaveLobby -> 5. State 5 -> 0 once the join object is idle.
5. Leaving the room: 0x0046E540 (state 7) LeaveLobby + 0x360004 x3; cleanup 0x0046D010 leaves only in states
   2/3/4/7 (join) and 3 (create).

### Invite / join friend
GameLobbyJoinRequested_t -> 0x0041CAC0: store lobby id at CNetworker+0x3A44, +0x1218 = 3. The UI (part F) polls
vtable slot 20, clears with slot 22, then calls 0x0046DE70 -> join flow above with +0xC80=1 (initial state 6).
Cold start "+connect_lobby <id>" is parsed by WinMain helper 0x0044EB10. Invite sending is never done by the game
(no InviteUserToLobby / ActivateGameOverlayInviteDialog); invites come from the Steam overlay/friends list.

### Avatar Network Lobby (CSTEAMNetworkLobbyData) - not Steam matchmaking
Commands: enter (0x0046BB00): CNetworker_EnterAvatarLobby -> SteamWorkMgr strategy 4 -> poll until workmgr state
leaves 0 (no timeout) -> status 4. Leave (0x0046BCC0): if a cabinet match Steam lobby is recorded (+0x4D28),
LeaveLobby it via a temporary join object; SteamWorkMgr strategy 5; wait; reset.
Per frame (0x0046C140): member sync (greetings), random pings every 180 frames to <=8 members, duel SM, UI rows,
member-data broadcast every 60 frames or on duel state change (SteamWorkMgr strategy 6).
Duel/cabinet SM (+0x3112): 0 idle -(sit, no older same-side rival)-> 1 seated; 1 -> 0 if an older same-side rival
appears or the other-side occupant is not in state 2, -> 2 after 180 frames; 2 ready: stand -> 0, challenge -> 4 if
opponent in 3/4 else 3; 3 waiting: opponent 3/4 -> 4, back -> 2, opponent gone -> 2; 4: both targets match each
other and opponent in 4/5 -> 5, opponent back to 2 -> 3, gone -> 2; 5 matched: receives match Steam lobby id
(flag 0x10, from the cabinet CreateLobby, type 2 Invisible) and result (0x20) -> 6; 6: match ended (0x40) -> 7
(or 2 if result 3); 7: 300 frames, then 0 (result 0/2) or 2 (result 1, +0x310C++).

### Threads
All ISteamMatchmaking/ISteamNetworking calls in this layer and all their handlers run on the main thread
(game frame and SteamAPI_RunCallbacks in 0x0044F1E0). Callbacks run once per loop iteration after the frame and
only when the Windows message queue is empty. AASTEAM_CAsyncTaskThread workers (2, owned by
AASTEAM_CRankingThreadManager) run only leaderboard/replay UGC jobs; they never touch lobby objects.

## Failure paths

| Where | Condition | Cause side | What happens / user sees | Recovers? |
|---|---|---|---|---|
| 0x0041BF90 / 0x0041C110 / 0x0041C270 | object busy (+4) | local | returns 0x66; CNetworkServer returns 0x66 to the caller | only after the pending result arrives |
| 0x0041BF90 | CCallResult handle already non-zero | local | returns 100 | - |
| same three | Steam returns k_uAPICallInvalid (not logged on, Steam IPC gone) | Steam / local | handle 0 not registered, busy=1 forever; poll -100 forever | NO - object stuck until process restart |
| 0x0041CB90 | m_eResult != 1 (limit exceeded, timeout, access denied...) | Steam | result 100, reason lost; CNetworkServer create state -> 0 | caller may retry |
| 0x0041CE10 | response 2 (doesn't exist) | server/peer (host left) | result 100 | no retry |
| 0x0041CE10 | response 4 (full) | stale list (no slots filter) | result 100, same message as above | no retry |
| 0x0041CE10 | response 5 (error), 6 banned, 7 limited account, 8/9 bans, 10/11 blocked | Steam / account | result 100, same message | no |
| 0x0041CE10 | ioFailure=true | Steam | flag ignored; struct contents decide (normally response != 1 -> fail) | - |
| 0x0041CF40 | ioFailure=true | Steam | flag ignored; m_nLobbiesMatching used as is; result 0 = "empty/garbage list" treated as success | - |
| 0x0046D370 / 0x0046D0A0 state 1 | joined, but GetLobbyOwner invalid | Steam/host | state 5 -> 0 without LeaveLobby (UI sees failure, client remains a member of that Steam lobby) | NO leave |
| state 2 | no reply 0x33 within 1801 frames (~30 s) | peer/P2P | cancel + LeaveLobby -> 5 -> 0 ("failed to connect") | yes |
| state 4 (msg 0x34) | host refused | peer logic | cancel + LeaveLobby -> 5 -> 0 | yes |
| 0x0041D0E0 | inbound packet buffer full | peer flood / long stall | infinite loop at 0x0041D1E0 - game freezes | NO |
| 0x0046BB00 step 1 | workmgr state 0xB | uei server | status 2, "failed to enter lobby" | yes |
| 0x0046BB00 step 1 | workmgr never leaves 0 | server/network | waits forever (no timeout here) | depends on part A |
| 0x0046BCC0 | match-room leave via temp join obj | - | LeaveLobby always issued (fresh object, never busy) | yes |
| 0x0041AC30/0x0041AD90 | async thread does not stop in 3000 ms | local | "Thread Delete Time Out" message box (exit path) | - |
| P2PSessionConnectFail_t | any P2P failure | peer/NAT | handler 0x00429DC0 does nothing; game waits for its own 30 s timeouts | via timeouts only |

## Suspected bugs

1. **In-flight join/create completes after the state machine gave up -> ghost lobby / phantom member.**
   Evidence: busy (+4) is cleared only in 0x0041CB90/0x0041CE10; leave wrappers 0x0041C080/0x0041C1E0 return 0x66
   when busy and do not leave; the cleanup 0x0046D010 only leaves when join state is 2/3/4/7 or create state is 3,
   so states 1/6 (JoinLobby pending) and create state 1 (CreateLobby pending) are skipped; the reset 0x0046DD20
   zeroes the state bytes (+0x25DC..+0x25E7) but never touches the session objects. When the result then arrives,
   busy is still 1, so the handler treats it as a fresh success: CreateLobby -> a lobby with ownerID/ranked keys that
   no state machine hosts (other players see it in the ranked list, join, get no 0x33 reply and time out after 30 s:
   "failed to connect"); JoinLobby -> we stay a Steam member of someone's room (host sees a member that never talks;
   host-side stale pair). Symptoms: "failed to connect" on listed rooms, "client stays in lobby", stale rooms.
   Confidence: medium (depends on whether part C/F can trigger cleanup/reset while state is 1/6, e.g. backing out of
   the "Connecting..." popup or leaving the ranked menu during a search). Mod fix: on cleanup/reset, if a join/create
   handle is pending, remember it and LeaveLobby the lobby from the late LobbyEnter_t/LobbyCreated_t (hook the
   handlers at 0x0041CE10/0x0041CB90 or the mod's existing CCallResult proxy).
2. **Invalid SteamAPICall handle wedges the session object forever.** 0x0041BF90/0x0041C110/0x0041C270 set busy=1 even
   when the returned handle is 0. Every later create/join/search returns 0x66 and polls return -100 ("connecting"
   forever) until restart. Confidence: high that the code does this; trigger frequency unknown (Steam logged off /
   IPC failure). Fixable: hook the wrappers and clear busy / report 100 when the handle is 0.
3. **Join failure reason discarded.** 0x0041CE10 maps every EChatRoomEnterResponse != 1 to 100; CNetworkServer maps 100
   to state 0. A room that still exists but is full (4), rate-limited (5), or blocked (10/11) produces the same UI as
   "room no longer exists". Confidence: high. Fixable (the mod already sees LobbyEnter_t; it can surface the code).
4. **Joined but owner invalid -> no LeaveLobby.** 0x0046D370/0x0046D0A0 state 1: if GetLobbyOwner is invalid the code
   jumps to state 5 without calling 0x0041C1E0. Client remains in the Steam lobby. Confidence: high for the code path,
   low for frequency (owner invalid right after a successful enter). Fixable.
5. **No membership callbacks.** LobbyChatUpdate_t/LobbyKicked_t are never registered; leave/close notifications
   (0x360004, 0x370004) are unreliable P2P sends repeated 3 times with no ack, and P2PSessionConnectFail_t is a no-op
   for the transport. A crashed or disconnected joiner stays in the host's game-side room until some higher timeout;
   matches the documented "ranked stale room" symptom. Confidence: medium-high. Fixable: the mod already wraps
   LobbyChatUpdate_t and can drive the room cleanup from it.
6. **Hosts stay searchable while busy.** No SetLobbyJoinable(false)/SetLobbyType/metadata change when a ranked match
   starts or when a room is closing; searchers have no slots-available filter. Lobbies of hosts who are mid-match
   (or full) can be listed and joined, giving full/refused/timeout failures. Confidence: medium (Steam may hide full
   lobbies by default; not verified). Fixable: SetLobbyJoinable on match start/end from the mod, or add
   AddRequestLobbyListFilterSlotsAvailable(1) in the RequestLobbyList wrapper.
7. **Infinite loop on full inbound packet buffer** (0x0041D1E0 `jmp 0041D1E0`). Any burst of channel-0 packets larger
   than the packet manager capacity (set in 0x004725B0) during one drain freezes the main thread forever. Confidence:
   high for the code, unknown trigger rate (a long frame stall while a peer keeps sending, or a malicious peer).
   Fixable: patch the jump to break out / drop the packet.
8. **Search handler trusts m_nLobbiesMatching.** 0x0041CF40 writes up to that many 0x14-byte entries into a 50-entry
   array and ignores ioFailure. Safe with Steam's default 50 cap; an IO-failed result or a future result-count filter
   > 50 would overflow the search object (and CNetworkServer). Confidence: low impact. Fixable (clamp).
9. **hDCODE member-data plumbing is dead / buggy.** Nothing calls SetLobbyMemberData; 0x0041D480 would fill every free
   slot with the same id. Harmless today.
10. **Search result owner = claimed metadata, not GetLobbyOwner.** Results carry owner from "ownerID" lobby data
    (0x0041CF40); if host ownership migrated (Steam reassigns owner when the creator leaves but another member
    remains), the row still shows the old owner and the game P2P-requests the real owner only after joining.
    Confidence: low-medium. Fixable (read GetLobbyOwner in the mod's list filter).
11. **SearchResultNode vtable slot 2 (0x0046EA70) reads address 0.** Would crash if called; no caller found.

## Cross-part edges

- Called by part C (GAMESTEAM_CNetworkServer 0x00A62730): create 0x0046E0A0 -> 0x0041BF90; join 0x0046DE70/0x0046DF20/
  0x0046DFA0 -> 0x0041C110; search 0x0046F940 -> 0x0041C270; polls 0x0046D640/0x0046D6C0/0x0046D890/0x0046D370/
  0x0046D0A0/0x0046ECB0/0x0046FB80 -> 0x0041C5F0; leaves 0x0046D010/0x0046E440/0x0046E540/0x0046D0A0/0x0046D370 ->
  0x0041C080/0x0041C1E0; result parse 0x0046FCC0 builds GAMESTEAM_SearchResultNode; key refresh 0x0046FFF0/0x0046E660.
- This part calls into C: 0x0046E8C0/0x0046FCA0 (set CNetworkServer current lobby id +0x25D0/+0x25D4) from every
  handler/leave; CSTEAMNetworkLobbyData slot 20 is called back from 0x0046D640.
- Parts A/B (uei SteamWorkMgr 0x00A5A050, strategies via 0x004291D0): avatar lobby enter/leave/member data/member list.
- Part E (connection/netcode): packet manager 0x00A129B0 (0x0040A3C0 getter, 0x0040A7E0 queue, 0x0040A6B0 pop,
  0x0040A770 push), GAMESTEAM_CNetworkTransport P2P callbacks (0x0046C7C0, accept 0x0046F650), room request
  messages channel 8 (0x0040A890/0x0040A9E0).
- Part F (UI): reads CNetworker slot 20 (invite pending), CNetworkServer results; CSTEAMNetworkLobbyData vtable
  (enter/leave/sit/challenge/result slots 9-28) is the avatar-lobby UI API.
- Main loop 0x0044F1E0: SteamAPI_RunCallbacks; WinMain helper 0x0044EB10: "+connect_lobby".
- Mod hooks already present: SteamMatchmakingWrapper (RequestLobbyList/GetLobbyByIndex/JoinLobby/LeaveLobby,
  LobbyChatUpdate_t), detours of SteamAPI_RegisterCallResult/UnregisterCallResult/RunCallbacks (lobby-list proxy).

## Open questions

- Can part C/F run the cleanup (CNetworkServer slot 30, 0x0046D010) or reset (slot 29, 0x0046DD20) while join state
  is 1/6 or create state is 1? This decides whether suspected bug 1 happens in practice (check the "Connecting"
  popup cancel and menu-exit paths).
- Does Steam's RequestLobbyList return full lobbies when no slots-available filter is set? (Determines bug 6 scope.)
- Capacity of the inbound packet buffer (third arg of 0x0040A430 in 0x004725B0) and how often it is drained.
- What the UI shows for create/join result 100 and for state 2 timeouts (exact strings) - part F.
- Exact semantics of the room-join request (channel 8 msg 10) and reply object fields +0x164/+0x148 - part C/E.
- Values of session params P+0xFC (max members) per mode, and whether ranked uses NETWORK_VERSION/MATCHING_FILTER
  values that differ between game versions (would make modded/vanilla mixes invisible to each other).
