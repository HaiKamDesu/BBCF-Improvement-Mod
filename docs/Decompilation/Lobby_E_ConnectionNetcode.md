# E - Connection setup and netcode transport

## Summary
- BBCF's match netcode is **stock GGPO** (source paths `..\SmallSrc\Rollback\ggpo\lib\...`, build tree `d:\dev_bbcf\bbcf-ggpo-upgrade\`) with the UDP layer swapped for Steam P2P (`backends\steam\steam_p2p.cpp`, `steam_spectator.cpp`, `network\steam\steam_udp_proto.cpp`). Class names map 1:1 to GGPO: SteamPeer2PeerBackend = Peer2PeerBackend, SteamSpectatorBackend = SpectatorBackend, SteamUdp = Udp, SteamUdpProtocol = UdpProtocol. The only real differences: a peer is a 64-bit SteamID instead of a sockaddr, timeouts are set by the game to 3000/1000 ms, a few BBCF events (0xB confirmed frame, 0xC synchronized inputs, 0xD host inputs lost), time-sync every 120 frames, and a ring push that returns false instead of asserting.
- **Transport channels** (all through `SteamInternal_ContextInit(0x9D3210)+0x20` = ISteamNetworking): GGPO = channel **3**, `k_EP2PSendUnreliable` (0x77F950, read 0x77F860). BBCF desync checker = channel **4**, `k_EP2PSendReliable`, 20-byte packets (0x783D60 send, 0x783C50 read). Lobby-level AASTEAM_CNetworker + voice = channel **0**, unreliable (0x41D260 send, 0x41D0E0 read). The mod's own packets also use channel 0.
- **P2P session setup is implicit.** Nobody "connects": both sides' GGPO endpoints call `SendP2PPacket` (a SyncRequest) the moment the session is created (AddRemotePlayer -> Synchronize, 0x77CEF0 -> 0x781240), which makes Steam open the session. Both sides send first, at the same time.
- **Every incoming P2PSessionRequest_t is accepted, no checks.** `GAMESTEAM_CNetworkTransport::OnP2PSessionRequest` = 0x46F650 calls `AcceptP2PSessionWithUser(request.m_steamIDRemote)` without checking anything (not lobby membership, not the expected opponent). The voice manager's handler 0x424CE0 does check membership, but Steam sends the callback to both handlers, so the transport's handler accepts the session anyway.
- **P2PSessionConnectFail_t is ignored.** The transport's handler is 0x429DC0 = `ret 4` (does nothing). The voice manager only stores `m_eP2PSessionError` in its member entry (0x424CA0). The game never calls `GetP2PSessionState`, `CloseP2PSessionWithUser`, `CloseP2PChannelWithUser` or `AllowP2PPacketRelay`. Steam sessions are never closed by the game. They outlive the match, the rematch and leaving the room.
- **Sender validation exists only per packet, on channel 3:** `HandlesMsg` 0x77FD00 compares the 64-bit sender ID against each endpoint's peer ID. Packets from an unexpected Steam ID are silently dropped after being read. Inside an endpoint, Input, QualityReport, KeepAlive and InputAck also need the 16-bit session magic and a sequence window. SyncRequest and SyncReply bypass the magic check. Channel 4 reads have **no sender check at all**.
- **Handshake:** each side independently does **5 SyncRequest/SyncReply round trips** (nonce = `rand()&0xFFFF`). The first retry fires after 500 ms, later ones after 2000 ms of *send silence*. When a side finishes its own 5 round trips, it goes straight to Running and latches the peer's magic. The GGPO RUNNING event (3) fires per side as soon as **its own** endpoints are Running. There is no "both sides ready" barrier. **The Syncing state has no timeout at all.** Only the game's own 1200-frame counter (+0x22CC4 > 0x4B0, part B/F) ends a sync that never completes.
- **Running-state liveness:** the game sets disconnect timeout 3000 ms and notify start 1000 ms (0x783750, also for spectators in 0x783940). Both are measured in wall-clock ms (timeGetTime) from the last *accepted* packet, not in frames. Sends: inputs re-sent every 200 ms, a keepalive after 200 ms of send silence, a quality report (ping, frame advantage) every 1000 ms. Rollback window: 8 frames (Sync config).
- **Failure ends the process:** about 40 GGPO assertion sites call `MessageBoxA("GGPO Assertion Failed")` and then `_exit(0)` (0x781890 + `_exit`). Some of them can be triggered by peer data: msg type > 7, a non-monotonic peer connect status, an input frame gap, or a full event or send queue.
- Most likely "connected on one side only" mechanism: one side finishes sync and goes Running (its 3000 ms timer is now armed, and the peer's still-arriving SyncRequests keep that timer alive), while the other side keeps losing replies and stays in Syncing with no timeout. The Running side stalls at the 8-frame prediction barrier, and both sides wait for the game-level 20 s counter.

## Functions
| VA | Name | Size | Level | Notes |
|---|---|---|---|---|
| 0x0046CE80 | GAMESTEAM_QoSListner_BaseDtor | 7 | L1 | thiscall(this), ret. Only writes vtable 0x89CB44. QoSListner is a vtable-only member at CNetworkServer+0x1178 (set in CNetworkServer ctor 0x46C6E0); vtable has ONE slot (dtor). No QoS logic exists in the binary. |
| 0x0046CF50 | GAMESTEAM_QoSListner_DeletingDtor | 34 | L1 | thiscall(this, flags), ret 4. vtable 0x89CB44 slot 0; frees if flags&1. Stub class. |
| 0x004C15A0 | IPollSink_DefaultTrue | 5 | L2 | thiscall(this, cookie), ret 4; `mov al,1`. Shared default for OnHandlePoll/OnMsgPoll/OnLoopPoll in SteamUdp/SteamUdpProtocol/backends' IPollSink subobject (keep polling). |
| 0x005FBF50 | GGPOSession_GetNetworkStats_Base | 5 | L2 | thiscall(this, stats*, handle), ret 8; returns 0 (OK). Base GGPOSession vtable 0x985AFC slot 8. |
| 0x00701B30 | GGPOSession_Unsupported2 | 8 | L2 | thiscall(this, a, b), ret 8; returns 5 (GGPO_ERRORCODE_UNSUPPORTED). Base SetFrameDelay (slot 10); in SteamSpectatorBackend also AddPlayer (2), GetNetworkStats (8), SetFrameDelay (10). |
| 0x0077C8C0 | GGPOSession_BaseDtor | 7 | L1 | thiscall(this), ret. Restores base vtable 0x985AFC (SEH unwind / dtor chain). |
| 0x0077C8D0 | GGPOSession_DeletingDtor | 34 | L1 | thiscall(this, flags), ret 4. Base slot 0. |
| 0x0077C900 | GGPOSession_Logv | 20 | L1 | thiscall(this, fmt, va_list), ret 8; forwards to the GGPO logger (indirect call through vtable+0x24 as decompiled) and returns 0. Slot 9 in all three session vtables. Logging is a no-op sink in retail (FUN_006A9DB0 = `xor eax,eax; ret`-class stub). |
| 0x0077C920 | SteamPeer2PeerBackend_ctor | 542 | L2 | thiscall(this, GGPOSessionCallbacks* cb, const char* game, int num_players, int input_size), ret 10h. this=new(0x21790) from 0x77C7A0. Sets 3 vtables (+0 GGPOSession, +4 IPollSink, +8 Udp::Callbacks), Poll ctor at +0x28, Sync ctor at +0x53C (local_connect_status ptr=+0x21780), SteamUdp ctor at +0xA4C, 6 spectator UdpProtocol slots at +0xA60 (0x5780 each). Fields: +0x2176C num_players, +0x21760 num_spectators=0, +0x21764 input_size, +0x21774 next_spectator_frame=0, +0x21778 disconnect_timeout=5000, +0x2177C notify_start=750 (0x2EE), +0x21768 synchronizing=1, +0x21770 next_recommended_sleep=0. Copies 7 callbacks to +0xC..+0x24. Sync config: num_prediction_frames=8. Udp::Init(poll,+8 callbacks). Allocates num_players endpoints -> +0xA58. local_connect_status[4] = {disc=0,last_frame=-1}. Calls cb->begin_game(game="BBCF"). |
| 0x0077CB40 | SteamUdp_Callbacks_BaseDtor | 7 | L1 | thiscall(this), ret. Writes Udp::Callbacks vtable 0x985B6C. |
| 0x0077CB60 | SteamPeer2PeerBackend_dtor | 213 | L2 | thiscall(this), ret. Deletes endpoint array (+0xA58, vector-delete), destroys 6 spectator endpoints (+0xA60), SteamUdp (+0xA4C), Sync (frees saved states via cb->free_buffer). Does NOT call CloseP2PSessionWithUser or send any goodbye packet. |
| 0x0077CC35 | SteamPeer2PeerBackend_DeletingDtor_IPollSinkThunk | 8 | L1 | adjustor thunk (IPollSink subobject, vtable 0x985BC4 slot 0) -> 0x77CD10. |
| 0x0077CC3D | SteamPeer2PeerBackend_DeletingDtor_CallbacksThunk | 8 | L1 | adjustor thunk (Udp::Callbacks subobject, vtable 0x985BDC slot 0) -> 0x77CD10. |
| 0x0077CC50 | SteamUdpProtocol_VectorDeletingDtor | 88 | L1 | thiscall(this, flags), ret 4. flags&2: array delete using count at this-4, element size 0x5780. |
| 0x0077CCB0 | SteamUdp_Callbacks_DeletingDtor | 34 | L1 | thiscall(this, flags), ret 4. |
| 0x0077CD10 | SteamPeer2PeerBackend_DeletingDtor | 33 | L1 | thiscall(this, flags), ret 4. Slot 0 of 0x985B7C; calls 0x77CB60 then free. Reached from ggpo_close_session 0x77C6C0. |
| 0x0077CD40 | SteamPeer2PeerBackend_AddLocalInput | 306 | L2 | thiscall(this, int handle, void* values, int size), ret 0Ch. Slot 3. Returns 7 IN_ROLLBACK if Sync._rollingback (+0x628); 6 NOT_SYNCHRONIZED if +0x21768; 2 if handle out of range; 4 PREDICTION_THRESHOLD if Sync::AddLocalInput rejects (prediction barrier 8 frames); else updates local_connect_status[q].last_frame and SendInput to every endpoint with udp!=0 (result ignored). 0 OK. |
| 0x0077CE80 | SteamPeer2PeerBackend_AddPlayer | 98 | L2 | thiscall(this, GGPOPlayer* p, int* handle), ret 8. Slot 2. p->type (+4): 2 spectator -> AddSpectator(steamid +0x38/+0x3C) (handle NOT written); else p->player_num (+8) must be 1..num_players else 3 PLAYER_OUT_OF_RANGE; *handle=player_num; type 1 remote -> AddRemotePlayer(steamid, queue=player_num-1). type 0 local: nothing. |
| 0x0077CEF0 | SteamPeer2PeerBackend_AddRemotePlayer | 120 | L2 | thiscall(this, steamid_lo, steamid_hi, int queue), ret 0Ch. synchronizing=1; endpoints[queue].Init(udp=+0xA4C, poll=+0x28, queue, steamid, local_connect_status=+0x21780); SetDisconnectTimeout(+0x21778); SetDisconnectNotifyStart(+0x2177C); Synchronize() -> first SyncRequest sent immediately. No check that steamid is a lobby member or differs from other endpoints. |
| 0x0077CF70 | SteamPeer2PeerBackend_AddSpectator | 149 | L2 | thiscall(this, steamid_lo, steamid_hi), ret 8. If num_spectators==6 -> 10 TOO_MANY_SPECTATORS; if !synchronizing -> 11 INVALID_REQUEST (no mid-match join); queue=1000+i; Init/SetTimeouts/Synchronize on +0xA60+i*0x5780. Returns 0. |
| 0x0077D010 | SteamPeer2PeerBackend_CheckInitialSync | 187 | L2 | thiscall(this), ret. If synchronizing: any player endpoint with udp!=0 and state!=Running(2) and local queue not disconnected -> return; any spectator endpoint with udp!=0 and state not Running/Disconnected -> return; else on_event(3 RUNNING) and synchronizing=0. Running is decided per side, no cross-peer barrier. |
| 0x0077D0D0 | SteamPeer2PeerBackend_DisconnectPlayer | 210 | L2 | thiscall(this, int handle), ret 4. Slot 7. 2 if bad handle; 9 PLAYER_DISCONNECTED if already disconnected; if endpoint is local (udp==0) disconnect every remote queue at current frame; else DisconnectPlayerQueue(q, local_connect_status[q].last_frame). Called on UdpProtocol Disconnected event via vtable+0x1C. |
| 0x0077D1B0 | SteamPeer2PeerBackend_DisconnectPlayerQueue | 203 | L2 | thiscall(this, int queue, int syncto), ret 8. endpoint.Disconnect() (state 3, shutdown timer now+5000 ms); local_connect_status[q] = disconnected/syncto<<1; if syncto < framecount and !=-1 -> Sync::AdjustSimulation(syncto) (rollback); on_event(4 DISCONNECTED_FROM_PEER, player=q+1); CheckInitialSync. |
| 0x0077D280 | SteamPeer2PeerBackend_DoPoll | 744 | L2 | thiscall(this, int timeout), ret 4. Slot 1 (ggpo_idle). If !rollingback: Poll::Pump(0) (reads channel 3 + runs every endpoint OnLoopPoll), PollUdpProtocolEvents; if !synchronizing: Sync::CheckSimulation, SetLocalFrameNumber on endpoints, total_min_confirmed via Poll2Players (<3 players) or PollNPlayers; asserts != INT_MAX (MessageBox+_exit); pushes confirmed frames [next_spectator_frame, min) to each spectator (SendInput false -> spectator endpoint Disconnect 0x77FC30); SetLastConfirmedFrame -> if changed on_event(0xB confirmed frame); every 120 frames (0x78, +0x21770) asks endpoints for RecommendFrameDelay and fires on_event(6 TIMESYNC, max). |
| 0x0077D580 | SteamPeer2PeerBackend_GetNetworkStats | 97 | L2 | thiscall(this, GGPONetworkStats* s, int handle), ret 8. Slot 8. 2 if bad handle; zero 7 dwords; endpoint stats: s[0]=pending_output size(+0x3498), s[2]=ping/RTT ms(+0x1850), s[3]=kbps(+0x185C), s[5]=local frame adv(+0x1888), s[6]=remote frame adv(+0x188C); s[4]=framecount-last_confirmed. Endpoint chosen by ECX in asm (endpoint of handle). |
| 0x0077D5F0 | SteamPeer2PeerBackend_IncrementFrame | 128 | L2 | thiscall(this), ret. Slot 5. Sync::IncrementFrame (save state); DoPoll(0) via vtable+4; drains Sync events into OnSyncEvent (slot 13 = no-op 0x429DC0). |
| 0x0077D670 | SteamPeer2PeerBackend_OnMsg | 201 | L2 | thiscall(this=+8 Udp::Callbacks subobject, steamid_lo, steamid_hi, UdpMsg* msg, int len), ret 10h. For each player endpoint then each spectator endpoint: HandlesMsg(steamid) -> that endpoint's OnMsg(msg,len), return. Packets from a Steam ID that matches no endpoint are silently DROPPED (already consumed from Steam). Only the 64-bit SteamID is checked. |
| 0x0077D740 | SteamPeer2PeerBackend_OnUdpProtocolEvent | 188 | L2 | thiscall(this, UdpEvent* e, int player), ret 8. Slot 14. Maps UdpProtocol event -> GGPO event: 0 Connected->0, 1 Synchronizing->1 (count,total), 2 Synchronized->2 then CheckInitialSync, 5 NetworkInterrupted->7 (with timeout), 6 Resumed->8. Others ignored. |
| 0x0077D820 | SteamPeer2PeerBackend_OnUdpProtocolPeerEvent | 300 | L2 | thiscall(this, UdpEvent* e, int queue), ret 8. Slot 15. Calls OnUdpProtocolEvent(e, queue+1). Input(3): if queue not locally disconnected, asserts new frame == last+1 (else MessageBox "GGPO Assertion Failed" + _exit(0)); Sync::AddRemoteInput; local_connect_status[q].last_frame = frame. Disconnected(4): DisconnectPlayer(queue+1) via vtable+0x1C. |
| 0x0077D950 | SteamPeer2PeerBackend_OnUdpProtocolSpectatorEvent | 113 | L2 | thiscall(this, UdpEvent* e, int idx), ret 8. Slot 16. OnUdpProtocolEvent(e, 1000+idx); Disconnected(4): endpoint.Disconnect, on_event(5 SPECTATOR_DISCONNECTED, 1000+idx). |
| 0x0077D9D0 | SteamPeer2PeerBackend_Poll2Players | 213 | L2 | fastcall(this), ret 4 (asm: thiscall, 1 dummy arg = current frame). For each queue: if endpoint Running, queue_connected = endpoint.GetPeerConnectStatus(i); min over non-disconnected local_connect_status; if peer reports disconnected and not locally -> DisconnectPlayerQueue(i, min) ("disconnecting i %d by remote request"). Returns total_min_confirmed (init 0x0EFFFFFF). |
| 0x0077DAB0 | SteamPeer2PeerBackend_PollNPlayers | 379 | L2 | thiscall(this, frame), ret 4. N-player variant (>=3 players; unused for 2-player BBCF matches). Same semantics, per-queue min over all running endpoints. |
| 0x0077DC30 | SteamPeer2PeerBackend_PollUdpProtocolEvents | 201 | L2 | thiscall(this), ret (tail). Drains each player endpoint's event queue into slot 15, each spectator's into slot 16. |
| 0x0077DD00 | SteamPeer2PeerBackend_SetDisconnectNotifyStart | 81 | L2 | thiscall(this, int ms), ret 4. Slot 12. +0x2177C=ms; endpoints with udp!=0 get +0x3504. Game sets 1000. |
| 0x0077DD60 | SteamPeer2PeerBackend_SetDisconnectTimeout | 81 | L2 | thiscall(this, int ms), ret 4. Slot 11. +0x21778=ms; endpoints get +0x3500. Game sets 3000. |
| 0x0077DDC0 | SteamPeer2PeerBackend_SetFrameDelay | 47 | L2 | thiscall(this, int handle, int delay), ret 8. Slot 10. queue=handle-1; InputQueue[q].frame_delay(+0x24)=delay. Game passes the online delay (2) for the local player only. |
| 0x0077DDF0 | SteamPeer2PeerBackend_SyncInput | 53 | L2 | thiscall(this, void* values, int size, int* disconnect_flags), ret 0Ch. Slot 4. 6 NOT_SYNCHRONIZED while synchronizing; else Sync::SynchronizeInputs, flags out; 0. |
| 0x0077DE30 | SteamSpectatorBackend_ctor | 366 | L2 | thiscall(this, cb*, game, int num_players, int input_size, host_steamid_lo, host_steamid_hi), ret 18h. this=new(0x78E8) from 0x77C820. Poll at +0x28, SteamUdp at +0x53C, host SteamUdpProtocol at +0x548, +0x5CC8 synchronizing=1, +0x5CCC input_size, +0x5CD0 num_players, +0x5CD4 next_input_to_send=0, +0x5CDC/+0x5CE0 timeouts 5000/750, inputs ring [256] x 0x1C at +0x5CE4 (frame=-1). host.Init(queue 0, steamid, connect_status=NULL); Synchronize immediately; begin_game. |
| 0x0077DFA0 | SteamSpectatorBackend_dtor | 145 | L1 | thiscall(this), ret. Destroys host endpoint and udp; no P2P close. |
| 0x0077E031 | SteamSpectatorBackend_DeletingDtor_IPollSinkThunk | 8 | L1 | adjustor thunk -> 0x77E050. |
| 0x0077E039 | SteamSpectatorBackend_DeletingDtor_CallbacksThunk | 8 | L1 | adjustor thunk -> 0x77E050. |
| 0x0077E050 | SteamSpectatorBackend_DeletingDtor | 33 | L1 | thiscall(this, flags), ret 4. Slot 0 of 0x986098. |
| 0x0077E080 | GGPOSession_Unsupported1 | 8 | L2 | thiscall(this, a), ret 4; returns 5 UNSUPPORTED. Base slots 11/12; spectator DisconnectPlayer (7). |
| 0x0077E090 | SteamSpectatorBackend_DoPoll | 26 | L2 | thiscall(this, timeout), ret 4. Poll::Pump(0) + PollUdpProtocolEvents; returns 0. Detailed in docs/Research/SpectatorDesyncInvestigation.md. |
| 0x0077E0B0 | SteamSpectatorBackend_IncrementFrame | 44 | L2 | thiscall(this), ret. DoPoll(0) then PollUdpProtocolEvents; 0. |
| 0x0077E0E0 | SteamSpectatorBackend_OnMsg | 48 | L2 | thiscall(this=+8, steamid_lo, steamid_hi, msg, len), ret 10h. Only the host endpoint: HandlesMsg(steamid) -> OnMsg. Anything else dropped. |
| 0x0077E110 | SteamSpectatorBackend_PollUdpProtocolEvents | 423 | L2 | thiscall(this), ret. Connected->ev0; Synchronizing->ev1; Synchronized-> if synchronizing: ev2 + ev3 RUNNING, synchronizing=0; Input-> SetLocalFrameNumber, SendInputAck, store in ring[frame%256], if frame > +0x5CD8 ev 0xB; Disconnected-> host.Disconnect, ev4; Interrupted->ev7; Resumed->ev8. |
| 0x0077E2E0 | SteamSpectatorBackend_SetDisconnectNotifyStart | 48 | L2 | thiscall(this, ms), ret 4. +0x5CE0=ms; if host endpoint has a udp (+0x54C = host+4 != 0) push to endpoint else 11 INVALID_REQUEST. |
| 0x0077E310 | SteamSpectatorBackend_SetDisconnectTimeout | 48 | L2 | thiscall(this, ms), ret 4. +0x5CDC=ms; same guard; game sets 3000. |
| 0x0077E340 | SteamSpectatorBackend_SyncInput | 363 | L2 | thiscall(this, void* values, int size, int* flags), ret 0Ch. 6 while synchronizing; ring[next%256].frame < next -> return 4 (not arrived; caller 0x4E60D5 advances anyway = known spectator desync); > next -> on_event(0xD HOST_INPUTS_LOST) (game sets disconnected flag); size check assert; copy, next++ ; 0. |
| 0x0077E4B0 | BitVector_ClearBit | 51 | L1 | cdecl(u8* v, int* offset), ret. library: GGPO bitvector. |
| 0x0077E720 | BitVector_SetBit | 49 | L1 | cdecl(v, offset). library. |
| 0x0077E760 | BitVector_WriteNibblet | 697 | L1 | cdecl(v, int nibble, int* offset). library; assert nibble<256 (BITVECTOR_NIBBLE_SIZE 8). |
| 0x0077EBA0 | GameInput_Equal | 477 | L1 | thiscall(this, GameInput* other, bool bitsonly), ret 8. library (logs frame/size/bits mismatches, asserts sizes). |
| 0x0077ED80 | GameInput_Init | 224 | L1 | thiscall(this, int frame, void* bits, int size), ret 0Ch. library; asserts 0<size<19 (GAMEINPUT_MAX_BYTES*MAX_PLAYERS). |
| 0x0077EE90 | InputQueue_AddDelayedInputToQueue | 469 | L1 | thiscall(this, GameInput*, int frame), ret 8. library; ring of 128 inputs (0x1C each), prediction check sets first_incorrect_frame; asserts -> MessageBox+_exit. |
| 0x0077F070 | InputQueue_AddInput | 172 | L1 | thiscall(this, GameInput*), ret 4. library; applies frame_delay (+0x24) via AdvanceQueueHead. |
| 0x0077F120 | InputQueue_AdvanceQueueHead | 282 | L1 | thiscall(this, int frame), ret 4. library; fills delay gap by repeating last input. |
| 0x0077F240 | InputQueue_DiscardConfirmedFrames | 235 | L1 | thiscall(this, int frame), ret 4. library. |
| 0x0077F330 | InputQueue_GetConfirmedInput | 202 | L1 | thiscall(this, int frame, GameInput* out), ret 8. library. |
| 0x0077F400 | InputQueue_GetInput | 494 | L1 | thiscall(this, int frame, GameInput* out), ret 8. library; prediction = repeat last input. |
| 0x0077F5F0 | InputQueue_Init | 132 | L1 | thiscall(this, int id, int input_size), ret 8. library; 128-entry queue, all fields -1. |
| 0x0077F680 | InputQueue_ResetPrediction | 156 | L1 | thiscall(this, int frame), ret 4. library. |
| 0x0077F720 | SteamUdp_ctor | 88 | L2 | thiscall(this), ret. vtable 0x986664; +4 callbacks=0. |
| 0x0077F780 | SteamUdp_dtor | 72 | L1 | thiscall(this), ret. Back to IPollSink vtable. No P2P close. |
| 0x0077F7D0 | SteamUdp_DeletingDtor | 95 | L1 | thiscall(this, flags), ret 4. |
| 0x0077F830 | SteamUdp_Init | 33 | L2 | thiscall(this, Poll* poll, Udp::Callbacks* cb), tail-jumps to Poll::RegisterLoop. +4=cb, +8=poll; registers OnLoopPoll so every Poll::Pump reads channel 3. |
| 0x0077F860 | SteamUdp_OnLoopPoll | 240 | L2 | thiscall(this, cookie), ret 4. while ISteamNetworking::IsP2PPacketAvailable(&size, ch 3) { ReadP2PPacket(buf[0x1000], 0x1000, &len, &sender, ch 3); if ok cb->OnMsg(sender_lo, sender_hi, buf, len) }. No sender filtering here. Context+0x20 = ISteamNetworking (vtable +4 IsP2PPacketAvailable, +8 ReadP2PPacket). |
| 0x0077F950 | SteamUdp_SendTo | 54 | L2 | stdcall-like (steamid_lo, steamid_hi, void* data, int len), ret 10h (ECX unused). ISteamNetworking::SendP2PPacket(steamid, data, len, k_EP2PSendUnreliable=0, channel 3). Return value ignored. |
| 0x0077F990 | UdpEventRing_ctor | 54 | L1 | thiscall(this). RingBuffer<UdpProtocol::Event,256> init (type=-1, head/tail/size 0). Located at UdpProtocol+0x3770. |
| 0x0077F9D0 | SteamUdpProtocol_ctor | 419 | L2 | thiscall(this), ret. vtable 0x9866D8; zeroes state; magic 0, remote_magic 0, connected 0; TimeSync ctor (+0x3510); event ring ctor; 3 GameInputs = frame -1; peer_connect_status[4] = {0,-1}; send_latency=getenv int "ggpo.network.delay" (+0x1C), oop_percent=getenv "ggpo.oop.percent" (+0x20) (debug lag/out-of-order simulation, 0 in retail). |
| 0x0077FB80 | SteamUdpProtocol_dtor | 103 | L1 | thiscall(this), ret. ClearSendQueue; TimeSync dtor. |
| 0x0077FBF0 | SteamUdpProtocol_ClearSendQueue | 56 | L1 | thiscall(this), ret. Frees every queued message (send_queue size +0x1848). |
| 0x0077FC30 | SteamUdpProtocol_Disconnect | 31 | L2 | thiscall(this), ret. state(+0x1878)=3 Disconnected; shutdown_timeout(+0x34F8)=timeGetTime()+5000. No network packet, no Steam close; the next SendPendingOutput carries disconnect_requested=1. |
| 0x0077FC50 | SteamUdpProtocol_GetEvent | 62 | L1 | thiscall(this, Event* out), ret 4. Pops event ring (+0x5778 size); 1 if any. |
| 0x0077FC90 | SteamUdpProtocol_GetNetworkStats | 54 | L1 | thiscall(this, stats*), ret 4. See 0x77D580. |
| 0x0077FCD0 | SteamUdpProtocol_GetPeerConnectStatus | 37 | L2 | thiscall(this, int id, int* frame), ret 8. *frame = peer_connect_status[id].last_frame (+0x1868); returns !disconnected. |
| 0x0077FD00 | SteamUdpProtocol_HandlesMsg | 46 | L2 | thiscall(this, steamid_lo, steamid_hi), ret 0Ch. false if udp(+4)==NULL; true iff peer steamid (+8/+0xC) == sender. The ONLY sender validation on channel 3. |
| 0x0077FD30 | SteamUdpProtocol_Init | 69 | L2 | thiscall(this, SteamUdp* udp, Poll* poll, int queue, steamid_lo, steamid_hi, ConnectStatus* local_status), ret 18h. +4 udp, +0x14 queue, +8/+0xC peer steamid, +0x1864 local_status; magic(+0x10) = rand() repeated until nonzero (CRT rand, never srand'ed: seed 1 per thread); Poll::RegisterLoop(this). |
| 0x00780210 | SteamUdpProtocol_OnInvalid | 105 | L2 | thiscall(this, msg, len). Assert "Invalid msg in SteamUdpProtocol" -> MessageBoxA("GGPO Assertion Failed") + _exit(0). Reached for msg type >7. |
| 0x00780280 | IPollSink_OnPeriodicPoll_True | 5 | L2 | thiscall(this, a, b), ret 8; `mov al,1`. Also handler-table slot 6 (KeepAlive -> true). |
| 0x00780290 | SteamUdpProtocol_OnLoopPoll | 543 | L2 | thiscall(this, cookie), ret 4. Main timer state machine (see Flows): PumpSendQueue; Syncing: resend SyncRequest if last_send_time && last_send+ (roundtrips==5 ? 500 : 2000) < now; Running: every 200 ms SendPendingOutput (if no input recv in 200 ms), every 1000 ms QualityReport(type 4, ping=now, frame_adv), every 1000 ms UpdateNetworkStats, KeepAlive(type 6) if nothing sent for 200 ms, Interrupted event(5) at last_recv+notify_start, Disconnected event(4) at last_recv+timeout; Disconnected: after shutdown_timeout udp=NULL (endpoint stops handling msgs). |
| 0x007804B0 | SteamUdpProtocol_OnMsg | 161 | L2 | thiscall(this, UdpMsg* msg, int len), ret 8. Accept if type==SyncRequest(1) or SyncReply(2), OR (hdr.magic==remote_magic(+0x18) AND (u16)(seq - last_seq(+0x350C)) <= 0x8000). Then last_seq=seq; type>7 -> OnInvalid (process exit); dispatch table 0x986830; if handler true: last_recv_time(+0x34F4)=now, and if notify was sent and Running -> queue Resumed(6). len is never checked against the type. |
| 0x00780780 | UdpMsg_PayloadSize | 251 | L1 | thiscall(msg), ret. Per-type payload size (send length = size+5); unknown type asserts (udp_msg.h:100). |
| 0x007808A0 | SteamUdpProtocol_PumpSendQueue | 469 | L2 | thiscall(this), ret. For each queued msg: optional simulated latency/out-of-order (env knobs); asserts dest steamid!=0; SteamUdp_SendTo(dest, msg, size+5); free. All GGPO traffic = SendP2PPacket unreliable, channel 3. |
| 0x00780A80 | SteamUdpProtocol_QueueEvent | 196 | L2 | thiscall(this, Event* e), ret 4. Push to ring (+0x3770, 256x32 B, size +0x5778); full -> assert/_exit. |
| 0x00780B50 | SteamUdpProtocol_RecommendFrameDelay | 14 | L1 | thiscall(this), ret. TimeSync::recommend_frame_wait_duration(require_idle_input=false) on +0x3510. |
| 0x00780B60 | SteamUdpProtocol_SendInput | 84 | L2 | thiscall(this, GameInput*), ret 4. if udp: if Running: TimeSync advance_frame(local/remote adv), pending_output.push (returns false when 255 unacked inputs queued -> caller may Disconnect a spectator); SendPendingOutput. Returns true otherwise. |
| 0x00780BC0 | SteamUdpProtocol_SendInputAck | 123 | L1 | thiscall(this), ret. type 7, ack_frame = last_received_input.frame (+0x349C). Spectator side. |
| 0x00780C40 | SteamUdpProtocol_SendMsg | 306 | L2 | thiscall(this, UdpMsg* msg), ret 4. packets_sent++, last_send_time(+0x34F0)=now (ANY message, incl. sync replies), bytes_sent+=size+5; hdr.magic=own magic, hdr.seq=next_send_seq++ (+0x350A); push {time, dest=peer steamid, msg} to send_queue (256; full -> assert/_exit); PumpSendQueue. |
| 0x00780D80 | SteamUdpProtocol_SendPendingOutput | 959 | L2 | thiscall(this). Builds type 3 Input: start_frame, delta-compressed bits of all pending (unacked) inputs vs last_acked, ack_frame=last_received.frame, disconnect_requested = (state==3), peer_connect_status[4] copied from local_connect_status (+0x1864, 16 B). Asserts on frame gaps / >4096 bits. |
| 0x00781150 | SteamUdpProtocol_SendSyncRequest | 131 | L2 | thiscall(this), ret. random(+0x1880) = rand() & 0xFFFF; send type 1 {random}. |
| 0x007811E0 | SteamUdpProtocol_SetDisconnectNotifyStart | 16 | L2 | thiscall(this, ms), ret 4. +0x3504. |
| 0x007811F0 | SteamUdpProtocol_SetDisconnectTimeout | 16 | L2 | thiscall(this, ms), ret 4. +0x3500. |
| 0x00781200 | SteamUdpProtocol_SetLocalFrameNumber | 55 | L2 | thiscall(this, int local_frame), ret 4. local_frame_advantage(+0x1888) = (remote_frame + RTT_ms*60/1000) - local_frame. (remote_frame = last_received_input.frame +0x349C). |
| 0x00781240 | SteamUdpProtocol_Synchronize | 32 | L2 | thiscall(this), ret. If udp: state=Syncing(0), roundtrips_remaining(+0x187C)=5, SendSyncRequest. |
| 0x00781260 | SteamUdpProtocol_UpdateNetworkStats | 127 | L1 | thiscall(this), ret. stats_start_time init; kbps_sent(+0x185C) via ftol of bytes/time. |
| 0x007812E0 | RingBuffer_Event256_Front | 165 | L1 | thiscall(this), ret. library (ring_buffer.h assert). |
| 0x00781390 | RingBuffer_GameInput_Front | 174 | L1 | thiscall(this), ret. library; pending_output (+0x1890, size +0x3498). |
| 0x00781440 | RingBuffer_QueueEntry_Front | 166 | L1 | thiscall(this), ret. library; send_queue (+0x40, size +0x1848). |
| 0x007814F0 | RingBuffer_GameInput_Item | 193 | L1 | thiscall(this, int i), ret 4. library. |
| 0x007815C0 | RingBuffer_Event256_Pop | 192 | L1 | thiscall(this), ret. library. |
| 0x00781760 | RingBuffer_QueueEntry_Pop | 192 | L1 | thiscall(this), ret. library. |
| 0x00781830 | RingBuffer_GameInput_TryPush | 94 | L2 | thiscall(this, GameInput*), ret 4. BBCF change: returns false instead of asserting when size==255. |
| 0x007818B0 | Platform_GetConfigInt | 88 | L1 | cdecl(const char* env_name), ret. GetEnvironmentVariableA + atoi; 0 if unset. Used for ggpo.network.delay / ggpo.oop.percent. |
| 0x00781930 | Poll_StaticBufferInit | 56 | L1 | thiscall(this). library: zero 16 periodic sink slots. |
| 0x00781970 | Poll_ctor | 171 | L1 | thiscall(this). library: zero handle/sink tables, CreateEventW(manual) as handle 0. |
| 0x00781A30 | StaticBuffer_PeriodicSink_At | 164 | L1 | thiscall(this, i), ret 4. library assert. |
| 0x00781AE0 | StaticBuffer_PollSink_At | 162 | L1 | thiscall(this, i), ret 4. library assert. |
| 0x00781B90 | Poll_ComputeWaitTime | 219 | L1 | thiscall(this, int elapsed), ret 4. library. |
| 0x00781C70 | Poll_Pump | 344 | L2 | thiscall(Poll* this, int timeout), ret 4. WaitForMultipleObjects(handles, timeout capped by periodic sinks) then OnLoopPoll for every loop sink (SteamUdp first -> reads channel 3 -> dispatch; then each UdpProtocol timer), periodic sinks. Only runs when the game calls ggpo_idle/DoPoll (0x783430) -> no game-thread pumping = no packets read or sent. |
| 0x00781DD0 | Poll_RegisterLoop | 39 | L1 | cdecl(sink, cookie) (ECX=poll), ret 8. push to loop sinks (max 15). |
| 0x00781E00 | StaticBuffer_PollSink_PushBack | 180 | L1 | thiscall(this, PollSinkCb*), ret 4. library; assert if 15 sinks. |
| 0x00781EC0 | SavedStates_Init | 306 | L1 | thiscall(this), ret. library: 10 saved frames {buf 0, size 0, frame -1, checksum 0}. |
| 0x00782000 | Sync_ctor | 126 | L1 | thiscall(this, ConnectStatus* local_status), ret 4. vtable Sync; +0x50C local_connect_status; +0xF0 last_confirmed=-1. |
| 0x00782080 | Sync_dtor | 95 | L1 | thiscall(this), ret. Frees saved states via cb.free_buffer; deletes input queues. |
| 0x00782120 | Sync_AddLocalInput | 128 | L2 | thiscall(this, int queue, GameInput*), ret 8. Rejects if framecount >= max_prediction(8) and framecount-last_confirmed >= 8 ("reached prediction barrier"); saves frame 0; input.frame=framecount; AddInput. |
| 0x007821A0 | Sync_AddRemoteInput | 32 | L1 | thiscall(this, int queue, GameInput*), ret 8. InputQueue[q].AddInput. |
| 0x007821C0 | Sync_AdjustSimulation | 415 | L2 | thiscall(this, int seek_to), ret 4. on_event(9 rollback start, frames); LoadFrame(seek_to); ResetPrediction all queues; advance_frame x count; on_event(10 rollback end); asserts framecount consistency (MessageBox+_exit). |
| 0x00782360 | Sync_CheckSimulation | 37 | L1 | thiscall(this, timeout), ret 4. If CheckSimulationConsistency finds an incorrect frame -> AdjustSimulation. |
| 0x00782390 | Sync_CheckSimulationConsistency | 145 | L1 | thiscall(this, int* seek_to), ret 4. min first_incorrect_frame over queues. |
| 0x00782430 | Sync_CreateQueues | 231 | L1 | thiscall(this), ret. new InputQueue[num_players] (0xE44 each) -> +0xFC. |
| 0x00782540 | Sync_FindSavedFrameIndex | 175 | L1 | thiscall(this, frame), ret 4. assert if not in the 10 saved frames. |
| 0x00782600 | Sync_GetConfirmedInputs | 376 | L1 | thiscall(this, void* out, int size, int frame), ret 0Ch. For spectators; disconnected players -> zero input + flag bit. |
| 0x00782780 | Sync_GetEvent | 62 | L1 | thiscall(this, Event*), ret 4. library ring (32). |
| 0x007827C0 | Sync_IncrementFrame | 11 | L1 | thiscall(this). framecount++; SaveCurrentFrame. |
| 0x007827D0 | Sync_Init | 79 | L1 | thiscall(this, SyncConfig*), ret 4. config copy; framecount 0; max_prediction = config.num_prediction_frames (8); CreateQueues. |
| 0x00782820 | Sync_LoadFrame | 290 | L1 | thiscall(this, frame), ret 4. cb.load_game_state; asserts. |
| 0x00782950 | Sync_SaveCurrentFrame | 124 | L1 | thiscall(this), ret. cb.free_buffer old, cb.save_game_state. |
| 0x007829D0 | Sync_SetFrameDelay | 29 | L1 | thiscall(this, queue, delay), ret 8. InputQueue[q]+0x24. |
| 0x007829F0 | Sync_SetLastConfirmedFrame | 96 | L1 | thiscall(this, frame), ret 4. DiscardConfirmedFrames(frame-1); returns changed. |
| 0x00782A50 | Sync_SynchronizeInputs | 532 | L2 | thiscall(this, void* values, int size), ret 8. Fills inputs (disconnected queues -> zeros + flag); BBCF addition: fires on_event(0xC, frame, inputs) every call (game uses it for input display/replay). |
| 0x00782C70 | RingBuffer_SyncEvent_Front | 162 | L1 | thiscall(this). library. |
| 0x00782D20 | RingBuffer_SyncEvent_Pop | 187 | L1 | thiscall(this). library. |
| 0x00782DE0 | TimeSync_ctor | 61 | L1 | thiscall(this), ret. zero local/remote windows (40 frames); +0x25C=120 (0x78). |
| 0x00782E20 | TimeSync_dtor | 7 | L1 | thiscall(this), ret. |
| 0x00782E60 | TimeSync_AdvanceFrame | 97 | L1 | thiscall(this, GameInput*, int adv, int radv), ret 0Ch. library. |
| 0x00782ED0 | TimeSync_RecommendFrameWaitDuration | 297 | L1 | thiscall(this, bool require_idle_input), ret 4. Average local vs remote advantage over 40 frames; recommends sleep only if >=2 frames (cap 9). Global counter 0x182A91C. |

Related functions outside the list. They were read to answer the questions and are referenced below:

| VA | Role |
|---|---|
| 0x0046F650 | GAMESTEAM_CNetworkTransport::OnP2PSessionRequest. `AcceptP2PSessionWithUser(req->m_steamIDRemote)` with no condition. Registered in 0x46C7C0 through 0x46C290 (callback id 0x4B2 = 1202). |
| 0x00429DC0 | GAMESTEAM_CNetworkTransport::OnP2PSessionConnectFail. It is just `ret 4` (registered in 0x46C7C0, id 0x4B3 = 1203). The same address is the Peer2Peer `OnSyncEvent` no-op. |
| 0x00424CE0 / 0x00424CA0 | AASTEAM_CVoiceChatManager P2PSessionRequest / ConnectFail handlers. Accept only if the sender is in the voice member map; ConnectFail stores the error byte at member+0x18. |
| 0x0041D0E0 / 0x0041D260 | AASTEAM_CNetworker (vtable 0x84FF54 slots 4/3): channel-0 receive into the global message queue 0xA129B0 (voice 0xFF01 packets diverted), and channel-0 send (`SendP2PPacket(..., unreliable 0, ch 0)`, loopback when the destination is 0, 0x7F000001 or the local SteamID). |
| 0x0077C7A0 / 0x0077C820 | ggpo_start_session (new 0x21790) / ggpo_start_spectating (new 0x78E8). |
| 0x0077C680..0x0077C8A0 | ggpo_* wrappers (null session -> 1 INVALID_SESSION): add_local_input (slot 3), advance_frame (5), close_session (delete), get_network_stats (8), idle (1), set_disconnect_notify_start (12), set_disconnect_timeout (11), set_frame_delay (10), add_player (2), synchronize_input (4). |
| 0x00783750 / 0x00783940 | RollbackSessionManager StartSession (ret 30h) / StartSpectating (ret 2Ch). File `..\SmallSrc\Rollback\Network\RollbackSessionManager.cpp`, singleton at 0xA5BD08 (pointer 0x182A920). |
| 0x00783430 | RollbackSessionManager::Idle(timeout): desync checker send/recv (ch 4), then ggpo_idle. Called from 0x44F790 and 0x4E55E0. |
| 0x00783550 | RollbackSessionManager::Close: ggpo_close_session, deletes listener and desync checker, resets. Called from 0x4E5F40. |
| 0x00783FA0 | GGPO on_event callback (game side). Event-code strings are at 0x986DF4..0x986FC0. |
| 0x004E5740 | SCENE_CBattle session start: builds the GGPOPlayer[] from the room member table and calls StartSession/StartSpectating. |
| 0x0077FD80, 0x007806E0, 0x00780610, 0x00780580, 0x00780560, 0x00780190 | UdpProtocol handlers OnInput, OnSyncRequest, OnSyncReply, OnQualityReport, OnQualityReply, OnInputAck (dispatch table 0x986830, slot 6 KeepAlive = 0x780280). |

## Data

### Steam usage (ISteamNetworking = `SteamInternal_ContextInit(&0x9D3210) + 0x20`, interface SteamNetworking005)
| Use | Call | Channel | Send type | Where |
|---|---|---|---|---|
| GGPO send | SendP2PPacket (vtbl +0) | 3 | 0 Unreliable | 0x77F950 |
| GGPO recv | IsP2PPacketAvailable (+4) / ReadP2PPacket (+8), buffer 0x1000 | 3 | - | 0x77F860 |
| Desync checksum send | SendP2PPacket, 0x14 bytes | 4 | 2 Reliable | 0x783D60 |
| Desync checksum recv | Read, buffer 0x40, **sender ignored** | 4 | - | 0x783C50 |
| Lobby messages/voice send | SendP2PPacket | 0 | 0 Unreliable | 0x41D260 |
| Lobby messages/voice recv | Read, buffer 0x4F2 | 0 | - | 0x41D0E0 |
| Accept session | AcceptP2PSessionWithUser (+0xC) | - | - | 0x46F650 (always), 0x424CE0 (voice members) |
| Close session / channel, GetP2PSessionState, AllowP2PPacketRelay | **never called** by the game | | | decomp-wide search for `ctx+0x20` vtable +0x10/+0x14/+0x18/+0x1C finds none |

### GGPO error codes (unchanged from GGPO)
0 OK, 1 INVALID_SESSION, 2 INVALID_PLAYER_HANDLE, 3 PLAYER_OUT_OF_RANGE, 4 PREDICTION_THRESHOLD, 5 UNSUPPORTED, 6 NOT_SYNCHRONIZED, 7 IN_ROLLBACK, 8 INPUT_DROPPED, 9 PLAYER_DISCONNECTED, 10 TOO_MANY_SPECTATORS, 11 INVALID_REQUEST.

### GGPO event codes as BBCF numbers them (handled by 0x783FA0)
0 CONNECTED_TO_PEER (player status 1), 1 SYNCHRONIZING (progress = count*100/total), 2 SYNCHRONIZED (progress 100), 3 RUNNING (sets manager+0x110 = 1, which the network task 0x56BCB0 waits for), 4 DISCONNECTED_FROM_PEER (manager+0x112 = 1 disconnected flag, player status 3), 5 SPECTATOR_DISCONNECTED, 6 TIMESYNC (frames ahead added to manager+0x118; consumed by 0x7836D0, which skips 1 frame every 10), 7 CONNECTION_INTERRUPTED (player, timeout ms), 8 CONNECTION_RESUMED, 9 rollback begin, 10 rollback end, 0xB confirmed frame (BBCF), 0xC synchronized inputs (BBCF), 0xD HOST_INPUTS_LOST (spectator, sets the disconnected flag). The listener (manager+4) receives each one through vtable+0x10(code, a, b).

### UdpProtocol internal events (ring at +0x3770)
-1 Unknown, 0 Connected, 1 Synchronizing{total, count}, 2 Synchronized, 3 Input{GameInput}, 4 Disconnected, 5 NetworkInterrupted{disconnect_timeout}, 6 NetworkResumed.

### Wire format (UdpMsg, packed; send length = PayloadSize + 5)
| off | field |
|---|---|
| +0 u16 | magic (the sender's own random nonzero session magic) |
| +2 u16 | sequence number (sender `+0x350A`++) |
| +4 u8 | type: 1 SyncRequest, 2 SyncReply, 3 Input, 4 QualityReport, 5 QualityReply, 6 KeepAlive, 7 InputAck. Anything >7 = assert + process exit |
| SyncRequest +5 | u32 random_request (rand()&0xFFFF) |
| SyncReply +5 | u32 random_reply (echo) |
| Input +5..+0x14 | peer_connect_status[4] (bit0 disconnected, last_frame<<1) |
| Input +0x15 | start_frame; +0x19 bit0 disconnect_requested, bits1..31 ack_frame; +0x1D u16 num_bits; +0x1F u8 input_size; +0x20 delta-compressed bits (max 4096 bits) |
| QualityReport +5 | s8 frame_advantage; +6 u32 ping (sender timeGetTime) |
| QualityReply +5 | u32 pong (echo of ping) -> RTT = now - pong |
| InputAck +5 | ack_frame (31 bits) |

### SteamUdpProtocol (0x5780 bytes, vtable 0x9866D8)
+0x04 SteamUdp* (NULL = endpoint not used, or shut down) · +0x08/+0x0C peer SteamID · +0x10 u16 own magic · +0x14 queue (0..n-1 player, 1000+ spectator) · +0x18 u16 remote magic (0 until own sync completes) · +0x1A u8 connected-event-sent · +0x1C send_latency (env ggpo.network.delay) · +0x20 oop_percent (env ggpo.oop.percent) · +0x28..+0x38 out-of-order packet slot · +0x40 send_queue[256] of {time, ?, dest SteamID, msg*} (head +0x1840, size +0x1848) · +0x1850 round_trip_time ms · +0x1854 packets_sent · +0x1858 bytes_sent · +0x185C kbps_sent · +0x1860 stats_start_time · +0x1864 local_connect_status* · +0x1868 peer_connect_status[4] · **+0x1878 state: 0 Syncing, 1 Synchronized (unused), 2 Running, 3 Disconnected** · +0x187C sync.roundtrips_remaining / running.last_quality_report_time · +0x1880 sync.random / running.last_network_stats_interval · +0x1884 running.last_input_packet_recv_time · +0x1888 local_frame_advantage · +0x188C remote_frame_advantage · +0x1890 pending_output ring (size +0x3498, capacity 255) · +0x349C last_received_input (frame), +0x34B8 last_sent_input, +0x34D4 last_acked_input · +0x34F0 last_send_time · +0x34F4 last_recv_time · +0x34F8 shutdown_timeout · +0x34FC disconnect_event_sent · +0x3500 disconnect_timeout · +0x3504 disconnect_notify_start · +0x3508 u8 disconnect_notify_sent · +0x350A u16 next_send_seq · +0x350C u16 next_recv_seq · +0x3510 TimeSync (0x260) · +0x3770 event ring [256 x 32] (head +0x5770, tail +0x5774, size +0x5778).

### SteamPeer2PeerBackend (0x21790 bytes; vtables 0x985B7C / 0x985BC4 (IPollSink) / 0x985BDC (Udp::Callbacks))
+0x0C..+0x24 GGPOSessionCallbacks {begin_game, save_game_state, load_game_state, log_game_state, free_buffer, advance_frame, on_event} · +0x28 Poll (0x514) · +0x53C Sync (+0x628 rollingback, +0x62C last_confirmed_frame, +0x630 framecount, +0x634 max_prediction=8) · +0xA4C SteamUdp {vtbl, +4 callbacks, +8 poll} · +0xA58 SteamUdpProtocol* endpoints[num_players] · +0xA60 spectators[6] (inline) · +0x21760 num_spectators · +0x21764 input_size · +0x21768 u8 synchronizing · +0x2176C num_players · +0x21770 next_recommended_sleep (frame) · +0x21774 next_spectator_frame · +0x21778 disconnect_timeout · +0x2177C disconnect_notify_start · +0x21780 local_connect_status[4] (bit0 disconnected, last_frame<<1).
Vtable slots: 0 dtor, 1 DoPoll, 2 AddPlayer, 3 AddLocalInput, 4 SyncInput, 5 IncrementFrame, 6 Chat (OK), 7 DisconnectPlayer, 8 GetNetworkStats, 9 Logv, 10 SetFrameDelay, 11 SetDisconnectTimeout, 12 SetDisconnectNotifyStart, 13 OnSyncEvent (no-op), 14 OnUdpProtocolEvent, 15 OnUdpProtocolPeerEvent, 16 OnUdpProtocolSpectatorEvent.

### SteamSpectatorBackend (0x78E8 bytes; vtables 0x986098 / 0x9860D0 / 0x9860E8)
+0x28 Poll · +0x53C SteamUdp · +0x548 host SteamUdpProtocol · +0x5CC8 u8 synchronizing · +0x5CCC input_size · +0x5CD0 num_players · +0x5CD4 next_input_to_send · +0x5CD8 last confirmed frame received · +0x5CDC disconnect_timeout · +0x5CE0 notify_start · +0x5CE4 inputs[256] x 0x1C (frame %256).

### GGPOPlayer (0x40 bytes, built by 0x4E5740)
+0 size (0x40), +4 type (0 local, 1 remote, 2 spectator), +8 player_num (1,2 players; 3.. spectators), +0x38/+0x3C SteamID (low/high). The opponent's SteamID comes from the room member table: `roomData(0x4A0FE0) + 0x23244 + idx*4 -> member +0x78/+0x7C`. The local index is `member+0x2C` bit0 (only if bit1 is set; otherwise -1). Spectators come from 8 slots at roomData+0x22D84 (stride 0x98): kind==2, same room, flag 0x1000, status 10 or 11. SteamID at slot+0x4C/+0x50. Only mode 1 (see Flows) adds spectators.

### RollbackSessionManager (static at 0xA5BD08, ptr 0x182A920)
+0 GGPOSession* · +4 listener (game, PerformanceMonitor for spectators) · +8 desync checker (0x220 B, 0x783A00: +0x20C interval 0x5A = 90 frames, +0x210 mismatch count, +0x218/+0x21C peer SteamID = first remote player) · +0xC..+0x24 callbacks copy · +0x28 local handle · +0xEC num_players · +0xF0 num_spectators · +0x110 RUNNING reached · +0x111 is_spectator · +0x112 disconnected / host-inputs-lost · +0x114 in rollback · +0x118 timesync frames to absorb · +0x120 input size.
`0x7836A0` "session broken" = disconnected flag OR desync mismatches > 5. `0x783730` = mismatches > 1 (the "DESYNC DETECTED" overlay, 0x4E5340).

### Constants
| Constant | Value | Where |
|---|---|---|
| NUM_SYNC_PACKETS | 5 | 0x781240 |
| first sync retry / later | 500 / 2000 ms (since last send of ANY message) | 0x780290 |
| running input resend | 200 ms | 0x780290 |
| keepalive | 200 ms of send silence | 0x780290 |
| quality report / net stats | 1000 / 1000 ms | 0x780290 |
| disconnect timeout / notify (GGPO default) | 5000 / 750 ms | ctors 0x77C920, 0x77DE30 |
| disconnect timeout / notify (game sets) | **3000 / 1000 ms** | 0x783750, 0x783940 |
| endpoint shutdown after Disconnect | 5000 ms | 0x77FC30 |
| prediction frames | 8 | 0x77C920 (Sync config) |
| online frame delay | from 0x4E5974 (2), local player only | 0x783750 -> SetFrameDelay |
| time-sync recommendation interval | 120 frames | 0x77D280 (+0x78) |
| max spectators | 6 | 0x77CF70 |
| spectator input ring | 256 frames | 0x77DE30 |
| pending_output capacity | 255 inputs | 0x781830 |
| desync check interval | 90 frames, 10 slots | 0x783A00 |
| game sync give-up | roomData+0x22CC4 > 0x4B0 (1200 frames, about 20 s) -> error 4 | 0x4ACED0, 0x4AF1A0 (part B/F) |

## Flows

### 1. Session creation (per match, SCENE_CBattle 0x4E5740)
1. 0x49A340 returns the mode. **3 = spectate**: StartSpectating(host SteamID) -> 0x77C820 -> spectator ctor, which immediately calls `host.Synchronize()`. **1 or 2 = player**: two GGPOPlayers (local + remote with the opponent's SteamID). In mode 1 spectator players (type 2) are also appended. Then StartSession 0x783750.
2. StartSession: creates the listener and the desync checker (peer = first remote player's SteamID), ggpo_start_session(cb, "BBCF", num_players, input_size), `SetDisconnectTimeout(3000)`, `SetDisconnectNotifyStart(1000)`, then AddPlayer for each entry (local: SetFrameDelay(handle, delay)). AddPlayer(remote) -> AddRemotePlayer -> `endpoint.Init` + `Synchronize` -> **SyncRequest #1 queued and sent immediately** through SendP2PPacket ch 3. If no Steam P2P session exists yet, this first send makes Steam open one.
3. The peer's Steam client raises P2PSessionRequest_t -> 0x46F650 accepts it (and both sides are already sending, so Steam would accept implicitly anyway).

### 2. GGPO endpoint state machine (SteamUdpProtocol, timers in OnLoopPoll 0x780290, run once per Poll::Pump = once per game-thread `Idle`)
- **Syncing (0)**: entry sets roundtrips=5 and sends a SyncRequest. Retry: if `last_send_time + (roundtrips==5 ? 500 : 2000) < now`, send a new SyncRequest with a new nonce. `last_send_time` is refreshed by **every** sent message, including our SyncReplies to the peer. **No timeout**: Syncing can last forever.
  - On SyncRequest from the peer (0x7806E0): rejected only if `remote_magic != 0 && hdr.magic != remote_magic`. Otherwise reply SyncReply(echo nonce). This works in every state.
  - On SyncReply (0x780610): if not Syncing, return `hdr.magic == remote_magic`. If the nonce differs from our last request, ignore it (a reply to an *older* request is ignored too). First good reply: event Connected once. `--roundtrips`; when it reaches 0: event Synchronized, **state = Running (2)**, last_received_input = -1, `remote_magic = hdr.magic`. Otherwise event Synchronizing(5, 5-remaining) and send the next SyncRequest at once.
  - Result: 5 sequential round trips **per direction**, each side counting only replies to its own requests. Best case: about 5 RTT. Every lost request or reply costs 500 ms (first) or 2000 ms (later) of send silence.
- **Running (2)**: every pump: if no input packet was received in 200 ms, SendPendingOutput (all unacked inputs). Every 1000 ms QualityReport (ping, local frame advantage); the reply sets RTT. Every 1000 ms UpdateNetworkStats. KeepAlive if nothing was sent for 200 ms. If `last_recv + notify_start(1000) < now` and not yet notified: event NetworkInterrupted(timeout - notify = 2000). If `last_recv + timeout(3000) < now`: event Disconnected (once). Any accepted message refreshes last_recv. If notify was sent, the next accepted message raises NetworkResumed.
  - Input with `disconnect_requested` set -> event Disconnected (peer-initiated graceful disconnect).
- **Disconnected (3)**: entered through Disconnect() 0x77FC30 (local decision). Sets the shutdown timer to now+5000; when it expires, `udp = NULL`, the endpoint ignores all messages (HandlesMsg false) and stops sending. Inputs sent in this state carry disconnect_requested=1.

### 3. Session-level (SteamPeer2PeerBackend)
- `synchronizing=1` from AddRemotePlayer until CheckInitialSync (0x77D010) sees every used player endpoint Running and every spectator Running/Disconnected -> on_event(3 RUNNING) -> manager+0x110 = 1 -> the network task (0x56BCB0, "NTTSK_Synchronizing") continues into the battle. While synchronizing, AddLocalInput and SyncInput return 6 NOT_SYNCHRONIZED.
- Running: DoPoll pumps the network, rolls back when needed, computes the confirmed frame, pushes confirmed frames to spectators, and every 120 frames fires a TIMESYNC recommendation. Each AddLocalInput sends the input (plus all unacked ones) to every endpoint immediately. AddLocalInput fails with 4 PREDICTION_THRESHOLD once the local side is 8 frames ahead of the last confirmed frame. The game then stalls waiting for the peer (it does not disconnect).
- Disconnect paths: (a) the endpoint Disconnected event (timeout or peer request) -> DisconnectPlayer -> DisconnectPlayerQueue -> rollback to the last confirmed frame of that queue -> on_event(4) -> manager+0x112 -> the network task shows the error (0x7836A0 checked in 0x4ACED0/0x4AF1A0 -> error 0x1D/0x10). (b) Poll2Players: the peer reports through its peer_connect_status that a queue is disconnected -> the same path ("disconnecting i %d by remote request").

### 4. Spectator
Spectator: one endpoint to the host, Synchronize at creation. Host: AddSpectator only while synchronizing (otherwise INVALID_REQUEST), max 6, queue 1000+i. After the host is Running, it sends only *confirmed* frames to spectators (both players' inputs combined, 0x782600). The spectator acks with InputAck. A full spectator pending queue (255 frames unacked) makes the host Disconnect that spectator. The spectator side treats a missing frame as "return 4". The caller ignores that, which is the desync documented in docs/Research/SpectatorDesyncInvestigation.md. A frame that was overwritten in the 256 ring gives event 0xD -> disconnected flag.

### 5. Teardown, rematch, leave
Match end: 0x4E5F40 -> 0x783550 -> ggpo_close_session -> deleting dtor 0x77CD10 / 0x77E050. All endpoints, queued messages and events are freed. **No goodbye packet, no CloseP2PSessionWithUser.** The peer finds out only through its own 3000 ms timeout or its own close. Rematch / next match = a brand-new GGPO session (new magic numbers, new handshake) over the same still-open Steam P2P session. Between sessions nobody reads channel 3. Packets that arrive in that gap stay in Steam's queue and are drained by the first Pump of the next session; they are rejected there by magic or nonce (see Suspected bugs #5). Steam sessions are left to Steam's own idle timeout. Leaving the room does not close them either.

### 6. Desync checker (BBCF, channel 4)
Every 90 confirmed frames, the save callback 0x7842B0 records (frame, checksum) in slot `(frame/90)%10` and compares it against the remote entry for the same frame (0x783AD0). Equal resets the mismatch count to 0; different increments it (up to 10 entries are logged). On each Idle (0x783430): unsent local entries are sent reliably on ch 4 (0x783D60). All ch-4 packets are drained into remote slots (0x783C50) **without checking who sent them**. More than 1 mismatch -> "DESYNC DETECTED" overlay; more than 5 -> treated like a disconnect (0x7836A0).

## Failure paths
| # | Condition (address) | Cause side | What the user sees | Recovery |
|---|---|---|---|---|
| F1 | Steam cannot establish the P2P session (NAT, relay, peer not logged in): P2PSessionConnectFail_t ignored (0x429DC0) | Steam / P2P | Nothing directly; sync never progresses ("Synchronizing" forever) | GGPO keeps re-sending SyncRequests every 2 s, so Steam retries the session each time. No GGPO timeout; the game gives up at the 1200-frame counter (error 4) |
| F2 | Sync packets lost (unreliable ch 3) | P2P | Slow "connecting"; each loss adds 500/2000 ms | Automatic retry; never fails on its own |
| F3 | One side Running, the other still Syncing (0x77D010 is per-side) | P2P + local logic | The Running side enters battle and freezes at frame about 8 (prediction barrier 0x782120, AddLocalInput -> 4). The other side is still on the sync screen | Resolves if the lagging side gets its 5th reply. Otherwise both sides wait for game-level timers (error 4 on the syncing side; the Running side keeps getting the peer's SyncRequests, so its 3000 ms timeout never fires) |
| F4 | No accepted packet for 1000 ms in Running (0x780290) | peer / P2P / peer's game thread stalled | "Connection interrupted" (event 7) | Resumes on the next packet (event 8) |
| F5 | No accepted packet for 3000 ms in Running | same | Disconnect (event 4 -> manager+0x112 -> network error 0x1D/0x10) | None; the match ends |
| F6 | Peer sends disconnect_requested (its own Disconnect) | peer | Disconnect | None |
| F7 | Peer reports a queue disconnected through connect status (Poll2Players 0x77D9D0) | peer | Disconnect, with a rollback to the min confirmed frame | None |
| F8 | Any GGPO assertion (about 40 sites; 0x781890 MessageBox "GGPO Assertion Failed" then `_exit(0)`), e.g. msg type >7 (0x7804B0 -> 0x780210), peer connect status going backwards (0x77FD80 line 0x21F), input frame gap (0x77D820, 0x77FD80), send/event queue full (0x780C40, 0x780A80), total_min_confirmed == INT_MAX (0x77D280) | peer data or local logic | Message box, then the game closes silently with exit code 0 | None |
| F9 | Spectator starved (0x77E340 returns 4) / overwritten (event 0xD) | host / P2P | Spectator desync, or spectator disconnect | Documented in SpectatorDesyncInvestigation.md |
| F10 | AddSpectator after sync is done (0x77CF70 -> 11) or 7th spectator (-> 10) | local logic | Spectator never joins | None |
| F11 | Desync checksum mismatches >5 (0x7836A0) | game simulation, or a stray ch-4 packet | Network error (same path as a disconnect) | None |
| F12 | Channel-0 receive queue full: 0x41D1E0 `jmp 0041D1E0` (an infinite loop on purpose) when 0x40A770 cannot append to the global queue 0xA129B0 | peer flooding ch 0, or the consumer not draining | **Hard freeze** of the thread that polls CNetworker | None (process hang) |
| F13 | Sender SteamID not a known endpoint (0x77D670 / 0x77E0E0) | third party / stale peer | Nothing (silently dropped) | n/a |

## Suspected bugs
1. **No validation when accepting P2P sessions** (0x46F650). Every P2PSessionRequest is accepted while CNetworkServer exists. *Symptom:* any Steam user can open a session and push data into channels 0/3/4. Channel 3 is safe (SteamID filter), but channel 0 is queued with the sender ID for the lobby layer, and channel 4 is trusted blindly (see 2). Confidence high. Mod-fixable: hook 0x46F650 or the callback and accept only current room/lobby members plus the expected opponent and spectators. The mod already closes some sessions in `src/Network/RankedListConnectionFilter.cpp`.
2. **Channel-4 desync checker trusts any sender and any session** (0x783C50 reads `ReadP2PPacket(..., ch 4)` and ignores `psteamIDRemote`; the packet carries no session id). *Symptom:* false "DESYNC DETECTED" or network error from a stray, stale or foreign ch-4 packet (reliable packets queued at the end of the previous match are delivered to the next session's checker). Collisions need the same `frame` value in the same slot, so they are rare. Confidence medium-low on real-world impact, high on the mechanism. Mod-fixable: hook 0x783C50 and drop packets whose sender != checker+0x218.
3. **Connect failures are invisible and there is no GGPO sync timeout** (0x429DC0, plus no timeout in the Syncing state of 0x780290). *Symptom:* "rooms stuck / failed to connect after a long wait". Every P2P failure turns into a silent 20 s wait for the game-level 1200-frame counter, instead of an early, accurate error. Confidence high. Mod-fixable: handle P2PSessionConnectFail_t (log m_eP2PSessionError, 1 NotRunningApp, 2 NoRightsToApp, 3 DestinationNotLoggedIn, 4 Timeout), and/or query GetP2PSessionState during Syncing for diagnostics (relay vs direct, connecting).
4. **Asymmetric "connected" state** (Flows 2/3, F3). Each side becomes Running on its own 5th reply. The Running side's 3000 ms timer keeps being fed by the lagging side's SyncRequests (OnSyncRequest returns true -> last_recv refreshed in 0x7804B0), so it neither disconnects nor makes progress. This is a direct cause of "one side thinks it is connected while the other is not". Confidence high on the mechanism (stock GGPO behaviour). Mod-fixable in part: log sync progress per side (events 0/1/2/3 already reach 0x783FA0), or add a sync deadline.
5. **No end-of-session cleanup on the wire** (0x783550 -> dtors; no CloseP2PSessionWithUser, no channel drain). Stale ch-3 packets are rejected by magic or nonce, so this is mostly harmless. The weak point is the magic itself: `rand()` from the CRT, **never srand'ed** (only `_holdrand = 1` at thread init, 0x7A0000.c), 16-bit, generated at endpoint Init (0x77FD30). Two sessions in a row on the same machine can draw the same magic if the main thread's rand stream is consumed identically. A late Input from the previous match would then pass the magic check and hit the frame assertions (F8 -> process exit). Confidence low. Mod-fixable: seed rand, or flush ch 3 (read and discard) right before ggpo_start_session.
6. **Self-hang on the channel-0 queue** (0x41D1E0 `jmp $`). If the global lobby message queue fills (0x40A770 returns 0), the game spins forever. *Symptom:* hard freeze in lobby/room, more likely when the mod or a peer floods ch 0 (mod packets share ch 0). Confidence medium (the consumer side belongs to part C/D). Mod-fixable: hook the receive loop and drop packets instead of spinning.
7. **Assertion = silent exit** (0x781890 + `_exit(0)`). Peer-controlled data (connect-status regression, input frame gaps, type >7 behind a valid magic) can terminate the client with exit code 0 and only a MessageBox (which may be hidden behind a fullscreen D3D window). *Symptom:* "game closed during/at start of an online match". Confidence medium. Mod-fixable: hook 0x781890 to log the assertion text to DEBUG.txt (cheap, high value).
8. **Timeouts are wall-clock and pumped only by the game thread.** GGPO sends and reads only inside Idle/DoPoll (0x783430 -> Poll::Pump). If either side's game thread blocks for more than 1 s at match start (stage/palette loads, the 100 MB arena allocation 0x7864C0, cold-boot stalls), the peer sees "interrupted"; more than 3 s means a disconnect. The timers start at Running, which can come before the slower side has finished loading (open question 1). Confidence medium. Partly mod-fixable: raise the game's 3000 ms setting at 0x783750 (push imm), or keep pumping during loads.
9. Edge: in 0x4E5740, if the local member's flag bit1 is clear, the local index is -1, so **both** players are added as remote with the same SteamID (`idx==0` member). The second endpoint can never receive (HandlesMsg returns the first match), so sync never completes. Confidence low (the state may be unreachable).

## Cross-part edges
- **From part B (network task / scene):** 0x56BCB0 (NTTSK_Synchronizing) waits for manager+0x110 (RUNNING). 0x4ACED0 / 0x4AF1A0 compare the counter roomData+0x22CC4 against 1200 frames (error 4) and check `0x7836A0` (disconnect/desync -> errors 0x1D/0x10). 0x4E5740 / 0x4E5F40 create and close the session per battle. 0x44F790 / 0x4E55E0 call Idle (pump).
- **From part C/D (network server / Steam lobby):** GAMESTEAM_CNetworkServer (ctor 0x46C6E0) owns CNetworkTransport (callbacks registered in 0x46C7C0) and the stub QoSListner (+0x1178). The opponent's and spectators' SteamIDs come from the room member table at roomData(0x4A0FE0)+0x23244 / +0x22D84, so a wrong or stale member entry there means GGPO syncs against the wrong ID forever. AASTEAM_CNetworker channel-0 transport (0x41D0E0 / 0x41D260) carries the lobby-level peer messages (unreliable!) into queue 0xA129B0, which parts C/D consume.
- **Voice (AASTEAM_CVoiceChatManager):** shares ch 0 (packet type 0xFF01), has its own session-request filter, and records P2P connect-fail errors per member.
- **Mod:** `SteamNetworkingWrapper` replaces the context's ISteamNetworking pointer, so every call above goes through it (LOG level 7). `NetworkManager.cpp` sends mod packets on ch 0. `RankedListConnectionFilter.cpp` already calls CloseP2PSessionWithUser for some peers. `NetplayArenaKeeper.cpp` concerns the 100 MB arena allocated in the same battle init (0x4E5C15). Spectator hooks are at 0x77E340 / 0x4E60F1.

## Open questions
1. Order inside battle init: is the GGPO session (0x4E5740) started before or after the heavy loads (arena 0x4E5C15, stage data)? If before, the 1000/3000 ms timers can fire while one side is still loading (Suspected bug 8). Needs a caller trace of 0x4E56F0 and timestamps from an in-game log.
2. Who drains the channel-0 queue 0xA129B0, and does the consumer validate the stored sender SteamID against room membership? (part C/D)
3. Does anything check `ISteamNetworking::SendP2PPacket` return values on ch 0? GGPO ignores them (0x77F950).
4. Which error strings do the codes 4 / 0x1D / 0x10 / 0x25..0x28 written by 0x4ACED0 map to? (UI part F)
5. Exact lifetime of CNetworkServer: is OnP2PSessionRequest live during offline modes (it accepts sessions whenever the object exists)?
6. GGPO base-vtable slot 9 (0x77C900) decompiles as a self-call through vtable+0x24. The asm should be confirmed if logging is ever re-enabled (harmless in retail).
