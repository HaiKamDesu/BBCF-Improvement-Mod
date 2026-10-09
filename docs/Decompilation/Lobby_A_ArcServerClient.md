# A - uei lobby-server client (ArcSys web API + TCP lobby)

## Summary

`uei` is ArcSys's own online backend client, separate from Steam. It has **two transports**:

1. **HTTP web API** at `http://153.122.81.62/steam/api/<endpoint>` (plain HTTP, hard-coded IP, Apache + PHP 5.3.3), driven through statically linked libcurl 7.54.1. One thread and one fresh curl handle per request, POST only, 10 s connect timeout, 10 s total timeout, no retries in the transport. Body: `data=` + md5hex(json+"dummy") + base64(json XOR "dummy"). The response uses the same envelope and must pass the MD5 check. Every request carries `{steamId, session, language, date, version "0.0.1", platform 2, param{...}}`.
2. **A raw TCP "TCP/IP LOBBY" connection** to the `ip:port` of a lobby. The address comes from `catalog/get_lobby`, so it is an ArcSys server, not a Steam host. Frames have a 0x36-byte `BBCF` header with the payload length and a message id, and a msgpack payload XORed with "dummy". This carries entering and leaving a lobby, the lobby member list, user-data updates, and chat and system messages.

The client issues only **7 of the 12** web endpoints: `user/create`(0), `user/login`(1), `catalog/get_region|get_area|get_lobby`(2-4), `tus/read`(9) and `tus/write`(10). There is no request class and no parser for `lobby/get_status`, `matching/start|confirm|end` or `tss/read`, so in this build they are dead. **ArcSys's server does not run ranked matchmaking or player rooms.** Those use Steam lobbies (parts B, C and D). The uei server gives you login and session, the region/area/lobby catalog, the "Lobby" mode's own TCP chat rooms, and the D-Code/profile TUS blob.

The work happens in "think-logic strategies" held by the work manager singleton `DAT_00A5A050` (`WorkMgr_Get` 0x427CD0). The manager has **two independent slots**. Slot A at `+0xE0` holds Login, CreateUser, GetLobby, EnterLobby, LeaveLobby and UpdateLobbyUser/InLobby, and reports through `mgr+0x00`. Slot B at `+0xE4` holds DownloadTUS and UploadTUS and reports through `mgr+0x04`. `AASTEAM_CNetworker` vtable slot +0x14 (0x41D410 -> 0x428260) ticks both slots every frame on the game thread. A strategy is swapped only by the request functions that callers outside this part invoke (0x427EC0..0x4281E0). Strategies never chain to each other: the caller polls `mgr+0`/`mgr+4` and decides what comes next.

Most important findings:
- **Many wait states have no time limit, and "transport failed" looks exactly like "still waiting".** `WebApi_ParseResponse` (0x433D90) returns 0 when the request is in flight, when curl failed (`+0xE3C==2`), when the MD5 signature is bad and when the JSON is bad. Only Login (181 frames) and EnterLobby's reply wait (181 frames) give up. CreateUser, DownloadTUS, UploadTUS, LeaveLobby's reply and UpdateLobbyUser's reply wait forever. GetLobby re-requests every 300 frames, forever.
- **Some failures never write a result, so the caller sees "pending" forever.** Login rejected with any `param.status` other than 101, GetLobby after 3 server errors, and UpdateLobbyUser with no connection. These callers treat `mgr+0==0` as -100 "pending" (0x46A390, 0x46A3D0, 0x41C540).
- **A failed TCP connect is reported as "still connecting" forever.** `Socket_PollConnect` (0x426EB0) returns 1 both when `select` reports the connect error in exceptfds and when it is truly pending. EnterLobby has no limit in state 1, so an unreachable lobby server hangs lobby entry.
- **A server-side close of the lobby socket is never detected.** `recv()==0` is treated as success (0x427000/0x4273F0), and there is no heartbeat.
- **Use-after-free by design on the HTTP path.** `WebApi_Submit` (0x434750) and `WebApi_Reset` (0x434D30) free the request descriptor while its detached `HttpRequest` thread may still be inside curl, which can run for up to 10 s. That thread then writes the response and `+0xE38` into freed heap. GetLobby's 5 s re-request and Login and TUS re-arms are all shorter than the 10 s curl timeout.
- **The session is lost and never recovered natively.** Every parsed response overwrites the client token (0x433C00), even error echoes and null sessions. Login sets the latch `mgr+0x20=1`, and the native re-login (0x428050) is a no-op while that latch is set. Only `WorkMgr_Init`/CNetworker slot 1 (0x427E40) or a restart clears it. The mod's `DCodeAutoRelogin` already works around this for TUS (DCodeNetworkStallBug.md).

## Functions
| VA | Name | Size | Level | Notes |
|---|---|---|---|---|
| 0x00401000 | json_delete_object | 26 | L1 | library: Jansson JSON (json_delete_object (jansson)). |
| 0x00401050 | json_object_set_new_nocheck | 91 | L1 | library: Jansson JSON (json_object_set_new_nocheck (jansson)). |
| 0x004010B0 | json_object_set_new | 91 | L1 | library: Jansson JSON (json_object_set_new (jansson)). |
| 0x00401110 | json_object_iter | 26 | L1 | library: Jansson JSON (json_object_iter (jansson)). |
| 0x00401130 | json_object_iter_next | 30 | L1 | library: Jansson JSON (json_object_iter_next (jansson)). |
| 0x004011B0 | json_array_size | 24 | L1 | library: Jansson JSON (json_array_size (jansson)). |
| 0x004011D0 | json_array_get | 35 | L1 | library: Jansson JSON (json_array_get (jansson)). |
| 0x00401200 | json_array_append_new | 110 | L1 | library: Jansson JSON (json_array_append_new (jansson)). |
| 0x00401270 | json_string_nocheck | 105 | L1 | library: Jansson JSON (json_string_nocheck (jansson)). |
| 0x00401320 | json_string_value | 24 | L1 | library: Jansson JSON (json_string_value (jansson)). |
| 0x00401340 | json_integer | 38 | L1 | library: Jansson JSON (json_integer (jansson)). |
| 0x00401370 | json_integer_value | 24 | L1 | library: Jansson JSON (json_integer_value (jansson)). |
| 0x00401390 | json_real/alloc | 87 | L1 | library: Jansson JSON (json_real/alloc (jansson)). |
| 0x00401460 | pack scanner next_token | 148 | L1 | library: Jansson JSON (pack scanner next_token (jansson)). |
| 0x00401500 | pack scanner prev_token | 41 | L1 | library: Jansson JSON (pack scanner prev_token (jansson)). |
| 0x00401530 | pack set_error | 198 | L1 | library: Jansson JSON (pack set_error (jansson)). |
| 0x00401600 | pack_string | 587 | L1 | library: Jansson JSON (pack_string (jansson)). |
| 0x00401850 | pack_object | 510 | L1 | library: Jansson JSON (pack_object (jansson)). |
| 0x00401A50 | pack_array | 379 | L1 | library: Jansson JSON (pack_array (jansson)). |
| 0x00401BD0 | pack "s" helper | 186 | L1 | library: Jansson JSON (pack "s" helper (jansson)). |
| 0x00401C90 | pack_value | 269 | L1 | library: Jansson JSON (pack_value (jansson)). |
| 0x00401E00 | json_vpack_ex | 270 | L1 | library: Jansson JSON (json_vpack_ex (jansson)). |
| 0x00401F10 | json_pack | 20 | L1 | library: Jansson JSON (json_pack (jansson)). |
| 0x00403620 | dump_indent | 119 | L1 | library: Jansson JSON (dump_indent (jansson)). |
| 0x004036A0 | dump_string | 607 | L1 | library: Jansson JSON (dump_string (jansson)). |
| 0x004039C0 | do_dump | 1537 | L1 | library: Jansson JSON (do_dump (jansson)). |
| 0x00403FF0 | json_dumps | 171 | L1 | library: Jansson JSON (json_dumps (jansson)). |
| 0x004040A0 | hashlittle | 443 | L1 | library: Jansson JSON (hashlittle (jansson hashtable)). |
| 0x00404290 | hashtable insert_to_bucket | 68 | L1 | library: Jansson JSON (hashtable insert_to_bucket (jansson)). |
| 0x00404330 | hashtable_do_rehash | 238 | L1 | library: Jansson JSON (hashtable_do_rehash (jansson)). |
| 0x00404420 | hashtable_init | 106 | L1 | library: Jansson JSON (hashtable_init (jansson)). |
| 0x00404490 | hashtable_set | 363 | L1 | library: Jansson JSON (hashtable_set (jansson)). |
| 0x004046B0 | strbuffer_value | 18 | L1 | library: Jansson JSON (strbuffer_value (jansson)). |
| 0x004046D0 | dump callback / strbuffer | 75 | L1 | library: Jansson JSON (dump callback / strbuffer (jansson)). |
| 0x00404840 | utf8_iterate | 185 | L1 | library: Jansson JSON (utf8_iterate (jansson)). |
| 0x00404900 | utf8_check_string | 140 | L1 | library: Jansson JSON (utf8_check_string (jansson)). |
| 0x00404990 | seed_from_windows_cryptoapi | 237 | L1 | library: Jansson JSON (seed_from_windows_cryptoapi (jansson)). |
| 0x00404A80 | json_object_seed | 104 | L1 | library: Jansson JSON (json_object_seed (jansson)). |
| 0x00404B60 | jsonp_error_vset | 68 | L1 | library: Jansson JSON (jsonp_error_vset (jansson)). |
| 0x00404BB0 | strbuffer_append_bytes | 174 | L1 | library: Jansson JSON (strbuffer_append_bytes (jansson)). |
| 0x00404D20 | jsonp_dtostr | 233 | L1 | library: Jansson JSON (jsonp_dtostr (jansson)). |
| 0x00407FB0 | LobbyUI_PostChat | 110 | L1 | pushes a received chat line into the lobby UI (blocked-user check 0x4B3630). Part F/UI. |
| 0x00408020 | LobbyUI_PostSystemMsg | 45 | L1 | pushes a server system message into the lobby UI. |
| 0x00408210 | LobbyUI_PostMessage | 570 | L1 | formats "NMFP_MSG%03d" UI message. |
| 0x0040DF10 | ProfileChecksum16 | 56 | L2 | already L2 in ProfileSync.md (ones-complement sum == 0xFFFF). |
| 0x0041ADF0 | Locale_AddCodecvt | 194 | L1 | std::locale facet install (library). |
| 0x0041B010 | Codecvt_ctor | 178 | L1 | library. |
| 0x0041B0D0 | WStringConvert_ctor | 208 | L1 | library (wstring_convert). |
| 0x0041D830 | Locale_ctor_helper | 165 | L1 | library. |
| 0x0041D930 | WString_AssignSub | 263 | L1 | std::wstring assign (library). |
| 0x0041DA40 | WString_Assign | 294 | L1 | std::wstring assign(ptr,len) (library). |
| 0x0041DE00 | StrategyLeaveLobby_GetType | 6 | L1 | returns 5 (shared constant thunk). |
| 0x0041DF50 | Utf8ToWString_locale | 805 | L1 | wstring_convert helper (lobby message text). |
| 0x0041E280 | Utf8ToWString_locale_wrapper | 113 | L1 | calls 0x41DF50. |
| 0x004262D0 | VectorChar_MoveHelper | 32 | L1 | memmove helper. |
| 0x004262F0 | Md5 | 309 | L1 | CryptoAPI CALG_MD5 (0x8003) over (buf,len) -> 16 bytes. |
| 0x00426430 | Base64DecodeRaw | 141 | L1 | CryptStringToBinaryA(CRYPT_STRING_BASE64). |
| 0x00426600 | Base64DecodeToVector | 339 | L1 | tusData decode. |
| 0x00426760 | Base64Encode_A | 128 | L1 | CryptBinaryToStringA (msgpack userData). |
| 0x004267E0 | Base64Encode_raw | 114 | L1 | CryptBinaryToStringA wrapper. |
| 0x00426860 | Base64Encode_B | 163 | L1 | request body / tusData. |
| 0x00426960 | VectorChar_Reserve | 172 | L1 | std::vector<char> grow. |
| 0x00426CC0 | WideToUtf8 | 143 | L1 | WideCharToMultiByte to heap string (URL, session). |
| 0x00426D50 | Socket_Close | 36 | L1 | closesocket. |
| 0x00426D80 | Socket_Connect | 57 | L1 | connect; WSAEWOULDBLOCK = ok. |
| 0x00426DC0 | Socket_Create | 155 | L1 | WSAStartup once, socket(AF_INET, SOCK_STREAM, TCP). |
| 0x00426E60 | Socket_Destroy | 41 | L1 | closesocket + free. |
| 0x00426E90 | Wsa_Cleanup | 30 | L1 | WSACleanup when refcount > 0. |
| 0x00426EB0 | Socket_PollConnect | 180 | L2 | fastcall(SOCKET*), plain ret. select(except) != 0 -> returns 1; else select(write) > 0 -> 2; else 1. A FAILED nonblocking connect (reported in exceptfds) therefore returns 1 = "still connecting", forever. |
| 0x00426F70 | Wsa_Singleton | 108 | L1 | static WSA refcount object for the TCP lobby sockets (DAT_00A59E70, +400 = refcount). |
| 0x00426FE0 | WSAGetLastError_thunk | 6 | L1 | import thunk. |
| 0x00427000 | Socket_Recv | 37 | L1 | recv; returns ret>=0 (0 counts as success). |
| 0x00427030 | Socket_Send | 37 | L1 | send. |
| 0x00427060 | SockAddr_SetIp | 57 | L1 | inet_addr into sockaddr_in; *ok=1 on success. |
| 0x004270A0 | Socket_SetNonBlocking | 40 | L1 | ioctlsocket(FIONBIO). |
| 0x004270D0 | SockAddr_SetPort | 24 | L1 | htons. |
| 0x004270F0 | LobbyConn_ctor | 69 | L2 | copies sockaddr_in to +0xC..+0x1B; socket +0, cb +4, ctx +8, recv thread +0x1C, dead flag +0x20 = 0. |
| 0x00427140 | LobbyConn_vtbl_reset | 7 | L1 | EH unwind helper (7 bytes). |
| 0x00427150 | LobbyConn_Close | 75 | L2 | fastcall(conn): stop+join Recv thread (infinite wait), closesocket, free, WSACleanup refcount. |
| 0x004271A0 | LobbyConn_Open | 217 | L2 | thiscall(conn, cb, ctx, char blocking), ret 0xC. Recreates socket (WSAStartup refcount "TCP/IP LOBBY"), FIONBIO=1, connect (WSAEWOULDBLOCK accepted); blocking: spins Sleep(0) while poll==1 then starts Recv thread. Failure -> LobbyConn_Close. |
| 0x00427280 | LobbyConn_PollConnect | 78 | L2 | fastcall(conn): returns 0 while poll==1; on poll==2 starts the Recv thread (failure closes) and returns 1. The "error" branch is unreachable because SocketPollConnect never returns anything but 1 or 2. |
| 0x004272D0 | LobbyConn_IsAlive | 20 | L2 | fastcall(conn): socket!=0 && +0x20==0. |
| 0x004272F0 | LobbyConn_StartRecvThread | 246 | L2 | fastcall(conn): new uei::Receiver{conn}, FThread_Create(L"Recv", owned handle, not auto-delete, priority idx 0 = ABOVE_NORMAL) -> conn+0x1C. |
| 0x004273F0 | Receiver_Run | 210 | L2 | fastcall(Receiver*), FRunnable slot 0. Loop until this+0x10: recv(0x400) nonblocking; -1 and != WSAEWOULDBLOCK -> conn+0x20=1; >0 -> cb(buf,len,ctx); Sleep(10). recv()==0 (server FIN) is NOT treated as an error: a gracefully closed lobby socket is never flagged dead. |
| 0x004274D0 | LobbyConn_SendAll | 131 | L2 | thiscall(conn, buf, len), ret 8: loops send; WSAEWOULDBLOCK busy-retries; other error -> conn+0x20=1, returns false. |
| 0x00427560 | Receiver_Stop | 8 | L1 | this+0x10 = 1 (FRunnable slot 2). |
| 0x00427570 | AreaRecord_dtor | 173 | L1 | 0x28-byte area record (frees lobby vector). |
| 0x00427AE0 | LobbyRecord_dtor | 154 | L1 | 0x44-byte lobby record (frees name/ip). |
| 0x00428280 | StdString_AssignCStr | 54 | L1 | assigns lobby ip into record +0x1C. |
| 0x004284A0 | Response_ctor | 236 | L2 | fastcall(resp): init 7 std::wstring at +0x00,+0x1C,+0x34,+0x4C,+0x64,+0x7C,+0x94 (session, date, psnVer, steamVer, acVer, dataVer, serverVer); result +0x18, status +0xAC, list/blob vector +0xB0. |
| 0x00428780 | WorkMgr_SetErrorText | 91 | L2 | thiscall(mgr, const wchar_t*), ret 4: assigns mgr+0x08 std::wstring (last error text, placeholder Japanese "...てきなメッセージ" strings). |
| 0x004287E0 | StrategyCreateUser_Tick | 362 | L2 | thiscall(this, mgr), ret 4. state0 submit UserCreateRequestParam{type 0, steamId}; state1: parse every frame with NO frame limit; result==0 -> mgr+0=4; else status 1 NONUNIQUE / 0xCD LLIMIT / 0xCE ULIMIT / other ANY -> mgr+8 message, mgr+0=0xB. HTTP failure never parses -> waits forever. |
| 0x00428950 | TusKey_Build | 86 | L2 | writes the 16-byte RC4/SHA-512 key {0x84A9E134,0x7B8315F0,steamLo,steamHi}. See ProfileSync.md. |
| 0x00428A20 | ResponseList_dtor | 130 | L1 | frees +0xB0 list vector then Response_dtor. |
| 0x00428AB0 | Strategy_ResetState | 10 | L2 | fastcall(this): this+4 = 0. Reset (slot 5) for CreateUser/Leave/Update/TUS. |
| 0x00428AC0 | StrategyDownloadTUS_Tick | 580 | L2 | thiscall(this, mgr), ret 4. state0 ReadTusRequestParam{type 9, steamId mgr+0xD0/D4}; submit fail -> mgr+4=0xB. state1 parse (no frame limit): result 0 and len==mgr+0xDC -> checksum, else RC4-decrypt with key from 0x428950 and re-check -> memcpy to mgr+0xD8, mgr+4=7; length mismatch -> mgr+4=9 "no TUS data"; result!=0 -> mgr+4=0xB "TUS read failed". Covered by DCodeNetworkStallBug.md. |
| 0x00428D10 | StrategyCreateUser_ctor | 81 | L1 | sets Base then CreateUser vtable 0x850484. |
| 0x00428D70 | StrategyDownloadTUS_ctor | 81 | L1 | vtable 0x850564. |
| 0x00428DD0 | StrategyEnterLobby_ctor | 81 | L1 | vtable 0x8504CC. |
| 0x00428E30 | StrategyGetLobby_ctor | 81 | L1 | vtable 0x8504A8. |
| 0x00428E90 | StrategyLeaveLobby_ctor | 81 | L1 | vtable 0x8504F0. |
| 0x00428EF0 | StrategyLogin_ctor | 81 | L1 | vtable 0x850460. |
| 0x00428F50 | StrategyUpdateLobbyUser_ctor | 94 | L1 | Base -> InLobby -> UpdateLobbyUser vtable 0x85053C. |
| 0x00428FB0 | StrategyUploadTUS_ctor | 81 | L1 | vtable 0x850588. |
| 0x00429010 | StrategyBase_vtbl_reset | 7 | L1 | EH-unwind helper: writes Base vtable (7 bytes). |
| 0x00429020 | StrategyInLobby_vtbl_reset | 72 | L1 | EH-unwind helper for InLobby/UpdateLobbyUser. |
| 0x00429070 | StrategyBase_scalar_dtor | 34 | L1 | thiscall(this, flags); resets Base vtable, frees if flags&1. |
| 0x004290A0 | StrategyInLobby_scalar_dtor | 95 | L1 | InLobby dtor (slot 4 of InLobby vtable). |
| 0x00429100 | StrategyUpdateLobbyUser_scalar_dtor | 102 | L1 | slot 4 of UpdateLobbyUser vtable. |
| 0x00429170 | StrategyLeaf_scalar_dtor | 89 | L1 | shared dtor (slot 4) of Login/CreateUser/GetLobby/EnterLobby/LeaveLobby/TUS. |
| 0x004291D0 | ThinkLogic_CreateStrategy | 407 | L2 | cdecl(int type, WorkMgr* mgr), plain ret; returns new strategy. 0 Idle(4B),1 Login(0xC),2 CreateUser(8),3 GetLobby(0x18),4 EnterLobby(0x10),5 LeaveLobby(8),6 UpdateLobbyUser(8),7 DownloadTUS(8),8 UploadTUS(0xC). Calls new->vtbl[1](mgr) (=Enter: zero result field, then vtbl[5] Reset). Unknown type: calls through a null vtable (crash) - unreachable. |
| 0x004293C0 | Strategy_Exit | 9 | L2 | fastcall(this) slot 2: tail-jumps vtbl[6] (OnExit, always the no-op 0x429DC0). Called by Strategy_Release before free. |
| 0x004293D0 | StrategyDownloadTUS_GetType | 6 | L2 | returns 7 (slot 0). |
| 0x004293F0 | Strategy_Enter_A | 21 | L2 | thiscall(this, mgr) slot 1 for slot-A strategies: mgr->resultA(+0x00)=0, tail-jumps vtbl[5] Reset. |
| 0x00429410 | Strategy_Enter_B | 22 | L2 | thiscall(this, mgr) slot 1 for TUS strategies: mgr->resultB(+0x04)=0, tail-jumps vtbl[5]. |
| 0x00429430 | Strategy_Update | 9 | L2 | fastcall(this) slot 3: tail-jumps vtbl[7] Tick with the caller's stack arg (mgr). Called every frame by WorkMgr_Update 0x428260. |
| 0x00429440 | StrategyInLobby_Update | 29 | L2 | thiscall(this, mgr), ret 4, slot 3 of InLobby/UpdateLobbyUser: vtbl[7](mgr) then vtbl[8] = InLobby_Tick(mgr). |
| 0x00429460 | WebApi_LobbySendEnter | 463 | L2 | thiscall(api, EnterParam*), ret 4: msgpack {lobbyId, userData(base64)} (0x429630/0x433080), XOR "dummy", header len +0xA2, msg id +0xA6 = 100000, prepend 0x36-byte header (+0x74), SendAll; on success clears +0xEC under lock. Returns 0 if no connection. |
| 0x00429630 | MsgPack_PackEnter | 217 | L1 | msgpack sbuffer for ENTER (fixmap 2). |
| 0x00429740 | MemberVector_internal | 34 | L1 | library. |
| 0x00429770 | MemberVector_internal | 36 | L1 | library. |
| 0x004297A0 | MemberVector_internal | 119 | L1 | library. |
| 0x00429830 | MemberVector_internal | 90 | L1 | library. |
| 0x00429960 | MemberVector_MoveAssign | 353 | L1 | library. |
| 0x00429AE0 | VectorChar_Append | 331 | L1 | frame builder append. |
| 0x00429C90 | WebApi_LobbyTakeEnterReply | 183 | L2 | thiscall(api, int* status), ret 4: under lock, if +0xEC: *status=+0xDC, move member vector +0xE0 out, clear flag -> 1; else 0. |
| 0x00429D50 | MemberVector_Alloc | 101 | L1 | library. |
| 0x00429DD0 | StrategyEnterLobby_Reset | 21 | L2 | fastcall(this): +4=0, +8=0, byte +0xC=0. |
| 0x00429DF0 | StrategyEnterLobby_Tick | 700 | L2 | thiscall(this, mgr), ret 4. state0 WebApi_LobbyConnect(mgr+0x44 ip, mgr+0x5C port, nonblocking); state1 wait WebApi_LobbyConnectPoll (no limit); state2 if connected send ENTER (msg 100000, msgpack {lobbyId mgr+0x40, userData mgr+0x60/len mgr+0xA0}) -> state3, else state6 mgr+0=0xB "lobby enter failed"; state3 wait reply (+8 counter, >0xB4 frames -> 0xB): status 0 and own SteamID in member list -> copy members to mgr+0x30, state5, mgr+0=5; own id missing -> 0xB; 100001 -> 0xC (full); 4 -> 0xD (maintenance); other -> 0xB. Every failure closes the TCP connection. |
| 0x0042A0C0 | MemberVector_Grow | 204 | L1 | vector<Member 0x48> internals. |
| 0x0042A1B0 | MemberVector_Reserve | 162 | L1 | "vector<T> too long". |
| 0x0042A260 | MemberVector_PushBack | 185 | L2 | push {steamId64, userData[0x40]} (0x48 bytes) into mgr+0x30. |
| 0x0042A320 | Locale_AddCodecvt2 | 194 | L1 | library. |
| 0x0042A3F0 | RegionAreaLobbyVector_internal | 34 | L1 | library: std::vector<Region/Area/Lobby record> copy/move/grow internals for the GetLobby catalog tree. |
| 0x0042A420 | RegionAreaLobbyVector_internal | 34 | L1 | library: std::vector<Region/Area/Lobby record> copy/move/grow internals for the GetLobby catalog tree. |
| 0x0042A450 | RegionAreaLobbyVector_internal | 36 | L1 | library: std::vector<Region/Area/Lobby record> copy/move/grow internals for the GetLobby catalog tree. |
| 0x0042A480 | RegionAreaLobbyVector_internal | 36 | L1 | library: std::vector<Region/Area/Lobby record> copy/move/grow internals for the GetLobby catalog tree. |
| 0x0042A4B0 | RegionAreaLobbyVector_internal | 36 | L1 | library: std::vector<Region/Area/Lobby record> copy/move/grow internals for the GetLobby catalog tree. |
| 0x0042A4E0 | RegionAreaLobbyVector_internal | 122 | L1 | library: std::vector<Region/Area/Lobby record> copy/move/grow internals for the GetLobby catalog tree. |
| 0x0042A580 | RegionAreaLobbyVector_internal | 122 | L1 | library: std::vector<Region/Area/Lobby record> copy/move/grow internals for the GetLobby catalog tree. |
| 0x0042A620 | RegionAreaLobbyVector_internal | 119 | L1 | library: std::vector<Region/Area/Lobby record> copy/move/grow internals for the GetLobby catalog tree. |
| 0x0042A6C0 | RegionAreaLobbyVector_internal | 119 | L1 | library: std::vector<Region/Area/Lobby record> copy/move/grow internals for the GetLobby catalog tree. |
| 0x0042A760 | RegionAreaLobbyVector_internal | 119 | L1 | library: std::vector<Region/Area/Lobby record> copy/move/grow internals for the GetLobby catalog tree. |
| 0x0042A800 | RegionAreaLobbyVector_internal | 144 | L1 | library: std::vector<Region/Area/Lobby record> copy/move/grow internals for the GetLobby catalog tree. |
| 0x0042A890 | RegionAreaLobbyVector_internal | 84 | L1 | library: std::vector<Region/Area/Lobby record> copy/move/grow internals for the GetLobby catalog tree. |
| 0x0042A8F0 | RegionAreaLobbyVector_internal | 144 | L1 | library: std::vector<Region/Area/Lobby record> copy/move/grow internals for the GetLobby catalog tree. |
| 0x0042AA80 | Codecvt_ctor2 | 178 | L1 | library. |
| 0x0042AB40 | RegionAreaLobbyVector_internal | 156 | L1 | library: std::vector<Region/Area/Lobby record> copy/move/grow internals for the GetLobby catalog tree. |
| 0x0042ABF0 | RegionAreaLobbyVector_internal | 156 | L1 | library: std::vector<Region/Area/Lobby record> copy/move/grow internals for the GetLobby catalog tree. |
| 0x0042ACA0 | WStringConvert_ctor2 | 208 | L1 | library. |
| 0x0042AD70 | ResponseList_ctor | 109 | L1 | Response_ctor + empty +0xB0 vector. |
| 0x0042ADE0 | AreaRecord_ctor | 111 | L1 | id=-1, name, empty lobby vector +0x1C. |
| 0x0042AE50 | RegionAreaLobbyVector_internal | 182 | L1 | library: std::vector<Region/Area/Lobby record> copy/move/grow internals for the GetLobby catalog tree. |
| 0x0042AF10 | LobbyRecord_ctor | 135 | L2 | fastcall: id=-1, name wstring +4, ip std::string +0x1C, port +0x34=-1, enter_num +0x38, enter_max +0x3C, beginner +0x40 = 0. |
| 0x0042AFA0 | RegionRecord_ctor | 111 | L1 | id=-1, name, empty area vector +0x1C. |
| 0x0042B080 | Record_dtor_name | 176 | L1 | frees name wstring. |
| 0x0042B130 | WString_AssignCStr | 96 | L1 | std::wstring assign from wchar_t*. |
| 0x0042B370 | RegionAreaLobbyVector_internal | 101 | L1 | library: std::vector<Region/Area/Lobby record> copy/move/grow internals for the GetLobby catalog tree. |
| 0x0042B3E0 | RegionAreaLobbyVector_internal | 105 | L1 | library: std::vector<Region/Area/Lobby record> copy/move/grow internals for the GetLobby catalog tree. |
| 0x0042B4B0 | WString_Grow | 140 | L1 | library. |
| 0x0042B573 | WString_Grow_tail | 136 | L1 | library. |
| 0x0042B700 | Locale_ctor_helper2 | 165 | L1 | library. |
| 0x0042B7B0 | StrategyGetLobby_Reset | 93 | L2 | thiscall(this, mgr): zero +4..+0x14 and clear mgr region vector (+0x24/+0x28, destroying 0x28-byte region records). |
| 0x0042B810 | StrategyGetLobby_Tick | 1814 | L2 | thiscall(this, mgr), ret 4. Walks region->area->lobby lists: state0 submit get_region (type 2), 1 wait, 2 per region submit get_area (type 3, {regionId}), 3 wait, 4 per area submit get_lobby (type 4, {areaId}), 5 wait, done -> state6, mgr+0=3. Wait states have a 300-frame (5 s) countdown at +0x10; on expiry state goes back to the submit state and re-requests WITHOUT counting (infinite). Server result!=0 -> ++retry +0x14, <3 back to submit, else state7 with NO mgr result (pending forever). Submit failure (alloc only) x3 -> state7, mgr+0=0xB. |
| 0x0042BF40 | RegionAreaLobbyVector_internal | 232 | L1 | library: std::vector<Region/Area/Lobby record> copy/move/grow internals for the GetLobby catalog tree. |
| 0x0042C040 | RegionAreaLobbyVector_internal | 233 | L1 | library: std::vector<Region/Area/Lobby record> copy/move/grow internals for the GetLobby catalog tree. |
| 0x0042C140 | RegionAreaLobbyVector_internal | 232 | L1 | library: std::vector<Region/Area/Lobby record> copy/move/grow internals for the GetLobby catalog tree. |
| 0x0042C240 | RegionAreaLobbyVector_internal | 162 | L1 | library: std::vector<Region/Area/Lobby record> copy/move/grow internals for the GetLobby catalog tree. |
| 0x0042C2F0 | RegionAreaLobbyVector_internal | 162 | L1 | library: std::vector<Region/Area/Lobby record> copy/move/grow internals for the GetLobby catalog tree. |
| 0x0042C3A0 | RegionAreaLobbyVector_internal | 162 | L1 | library: std::vector<Region/Area/Lobby record> copy/move/grow internals for the GetLobby catalog tree. |
| 0x0042C450 | WString_Alloc | 48 | L1 | library. |
| 0x0042C8C0 | StrategyUpdateLobbyUser_GetType | 6 | L1 | returns 6 (shared constant thunk). WorkMgr 0x428110 tests for 5/6. |
| 0x0042CD30 | Utf8ToWString | 805 | L1 | codecvt conversion used for list names. |
| 0x0042D060 | Utf8ToWString_wrapper | 113 | L1 | calls 0x42CD30. |
| 0x0042D0E0 | AreaVector_PushBack | 122 | L1 | vector<0x28>::push_back. |
| 0x0042D160 | LobbyVector_PushBack | 193 | L1 | vector<LobbyRecord 0x44>::push_back. |
| 0x0042D230 | RegionVector_PushBack | 122 | L1 | vector<0x28>::push_back. |
| 0x0042D2B0 | WebApi_LobbySendChat | 463 | L2 | same framing, msg id 100040 (0x186C8). |
| 0x0042D480 | MsgPack_PackChat | 217 | L1 | msgpack sbuffer for chat. |
| 0x0042D560 | SteamIdVector_internal | 42 | L1 | library. |
| 0x0042D5C0 | ChatMsg_dtor | 113 | L1 | frees chat buffers. |
| 0x0042D640 | WorkMgr_ClearChatPending | 48 | L2 | fastcall(mgr): +0xB4=0, wstring +0xA4 cleared, +0xC0/+0xC4=0, byte +0xC8=0. |
| 0x0042D710 | StrategyInLobby_Tick | 806 | L2 | thiscall(this, mgr), ret 4 (vtbl[8], runs every frame after the UpdateLobbyUser tick). Drains WebApi system-message queue (+0xD4) and chat queue (+0xCC) into the lobby UI (0x408020 / 0x407FB0); if byte mgr+0xC8 and mgr+0xA4 text pending, sends CHAT (msg 100040, {steamIdList (target+self if mgr+0xC0/C4 set), comment}), clears pending only on send success. |
| 0x0042DA40 | SteamIdVector_Grow | 185 | L1 | vector<u64> (chat steamIdList). |
| 0x0042DB10 | SteamIdVector_Reserve | 115 | L1 | library. |
| 0x0042DB90 | String_Assign | 247 | L1 | std::string assign (library). |
| 0x0042DC90 | String_Append | 294 | L1 | library. |
| 0x0042DDC0 | SteamIdVector_PushBack | 172 | L1 | library. |
| 0x0042DE70 | WStringToUtf8_wrapper | 107 | L1 | calls 0x42DEE0 (chat text). |
| 0x0042DEE0 | WStringToUtf8 | 819 | L1 | codecvt conversion. |
| 0x0042E220 | WebApi_LobbySendLeave | 463 | L2 | same framing, msg id 100020 (0x186B4). |
| 0x0042E3F0 | MsgPack_PackLeave | 217 | L1 | msgpack sbuffer for leave. |
| 0x0042E4D0 | WebApi_LobbyTakeLeaveReply | 168 | L2 | thiscall(api, int*), ret 4: +0x108 flag / +0x104 status. |
| 0x0042E580 | StrategyLeaveLobby_Tick | 182 | L2 | thiscall(this, mgr), ret 4. state0: not connected -> state2, mgr+0=6; else send LEAVE (msg 100020) -> state1 (send failure retries next frame). state1: connection lost -> state2 mgr+0=6; reply not yet -> return (NO time limit); reply -> close TCP; status!=0 -> state3, mgr+0=0xB "lobby leave failed", else mgr+0=6. |
| 0x0042E640 | StrategyLogin_Reset | 17 | L2 | fastcall(this): state +4=0, poll counter +8=0. |
| 0x0042E660 | StrategyLogin_Tick | 344 | L2 | thiscall(this, mgr), ret 4. state0: build LoginRequestParam{type 1, steamId=own} and WebApi_Submit; fail -> state3, mgr+0=0xB. state1: WebApi_ParseLogin each frame; not parsed -> ++this+8, >0xB4 (181 frames ~3 s) -> state3, mgr+0=0xB; parsed result==0 -> state2, byte mgr+0x20=1 (login latch), mgr+0=1; result!=0 -> state3, mgr+0=2 ONLY if param.status==0x65 (101, not registered); any other status leaves mgr+0 at 0 (pending forever). |
| 0x0042E7C0 | WebApi_LobbySendUserUpdate | 463 | L2 | same framing, msg id 100010 (0x186AA). |
| 0x0042E990 | MsgPack_PackUserUpdate | 217 | L1 | msgpack sbuffer. |
| 0x0042EAE0 | WebApi_LobbyTakeUpdateReply | 183 | L2 | thiscall(api, int*), ret 4: +0x100 flag, +0xF0 status, members +0xF4. |
| 0x0042EBA0 | StrategyUpdateLobbyUser_Tick | 475 | L2 | thiscall(this, mgr), ret 4. state0: not connected -> state2 (no result written!); else send USER_UPDATE (msg 100010, {userData}) -> state1. state1: reply (no time limit): status 0 -> members -> mgr+0x30, byte mgr+0x3C=1, mgr+0=10; 4 -> mgr+0=0xD + close TCP; other -> 0xB (uses the "lobby leave failed" message). |
| 0x0042EDD0 | StrategyUploadTUS_Tick | 530 | L2 | thiscall(this, mgr), ret 4. state0: checksum mgr+0xD8/DC (bad -> 0xB), RC4-encrypt copy with SHA-512 key {0x84A9E134,0x7B8315F0,steamId}, WriteTusRequestParam{type 10, tusData base64}; state1 parse (no limit): result 0 -> mgr+4=8, else 0xB "TUS write failed". |
| 0x0042EFF0 | FThread_Create | 253 | L2 | stdcall(runnable, name, ownsHandle, autoDelete, unused, prioIdx), ret 0x18. CreateThread suspended (proc 0x42F140), 8-byte handle object {HANDLE, runnable}, SetThreadPriority(table[prioIdx]), ResumeThread. |
| 0x0042F0F0 | FThread_Destroy | 67 | L2 | stdcall(threadObj), ret 4: runnable->Stop, WaitForSingleObject(INFINITE), CloseHandle, free. |
| 0x0042F140 | FThread_Proc | 96 | L2 | thread proc: runnable Init, Run; close own handle if owned; if autoDelete: Exit and free runnable. |
| 0x0042F9D0 | ParsedListVector_internal | 36 | L1 | library: std::vector<0xCC/0xD0/0x108 parsed list records> push_back/grow (Region/Area/Lobby list parsers). |
| 0x0042FA00 | ParsedListVector_internal | 36 | L1 | library: std::vector<0xCC/0xD0/0x108 parsed list records> push_back/grow (Region/Area/Lobby list parsers). |
| 0x0042FA30 | ParsedListVector_internal | 36 | L1 | library: std::vector<0xCC/0xD0/0x108 parsed list records> push_back/grow (Region/Area/Lobby list parsers). |
| 0x0042FA90 | ParsedListVector_internal | 125 | L1 | library: std::vector<0xCC/0xD0/0x108 parsed list records> push_back/grow (Region/Area/Lobby list parsers). |
| 0x0042FB20 | ParsedListVector_internal | 125 | L1 | library: std::vector<0xCC/0xD0/0x108 parsed list records> push_back/grow (Region/Area/Lobby list parsers). |
| 0x0042FBB0 | ParsedListVector_internal | 125 | L1 | library: std::vector<0xCC/0xD0/0x108 parsed list records> push_back/grow (Region/Area/Lobby list parsers). |
| 0x0042FFD0 | ParsedListVector_internal | 90 | L1 | library: std::vector<0xCC/0xD0/0x108 parsed list records> push_back/grow (Region/Area/Lobby list parsers). |
| 0x00430030 | ParsedListVector_internal | 90 | L1 | library: std::vector<0xCC/0xD0/0x108 parsed list records> push_back/grow (Region/Area/Lobby list parsers). |
| 0x00430150 | ParsedListVector_internal | 90 | L1 | library: std::vector<0xCC/0xD0/0x108 parsed list records> push_back/grow (Region/Area/Lobby list parsers). |
| 0x00430280 | MsgPack_PackInt | 328 | L1 | msgpack integer packer. |
| 0x004309B0 | WebApi_ctor | 409 | L2 | fastcall(api 0x148): zero fields, two std::list heads at +0xCC/+0xD4, member vectors +0xE0/+0xF4, FCriticalSection +0x128 (InitializeCriticalSection +0x12C), memset slots +0x40 (0x30). |
| 0x00431B70 | FCriticalSection_dtor | 17 | L1 | DeleteCriticalSection (EH unwind). |
| 0x004324D0 | WebApi_LobbyConnect | 270 | L2 | thiscall(api, const char* ip, u16 port, bool blocking), ret 0xC. Closes any old connection at api+0x70; inet_addr only (no DNS; 0 or INADDR_NONE -> no connection object); new 0x24 LobbyConn; LobbyConn_Open(recv cb 0x434D10, ctx api, blocking). EnterLobby passes blocking=0. |
| 0x004325E0 | GetAreaListRequestParam_Build | 22 | L2 | fastcall(param): json_pack("{si}","regionId",param+8). |
| 0x00432600 | GetLobbyListRequestParam_Build | 22 | L2 | fastcall(param): json_pack("{si}","areaId",param+8). |
| 0x00432620 | GetRegionListRequestParam_Build | 14 | L2 | json_pack("{}") - empty param. |
| 0x00432630 | SteamIdRequestParam_Build | 74 | L2 | fastcall(param): json_pack("{ss}","steamId","%lld" of param+8/+0xC). Shared by Login, UserCreate, ReadTus. |
| 0x00432680 | WriteTusRequestParam_Build | 167 | L2 | fastcall(param): base64(param+8 buf, param+0xC len) -> json_pack("{ss}","tusData",b64). |
| 0x00432730 | WebApi_BuildEnvelope | 154 | L2 | cdecl(WebApi* api, json_t* param) -> json_t*. json_pack("{ss,ss,si,si,ss,si}", steamId "%lld" of api+0, session api+8, language api+0x2C, date api+0x30 (time64 low dword), version "0.0.1", platform api+0x38 (2)); then object_set "param". |
| 0x004327D0 | WebApi_LobbyClose | 42 | L2 | fastcall(api): LobbyConn_Close + free +0x70. |
| 0x00432800 | WebApi_LobbyConnectPoll | 22 | L2 | fastcall(api): 1 if no connection object (done/failed) else LobbyConn_PollConnect. |
| 0x00432960 | vsprintf_s_wrapper | 26 | L1 | library. |
| 0x004329E0 | WebApi_LobbyPopChat | 187 | L2 | thiscall(api, out 0x408 bytes), ret 4: pops std::list +0xCC (count +0xD0). |
| 0x00432AA0 | WebApi_LobbyPopSystemMsg | 187 | L2 | thiscall(api, out 0x400 bytes), ret 4: pops +0xD4 (count +0xD8). |
| 0x00432B60 | ResponseParser_GetResult | 4 | L1 | fastcall: returns this+4 (slot 1 of every parser vtable). Shared tiny getter with 150+ callers. |
| 0x00432EE0 | Md5ToHex | 106 | L1 | sprintf 16 bytes as %02x. |
| 0x00432FF0 | WebApi_LobbyIsConnected | 25 | L2 | fastcall(api): 1 iff conn exists, socket exists and conn+0x20 (dead) == 0. |
| 0x00433010 | WebApi_IsInFlight | 90 | L2 | thiscall(api, int type), ret 4: 1 if slot[type] has a descriptor with +0xE38==0; type -1 scans all 12. |
| 0x00433070 | FCriticalSection_Lock | 11 | L1 | EnterCriticalSection(this+4). |
| 0x00433080 | MsgPack_Enter_Body | 527 | L1 | packs "lobbyId", "userData" (base64). |
| 0x00433290 | MsgPack_Leave_Body | 46 | L1 | packs leave map. |
| 0x004332C0 | MsgPack_Chat_Body | 773 | L1 | packs "steamIdList" ("%lld" strings), "comment". |
| 0x004335D0 | MsgPack_UserUpdate_Body | 428 | L1 | packs "userData" (base64). |
| 0x00433780 | ResponseParserGetAreaList_Parse | 285 | L2 | status; area_list[] {regionId, areaId, name} -> 0xD0 records. |
| 0x004338A0 | ResponseParserGetLobbyList_Parse | 443 | L2 | status; lobby_list[] {regionId, areaId, lobbyId, name, enter_num, enter_max, seat_num, ip (string, char[0x20]), port, beginner} -> 0x108 records. strcpy_s of a missing name/ip (NULL) would hit the CRT invalid-parameter handler. |
| 0x00433A60 | ResponseParserGetRegionList_Parse | 258 | L2 | thiscall(parser, param), ret 4: status; region_list[] {regionId int, name string} -> 0xCC records (name char[0xC6]). |
| 0x00433B70 | ResponseParser_StatusOnly | 47 | L2 | thiscall(parser, json_t* param), ret 4: r->status(+0xAC) = param["status"]. Used by Login, UserCreate, WriteTus parsers. |
| 0x00433BA0 | ResponseParserReadTus_Parse | 91 | L2 | status -> +0xAC; "tusData" base64 -> +0xB0 vector (0x426600). Returns 0 if tusData missing (ignored by caller). |
| 0x00433EE0 | WebApi_ParseGetAreaList | 50 | L2 | thiscall(api, resp), ret 4: slot type 3 (api+0x4C) with ResponseGetAreaList parser. |
| 0x00433F20 | WebApi_ParseGetLobbyList | 50 | L2 | slot type 4 (api+0x50). |
| 0x00433F60 | WebApi_ParseGetRegionList | 50 | L2 | slot type 2 (api+0x48). |
| 0x00433FA0 | WebApi_ParseLogin | 50 | L2 | slot type 1 (api+0x44), ResponseLogin parser. |
| 0x00433FE0 | WebApi_ParseReadTus | 50 | L2 | slot type 9 (api+0x64). |
| 0x00434020 | WebApi_ParseUserCreate | 50 | L2 | slot type 0 (api+0x40). |
| 0x00434060 | WebApi_ParseWriteTus | 50 | L2 | slot type 10 (api+0x68). |
| 0x00434750 | WebApi_Submit | 736 | L2 | thiscall(WebApi* api, RequestParam* rp), ret 4; returns 1 ok / 0 only if the 0xE44 descriptor alloc fails. Type = rp+4. Destroys any previous descriptor in slot api+0x40+type*4 EVEN IF ITS THREAD IS STILL RUNNING (the "already in flight" gate at 0x43478A..0x4347A8 passes in both branches), allocs a 0x50 slot, stamps api+0x30 = _time64, snapshots api[0..0x3F] into slot+8, slot+0x48=type, builds envelope, json_dumps(JSON_COMPACT 0x20), encodes body, URL = L"http://153.122.81.62/steam/api/" + table[type] (0x9D4C54), HttpDesc_Create(url, POST, body, len) and HttpRequest_Start. |
| 0x00434A40 | WebApi_EncodeBody | 707 | L2 | 2 stack args, ret 8 (out std::vector<char>*, const char* json). body = "data=" + md5hex(json + "dummy") + base64(json XOR "dummy"). Key/salt string at 0x850764 via 0x9D4C50/0x9D4C84. |
| 0x00434DA0 | FCriticalSection_Unlock | 11 | L1 | LeaveCriticalSection(this+4). |
| 0x00435450 | StdList_AllocHead_Chat | 147 | L1 | std::list head node (WebApi +0xCC chat queue). |
| 0x00435500 | StdList_AllocHead_Msg | 147 | L1 | std::list head node (WebApi +0xD4 system message queue). |
| 0x004357F0 | ParsedListVector_internal | 75 | L1 | library: std::vector<0xCC/0xD0/0x108 parsed list records> push_back/grow (Region/Area/Lobby list parsers). |
| 0x00435A80 | ParsedListVector_internal | 210 | L1 | library: std::vector<0xCC/0xD0/0x108 parsed list records> push_back/grow (Region/Area/Lobby list parsers). |
| 0x00435B70 | ParsedListVector_internal | 210 | L1 | library: std::vector<0xCC/0xD0/0x108 parsed list records> push_back/grow (Region/Area/Lobby list parsers). |
| 0x00435C60 | ParsedListVector_internal | 212 | L1 | library: std::vector<0xCC/0xD0/0x108 parsed list records> push_back/grow (Region/Area/Lobby list parsers). |
| 0x00435EA0 | ParsedListVector_internal | 162 | L1 | library: std::vector<0xCC/0xD0/0x108 parsed list records> push_back/grow (Region/Area/Lobby list parsers). |
| 0x00435F50 | ParsedListVector_internal | 162 | L1 | library: std::vector<0xCC/0xD0/0x108 parsed list records> push_back/grow (Region/Area/Lobby list parsers). |
| 0x00436000 | ParsedListVector_internal | 111 | L1 | library: std::vector<0xCC/0xD0/0x108 parsed list records> push_back/grow (Region/Area/Lobby list parsers). |
| 0x004362C0 | MsgPack_PackRaw | 53 | L1 | msgpack raw/str. |
| 0x00437500 | MsgPack_PackArrayHdr | 192 | L1 | msgpack array header. |
| 0x004375C0 | MsgPack_PackStrHdr | 252 | L1 | msgpack str header. |
| 0x004378C0 | ParsedListVector_internal | 188 | L1 | library: std::vector<0xCC/0xD0/0x108 parsed list records> push_back/grow (Region/Area/Lobby list parsers). |
| 0x00437980 | ParsedListVector_internal | 188 | L1 | library: std::vector<0xCC/0xD0/0x108 parsed list records> push_back/grow (Region/Area/Lobby list parsers). |
| 0x00437A40 | ParsedListVector_internal | 190 | L1 | library: std::vector<0xCC/0xD0/0x108 parsed list records> push_back/grow (Region/Area/Lobby list parsers). |
| 0x004387E0 | HttpDesc_dtorFields | 165 | L1 | frees response vector and URL. |
| 0x00438890 | HttpDesc_Create | 403 | L2 | cdecl(const wchar_t* url, int method(1=POST), const char* body, int len) -> desc (0xE44) or 0. url wstring +0x00, method +0x18, body copy +0x1C (NUL-padded), len +0x20, response vector +0x24, https flag +0x30, header flag +0xE34=0, done +0xE38=0, result +0xE3C=0, httpCode +0xE40. |
| 0x00438A30 | HttpDesc_Destroy | 46 | L2 | cdecl(desc): frees body, response, URL, desc. No synchronisation with the running HttpRequest thread. |
| 0x00438A60 | HttpDesc_HeaderString | 6 | L1 | returns the (unused) extra header string; only used when +0xE34 != 0, never set. |
| 0x00438A70 | HttpPerformGet | 305 | L2 | stdcall(desc), ret 4: curl_easy_init; URL, SSL_VERIFYPEER 0, HTTPGET, WRITEFUNCTION 0x438E20, WRITEDATA desc, VERBOSE 1, USERAGENT "BBCF/1.00", NOSIGNAL, CONNECTTIMEOUT 10, TIMEOUT 10; perform; +0xE3C = (rc!=0)+1; RESPONSE_CODE -> +0xE40; cleanup. Unused by the game (all requests are POST). |
| 0x00438BB0 | HttpPerformPost | 353 | L2 | stdcall(desc), ret 4: as GET plus NOBODY 1 then POST 1 (POST clears no_body), POSTFIELDSIZE +0x20, POSTFIELDS +0x1C, FAILONERROR 1 (HTTP >=400 -> curl error). No retries. +0xE3C = 1 OK / 2 any curl error. |
| 0x00438D20 | HttpRequest_Start | 170 | L2 | cdecl(desc) -> 1/0. new HttpRequestThread{runnable+0xC=desc}; FThread_Create(L"HttpRequest", ownsHandle=1, autoDelete=1, 0, priority idx 5 = NORMAL). Thread handle is not kept: requests cannot be cancelled or joined. |
| 0x00438DD0 | HttpRequestThread_Run | 68 | L2 | fastcall(runnable): method 0 -> HttpPerformGet, 1 -> HttpPerformPost; then InterlockedExchange(desc+0xE38, 1); Sleep(0). |
| 0x00465540 | StrategyEnterLobby_GetType | 6 | L1 | returns 4 (shared constant thunk; also used by curl). |
| 0x0047EB50 | StrategyCreateUser_GetType | 6 | L1 | returns 2 (shared constant thunk). |
| 0x00538FF0 | StrategyGetLobby_GetType | 6 | L1 | returns 3 (shared constant thunk). |
| 0x00778D00 | Sha512 | 243 | L2 | CryptoAPI SHA-512; ProfileSync.md. |
| 0x00778E00 | Rc4Decrypt | 222 | L2 | CryptDeriveKey(CALG_RC4); ProfileSync.md. |
| 0x00778EE0 | Rc4Encrypt | 223 | L2 | ProfileSync.md. |
| 0x007D8BD0 | curl_easy_cleanup | 21 | L1 | library: libcurl 7.54.1 (-> Curl_close 0x7DEB90). |
| 0x007D8BF0 | curl_easy_getinfo | 22 | L1 | library: libcurl (-> Curl_getinfo 0x7E5910). |
| 0x007D8C10 | curl_easy_init | 61 | L1 | library: libcurl (global init + Curl_open). |
| 0x007D8C50 | curl_easy_perform | 18 | L1 | library: libcurl (easy_perform 0x7D8CC0). |
| 0x007D8C70 | curl_easy_setopt | 35 | L1 | library: libcurl vararg wrapper -> Curl_setopt 0x7DF840. |
| 0x007D8CA0 | libcurl_internal | 18 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007D8CC0 | easy_perform | 201 | L1 | library: libcurl ("easy handle already used in multi handle"). |
| 0x007D8D90 | libcurl_internal | 244 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007D8E90 | libcurl_internal | 162 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007D8F40 | win32_init | 99 | L1 | library: libcurl WSAStartup. |
| 0x007D8FB0 | libcurl_internal | 64 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007D8FF0 | curl_slist_append | 60 | L1 | library: libcurl. |
| 0x007D9070 | libcurl_internal | 30 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007D9240 | libcurl_internal | 129 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007D9370 | libcurl_internal | 273 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007D9500 | libcurl_internal | 77 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007D9570 | libcurl_internal | 193 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007D9640 | libcurl_internal | 83 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007D96A0 | libcurl_internal | 300 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007D97D0 | libcurl_internal | 263 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007D98E0 | libcurl_internal | 90 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007D9940 | libcurl_internal | 191 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007D9A00 | libcurl_internal | 432 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007D9BB0 | libcurl_internal | 352 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007D9D10 | libcurl_internal | 697 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007DA080 | libcurl_internal | 47 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007DA280 | libcurl_internal | 33 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007DA490 | libcurl_internal | 204 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007DA640 | multi_runsingle | 3816 | L1 | library: libcurl multi state machine ("Connection timed out after %ld milliseconds"). |
| 0x007DB580 | libcurl_internal | 151 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007DB620 | libcurl_internal | 94 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007DB680 | libcurl_internal | 22 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007DB6B0 | libcurl_internal | 32 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007DB6D0 | libcurl_internal | 34 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007DB700 | libcurl_internal | 584 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007DB950 | libcurl_internal | 219 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007DBAB0 | Curl_cookie_add | 2687 | L1 | library: libcurl cookies. |
| 0x007DC550 | libcurl_internal | 40 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007DC580 | libcurl_internal | 37 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007DC5B0 | libcurl_internal | 95 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007DC610 | libcurl_internal | 33 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007DC7D0 | libcurl_internal | 435 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007DC990 | libcurl_internal | 127 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007DCA10 | libcurl_internal | 122 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007DCA90 | libcurl_internal | 187 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007DCB50 | libcurl_internal | 273 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007DCE60 | libcurl_internal | 92 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007DCEC0 | libcurl_internal | 103 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007DCF30 | libcurl_internal | 154 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007DCFD0 | libcurl_internal | 31 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007DD0F0 | libcurl_internal | 114 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007DD170 | libcurl_internal | 131 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007DD200 | libcurl_internal | 32 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007DD220 | libcurl_internal | 101 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007DDB30 | libcurl_internal | 77 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007DDB80 | libcurl_internal | 65 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007DDF00 | libcurl_internal | 33 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007DEB90 | Curl_close | 524 | L1 | library: libcurl. |
| 0x007DF050 | libcurl_internal | 57 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007DF090 | libcurl_internal | 147 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007DF130 | libcurl_internal | 154 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007DF2B0 | libcurl_internal | 511 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007DF580 | libcurl_internal | 288 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007DF800 | libcurl_internal | 53 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007DF840 | Curl_setopt | 7830 | L1 | library: libcurl ("CURLOPT_SSL_VERIFYHOST no longer supports 1"). |
| 0x007E3CD0 | libcurl_internal | 505 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007E5680 | libcurl_internal | 60 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007E56C0 | libcurl_internal | 179 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007E5910 | Curl_getinfo | 184 | L1 | library: libcurl. |
| 0x007E59D0 | libcurl_internal | 272 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007E5AE0 | libcurl_internal | 213 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007E5C20 | libcurl_internal | 393 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007E5E10 | libcurl_internal | 489 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007E6090 | libcurl_internal | 161 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007E6160 | libcurl_internal | 41 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007E6190 | libcurl_internal | 978 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007E6930 | libcurl_internal | 39 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007E7680 | libcurl_internal | 117 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007E8A80 | libcurl_internal | 14 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007E8A90 | libcurl_internal | 73 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007E8AE0 | libcurl_internal | 25 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007EA650 | libcurl_internal | 11 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007EA900 | libcurl_internal | 20 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007EAA10 | libcurl_internal | 77 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007EAA60 | libcurl_internal | 138 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007EAE10 | libcurl_internal | 250 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007EAF10 | libcurl_internal | 156 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007EB0A0 | libcurl_internal | 242 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007EEF90 | libcurl_internal | 102 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007EF150 | libcurl_internal | 21 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007EF1F0 | libcurl_internal | 72 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007EF240 | libcurl_internal | 115 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007EF2C0 | libcurl_internal | 34 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007F0630 | libcurl_internal | 9 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007F08B0 | libcurl_internal | 18 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007F08D0 | libcurl_internal | 18 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007F0A50 | libcurl_internal | 162 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007F0B00 | libcurl_internal | 214 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007F0C70 | libcurl_internal | 20 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007F0C90 | libcurl_internal | 20 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007F1010 | libcurl_internal | 59 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007F11B0 | Curl_parsedate | 1188 | L1 | library: libcurl date parser. |
| 0x007F1690 | libcurl_internal | 124 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007F1790 | libcurl_internal | 32 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007F17B0 | libcurl_internal | 41 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x007F19E0 | libcurl_internal | 28 | L1 | library: libcurl 7.54.1 internals (multi/connect/cookie/splay/hash/llist/getinfo/date), reached only from the curl_easy_* calls in HttpPerformPost. |
| 0x00803F80 | Curl_ssl_close_all? | 38 | L1 | library: libcurl (called from Curl_close). |
| 0x008086D0 | Curl free helper | 128 | L1 | library: libcurl. |

## Data

### Globals
| Address | What |
|---|---|
| 0x00A5A050 | `WorkMgr` singleton (`WorkMgr_Get` 0x427CD0 returns it in EAX; guard bit `DAT_00A5A138&1`). `DAT_00A5A070` = `mgr+0x20` login latch. |
| 0x00A5A168 | `WebApi*` (0x148 bytes), created lazily by `WebApi_Get` 0x432E10. |
| 0x009D4C48 | -> L"http://153.122.81.62/steam/api" (0x85071C). |
| 0x009D4C4C | -> "0.0.1" (version field). |
| 0x009D4C50 / 0x009D4C84 | -> "dummy" (0x850764). This string is the MD5 salt, the HTTP XOR key and the TCP payload XOR key. |
| 0x009D4C54 | endpoint table, wchar_t*[12]: 0 user/create, 1 user/login, 2 catalog/get_region, 3 catalog/get_area, 4 catalog/get_lobby, 5 lobby/get_status, 6 matching/start, 7 matching/confirm, 8 matching/end, 9 tus/read, 10 tus/write, 11 tss/read. The URL is base + "/" (0x850BC0) + entry. |
| 0x009D5124 | -> "BBCF/1.00" (User-Agent). |
| 0x00A59E70 | WSA refcount object for the lobby sockets (`+400` = refcount). |

### WorkMgr (`DAT_00A5A050`), reset by 0x4282C0
| Off | Field |
|---|---|
| +0x00 | resultA (slot A strategies): 0 pending, 1 login ok, 2 login "user not registered" (status 101), 3 catalog loaded, 4 user/create ok, 5 lobby entered, 6 lobby left, 10 lobby user updated, 0xB failed, 0xC lobby full (100001), 0xD maintenance (4) |
| +0x04 | resultB (TUS): 0 pending, 7 tus/read ok, 8 tus/write ok, 9 "no TUS data" (length mismatch), 0xB failed |
| +0x08 | std::wstring last error text (`WorkMgr_SetErrorText` 0x428780) |
| +0x20 | byte login latch (set by Login on success, cleared only by 0x4282C0 via 0x427E40 / 0x4281E0) |
| +0x24/+0x28/+0x2C | vector<RegionRecord 0x28>: +0 regionId, +4 name wstring, +0x1C vector<AreaRecord 0x28> {+0 areaId, +4 name, +0x1C vector<LobbyRecord 0x44>} |
| LobbyRecord | +0 lobbyId, +4 name wstring, +0x1C ip std::string, +0x34 port, +0x38 enter_num, +0x3C enter_max, +0x40 beginner (seat_num is parsed but dropped). Read by 0x427D50 / 0x46A8F0. |
| +0x30/+0x34/+0x38 | vector<Member 0x48> {u64 steamId, u8 userData[0x40]} for the current lobby (0x427DC0/0x427DE0) |
| +0x3C | byte "member list updated" (UpdateLobbyUser) |
| +0x40 | target lobbyId; +0x44 std::string ip; +0x5C port; +0x60 userData[0x40]; +0xA0 userData len (set by 0x4283A0 / 0x428460) |
| +0xA4 | std::wstring chat text (size +0xB4); +0xC0/+0xC4 whisper target steamId; +0xC8 byte chat pending |
| +0xD0/+0xD4 | TUS target steamId; +0xD8 buffer; +0xDC size (0x6800) |
| +0xE0 / +0xE4 | strategy slot A / slot B |

### WebApi (0x148, `DAT_00A5A168`)
| Off | Field |
|---|---|
| +0x00 | u64 own steamId |
| +0x08 | char session[0x21]. Overwritten from every parsed response (0x433C00). |
| +0x2C | language (1/2, 0x434A30; 2 unless option 0x439980 says otherwise) |
| +0x30 | time64 "date", re-stamped on every submit |
| +0x38 | platform = 2 |
| +0x40 + type*4 | PendingSlot* [12]: 0x50 bytes {+0 HttpDesc*, +8 snapshot of api[0..0x3F], +0x48 type} |
| +0x70 | LobbyConn* (TCP) |
| +0x74..+0xA9 | TCP frame header template (0x36 bytes): +0x74 u32 'BBCF' (0x46434242), +0x78 u32 1, +0x7C u32 0, +0x80 u16 2, +0x82 char[0x20] "%lld" steamId, +0xA2 u32 payload len, +0xA6 u32 message id |
| +0xAC/+0xB0/+0xB4 | TCP receive buffer vector; +0xB8 offset of the current "BBCF" frame (-1 = none) |
| +0xCC/+0xD0 | std::list chat {steamId, comment[0x400]} + count |
| +0xD4/+0xD8 | std::list system message {message[0x400]} + count |
| +0xDC, +0xE0..E8, +0xEC | enter reply: status, member vector, ready flag |
| +0xF0, +0xF4.., +0x100 | user-update reply: status, members, flag |
| +0x104, +0x108 | leave reply: status, flag |
| +0x11C / +0x124 | flags for replies 110030 / 110040 (no reader found) |
| +0x128 | `uei::tl::FCriticalSection` (vtable 0x8509F8, CRITICAL_SECTION +0x12C) guards all reply fields |

### HttpDesc (0xE44, 0x438890)
`+0x00` std::wstring URL, `+0x18` method (1 POST), `+0x1C` body (heap, NUL-terminated), `+0x20` body length, `+0x24` response vector<char>, `+0x30` https flag (always 0), `+0x34..+0xE33` unused buffers, `+0xE34` extra-header flag (0), `+0xE38` done (InterlockedExchange 1, written last), `+0xE3C` curl result (0 running, 1 CURLE_OK, 2 any curl error, including HTTP >= 400 because of FAILONERROR), `+0xE40` HTTP status code.

### Response (stack object, 0x4284A0)
`+0x00` session wstring, `+0x18` result int, `+0x1C` date wstring (the server sends an int, so this stays empty), `+0x34` psnVer, `+0x4C` steamVer, `+0x64` acVer, `+0x7C` dataVer, `+0x94` serverVer, `+0xAC` param.status, `+0xB0` vector (list records or the decoded tusData).

### LobbyConn (0x24, 0x4270F0)
`+0x00` SOCKET*, `+0x04` receive callback (0x434D10), `+0x08` ctx (WebApi), `+0x0C` sockaddr_in, `+0x1C` Recv thread object, `+0x20` dead flag (set only on a send or recv error other than WSAEWOULDBLOCK).

### Strategy objects (vtables 0x850428..0x850588)
Slot order: [0] GetType, [1] Enter(mgr): zero the result field then [5]; [2] Exit -> [6]; [3] Update(mgr) -> [7] (InLobby: [7] then [8]); [4] scalar dtor; [5] Reset; [6] OnExit (no-op); [7] Tick(mgr); [8] InLobby_Tick. Fields: +4 state; +8 frame counter (Login, EnterLobby) or region index (GetLobby); GetLobby also +0xC area index, +0x10 timeout countdown, +0x14 retry count.

Type -> request function (outside this part, all `fastcall(mgr)`): 0 Idle 0x427E40/0x4281E0, 1 Login 0x428050 (gated on latch), 2 CreateUser 0x427EC0, 3 GetLobby 0x427FD0, 4 EnterLobby 0x427F60, 5 LeaveLobby 0x428020, 6 UpdateLobbyUser 0x428110, 7 DownloadTUS 0x427EF0, 8 UploadTUS 0x428180.

### Web endpoints actually used
| type | endpoint | param | response param fields |
|---|---|---|---|
| 0 | user/create | {steamId} | status (1 NONUNIQUE, 0xCD LLIMIT, 0xCE ULIMIT) |
| 1 | user/login | {steamId} | status (0x65 = not registered) |
| 2 | catalog/get_region | {} | status, region_list[{regionId, name}] |
| 3 | catalog/get_area | {regionId} | status, area_list[{regionId, areaId, name}] |
| 4 | catalog/get_lobby | {areaId} | status, lobby_list[{regionId, areaId, lobbyId, name, enter_num, enter_max, seat_num, ip, port, beginner}] |
| 9 | tus/read | {steamId of the target} | status (3 = session rejected, 1 = null session; DCode doc), tusData (base64) |
| 10 | tus/write | {tusData} | status |

Every response header carries: session (stored), result (0 = ok), date, psnVer, steamVer, acVer, dataVer, serverVer (the version strings are parsed and then ignored).

### TCP lobby message ids
Client to server: 100000 ENTER {lobbyId, userData}, 100010 USER_UPDATE {userData}, 100020 LEAVE, 100040 CHAT {steamIdList, comment}.
Server to client: 110000 enter reply {status, lobbyUserList}, 110010 update reply, 110020 leave reply, 110030 and 110040 (flags only), 100488 member-list push, 100498 chat push {steamId, comment}, 100508 system message {message}. Status values: 0 ok, 4 maintenance, 100001 full.

### Error text (UTF-16 placeholders, 0x8503D4..0x8506F4)
「TUSデータなし」 (no TUS data) 0x8503DC, 「TUS読み込み失敗」 (TUS read failed) 0x850400, 「ロビー入室失敗」 (lobby enter failed) 0x8505C4, 「定員オーバー」 (lobby full) 0x8505E4, 「メンテナンス中」 (maintenance) 0x850604, 「ロビーに居ねー」 (not in the lobby) 0x850624, 「ロビー退室失敗」 (lobby leave failed) 0x8506C4, 「TUS書き込み失敗」 (TUS write failed) 0x8506F4, plus L"TL_CREATE_USER_ERR_*". They all go only to `mgr+0x08`. The UI shows its own `LOBBY_*` strings instead (part B).

## Flows

Frames are game frames (the ticker runs once per frame on the game thread, about 60 Hz): 180 frames is about 3 s, 300 frames about 5 s.

**Request pipeline (all web requests).** Strategy tick -> `WebApi_Submit`. It destroys the previous descriptor for the same type, stamps the date, builds the JSON, `json_dumps` it, encodes it, builds the URL, creates the descriptor and starts a detached thread. The thread runs curl POST (connect up to 10 s, whole transfer up to 10 s), then writes `+0xE3C` and finally `+0xE38`. On every later frame the strategy calls `WebApi_Parse*`, which returns 1 once `+0xE3C==1`, the signature checks out and the JSON parses. The header parse then overwrites the session.

**Login (type 1).** 0 submit -> 1 poll. Per frame: no parse -> counter++, >180 -> resultA=0xB. Parsed, result 0 -> latch=1, resultA=1. Parsed, result != 0 -> state 3, and resultA=2 only if status==101.
- Caller: network entry 0x4ABF50 state 100 (part B). It runs `WorkMgr_ResetAll` 0x4281E0 first, which also clears the latch. -100 keeps waiting, 0 -> catalog (0x67), 0x6E (result 2) -> registration dialog -> CreateUser, 100 (0xB) -> "LOBBY_CannotLogin" and `DAT_00CF77A8=1` (the TUS-unavailable latch).

**CreateUser (type 2).** 0 submit -> 1 poll forever. Result 0 -> 4; error -> 0xB with a TL_CREATE_USER_ERR text. Caller 0x4ABF50 state 0x66 then goes back to Login.

**GetLobby (type 3).** State order: 0 region submit -> 1 wait (300f) -> 2 per region: area submit -> 3 wait (300f) -> 2 ... -> 4 per area/lobby: lobby submit -> 5 wait (300f) -> 4 ... -> 6 done, resultA=3.
- Expiry of the 300-frame countdown sends the state back to the submit state; this is unbounded.
- A server result != 0 increments +0x14; at 3 the state goes to 7 with no result.
- The retry counter is reset after each successful step.
- Caller 0x4ABF50 state 0x67. The catalog is fetched only at network entry and the user counts (`enter_num`) are never refreshed: `lobby/get_status` is unused.

**EnterLobby (type 4) - TCP.** 0 connect (non-blocking) -> 1 wait for connect (no limit) -> 2 send ENTER or fail -> 3 wait for reply up to 180f -> 5 entered (resultA=5) / 6 failed (0xB, 0xC, 0xD).
- Caller: CNetworker 0x41BEA0, which sets CNetworker+0x1220=4. 0x46BB00 polls 0x41C540, which maps 0xC -> CNetworker state 10, 0xD -> 0xB, other non-zero -> 5, 0xB -> 100 (failure).
- Then 0x41D4C0 copies the members, and UpdateLobbyUser (0x46A9C0 -> 0x41D3D0 -> 0x428110) takes slot A.

**UpdateLobbyUser / InLobby (type 6).** This strategy stays resident in slot A while you are in a lobby.
- Each frame: Tick (send USER_UPDATE once, then wait for the reply, no limit) followed by InLobby_Tick (drain chat and system messages to the UI, send a pending chat).
- Re-arming it (0x428110 while type 6 is active) just re-enters it: resultA=0 and state=0.
- If slot A holds LeaveLobby (type 5), the update request is ignored.

**LeaveLobby (type 5).** Caller 0x41BF70 (CNetworker+0x1220=6), polled by 0x41C590. 0 send LEAVE (or done if not connected) -> 1 wait for reply (no limit; done if the socket is flagged dead) -> 2 left (6) / 3 failed (0xB). The TCP connection is closed in both cases.

**DownloadTUS / UploadTUS (types 7 and 8, slot B).** 0 submit -> 1 poll forever.
- Download: 7 ok / 9 size mismatch / 0xB.
- Upload: 8 ok / 0xB.
- Driven from the CUMSTask worker thread (0x422B00 / 0x423A50, part B and DCode doc), which waits up to 3 s on mgr+4. Full detail is in DCodeNetworkStallBug.md and ProfileSync.md.

**Reset paths.**
- `WorkMgr_Init` 0x427E40 (CNetworker vtable slot 1): WebApi_Init, which runs WebApi_Reset (frees all descriptors, closes TCP) and clears the session; then reset mgr including the latch; both slots become Idle.
- `WorkMgr_ResetAll` 0x4281E0: language, reset mgr including the latch, both slots Idle. It does not reset WebApi.
- `WorkMgr_Shutdown` 0x427C40: frees both strategies and runs WebApi_Reset.

## Failure paths

| # | Where | Condition | Side | Result for the game | Recovers? |
|---|---|---|---|---|---|
| 1 | Login 0x42E702 | no parse within 181 frames (server slow >3 s, curl error, HTTP >=400, bad MD5/JSON) | server/transport | resultA=0xB -> "LOBBY_CannotLogin", `DAT_00CF77A8=1` | the user retries; the old request is still running (see bug 5) |
| 2 | Login | result!=0 and status!=101 (maintenance, ban, version...) | server | resultA stays 0 -> 0x46A390 returns -100 forever | **stuck** unless part B's UI times out or cancels |
| 3 | Login | status 101 | server | resultA=2 -> registration dialog -> CreateUser | yes |
| 4 | CreateUser 0x4287E0 | any transport failure | transport | waits forever (resultA 0) | **stuck** |
| 5 | CreateUser | result!=0 | server | 0xB, TL_CREATE_USER_ERR_* -> "LOBBY_CannotRegister" | user |
| 6 | GetLobby 0x42B810 | no parse in 300 frames | server/transport | re-request, forever, never 0xB | **stuck at "connecting"**, with UAF risk |
| 7 | GetLobby | result!=0 three times on one step | server | state 7, resultA stays 0 | **stuck** |
| 8 | GetLobby | descriptor alloc fails 3x | local | 0xB -> 0x4ABF50 sets error 0x55 | yes |
| 9 | EnterLobby 0x429DF0 st0 | ip not dotted-quad (inet_addr fails) | server data | no conn -> st2 -> 0xB "enter failed" | yes |
| 10 | EnterLobby st1 | TCP connect refused or timed out | server/network | `Socket_PollConnect` keeps returning 1 -> waits forever | **stuck** (bug 1) |
| 11 | EnterLobby st3 | no reply in 181 frames | server | 0xB, connection closed | yes |
| 12 | EnterLobby st3 | status 100001 / 4 / other / own id missing | server | 0xC full / 0xD maintenance / 0xB | yes |
| 13 | in lobby | server closes the socket gracefully (FIN) | server | not detected; member list and chat freeze silently | no |
| 14 | in lobby | RST or send error | network | conn+0x20=1; later Update/Leave see "not connected". Update gives state 2 with no result (resultA stays 0); Leave reports 6 | partial |
| 15 | UpdateLobbyUser 0x42EBA0 | no reply | server | waits forever, resultA 0 | **stuck** if the caller waits |
| 16 | LeaveLobby 0x42E580 | socket alive, no reply | server | waits forever | **stuck** |
| 17 | Download/UploadTUS | transport failure or no parse | server/transport | no result inside the strategy; the CUMSTask worker gives up after 3 s (part B) | via the worker |
| 18 | TUS | `result:1, status:3` (server dropped the session) | server | 0xB on every later call; the token never changes | **only via mod re-login** (DCodeAutoRelogin) |
| 19 | any parse | response without "session" or with null session | server | the token is blanked (0x433C00) | no; every later request fails until a re-login |

## Suspected bugs

1. **A failed lobby TCP connect hangs EnterLobby forever.** Evidence: in `Socket_PollConnect` 0x426EB0, `esi=1`; `select(except)!=0 -> return 1`; the write-ready path returns 2; nothing else returns another value. `LobbyConn_PollConnect` 0x427280 treats 1 as "keep waiting", and EnterLobby state 1 has no counter. Symptom: entering a Lobby-mode lobby whose server is down or unreachable spins forever (CNetworker+0x1220 stays 4, 0x41C540 returns -100). Confidence high (read from asm). Client-side and fixable: patch 0x426F0A to return a third value, or add a frame limit to state 1.
2. **Silent "pending forever" on server errors.** Login's result!=0 with status!=101 (status compare at 0x42E6EA) never writes resultA. GetLobby's 3x server error goes to state 7 without a result. CreateUser, Leave and Update have no time limits. Symptom: "Connecting..." never ends, with no error dialog. Confidence high for the strategy behaviour; whether part B's UI has its own timeout still needs checking. Client-side and fixable: write 0xB on these paths.
3. **GetLobby re-requests forever on timeout.** 0x42B846..0x42B851: the countdown resets the state without incrementing +0x14. If the catalog server is down after a successful login, network entry hangs. Confidence high.
4. **The transport error is indistinguishable from in-flight.** 0x433DD8 requires `+0xE3C==1` and otherwise returns 0, so a curl error or HTTP 5xx costs the full wait or loops forever instead of failing fast. Client-side and fixable (check `+0xE38`/`+0xE3C==2`).
5. **Use-after-free of HttpDesc.** `WebApi_Submit` 0x4347B1..0x4347DF and 0x434942 free the slot's descriptor with no check that its thread finished. `WebApi_Reset` 0x434D30 does the same for all 12 slots. The detached thread (0x438DD0) keeps writing the response (0x438E20) and `+0xE38` for up to 10 s. Triggers: GetLobby's 5 s re-request, a user login retry after the 3 s Login timeout, a TUS re-request after the worker's 3 s timeout, and CNetworker re-init (0x427E40) during traffic. Symptom: rare heap corruption or crashes during online entry or profile sync on a slow server. Medium-high confidence on the mechanism; how often it happens is unknown. Fixable: keep the old descriptor alive until `+0xE38`, or make the per-type submit wait.
6. **The session token is clobbered by any response.** 0x433C00 copies the session even from error envelopes and when the field is missing or null. Cross-request reordering, with slots A and B in flight at once, can leave a stale token. This is already handled for TUS by the mod. Medium confidence that it contributes beyond the server-side session loss documented in DCodeNetworkStallBug.md.
7. **No native session recovery.** After login the latch `mgr+0x20=1` makes 0x428050 a no-op, and no code path reacts to status 3. Confirmed (DCode doc, 2026-09-20 tests). Already fixed in the mod for TUS. Lobby/catalog calls made after the session dies would fail the same way.
8. **Server-side close of the lobby TCP socket goes undetected.** In 0x4273F0, recv 0 is not an error. In 0x4274D0, send keeps succeeding into a half-closed socket. Symptom: a "ghost" lobby where nobody appears or leaves and chat goes nowhere. Medium confidence. Fixable.
9. **Cross-thread strategy swap in slot B.** 0x427EF0 and 0x428180 run on the CUMSTask worker thread. When the type changes (download <-> upload), they free `mgr+0xE4` while the game thread may be inside its tick (0x428260). The window is narrow, so a crash is possible but rare. Low-medium confidence.
10. **CRT invalid-parameter crashes on malformed server data.** `strcpy_s` of NULL (missing `name` or `ip` in catalog lists, 0x433A60/0x433780/0x4338A0) or a session longer than 32 characters (strcpy_s call at 0x433D6F) hits the invalid-parameter handler, which terminates the process. Low likelihood; server-side trigger.
11. **Cosmetic:** UpdateLobbyUser failures use the "lobby leave failed" text (push 0x8506C4 at 0x42ECB3). The `date` header is read as a string but sent as an int.

## Cross-part edges

- **Into this part (callers):**
  - Network entry task 0x4ABF50 and 0x4AFC90 (part B): Login 0x46BF10, CreateUser 0x46BF20, GetLobby 0x46BE70, ResetAll 0x46BF80. Polled through 0x46A390 (login, also checks the latch) and 0x46A3D0.
  - CNetworker (part C/D): slot 1 0x41CA20 -> `WorkMgr_Init` 0x427E40; slot +0x14 0x41D410 -> `WorkMgr_Update` 0x428260 (the per-frame ticker); EnterLobby 0x41BEA0 (from 0x46BB00), LeaveLobby 0x41BF70 (from 0x46BCC0), UpdateLobbyUser 0x41D3D0 (from 0x46A9C0); result mapping 0x41C540 / 0x41C590 into CNetworker+0x1220; member copy 0x41D4C0.
  - Shutdown 0x41C820 -> 0x427C40.
  - Lobby UI (part F): catalog getters 0x46A8F0 / 0x46A930 / 0x46A7E0 / 0x46A970 / 0x46A960 / 0x46A7C0 / 0x46A8D0 from 0x52E2E0..0x530AF0.
  - CUMSTask worker 0x422B00 / 0x423A50: TUS (part B, DCode doc).
- **Out of this part:** lobby chat and system messages go to the UI through 0x407FB0 / 0x408020. The blocked-user check is 0x4B3630. Steam is used only for `ISteamUser::GetSteamID` (vtable +8 through `SteamInternal_ContextInit(0x9D3210)`) and the option manager (0x4B9770 / 0x439980) for language.
- **None to Steam lobbies.** The uei TCP "lobby" and Steam matchmaking lobbies are different systems. Ranked, player rooms and P2P do not touch the uei server except through the D-Code TUS fetch during match setup (DCode doc: case 0x36 of 0x4AF1A0).

## Open questions

1. Does part B's network-entry UI (0x4ABF50 states 100 / 0x66 / 0x67, `+0x14`/`+0x18` counters, the dialog at `+0xB4`) have its own timeout or cancel that rescues failure paths 2, 4, 6, 7? If not, those are user-visible infinite "connecting" hangs.
2. The DCode endpoint dumps (2026-09-27) showed slots 2-4 (catalog) empty for a whole session, yet 0x4ABF50 state 0x67 should issue them on every network entry. Is 0x67 skipped for ranked/room entry, or does `WorkMgr_Init` (0x427E40 -> WebApi_Reset) run after the catalog fetch and wipe the slots? This decides whether the catalog/TCP lobby path matters for the reported failures at all.
3. Is the ArcSys TCP lobby (Lobby mode) still alive on the server side, and what do 110030 / 110040 and the 100488 push carry exactly (0x42F1A0 target struct)?
4. The thread and timing of the CUMSTask worker's re-request relative to the 10 s curl timeout: confirm with a log line on `WebApi_Submit` while `+0xE38==0` whether the UAF of bug 5 actually happens in normal play.
5. `param.status` values for Login other than 101 (maintenance? banned? version?) are unknown. The server's `serverVer`/`dataVer` fields are parsed and ignored. Could a version mismatch be signalled through them?
