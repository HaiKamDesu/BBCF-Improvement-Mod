# C - GAMESTEAM_CNetworkServer / GAME_CNetworkServer (session/room server object)

## Summary

`GAMESTEAM_CNetworkServer` is a single static object at **0x00A62730** (lazy-constructed by
`FUN_0046e8c0`, vtable 0x0089CB4C, abstract base `GAME_CNetworkServer` vtable 0x0089C9F4). Other code
never calls it directly: the network task gets it from the network manager
(`FUN_00486070()` -> `**mgr` -> vfunc +0x1C) and goes through ~30 tiny non-virtual base wrappers at
0x004A5510-0x004A5800, each of which calls one vtable slot with one fixed sub-block of the object as
the argument (`this+0x08` login block, `this+0xD8` create block, `this+0x2D0` search block,
`this+0x3D8` join/session block).

What it owns:
- Three Steam call objects from the lobby layer: `AASTEAM_CSessionCreateObj` at +0xBC0 (CreateLobby /
  LobbyCreated_t), `AASTEAM_CSessionJoinObj` at +0xC50 (JoinLobby / LobbyEnter_t), `AASTEAM_CSessionSearchObj`
  at +0xCC0 (filters + RequestLobbyList / LobbyMatchList_t). A second JoinObj at +0x1100 is never used by
  this class.
- Four independent byte state machines: login (+0x25DC), **host** (+0x25DD), search (+0x25DF) and
  **join** (+0x25E0). It also keeps one state byte per room slot (+0x25E3..+0x25EA) and some stub
  "session" bytes (+0x25DE, +0x25E1) that are left over from a console port.
- The game-level join handshake over P2P (unreliable packets, types 0x33-0x3A): join request 0x33,
  reject 0x34, room snapshot/accept 0x35, leave 0x36, room closed 0x37, ping 0x38, pong 0x39, chat 0x3A.
- A QoS RTT table (`GAMESTEAM_QoSListner` at +0x1178, 20 rows) behind the ranked list's Delay digit and
  the RTT-to-host value at +0x25D8.
- The search-result node list (base class, +0xADC, 50 `GAMESTEAM_SearchResultNode`s). It fills the nodes
  from lobby metadata and does the client-side filtering for player rooms.

It does **not** own the room/member table. That table is the room struct at `roomMgr+0x22D10`
(roomMgr = `FUN_004a0fe0()` = 0x00CAD0C0, so the room is at 0x00CCFDD0, the same `Room` the mod's
`GetRoomOne/Two` hooks capture). This class reads that table and adds or removes members in it as the
host. Building it on the guest side is done by the network task (part B).

Most important findings:
1. **Ranked "match found with strangers", root cause in this layer (high confidence, one runtime check
   pending):** the host-side join handler (`FUN_0046efb0`, packet 0x33) is gated only on
   "I have a created lobby" (`+0x25DD == 3`). It answers with whatever is in the single shared room
   table. The join path is never blocked while hosting (it checks only `+0x25E0 == 0`), and a
   successful join overwrites that same table with the other host's snapshot. So a ranked player who
   is hosting (waiting for challengers) and then joins someone else's room ends up with a room table
   that holds the other room's players. The next person who joins their still-open lobby is appended
   after those entries at matchPlayerIndex = memberCount-1 (4, 6...). They receive a snapshot whose
   0/1 pair are two strangers with stale OK flags. Details and a test are under Suspected bugs, #1.
2. Packets 0x34/0x35/0x37 are accepted from **any** sender and in **any** join state. A late reject
   kicks you out of the room you are in now. A late 0x35 "joins" you to a room. A late 0x37 from a
   room you already left makes you leave your current room.
3. Every Steam `LobbyEnter_t` failure (full, doesn't exist, banned, limited...) collapses to result
   100, and the UI shows "ConnectError". The lobby search never sets a slots-available filter, so full
   ranked lobbies are listed and clicking them gives that generic error.
4. The join handshake is one unreliable 0x33 resent every 10 network ticks. It gives up after
   tick-count > 0x708 (1800 ticks, about 30 s at 60 Hz). There are no timeouts on Steam call results.
5. QoS pings have no retransmit. One lost ping or pong freezes that peer's RTT at -1 forever (blank
   Delay digit, RTT-to-host never resolves). Only peers whose own host state is 3 answer pings.
6. If `GetLobbyOwner` is invalid right after `LobbyEnter`, the join fails **without LeaveLobby**. The
   client stays a member of that Steam lobby. For a 2-slot ranked lobby, that makes it read 2/2 (full)
   to everyone.

## Functions

Conventions: "thiscall" = ECX this, args on stack, callee pops (`ret N`). The `AASTEAM_CSession*Obj`
methods (0x0041Bxxx-0x0041Cxxx) are called with ECX = the `AASTEAM_CNetworker` singleton (0x00A25788,
returned in EAX by `FUN_0041c900`) and the session object as the first stack argument. Virtual-slot
numbers are in GAMESTEAM vtable order (offset = 4*slot).

| VA | Name | Size | Level | Notes |
|---|---|---|---|---|
| 0x00405CC0 | IntrusiveList_PopFront | 22 | L1 | fastcall(list) ret; returns the head node (+8) after unlinking it (FUN_00405d80), 0 if empty. Used by the base dtor to delete result nodes. Library. |
| 0x0041BF90 | CSessionCreateObj_CreateLobby | 240 | L2 | thiscall(ECX=networker, obj) ret 4. Returns 0x66 if obj busy (+4), 100 if a CreateLobby call is already pending (handle +0x68/+0x6C != 0), else 0. Clears networker+0x3640 (64 x 8 bytes). Lobby type = (obj+0x14 & 0xF) in {0,2} -> 3 Invisible, else 2 Public. Calls ISteamMatchmaking+0x34 CreateLobby(type, obj+0x18 maxMembers) and registers CCallResult<LobbyCreated_t> (obj+0x58, handler FUN_0041cb90). Sets busy=1, then calls FUN_004248d0/FUN_00425000 (start voice: ISteamUser+0x1C StartVoiceRecording). |
| 0x0041C080 | CSessionCreateObj_LeaveLobby | 134 | L2 | thiscall(ECX=networker, obj) ret 4. 0x66 if busy; clears obj+4/+8/+0xC; ISteamMatchmaking+0x3C LeaveLobby(obj+0x3C/0x40); zeroes the lobby id; server->+0x25D0/0x25D4 = 0 (FUN_0046fca0); done=1, result=0; stops voice (FUN_00425070). Returns 0. |
| 0x0041C110 | CSessionJoinObj_JoinLobby | 208 | L2 | thiscall(ECX=networker, obj, int unused=1) ret 8. 0x66 if busy. Clears networker+0x3840 (64 x 8). ISteamMatchmaking+0x38 JoinLobby(obj+0x20/0x24) -> CCallResult<LobbyEnter_t> at obj+0x38, handler FUN_0041ce10. busy=1, start voice. Returns 0. |
| 0x0041C1E0 | CSessionJoinObj_LeaveLobby | 134 | L2 | thiscall(ECX=networker, obj, int unused) ret 8. 0x66 if a JoinLobby is still pending (+4). Otherwise LeaveLobby(obj+0x20/0x24) unconditionally (even when the id is 0), zeroes it, server+0x25D0 = 0, done=1, result=0, stops voice. Returns 0. Does not cancel an in-flight JoinLobby. |
| 0x0041C270 | CSessionSearchObj_RequestLobbyList | 625 | L2 | thiscall(ECX=networker, obj) ret 4. 0x66 if busy. AddRequestLobbyListStringFilter("ownerID", "<own steamid>", 3=NotEqual) excludes your own lobby. DistanceFilter(obj+0x20 -> 0/2/3, else 1). Then for each of obj+0x28 filters (array obj+0x2C, 0x4C each: key ptr, char value[64], +0x44 kind, +0x48 cmp): kind 0 -> +0x1C NearValue(key, atoi), 1 -> +0x18 Numerical(key, atoi, cmp), 2 -> +0x14 String(key, value, cmp). cmp map 0..5 -> LessThan, GreaterThan, Equal, <=, >=, NotEqual. No ResultCount / SlotsAvailable filter. RequestLobbyList (+0x10) -> CCallResult<LobbyMatchList_t> at obj+0x420, handler FUN_0041cf40. Returns 0. |
| 0x0041C620 | CSessionCreateObj_ClearLobbyData | 80 | L1 | fastcall(obj): zero +0x10..+0x28 (pending lobby-data count), memset entries (+0x30, +0x2C x 0x44). |
| 0x0041C8F0 | LobbyDataEntry_GetInt | 13 | L1 | fastcall(entry) -> atoi(entry+4). |
| 0x0041D360 | LobbyDataEntry_SetString | 34 | L1 | thiscall(entry, key, const char* s) ret 8: entry.key=key; sprintf_s(entry+4, 64, "%s"). |
| 0x0041D390 | LobbyDataEntry_SetInt | 34 | L1 | thiscall(entry, key, int v) ret 8: sprintf "%d". |
| 0x0041D3F0 | SteamMatchmaking | 18 | L1 | returns SteamInternal_ContextInit(0x9D3210)->+0x10 (ISteamMatchmaking*). |
| 0x0041D420 | CSessionCreateObj_FlushLobbyData | 91 | L1 | fastcall(obj): for each queued entry with a non-null key, ISteamMatchmaking+0x50 SetLobbyData(obj+0x3C lobby, key, value). |
| 0x00423C50 | VoicePeerMap_AllocNode | 110 | L1 | library: std::map<CSteamID,VoicePeer> node alloc (FUN_004355b0) + value copy. Owner: networker voice map (+0x5618). |
| 0x00423CE0 | VoicePeerMap_InsertFixup | 522 | L1 | library: std::_Tree red-black insert + rebalance ("map/set<T> too long"). |
| 0x00423EF0 | VoicePeerMap_InsertHint | 416 | L1 | library: std::_Tree hinted insert. |
| 0x004240B0 | VoicePeerMap_InsertNohint | 266 | L1 | library: std::_Tree insert. |
| 0x004241D0 | VoicePeer_CopyCtor | 107 | L1 | library: copies a 0x28-byte map value (8 + 8 dwords). |
| 0x004246D0 | VoicePeerMap_Subscript | 186 | L1 | library: map::operator[] (key = 64-bit steamId). |
| 0x00424790 | SteamId64_Less | 37 | L1 | library: 64-bit unsigned compare. |
| 0x00424B90 | VoicePeerMap_ClearSeenFlags | 97 | L1 | fastcall(networker): sets +0x34=0 on every voice peer (mark phase). |
| 0x00424C00 | VoicePeerMap_TouchPeer | 155 | L1 | thiscall(networker, idLo, idHi) ret 8: if voice active (+0xC) and id != self (+4/+8): existing peer -> seen=1, else insert {4,0,..,1}. |
| 0x00425000 | Voice_Start | 103 | L1 | fastcall(networker): unless +0x5614: if not active, store own SteamID (+4/+8), ISteamUser+0x1C StartVoiceRecording, +0xC = 0x101. Returns 1. |
| 0x00425070 | Voice_Stop | 260 | L1 | fastcall(networker): release each peer's audio stream (FUN_00425c60()->+0xC), destroy map, release +0x18, ISteamUser+0x20 StopVoiceRecording, +0xC=0. |
| 0x00425180 | VoicePeerMap_EraseSubtree | 55 | L1 | library: recursive tree free. |
| 0x004251C0 | VoicePeerMap_LowerBound | 115 | L1 | library: std::_Tree lower_bound/find. |
| 0x00425900 | VoicePeerMap_IterDec | 94 | L1 | library: tree iterator --. |
| 0x0042B2D0 | WString_MoveCtor | 153 | L1 | library: std::wstring move. |
| 0x004355B0 | VoicePeerMap_NewNode | 139 | L1 | library: operator new(0x38) + ctor pieces. |
| 0x0044F600 | Timer_NowMicros | 64 | L1 | QueryPerformanceCounter*1e6/QueryPerformanceFrequency (freq cached at this+0x18). Used for QoS timestamps. |
| 0x00468610 | WString_CtorFromWchar | 96 | L1 | library: std::wstring(const wchar_t*). |
| 0x00468780 | WString_Assign | 250 | L1 | library: std::wstring::assign(ptr,len). |
| 0x0046C590 | Pkt35RoomInfo_Init | 281 | L1 | fastcall(pkt): header dword 0x00350388 (type 0x35, length 0x388), clears 8 member sub-blocks. |
| 0x0046C6B0 | Pkt33JoinRequest_Init | 41 | L1 | fastcall(pkt): header 0x00330068 (type 0x33, len 0x68), clears fields. |
| 0x0046C6E0 | GAMESTEAM_CNetworkServer_ctor | 219 | L2 | fastcall(this). Base ctor, vtable 0x89CB4C, CreateObj@+0xBC0, JoinObj@+0xC50, SearchObj@+0xCC0, JoinObj2@+0x1100, +0x1170=0, QoS listener vtable 0x89CB44 @+0x1178 (rows cleared), +0x25D0..+0x25D7 = 0, then FUN_0046dd20. |
| 0x0046C880 | GAME_CNetworkServer_ctor | 297 | L2 | fastcall(this). Base vtable 0x89C9F4; zeroes the id objects at +8, +0x108, +0x2D0, +0x3D8 (8 slots, FUN_0046c9b0) and +0x9E0; four 8-byte ids at +0xABC..+0xADB; list ctor at +0xADC; FUN_0046dde0. |
| 0x0046C9B0 | SessionBlock_InitMemberIds | 299 | L1 | fastcall(block): zeroes 8 CSteamID-like fields at stride 0x28 inside the +0x3D8 block. |
| 0x0046CDD0 | GAMESTEAM_CNetworkServer_dtor | 163 | L1 | fastcall: unregister/free transport (FUN_0046f890), destroy sub-objects, base dtor FUN_004a53b0. |
| 0x0046CF20 | GAMESTEAM_CNetworkServer_scalar_deleting_dtor | 33 | L1 | slot 34; thiscall(this, flags) ret 4. |
| 0x0046CFE0 | GAME_CNetworkServer_scalar_deleting_dtor | 33 | L1 | base slot 34. |
| 0x0046D010 | Server_AbortAll (slot 30) | 91 | L2 | fastcall(this) ret; returns 0. If host state==3: CreateObj leave (LeaveLobby), host state=4 (**no 0x37 sent to members**). If join state in {7,2,3,4}: JoinObj leave, join state=8 (**no 0x36 sent to host**). States 1/5/6 are not handled, so an in-flight JoinLobby continues. Caller: 0x004AA890 via wrapper 0x004A5640. |
| 0x0046D070 | Server_ResetStateBytes (slot 31) | 41 | L1 | fastcall: zero +0x25DC..+0x25EA (all state bytes). Wrapper 0x004A5520. |
| 0x0046D0A0 | Server_PollJoinInvite (slot 19) | 712 | L2 | thiscall(this, joinBlock=this+0x3D8) ret 4. Returns -1 (idle/not for me: +0xC80==0), -100 pending, 0 joined, >=100 failed. Same machine as slot 18 but state 6 is the "JoinLobby pending" state and the 0x33 flags word is always 0x1000 (prefer spectator). See Flows: Join. |
| 0x0046D370 | Server_PollJoinRoom (slot 18) | 714 | L2 | thiscall(this, joinBlock) ret 4; active only when +0xC80==0. State 1: JoinObj result <0 -> -100; >=100 -> state 0, return the code (100); owner = ISteamMatchmaking+0x8C GetLobbyOwner(+0xC70); if owner invalid -> state 5 **without LeaveLobby**, -100; else build 0x33 (own name wchar[33], own id, block+0x36A word, flags 0x1000 if block+0x36E, block+0x36F/0x370 bytes, own SteamID) and arm netmgr resend slot 8 (FUN_0040a890(8, interval 10, pkt, {owner,1000})), state 2, -100. State 2: wait while slot8.type==0x33 && slot8.elapsed<=0x708; on timeout -> clear slot 8, LeaveLobby, state 5. State 4 (rejected) -> same. State 3 (0x35 received) -> reset QoS, add the owner (+0xC78) to QoS, clear slot 8, +0x25D8=-1, state 7, return 0. State 5: when leave done -> state 0, return 100. State 6 -> state 5 (unreachable). |
| 0x0046D640 | Server_PollCreate (slot 15) | 128 | L2 | thiscall(this, createBlock=+0xD8) ret 4. Host state 1: -100 pending; result<100 -> if (block+0x108&0xF)==2 call thunk_FUN_0046a820()->+0x50(createdLobby); state 3; 0. Result>=100 or state 2 -> state 0, return 100. Else -1. |
| 0x0046D6C0 | Server_PollCloseRoom (slot 16) | 79 | L2 | thiscall(this, createBlock) ret 4. Host state 4: once the CreateObj leave has completed -> +0x25DD/+0x25DE = 0, +0x25E2 = 0, return 0; else -100. Otherwise -1. |
| 0x0046D710 | Server_PollLeave (slot 20) | 27 | L2 | thiscall(this, joinBlock) ret 4: join state 8 -> state 0, return 0; else -1. Never returns >=100. |
| 0x0046D730 | Server_PollSessionEnd (slot 23) | 48 | L1 | thiscall(this, int) ret 4: +0x25DE 4->5 returns 0; 6 -> 100; 5 -> 0; else -1. Nothing ever sets 6: console-port stub. |
| 0x0046D760 | Server_PollMemberJoined (slot 26) | 37 | L2 | thiscall(this, idx) ret 4: memberState[idx] (+0x25E3+idx) 1 -> 2, return 0 (network task event 0x1000000); else -1. |
| 0x0046D790 | Server_PollMemberLeft (slot 27) | 37 | L2 | thiscall(this, idx) ret 4: memberState[idx] 3 -> 0, return 0 (event 0x4000000); else -1. |
| 0x0046D7C0 | Server_PollLogin (slot 14) | 201 | L2 | thiscall(this, loginBlock=+8) ret 4. If +0x25DC==3 -> -1. networker->+0x2C (ISteamUser::BLoggedOn ? 0 : 100). If logged on: own SteamID, ISteamUtils+0x10 GetIPCountry -> block+0xCA = 2-char code (or 0), +0x25DC=3, return 0. Else +0x25DC=0, return 100. The wrapper 0x004A55B0 maps return 0x6B to error 9, but 0x6B is never returned. |
| 0x0046D890 | Server_PollSearch (slot 17) | 482 | L2 | thiscall(this, searchBlock=+0x2D0) ret 4. Search state !=1 -> -1. SearchObj result <0 -> -100; >=100 -> state 0, return the code. Else for each of SearchObj+0x30 lobbies (entries +0xCF4, stride 0x14) fill the next result node (FUN_0046fcc0). Player-room search (block+0xCC != 0) applies client filters: (sessionFlag&0x0F000000)==0 or node.slot7()==block+0xE8; room type (sessionValue&0xF0 -> 1..5)==block+0xD4 if set; member-count bucket block+0xDC (1: <3, 2: 3..5, 3: >5); (sessionFlag&0xF)==block+0xE0 if set; bit26 of session value == (block+0xD0!=0). Ranked: no filtering. +0xAE8 = kept count, cursor reset, state 0, return 0. |
| 0x0046DAE0 | Server_PollSessionStart (slot 21) | 50 | L1 | thiscall(this, int) ret 4: +0x25DE 1->2 returns 0; 2 -> 0; 3 -> 100 (never set); else -1. Stub. |
| 0x0046DB20 | Server_PollSlot24 (slot 24) | 27 | L1 | thiscall(this, block=+0x7E0) ret 4: +0x25E1 1->0 returns 0; else -1. Pairs with slot 11. Stub. |
| 0x0046DB40 | Server_PollResultRtts (slot 44) | 111 | L2 | fastcall(this). Runs the QoS ping pump (FUN_004702e0), then for each result node with +0x110 set: rtt = QoS lookup(node->+0x114 lobby-entry steamId +0xC/+0x10); if >=0, node+0x78 = rtt (ranked Delay digit source), +0x110 = 0. Caller 0x004AC6C0. |
| 0x0046DBB0 | QoS_ClearRows | 68 | L2 | fastcall(qos=this+0x1178): 20 rows x 0x68: zero send-ts[5], rtt[5], count, active, done. |
| 0x0046DC00 | SearchResultNode_Reset | 287 | L1 | fastcall(node): zero fields, +0x78=-1 (RTT unknown), +0x110/+0x114=0. |
| 0x0046DD20 | Server_ResetSession (slot 29) | 184 | L2 | fastcall(this). Lazily new(0x28) GAMESTEAM_CNetworkTransport at +0x1170 (registers P2PSessionConnectFail_t 0x4B3 callback FUN_00429dc0 and P2PSessionRequest handler FUN_0046f650); clears QoS; zeroes +0x25DC..+0x25EA; +0x25D8=-1; +0x25D0/+0x25D4=0. |
| 0x0046DDE0 | GAME_CNetworkServer_ClearBlocks | 137 | L1 | fastcall: +0xAAC=1, +0xAB4=0, zero blocks +8(0xCE), +0xD8(0x1F8), +0x2D0(0x108), +0x3D8(0x408), +0x7E0(0x200), +0x9E0(0xCA), +0xAB8=0. |
| 0x0046DE70 | Server_JoinFromInvite (slot 6) | 169 | L2 | thiscall(this, joinBlock) ret 4. Requires JoinObj idle (+0xC54==0) and join state 0, else 0x66. Clears JoinObj buffers; target = networker+0x3A44/0x3A48 (lobby from GameLobbyJoinRequested); +0xC80=1; JoinLobby; on 0 -> join state 6. |
| 0x0046DF20 | Server_JoinLobbyById (slot 5) | 113 | L2 | thiscall(this, joinBlock) ret 4. Same guard; target = block+0x378/0x37C; +0xC80=0; JoinLobby; state 1. Wrapper 0x004A5680 sets room.status=1 on success. Caller 0x004A8AB0. |
| 0x0046DFA0 | Server_JoinSearchResult (slot 4) | 253 | L2 | thiscall(this, joinBlock) ret 4. Same guard. node = result list[block+0x36D]; if node->+0x114 (lobby entry) is null -> 100. Target = entry+4/+8; +0xC80=0; copies node slots 15..18 into block+0x380..+0x383 and slot 19(i) for i<32 into block+0x384.. (host version/catalog); JoinLobby; state 1. Wrapper 0x004A56B0 (callers 0x004A4110, 0x004A89D0). |
| 0x0046E0A0 | Server_CreateRoom (slot 1) | 927 | L2 | thiscall(this, createBlock=+0xD8) ret 4. Only if host state 0. Queues lobby data: NETWORK_VERSION(block+0x110), HOST_NETCOLOR(+0x100); kind=(block+0x108&0xF): 2 -> MATCHING_FILTER(+0x154), RANK_RTT_FILTER(+0x136), PLAYER_MEMBER_MAX(+0xFC), PLAYER_SESSION_FLAG; 0 (ranked) -> MATCHING_FILTER(+0x138), RANK_MYAREA(+0x132), RANK_AREA_FILTER(+0x134), RANK_HOST_LEVEL(+0x130), RANK_LEVEL_MIN(+0x144), RANK_LEVEL_MAX(+0x148), RANK_RTT_FILTER(+0x136), RANK_DISCONNECT_RATE(+0x150), RANK_DISCONNECT(+0x14C); else (player room) MATCHING_FILTER(+0x114), PLAYER_ROOM_NAME(utf8 of +8), ROOM_TYPE(+0x118), MEMBER_MAX(+0xFC), PRIVATE_NUM(+0xFE), PRIVATE_MAX(+0xFD), SESSION_FLAG(+0x108), SESSION_VALUE(+0x10C). CreateObj.maxMembers=block+0xFC, flags=block+0x108. Writes own SteamID to block+0x1E0, crossplay flag (GetOptionData()+0x68==0) to +0x1E8. CreateLobby; on 0 -> host state 1. Void return (the wrapper reads EAX). Wrapper 0x004A56E0 (callers 0x004A41D0, 0x004A8BD0, 0x004A8D50). |
| 0x0046E440 | Server_CloseRoom (slot 2) | 251 | L2 | thiscall(this, createBlock) ret 4. If CreateObj idle and host state 3: CreateObj leave; if ok and +0xAAC: for each of 8 room members with flag 0x20000 send 0x37 (`0x00370004`) 3x via FUN_0040a7e0 (unreliable, no ack) to member+0x78; host state 4. |
| 0x0046E540 | Server_LeaveRoom (slot 7) | 211 | L2 | thiscall(this, joinBlock) ret 4. Requires JoinObj idle and join state 7, else returns 0x66. JoinObj leave; on 0: state 8, send 0x36 (`0x00360004`) 3x unreliable to the host member's address (roomMgr+0x2321C)->+0x78. Returns the leave result. Wrapper 0x004A5740 sets room.status=3 on 0. |
| 0x0046E620 | Server_SessionEnd (slot 10) | 12 | L1 | +0x25DE = 4; return 0. Stub. |
| 0x0046E660 | Server_Tick (slot 37) | 299 | L2 | fastcall(this). Host only (local member +0x2E bit0, host state 3): FUN_0046fff0 (republish dirty lobby data); if lobby PLAYER_PRIVATE_NUM != room+0x39 (spectator count) -> SetLobbyData. All roles: voice mark/sweep, then touch every room member with state 2 and a zero dword pair at member+0x15. Caller 0x0049AEA0 (network task tick). |
| 0x0046E790 | Server_GetHostRtt (slot 39) | 137 | L2 | thiscall(this, int reArm) ret 4. If join state 7 and the host id is valid: run the ping pump, +0x25D8 = QoS lookup(+0xC78 host); if reArm, re-add the host row. Returns +0x25D8 (-1 = unknown). Caller 0x0049E820. |
| 0x0046E9E0 | QoS_GetRtt | 142 | L2 | thiscall(qos, idLo, idHi) ret 8. Finds a row with active==0, id match and done!=0; otherwise -1. Returns (sum of 5 rtt - max)/4 ms (64-bit math, truncated). |
| 0x0046EA80 | Server_GetName (slot 55) | 8 | L1 | ret 4; returns 0x0084EC44 (constant string). Shared with base. |
| 0x0046EA90 | Server_GetNetworkerSub (slot 50) | 9 | L1 | returns networker+0x10. |
| 0x0046EB80 | Server_RemoveMember (slot 13) | 115 | L2 | thiscall(this, idx) ret 4. If member idx is the local member (roomMgr+0x23218): leave the joined lobby first, and if the leave returns nonzero (busy) the removal is **skipped**. Otherwise FUN_0049f050(idx) (clear slot, count--, recompact matchPlayerIndex, clear resend slot idx), memberState[idx]=3. Returns 0. Callers 0x00496FF0, 0x0049E630 via wrapper 0x004A5770 (jmp). |
| 0x0046EC00 | Server_OnPacket (slot 36) | 138 | L2 | thiscall(this, pkt, addr) ret 8; switch pkt type (word +2): 0x33 -> OnJoinRequest; 0x34 -> +0xAB0 = pkt word +4, join state = 4 (**no state/sender check**); 0x35 -> OnRoomInfo; 0x36 -> OnMemberLeave; 0x37 -> if +0xAAC: LeaveRoom wrapper (**no sender check**); 0x38 -> OnPing; 0x39 -> QoS OnPong(addr id, pkt ts); 0x3A -> OnChat. Caller 0x0049AEA0. |
| 0x0046ECB0 | Server_UpdateMemberAddrs (slot 41) | 241 | L2 | thiscall(this, memberArr, ids*, flags*) ret 0xC. When joined (state 7, JoinObj ok): for 8 entries (stride 0x98, address at +0x78) whose id is not us: if the entry address != given id (or is 0), set it to {-1,0}; if flags[i] and the address is {-1,0}: reset it, +0x84=0xFFFF, address = ids[i], port=1000. Guest P2P mesh setup. Caller 0x004A01E0. |
| 0x0046EDB0 | Server_OnRoomInfo (0x35) | 507 | L2 | thiscall(this, pkt, addr) ret 8. Copies sender addr -> +0x3D8, room header 13 dwords -> +0x3E8, +0x41C..+0x42B, 8 member ids (0x50 dwords) -> +0x42C, 8 x 0x20 -> +0x56C, then the host catalog/version bytes -> +0x758.. (networker->+0x30(8) is always 1). Join state = 3 **unconditionally** (no sender/state check). |
| 0x0046EFB0 | Server_OnJoinRequest (0x33, host) | 1390 | L2 | thiscall(this, pkt, addr) ret 8. Host state != 3 -> reject reason 1. If the sender id already matches a member (slots 0..7, member+4) -> resend the 0x35 snapshot. Else: players = cap(room+4) - maxSpec(room+5), cur = count(room+0x38) - spec(room+0x39); flags pkt+0x54: 0x1000 -> spectator if spec<maxSpec, else reject 3 if players<=cur; else 0x2000 -> reject 4; else reject 2 if players<=cur. Accept: FUN_0049ef50 adds the member. Builds 0x35 (0x388 bytes: room header, 8 member blocks, CreateObj+0x20/0x24, roomMgr+0x6A1C/0x6A68 blocks, own catalog), sends once (unreliable). memberState[idx] = 1 unless already 2. Reject = 0x00340006 + reason word. **No check that the sender is in our Steam lobby, no version check.** |
| 0x0046F520 | Server_OnMemberLeave (0x36, host) | 118 | L2 | thiscall(this, pkt, addr) ret 8. Finds the member whose address (+0x78/+0x7C, +0x80==addr[2]) equals the sender: FUN_0049f050(idx), memberState[idx]=3. |
| 0x0046F5A0 | Server_OnPing (0x38) | 62 | L2 | thiscall(this, pkt, addr) ret 8. Only if host state 3: reply 0x00390010 echoing the 8-byte timestamp. Non-hosts never answer pings. |
| 0x0046F5E0 | Server_OnChat (0x3A) | 50 | L1 | thiscall ret 8; networker->+0x30(1) (always 1) -> FUN_00407fb0(pkt+4 id, pkt+0xC text). |
| 0x0046F620 | Server_Login (slot 0) | 47 | L2 | thiscall(this, loginBlock) ret 4; r = networker->+0x2C(block+0xCC) (BLoggedOn ? 0 : 100); if 0, +0x25DC=2. Returns r. Wrapper 0x004A5780 (caller 0x004AFC90). |
| 0x0046F680 | Server_NewResultNode (slot 28) | 153 | L1 | new(0x118) GAMESTEAM_SearchResultNode + reset. Used by base FUN_004a54d0(50) at init. |
| 0x0046F720 | QoS_AddRow | 128 | L2 | thiscall(qos, idLo, idHi) ret 8. If an active row has this id: no-op. Else take the first inactive row: clear, id, count=0, done=0, active=1. If all 20 rows are active: **silently dropped**. |
| 0x0046F7A0 | QoS_OnPong | 238 | L2 | thiscall(qos, idLo, idHi, tsLo, tsHi) ret 0x10. Active row by id, then the send slot whose ts matches; rtt[i] = (now_us - ts)/1000; count++. |
| 0x0046F890 | Server_FreeTransport | 173 | L1 | unregister both callbacks of +0x1170, free it, +0x1170=0. |
| 0x0046F940 | Server_SearchRooms (slot 3) | 564 | L2 | thiscall(this, searchBlock=+0x2D0) ret 4. 0x66 if SearchObj busy or search state !=0. SearchObj+0x18 = block+0xCA. Filter0 NETWORK_VERSION == block+0xCD; ranked (block+0xCC==0): MATCHING_FILTER == +0xEC, distance(+0xF0: 0 -> Worldwide 3, 1 -> Far 2, else Default 1), RANK_LEVEL_MIN >= +0xF8, RANK_LEVEL_MAX <= +0xFC, if +0x104==1 RANK_DISCONNECT != 1. Player: MATCHING_FILTER == +0xD0, PLAYER_ROOM_TYPE == +0xD4 if set, distance from +0xD8. RequestLobbyList; state 1. Wrapper 0x004A57D0 (callers 0x004A4440, 0x004A94C0). |
| 0x0046FB80 | Server_SendChat (slot 49) | 287 | L1 | thiscall(this, wchar* text, ?) ret 8. If hosting (state 3, create ok) or joined (state 7, join ok): 0x003A004C {sender id, wchar[32]} to every member with state 2. Caller 0x004A6F70. |
| 0x0046FCA0 | Server_SetCurrentLobby | 25 | L2 | thiscall(this, lo, hi) ret 8: +0x25D0/+0x25D4 = lobby id. Written by the create/enter callbacks and the leaves. Single field shared by the host and guest roles. |
| 0x0046FCC0 | SearchResultNode_FillFromLobby | 711 | L2 | thiscall(node, lobbyEntry) ret 4. node+0x114 = entry; node+0x6D = ISteamMatchmaking+0x44 GetNumLobbyMembers; for every lobby-data pair (+0x54 count, +0x58 by index) match the 18-key table at 0x9D9610: HOST_NETCOLOR -> +0x74, ROOM_NAME -> +0x10 (wchar[20]), MEMBER_MAX -> +0x6C, PRIVATE_NUM -> +0x6F, PRIVATE_MAX -> +0x6E, SESSION_FLAG -> +0x64, SESSION_VALUE -> +0x68, RANK_MYAREA -> +0x5C, RANK_AREA_FILTER -> +0x5E, RANK_HOST_LEVEL -> +0x5A, RANK_RTT_FILTER -> +0x10A. node+0xC=1. NETWORK_VERSION/MATCHING_FILTER/LEVEL_MIN/MAX/DISCONNECT are not stored. |
| 0x0046FFD0 | Server_Slot35 | 29 | L1 | ret 8; touches the networker/roomMgr getters only, no effect. |
| 0x0046FFF0 | Server_RepublishLobbyData | 385 | L2 | thiscall(this, int unused) ret 4. If room.status==2 and dirty (+0x230): clear dirty, rebuild the queue: HOST_NETCOLOR(+0x1D8), MEMBER_MAX(+0x1D4 -> CreateObj+0x18); for player rooms MATCHING_FILTER, ROOM_NAME, ROOM_TYPE, SESSION_FLAG, SESSION_VALUE; flush SetLobbyData. |
| 0x00470180 | Server_OpenInviteOverlay (slot 46) | 32 | L1 | ISteamFriends+0x70 ActivateGameOverlay("LobbyInvite"). Caller 0x004AAAD0. |
| 0x004701A0 | Server_OpenUserOverlay (slot 45) | 42 | L1 | thiscall(this, ?, idLo, idHi) ret 0xC: ISteamFriends+0x74 ActivateGameOverlayToUser("steamid", id). |
| 0x004701D0 | ReturnZero_ret0C | 5 | L1 | base slots 41/45 stub (0, ret 0xC). |
| 0x004701E0 | Server_SessionStart (slot 8) | 12 | L1 | +0x25DE=1, return 0. Stub (wrapper 0x004A57F0, caller 0x0049F590). |
| 0x004701F0 | Server_QoSAllResults (slot 43) | 76 | L1 | fastcall: builds the index list 0..+0xAE8-1 and calls own slot 42 (FUN_00470240). Caller 0x004AC6C0. |
| 0x00470240 | Server_QoSStartForResults (slot 42) | 142 | L2 | thiscall(this, n, int* idx) ret 8. If search idle and results>0: n<1 -> 100; clear QoS; for each idx < count: add a QoS row for node->+0x114 lobby-entry steamId, node+0x110=1. Returns 0 (or 100). |
| 0x004702D0 | Server_Slot11 | 12 | L1 | +0x25E1=1, return 0. Stub (wrapper 0x004A5800 with +0x7E0). |
| 0x004702E0 | QoS_PumpPings | 256 | L2 | fastcall(qos). For each active row: count<5 and ts[count]==0 -> send 0x00380010 {ts_us} to the row id (unreliable) and store ts; count>=5 -> active=0, done=1. **No resend: a lost ping or pong stalls the row forever.** |
| 0x00499C50 | RoomMember_Clear | 90 | L1 | fastcall(member): state=0, clear ids/blocks, +0x56/+0x36/+0x8C=0xFFFF, +0x64=0xFF000000, +0x60="USER_ERR_ULIMIT". |
| 0x0049EF50 | Room_AddRemoteMember | 250 | L2 | stdcall-like with ECX unused(?), 7 stack args, ret 0x1C: (joinInfo, word, isSpectator, 0, addr*, isRankedRoomBit, word). count++, spec += isSpectator; slot = first with member+2==0, else **-1 (unchecked -> writes before the member array)**; state=2, id(+4), 24-byte blob(+0xD), name ptr(+0x28), address(+0x78..+0x87), flags(+0x2C)=0x20000 (+0x40000 if spectator); matchPlayerIndex = count-1. Returns slot. |
| 0x0049F050 | Room_RemoveMember | 151 | L2 | thiscall(?, idx) ret 4: clear member, count--, spec-- if 0x40000; null roomMgr+0x23244/+0x23248 if they point at an emptied slot; recompact the match indexes; clear netmgr resend slot idx. |
| 0x004A20C0 | Room_RecompactMatchIndexes | 437 | L2 | fastcall(room): renumber member+0x34 (matchPlayerIndex) of occupied slots 0..n-1 keeping the relative order. |
| 0x004A2330 | Room_SetNewMemberMatchIndex | 25 | L2 | thiscall(room, idx) ret 4: member[idx]+0x34 = room+0x38 (count) - 1. |
| 0x004A53B0 | GAME_CNetworkServer_dtor | 118 | L1 | pop and delete all result nodes (vfunc0(1)), list dtor. |
| 0x004A5740 | GAME_CNetworkServer_Leave | 48 | L2 | fastcall(this): +0xAB0=0; r = slot7(this+0x3D8); if r==0 roomMgr room.status(+0x22D10)=3. Callers: OnPacket 0x37, 0x00497320, 0x0049AEA0, 0x0049DF90. |
| 0x004BA8F0 | ReturnMinusOne_ret4 | 6 | L1 | slot 25 (and base 39): -1. |
| 0x0051B8A0 | Catalog_GetOwnMask | 48 | L1 | builds own DLC/catalog word (FUN_00532790/FUN_005323e0) for the 0x35 info. |
| 0x00530540 | Getter_Plus4 | 4 | L1 | library. |
| 0x005323E0 | Catalog_PackEntries | 82 | L1 | packs up to 32 entries (7-dword stride) into bitfield dwords. |
| 0x00532790 | Catalog_InitQuery | 55 | L1 | init (+0xC=2000, flags |=5). |
| 0x00683280 | Identity_ret4 | 10 | L1 | slot 40: returns its argument. |
| 0x00779430 | WStringToUtf8 | 385 | L1 | library: WideCharToMultiByte(CP_UTF8). |
| 0x007795C0 | Utf8ToWString | 323 | L1 | library: MultiByteToWideChar(CP_UTF8). |

## Data

### Globals
- `0x00A62730` GAMESTEAM_CNetworkServer singleton (guard 0x00A64D20 bit0, ctor FUN_0046c6e0, atexit 0x008485A0).
  Normally reached as `FUN_00486070()` (network manager, ranked-list MGR singleton family) -> `**mgr`
  vfunc +0x1C.
- `0x00A25788` AASTEAM_CNetworker (vtable 0x0084FF54) via FUN_0041c900. Holds a lobby-id/scratch area
  (+0x3640, +0x3840), the pending invite lobby id (+0x3A44), and the voice map (+0x5614 disabled flag,
  +0x5618 std::map root). Its vfunc +0x2C = ISteamUser::BLoggedOn ? 0 : 100 and +0x30(n) always returns 1.
- `0x00CAD0C0` roomMgr (FUN_004a0fe0). The room is at +0x22D10. +0x23218 = local member*,
  +0x2321C = host member*, +0x23244/+0x23248 = member pointers cleared on removal (likely the match pair),
  +0x6A1C/+0x6A68 = blocks copied into 0x35.
- `0x00A129B0` netmgr (FUN_0040a3c0). +0x20 is the resend-slot array (10 slots x 0x28: +0 interval
  frames, +4 countdown, +8 elapsed ticks, +0x10 addr {id64, 1000, x}, +0x20 buf, +0x24 word msg type).
  Slots 0..7 are per room member, slot 8 is the join request (0x33), slot 9 is other host traffic.
  +0x3C = simulated drop percentage (normally 0). FUN_0040a7e0 queues one unreliable datagram;
  everything is sent with SendP2PPacket(..., k_EP2PSendUnreliable, channel 0) in FUN_0041d260.
- Key table `0x009D9610` (18 char* used for create/parse): NETWORK_VERSION, MATCHING_FILTER,
  HOST_NETCOLOR, PLAYER_ROOM_NAME, PLAYER_ROOM_TYPE, PLAYER_MEMBER_MAX, PLAYER_PRIVATE_NUM,
  PLAYER_PRIVATE_MAX, PLAYER_SESSION_FLAG, PLAYER_SESSION_VALUE, RANK_MYAREA, RANK_AREA_FILTER,
  RANK_HOST_LEVEL, RANK_LEVEL_MIN, RANK_LEVEL_MAX, RANK_RTT_FILTER, RANK_DISCONNECT_RATE,
  RANK_DISCONNECT. A second table at `0x009D9658` is used by the search filters. Also "ownerID"
  (0x9D3E4C) and "ownerName" (0x9D3E50), set by the LobbyCreated handler FUN_0041cb90.

### GAMESTEAM_CNetworkServer layout (this = 0x00A62730)
| Off | Type | Meaning |
|---|---|---|
| 0x000 | vptr | 0x89CB4C |
| 0x008 | block 0xCE | login block (slot 0/14): +0xCA country code (2 chars), +0xCC short arg to BLoggedOn wrapper |
| 0x0D8 | block 0x1F8 | create request (slots 1, 2, 15, 16): +8 room name wchar, +0xFC member max (capacity), +0xFD max spectators ("PRIVATE_MAX"), +0xFE private num, +0x100 netcolor, +0x104 byte, +0x108 session flag (low nibble: 0 ranked, 2 = ?, other player room), +0x10C session value, +0x110 net version, +0x114/+0x138/+0x154 matching filter per kind, +0x118 room type, +0x130..+0x150 ranked fields, **+0x158 (=this+0x230) lobby-data dirty flag**, +0x1E0 own SteamID, +0x1E8 crossplay flag |
| 0x2D0 | block 0x108 | search request (slots 3, 17): +0xCA max results(?), +0xCC 1 = player-room search, +0xCD net version, +0xD0..+0xE8 player filters, +0xEC..+0x104 ranked filters |
| 0x3D8 | block 0x408 | join/session block (slots 4-7, 18-20). Overwritten by 0x35: +0x00 host addr (16), +0x10 room header copy (0x34), +0x54 8 member ids, +0x194 8 x 0x20. Request fields: +0x36A word, +0x36D result index, +0x36E want-spectate, +0x36F/+0x370, +0x378 target lobby id, +0x380..+0x403 host catalog/version (compared by the task -> errors 6/7) |
| 0x7E0 | block 0x200 | slot 11/24 arg (stub) |
| 0x9E0 | block 0xCA | slot 12/25 arg (stub) |
| 0xAAC | int | "obey host-close" flag; 1 at init, re-set to 1 by host-init and guest-join (task) |
| 0xAB0 | int | **last error/reason** read by the UI (2/3/4 MemberFull, 6 AppVer, 7 CatalogVer, else ConnectError; 8 and 9 set by wrappers) |
| 0xABC | 4 x 8 bytes | unknown ids |
| 0xADC | list | result nodes: +0xAE0 head, +0xAE8 valid count, +0xAEC/+0xAF0 cursor, +0xAF4 int[50] identity permutation |
| 0xBC0 | CSessionCreateObj | +4 busy, +8 done, +0xC result, +0x14 flags, +0x18 max members, +0x28 queued count, +0x2C cap (18), +0x30 entries[0x44], +0x3C/+0x40 created lobby id (=0xBFC/0xC00), +0x58 CCallResult<LobbyCreated_t> (handle +0x68) |
| 0xC50 | CSessionJoinObj | +4 busy (0xC54), +8 done, +0xC result, +0x20 target lobby (0xC70), +0x28 owner at enter (0xC78), +0x30 isInvite (0xC80), +0x38 CCallResult<LobbyEnter_t> |
| 0xCC0 | CSessionSearchObj | +4 busy (0xCC4), +0x18, +0x20 distance, +0x24 filter cap 7, +0x28 filter count, +0x2C filters (0xCEC), +0x30 result count (0xCF0), +0x34 results[50] x 0x14 (0xCF4), +0x420 CCallResult<LobbyMatchList_t> |
| 0x1100 | CSessionJoinObj #2 | unused by this class |
| 0x1170 | ptr | GAMESTEAM_CNetworkTransport (P2PSessionConnectFail_t 1203 callback) |
| 0x1178 | QoSListner | rows at +0x1180, 20 x 0x68: +0 id64, +8 ts_us[5], +0x30 rtt_ms[5] (u64), +0x58 count, +0x5C active, +0x60 done |
| 0x25D0 | u64 | current Steam lobby (one field shared by the host and guest roles) |
| 0x25D8 | int | RTT to host in ms (-1 unknown) |
| 0x25DC | u8 | login state (0, 2 requested, 3 ok) |
| 0x25DD | u8 | **host state**: 0 idle, 1 creating, 3 hosting, 4 closing (2 never set) |
| 0x25DE | u8 | stub session state 1/2, 4/5 (3, 6 never set) |
| 0x25DF | u8 | search state 0/1 |
| 0x25E0 | u8 | **join state**: 0 idle, 1 JoinLobby pending, 6 invite JoinLobby pending, 2 0x33 sent/awaiting, 3 0x35 received, 4 rejected (0x34), 5 leaving after failure, 7 joined, 8 left |
| 0x25E1 | u8 | stub |
| 0x25E2 | u8 | cleared on close |
| 0x25E3 | u8[8] | per room slot: 1 snapshot sent, 2 acknowledged by task, 3 removed |

### Room (roomMgr+0x22D10) as used here (agrees with src/Game/Room/Room.h)
- +0x00 status: 0 none, 1 creating/joining (wrappers), 2 functional (FUN_0049ee00 host init /
  FUN_0049ed30 guest), 3 terminating (leave wrapper).
- +0x04 capacity, +0x05 max spectators (the mod's "invitation"), +0x06, +0x08 name, +0x30 session flag,
  +0x34 session value, **+0x38 memberCount**, **+0x39 spectatorCount**, +0x3A flags (bit 0x10 passed as
  the "ranked" argument to AddMember).
- Members: 8 x 0x98 at +0x48. Member: +0 slot index, **+2 word state (0 free, 2 occupied)**, +4 SteamID,
  +0xD 24-byte blob from the join request, +0x28 name ptr, **+0x2C flags: 0x10000 self-created host,
  0x20000 added through a 0x33 join, 0x40000 spectator, 0x100000 set by the task**; +0x2E bit0 = host
  flag; +0x30 matchId; **+0x34 matchPlayerIndex**; **+0x78 P2P address {id64, 1000, ...}**.

### Packets (all P2P unreliable, channel 0; header = len word, type word)
| Type | Hdr dword | Dir | Payload | Handler |
|---|---|---|---|---|
| 0x33 | 0x00330068 | guest->owner, resent every 10 ticks via slot 8 | name wchar[33], own id, +0x52 word, +0x54 flags (0x1000 prefer spectator, 0x2000 -> reject 4), +0x56, +0x60 16-byte blob | FUN_0046efb0 |
| 0x34 | 0x00340006 | host->guest | reason word: 1 not hosting, 2 player slots full, 3 player and spectator full, 4 flag 0x2000 | inline in FUN_0046ec00 |
| 0x35 | 0x00350388 | host->guest, once per 0x33 | full room snapshot + host catalog | FUN_0046edb0 |
| 0x36 | 0x00360004 | guest->host, 3x | none | FUN_0046f520 |
| 0x37 | 0x00370004 | host->members (0x20000), 3x | none | FUN_004a5740 |
| 0x38 | 0x00380010 | any->host | u64 timestamp us | FUN_0046f5a0 (only if hosting) |
| 0x39 | 0x00390010 | host->pinger | echoed timestamp | FUN_0046f7a0 |
| 0x3A | 0x003A004C | any->members | sender id + wchar[32] | FUN_0046f5e0 |

### Vtable 0x0089CB4C (base 0x0089C9F4 is all __purecall for 0-31 and shares the trivial tails)
0 Login(+8) / 1 CreateRoom(+0xD8) / 2 CloseRoom(+0xD8) / 3 SearchRooms(+0x2D0) / 4 JoinSearchResult(+0x3D8) /
5 JoinLobbyById / 6 JoinFromInvite / 7 LeaveRoom / 8 SessionStart stub / 9 stub 0 (516A20) /
10 SessionEnd stub / 11 stub(+0x7E0) / 12 stub 0 (+0x9E0) / 13 RemoveMember(idx) / 14 PollLogin /
15 PollCreate / 16 PollClose / 17 PollSearch / 18 PollJoinRoom / 19 PollJoinInvite / 20 PollLeave /
21 PollSessionStart / 22 stub 0 / 23 PollSessionEnd / 24 PollSlot24 / 25 stub -1 (4BA8F0) /
26 PollMemberJoined(idx) / 27 PollMemberLeft(idx) / 28 NewResultNode / 29 ResetSession / 30 AbortAll /
31 ResetStateBytes / 32, 33 return 0 (6A9DA0) / 34 scalar dtor / 35 no-op (FFD0) / 36 OnPacket /
37 Tick / 38 return 1 (506AB0) / 39 GetHostRtt / 40 identity / 41 UpdateMemberAddrs /
42 QoSStartForResults / 43 QoSAllResults / 44 PollResultRtts / 45 OpenUserOverlay / 46 OpenInviteOverlay /
47, 48 return 0 / 49 SendChat / 50 networker+0x10 / 51 return 1 (41DB90) / 52 no-op / 53 return 0 /
54 return 0 / 55 name string / 56 return 0.
Network-task event bits for the per-frame polls (FUN_0049aea0, word0 / word1): slot14 0x100/0x200,
15 0x400/0x800, 16 0x1000/0x2000, 18 0x10000/0x20000, 19 0x40000/0x80000, 20 0x100000 (failure would set
**0x20000**, the join-failed bit, but slot 20 never fails), 17 0x4000/0x8000, 26 0x1000000/0x2000000,
27 0x4000000/0x8000000, 21 w1:1/2, 23 w1:4/8, 22 w1:0x10/0x20, 24 w1:0x100/0x200, 25 w1:0x1000/0x2000.
Return convention for every poll: -1 idle, -100 (0xFFFFFF9C) pending, 0 success, >=100 failure.

## Flows

**Login.** slot 0: BLoggedOn -> +0x25DC=2. slot 14 (polled every frame): BLoggedOn -> read the IP country
-> 3 / event 0x100 (the task copies the country to roomMgr+0xCA). If not logged on -> 0 / event 0x200.

**Host (create / close).** slot 1 (state 0 -> 1): queue lobby data, CreateLobby(Invisible for ranked
and kind 2, Public otherwise; maxMembers = capacity). LobbyCreated_t handler FUN_0041cb90: on
m_eResult==1 store the lobby, set current lobby, SetLobbyData ownerID/ownerName plus the queue, result
0. Otherwise result 100. If the request was cancelled it leaves the lobby. slot 15: 1 -> 3, event 0x400
-> the task runs FUN_0049ee00 (resets the room table and adds self at slot 0 with flags 0x10000).
Failure -> 0, event 0x800. While in state 3: 0x33/0x38 are answered, the tick republishes data and
PLAYER_PRIVATE_NUM. slot 2: 3 -> 4, LeaveLobby + 3x 0x37 to remote members; slot 16: 4 -> 0 when the
leave completes (event 0x1000). The room table is not cleared on close.

**Search.** slot 3 (0 -> 1): filters + RequestLobbyList. LobbyMatchList_t handler FUN_0041cf40 (part D)
fills SearchObj results. slot 17: fills nodes and applies the player-room filters -> 0 (event 0x4000).
The ranked list then calls slot 43/42 (QoS rows for every result, node+0x110=1), and slot 44 every frame
pumps pings and copies the finished RTTs into node+0x78.

**Join (guest).** slot 4/5 (-> 1) or slot 6 (-> 6): JoinLobby. LobbyEnter_t handler FUN_0041ce10:
response==1 -> store lobby, owner = GetLobbyOwner, current lobby, result 0. Anything else -> 100. If
cancelled, leave. Poll slot 18 (c80==0) / 19 (c80==1):
`1|6 --enter ok, owner valid--> 2 (0x33 resent every 10 ticks)`;
`1|6 --enter failed--> 0 (ret 100)`;
`1|6 --owner invalid--> 5 (no LeaveLobby)`;
`2 --0x35--> 3 --poll--> 7 (ret 0, event 0x10000/0x40000; the task runs FUN_0049ed30 -> FUN_004a01e0, copying the snapshot into the room table, status 2)`;
`2 --0x34--> 4 --poll--> LeaveLobby -> 5`;
`2 --slot8.elapsed > 0x708 (about 1801 ticks, ~30 s)--> LeaveLobby -> 5`;
`5 --leave done--> 0 (ret 100, event 0x20000/0x80000)`.
Leave: slot 7 (7 -> 8, 3x 0x36 to the host); slot 20: 8 -> 0 (event 0x100000). Abort: slot 30 from
{2,3,4,7} -> 8 without 0x36.

**Host accepting a member.** 0x33 -> capacity checks -> FUN_0049ef50 (first free slot,
matchPlayerIndex = count-1, flags 0x20000/0x40000, address = sender) -> 0x35 snapshot ->
memberState=1 -> slot 26 -> event 0x1000000. Removal: 0x36 from that address, or slot 13 from the task
(its disconnect detection) -> FUN_0049f050 -> memberState=3 -> slot 27 -> event 0x4000000.

**Membership via Steam callbacks.** This class handles **no LobbyChatUpdate_t or LobbyDataUpdate_t**.
Steam-level membership changes, including the host or Steam owner leaving, are not seen here. Game
membership changes only through 0x33, 0x36, 0x37 and the task's slot-13 calls. A guest learns that the
host left only from 0x37 (3 unreliable copies) or from the task's own timeouts / P2PSessionConnectFail.

**QoS.** AddRow -> PumpPings sends ping k only when ts[k]==0 (once per sample) -> pong stores rtt[k] ->
after 5 samples the row is done -> GetRtt = mean of the 4 lowest. There is no timeout and no resend.

## Failure paths
- **CreateLobby fails** (LobbyCreated m_eResult != 1): result 100 -> PollCreate 100 -> event 0x800.
  Error +0xAB0 = 0 -> generic error. Caused by Steam.
- **JoinLobby fails** (LobbyEnter m_EChatRoomEnterResponse != 1, e.g. 4 full, 2 doesn't exist, 5 error):
  collapsed to 100 at 0x0041CE10 -> join state 0 -> event 0x20000/0x80000 -> +0xAB0 = 0 ->
  "NTER_PlayerMatch_ConnectError". Caused by Steam (often a full or closed lobby). The user can retry.
- **Owner invalid after enter** (0x0046D41B / 0x0046D44D): state 5 without LeaveLobby -> 100
  ConnectError. The client stays in that Steam lobby (local cause, triggered by a host leaving at the
  same time).
- **Join request timeout**: no 0x35/0x34 within 1801 resend ticks (0x0046D54B `cmp [eax+148h],708h; jbe`)
  -> LeaveLobby -> ConnectError. Caused by the peer or P2P: the host isn't hosting (0x25DD!=3 gives
  reject 1, so a pure timeout means P2P never delivered), NAT, or a host that is not running the game
  state that processes packets. About 30 s of "connecting".
- **Rejected** (0x34 reason r): +0xAB0=r. 2/3/4 -> "MemberFull", 1 (host not hosting, e.g. the room
  closed during the join) -> ConnectError. The host decides.
- **Version / catalog mismatch**: no check on the host. The guest's task compares the catalog block
  (+0x758) after 0x35 and sets 6/7 -> AppVerDisagr / CatalogVerUn (part B).
- **Leave packet lost**: 0x36 x3 unreliable, no ack. If all 3 are lost (or the guest used Abort / slot
  30, or crashed), the host keeps the member until the task's slot-13 path fires. Until then the stale
  entry occupies a slot, counts toward capacity, and stays in every future 0x35 snapshot.
- **Host close lost**: 0x37 x3 unreliable. Guests that miss it rely on their own disconnect timeout.
- **Stray 0x34/0x35/0x37** (no sender or state validation): kick out of the current room, or a phantom
  join, or leaving the current room (see bugs).
- **QoS**: a lost ping or pong leaves the row active forever. The RTT stays -1 (blank Delay digit,
  +0x25D8 = -1). If all 20 rows leak, new peers are dropped silently until FUN_0046dbb0 runs (a new
  join success or a new result QoS pass).
- **Busy codes**: 0x66 (102) from every request when the Steam call object is busy or the state isn't
  idle. Search, join and create silently refuse to start.
- **No Steam call-result timeouts**: if CreateLobby / JoinLobby / RequestLobbyList never complete,
  the state stays 1/6 forever (polls keep returning -100). Recovery is up to the task UI.
- **Room-table overflow**: FUN_0049ef50 does not check for "no free slot" (-1) and would write the member
  0x98 bytes before the array (room-0x50). The capacity checks normally prevent it, but they count
  `count`/`spec`, not occupied slots, so slots leaked by unremoved members can break the assumption.

## Suspected bugs

1. **Ranked stale room / "match found with two strangers": a hosting client serves a room table that its
   guest role overwrote.** Confidence: high for the mechanism, medium-high that it is the captured case.
   - Evidence: the 0x33 handler is gated only by `host state == 3` (0x0046EFB0 checks `+0x25DD != 3`),
     takes capacity and members from the single roomMgr room (0x00CCFDD0), appends at the first free
     slot with `matchPlayerIndex = memberCount-1` (FUN_0049ef50 / FUN_004a2330), and sends that whole
     table as 0x35. The join entry points (slots 4/5/6, 0x0046DF20 etc.) check only JoinObj idle and
     `+0x25E0 == 0`, never `+0x25DD`, so a player can host (ranked "waiting for challengers") and join
     at the same time. On join success the task's FUN_0049ed30 -> FUN_004a01e0 copies the other host's
     snapshot into the same room table (status 2). The table is reset only on create (FUN_0049ee00 ->
     FUN_00499cb0). Nothing resets it when the guest role ends, and the host's Steam lobby (CreateObj,
     `+0x25DD==3`) stays open.
   - Result: the clicked owner (XxBig..., alone in their own lobby, 1/2) has a table from a room they
     visited (Raynne_ at matchPlayer 0, dqflynn at 1, themselves at 2 or 3, maybe a spectator). We are
     appended at slot 4 / matchPlayer 4. The popup shows the 0/1 pair with their stale OK flags, and no
     match can start because our client is not in the pair. This matches the "kicked, room readable:
     matchPlayer 4 or 6, matchId 0" signature, and why Steam liveness looked healthy.
   - Test: in the stale dump, the Steam owner's own entry should carry flags 0x20000 (joined someone)
     at member+0x2C (inside the mod's `pad_000C`), and not 0x10000 (self-created host) at matchPlayer 0.
     The 0/1 entries should have the owner's previous host at matchPlayer 0. Dumping member+0x2C and
     member+0x78 in `InspectRoomAfterJoin` settles it.
   - Fixable client-side. On the host: in the 0x33 handler, reject or ignore when the local member is
     not self-host (+0x2C & 0x10000), or when `+0x25E0 != 0`. Or close the hosted lobby (slot 2) when
     a join starts. On the joiner: the existing `matchPlayerIndex >= 2` detector. Only mod hosts benefit
     from host-side fixes.

2. **Unvalidated 0x34/0x35/0x37** (0x0046EC22, 0x0046EDB0, 0x0046EC49). There is no sender match
   against the join target / host address and no state check. A late 0x34 from a previous host turns a
   fresh join (or a joined state 7) into state 4 -> LeaveLobby -> "MemberFull/ConnectError". A late
   0x35 sets state 3 even when idle, and the next poll reports a successful join with foreign data.
   0x37 from any old host makes `FUN_004a5740` leave the **current** room (room.status=3). Old hosts
   keep sending 0x37 to us if our 0x36 was lost or we left by Abort. Symptom: random "room closed" or
   kicks, joins that fail right after another join attempt. Confidence medium-high. Client-side
   fixable: compare `addr` with +0xC78 (owner) or the host member +0x78, and require state 2 for
   0x34/0x35.

3. **LobbyEnter result detail discarded** (0x0041CE10: `param_2[4]==1 ? 0 : 100`). Full, doesn't
   exist, banned, limited and blocked all become ConnectError. Combined with **no
   SlotsAvailable/ResultCount filter in the search** (0x0041C270), full ranked lobbies are listed and
   produce "failed to connect". Confidence high. Fixable: add `AddRequestLobbyListFilterSlotsAvailable(1)`
   via the wrapper, and log or map the enter response.

4. **Owner-invalid path leaks Steam lobby membership** (0x0046D41B/0x0046D44D -> 0x0046D572: state 5
   without FUN_0041c1e0). The client stays in the lobby, and +0x25D0 still names it. For a 2-slot
   ranked lobby that makes it full for everyone and keeps it alive after the host leaves. Confidence
   medium (needs GetLobbyOwner to return nil right after enter, which happens when the host leaves at
   the same moment). Fixable: LeaveLobby on that path.

5. **QoS never retries** (0x004702E0 sends a ping only when ts[k]==0; no expiry). One lost UDP packet
   makes a row permanently incomplete: a blank Delay digit, and the RTT-to-host check (+0x25D8)
   stuck at -1. Pings are answered only by peers whose `+0x25DD==3` (0x0046F5A0). Confidence high.
   Fixable: re-send ping k after a timeout, or treat 3 or more samples as done.

6. **Abort (slot 30, 0x0046D010) is silent.** No 0x36 to the host when leaving a joined room, no 0x37
   to members when closing a hosted one, and no handling of states 1/5/6 (a pending JoinLobby
   completes after the UI cancelled). It feeds stale members on hosts (#1, #2). Confidence high for
   the behaviour. Whether that path is used on normal backs-out depends on 0x004AA890 (part B).

7. **One "current lobby" field and one room table for two roles** (+0x25D0 written by both callbacks
   and cleared by either leave; FUN_0041c080/FUN_0041c1e0). With host and guest active together, the
   current lobby can be 0 while still in a lobby. Low impact today (no reader found in this layer).

8. **The join handshake is a single unreliable request without an ack of 0x35.** If the 0x35 is lost
   and the host has moved our slot or changed state, the next 0x33 resend gets a reject or another
   snapshot. Recovery works through "already a member -> resend" (OK). Timeout is ~30 s. Low.

9. **Leave-poll failure bit mislabelled** (0x0049AEA0 sets 0x20000 for slot-20 >= 100). Harmless,
   since slot 20 never returns >= 100.

10. **AddMember has no free-slot check** (0x0049EF50 returns -1 and writes at room+0x48-0x98). Latent
    memory corruption if slots leak while count/spec do not. Low confidence that it can be triggered.

## Cross-part edges
- **Steam lobby layer (D):** CSessionCreateObj / JoinObj / SearchObj (0x0041BF90, 0x0041C080, 0x0041C110,
  0x0041C1E0, 0x0041C270) and their CallResult handlers FUN_0041cb90 (LobbyCreated), FUN_0041ce10
  (LobbyEnter), FUN_0041cf40 (LobbyMatchList), all writing back through FUN_0046e8c0 + FUN_0046fca0.
  AASTEAM_CNetworker 0x00A25788 (BLoggedOn, voice, invite lobby +0x3A44). ISteamMatchmaking offsets
  used: 0x10, 0x14, 0x18, 0x1C, 0x24, 0x34, 0x38, 0x3C, 0x44, 0x4C, 0x50, 0x54, 0x58, 0x8C. ISteamUser
  0x04, 0x08, 0x1C, 0x20. ISteamUtils 0x10. ISteamFriends 0x70, 0x74.
- **Network task (B):** FUN_0049aea0 polls slots 14-27 every frame and dispatches packets (slot 36).
  It calls the tick (slot 37) and turns poll results into event bits. Initiators: 0x004A41D0, 0x004A8BD0,
  0x004A8D50 (create), 0x004A4110, 0x004A89D0 (join result), 0x004A8AB0 (join by id), 0x0049F590 (Steam
  invite / stubs), 0x004A4440, 0x004A94C0 (search), 0x004AFC90 (login), 0x00496FF0, 0x0049E630 (remove
  member), 0x00497320, 0x0049DF90 (leave), 0x004AA890 (abort/reset), 0x004AC6C0 (QoS for the list),
  0x0049E820 (host RTT), 0x004A6F70 (chat), 0x004A01E0 (member addresses). Room setup on success:
  FUN_0049ee00 (host) and FUN_0049ed30 -> FUN_004a01e0 (guest). Error text from +0xAB0 at the 0x004A0000
  UI switch (case 10).
- **Connection/netcode (E):** netmgr 0x00A129B0 resend slots and FUN_0040a7e0 send queue; FUN_0041d260
  (send, unreliable) and FUN_0041d0e0 (receive). Note: FUN_0041d0e0 spins forever (`do {} while(true)`)
  if the receive ring is full. GAMESTEAM_CNetworkTransport at +0x1170 (P2PSessionConnectFail).
- **Ranked list UI (F / mod):** result nodes (GAMESTEAM_SearchResultNode, +0x74 HOST_NETCOLOR icon,
  +0x78 RTT digit, +0x114 lobby entry with SteamID at +0xC/+0x10), filled by FUN_0046fcc0 and FUN_0046db40.
- **Mod:** `GetRoomOne/Two` capture the same room (roomMgr+0x22D10). `RoomMemberEntry` +0x2C (flags),
  +0x78 (P2P address) and Room +0x39 (spectatorCount) are named here for the first time.

## Open questions
- Is host-and-join at the same time actually the ranked UI flow? Confirm in part B/F (does the ranked
  screen keep the CreateObj lobby while browsing and joining?), or with the +0x2C flag test in bug #1.
- Kind 2 of the session flag (`(flags & 0xF) == 2`): which mode is it, and what is
  `thunk_FUN_0046a820()->+0x50(lobby)` in PollCreate?
- Units of netmgr `elapsed` (+8): incremented once per FUN_0040a2b0 call. Assumed 1 per frame (60 Hz)
  -> ~30 s join timeout; verify the call rate.
- What exactly sets +0xAB0 to 6/7, and where the catalog comparison happens (part B).
- Where the task's slot-13 removal triggers (P2P timeout values) and whether it covers a guest that
  left the Steam lobby without 0x36.
- Is the member+0x15..+0x1C dword pair (voice touch filter) a mute or block id?
