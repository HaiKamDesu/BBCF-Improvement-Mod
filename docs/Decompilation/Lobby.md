# Online lobby system (ranked, player rooms, connection)

Why this exists: players report "failed to connect", rooms getting stuck, matches with people
they never picked, and connections that are never validated. This maps the whole system so each
symptom can be put on one of three sides: the **server** (Arc System Works' web server or
Steam's backend), the **client** (the game's own logic), or **peer-to-peer** (the connection
between the two players).

Status (2026-10-09): **mapped**. All 1,289 functions are L1 or higher, and 333 are L2. L2
covers every create, search, join, enter, leave, connect, timeout and failure path. Nothing has
been rewritten yet. The part docs were written by six parallel reverse-engineering passes over
the archive decompile and the disassembly. Where this page says *checked*, the claim was
re-verified by hand in the disassembly. Everything else is code reading, not yet confirmed
in-game.

## The parts

| Part | Doc | Functions | What it is |
|---|---|---|---|
| A | [Lobby_A_ArcServerClient.md](Lobby_A_ArcServerClient.md) | 389 | `uei` client for Arc System Works' server. Web API: login, create user, region/area/lobby catalog, TUS profile read/write. TCP chat server for Lobby mode. Includes libcurl, Jansson and msgpack. |
| B | [Lobby_B_NetworkTask.md](Lobby_B_NetworkTask.md) | 332 | `GAME_CNetworkTask`: the state machine of network mode (top menu, login, ranked, player match, room, 2D lobby, exit) and the background ranked matchmaker. Also maps every dialog. |
| C | [Lobby_C_NetworkServer.md](Lobby_C_NetworkServer.md) | 112 | `GAMESTEAM_CNetworkServer` (static at 0x00A62730): login, create, search and join slots; the game-level join handshake (P2P messages 0x33-0x3A); QoS ping table; room member table. |
| D | [Lobby_D_SteamLobby.md](Lobby_D_SteamLobby.md) | 247 | Steam lobby layer: `AASTEAM_CNetworker`, the create/join/search session objects and their call results, invites, lobby keys and search filters, channel-0 P2P messages. Also the 64-seat avatar lobby (`CSTEAMNetworkLobbyData`). |
| E | [Lobby_E_ConnectionNetcode.md](Lobby_E_ConnectionNetcode.md) | 138 | GGPO over Steam P2P: `SteamPeer2PeerBackend`, `SteamUdp(Protocol)`, `GGPOSession`, spectator backend, P2P session callbacks, sync handshake, timeouts. |
| F | [Lobby_F_MatchmakingUI.md](Lobby_F_MatchmakingUI.md) | 71 | The matchmaking menus and the network main-menu controller (0x0070C120): what each choice asks the network task for, the rematch protocol, and the notices. |

## How a match actually happens

Arc System Works' server is **not** in the path of ranked or player rooms. Those run only on
Steam lobbies plus the game's own messages over Steam P2P (part A summary). Arc's server is used
for login, creating a user, the profile (TUS / D-Code), and the 2D Lobby mode's chat rooms.

1. **Network entry (B tasks 2-3).** Steam login check, `user/login` on Arc's server, then the
   profile sync. The mod has already rewritten the profile sync (ProfileSync.md). If login
   fails, the game carries on online without Arc's server, so profiles do not save for the
   session.
2. **Ranked (B task 4/5, D, C).**
   - The player browses a list built from a Steam lobby search. The ranked filters are
     NETWORK_VERSION, MATCHING_FILTER, rank range, distance and optionally RANK_DISCONNECT. There
     is no slots-available filter.
   - Meanwhile the background matchmaker hosts an *invisible* 2-member Steam lobby of its own.
     In state 0x27 it does this after 300 frames.
   - Joining means clicking a row. Then: Steam `JoinLobby`, `LobbyEnter_t`, a game join request
     (message 0x33) to the lobby owner over P2P, and the owner's room snapshot (0x35).
   - The host starts the match once both seated players send READY (message 8). There is no
     connectivity check.
3. **Player rooms (B task 6/7).** A public Steam lobby, a list search filtered on
   MATCHING_FILTER and room type, then the same join handshake. A room refuses you if your RTT
   class is below its setting.
4. **Match (E).**
   - The opponent's SteamID comes from the room member table (roomData+0x23244).
   - A fresh GGPO session syncs with 5 round trips per side, on Steam P2P channel 3, then runs.
     Disconnect fires after 3000 ms wall clock without packets.
   - Steam P2P sessions are never closed; the next match reuses them.

## Verdict per symptom

| Symptom | Main causes | Side | Confidence | Fixable in the mod |
|---|---|---|---|---|
| **"Failed to connect" (RankMatch_ConnectError / PlayerMatch ConnectError)** | Every failed `LobbyEnter_t` (full, gone, banned, limited, error) becomes one generic result 100 (C, D). Searches have no slots filter and hosts are never made unjoinable, so full and busy lobbies are listed (D). The join handshake gives up after 0x708 ticks (C). Ghost lobbies left by late create results also time out with this error (D). | Client logic. The full or gone answer itself comes from Steam and is accurate. | High | Yes: slots filter or our own pre-check, specific messages, hide busy hosts, cancel late results |
| **Matched with someone you did not pick / "match found with strangers"** | One shared room table serves hosting and joining. A player can host and join at once, a successful join copies the other room in, and nothing clears it. The next joiner of your still-open lobby is seated next to the old pair (C, root cause of the known stale-room bug). The ranked screen also hosts its own room after 300 frames and jumps to "match found", discarding your click (F). | Client | Medium-high (needs one more field in the mod's stale-room dump to confirm) | Yes: clear the table, block join while hosting or close our lobby first |
| **Clicked one room, joined another** | Joins resolve the list row index (task+0x1EC), not the lobby ID, on the next tick; both lists auto-refresh every 600 frames (F). | Client | High | Yes: capture the lobby ID at click time |
| **Rooms stuck / members that never leave / UI stuck** | Leave and closed are 3 unreliable unacknowledged messages. Steam's own membership events (`LobbyChatUpdate_t`, `LobbyKicked_t`) are never registered (C, D). `Room_Leave` returns 0x66 without leaving while busy, and the callers call it once then wait with no timeout (B). Create, search and join waits have no timeout at any layer, and a Steam call handle of 0 wedges the session object for the whole session (B, D, F). A stale in-match member can block seating on the host (B, unverified). | Client | High for the mechanisms; how often each fires in practice is not measured | Yes: listen to Steam membership, retry leave, add watchdogs, reset wedged objects |
| **Connections not validated / one side connected, the other not** | Every P2P session request is accepted from anyone (checked: 0x0046F650 calls AcceptP2PSessionWithUser unconditionally). `P2PSessionConnectFail_t` is ignored (checked: 0x00429DC0 is `ret 4`). The match starts on READY with no connectivity check (B). GGPO's RUNNING is per side with no both-sides barrier, so one side runs and freezes while the other still syncs; each side's sync requests keep feeding the other side's disconnect timer (E). The only cap is the game's 1200-frame sync counter. | Client (design), with the network as the trigger | High | Partly: a both-sides ready barrier and P2P checks before READY, reacting to connect-fail, validating senders |
| **Endless "connecting" / login never finishes** | On Arc's server: "in flight", "curl failed", "bad MD5" and "bad JSON" all look the same, several requests wait forever, and some rejections never post a result (A). Lobby-mode TCP connect failures read as "still connecting" (A). | Client handling of server or network failures | High | Yes: result-aware polling, timeouts |
| **Disconnect at match start** | 3000 ms wall-clock disconnect, with the network pumped only on the game thread, so a load stall of 3 s or more on either side drops the peer (E). The 100 MB arena failing to allocate closes the game (fixed in the mod already). | Client (timing) | Medium | Possibly: pump GGPO during loads |
| **Game closes silently mid-match** | About 40 GGPO assertion sites end in a MessageBox and `_exit(0)` (0x00781890), and some are reachable from peer data (E). | Client, triggered by the peer | Medium | Log first (hook 0x00781890), then decide |
| **Hard freeze in a room** | The channel-0 receive loop spins forever (`jmp $` at 0x0041D1E0) when its queue is full (D, E). | Client | Medium (needs the queue size) | Yes |

**Conclusion.** Most of what players experience is the game's own client logic. There are no
timeouts or cancels, failure reasons are collapsed into one, nothing listens to Steam's own
membership events, there is no connectivity barrier before a match, the room table is shared
and stale, and joins go by list index. Steam's backend answers correctly when it says a lobby is
full or gone. The game just shows every answer as "failed to connect". Arc's server is not in
the ranked or room path at all. It can still make network entry hang, and the game handles its
failures badly. Real network conditions (NAT, Steam relay) still decide whether two particular
players can connect. The game never asks Steam about P2P state and never reacts to a failure, so
today that is invisible.

## Confirmed code bugs (independent of how often they fire)

- Ranked state 0x37 decrements +0x14 and tests +0x20, so its 3000-frame timeout never fires
  (checked: 0x004AF77A / 0x004AF78F). B.
- A failed client leave sets the join-failed flag 0x20000 (0x0049B3F1). B.
- `P2PSessionConnectFail_t` handler is empty (checked: 0x00429DC0). E.
- P2P session requests accepted from anyone (checked: 0x0046F650). E.
- `Socket_PollConnect` (0x00426EB0) reports a failed connect as "still connecting". A.
- `WebApi_Submit` (0x00434750) and `WebApi_Reset` (0x00434D30) free request descriptors that a
  detached HTTP thread may still be writing for up to 10 s (use-after-free). A.
- Join failure leaves the Steam lobby when the owner is invalid (state 1 -> 5 without
  `LeaveLobby`). C, D.
- AddMember (0x0049EF50) never checks for a free slot. C.
- Messages 0x34/0x35/0x37 (reject, room snapshot, room closed) are accepted from any sender in
  any join state. C.
- GGPO's session magic comes from `rand()` with no `srand`, so every session uses the same magic
  sequence. E.

## What needs runtime confirmation, and how

The mod already wraps ISteamMatchmaking and ISteamNetworking (`src/SteamApiWrapper`). An
instrumentation build should log:
- every lobby call and callback (create, join, search with the filters and the result count,
  `LobbyEnter_t` response, leave);
- every P2P session request and connect-fail with the remote SteamID and whether it is a lobby
  member;
- `GetP2PSessionState` for the opponent at READY and at match start;
- network task state changes (DAT_00CF7958 +0/+4) and every dialog id;
- GGPO assertions (hook 0x00781890).

Then: a few hours of real ranked and room play from testers, plus the stale-room dump extended
to member+0x2C/+0x78 (C). That turns the "medium" rows above into numbers and tells us which
fixes matter most.

## Proposed fix order (design not started)

1. **Pure client bugs with no protocol change:**
   - capture the lobby ID at click time;
   - give each failure its own specific message;
   - add a slots-available filter;
   - make the ranked 0x37 timer real;
   - always `LeaveLobby` on failure;
   - drop 0x34/0x35/0x37 from non-hosts;
   - fix `Socket_PollConnect`.
2. **Watchdogs:** a timeout and a reset for every create, search, join and leave wait, and for
   wedged session objects, with a visible error instead of an endless spinner.
3. **Membership:** register `LobbyChatUpdate_t` and `LobbyKicked_t` and remove members Steam
   says have left; clear the room table when a guest role ends; never host and join at once.
4. **Connection:**
   - reject P2P sessions from non-members;
   - react to `P2PSessionConnectFail_t`;
   - check the P2P session state before READY;
   - add a both-sides barrier before the match.

   A change that needs both players running the mod must degrade to vanilla against a vanilla
   peer.
