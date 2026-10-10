# Online lobby system (ranked, player rooms, connection)

Why this exists: players report "failed to connect", rooms getting stuck, matches with people
they never picked, and connections that are never validated. This maps the whole system so each
symptom can be put on one of three sides: the **server** (Arc System Works' web server or
Steam's backend), the **client** (the game's own logic), or **peer-to-peer** (the connection
between the two players).

Status (2026-10-09): **mapped**, first fixes in. All 1,289 functions are L1 or higher, and 333
are L2. L2 covers every create, search, join, enter, leave, connect, timeout and failure path.
The instrumentation build (`LobbyTrace`) and the first client-side fixes (`LobbyFixes`) are in
the mod; see [Fixes in the mod](#fixes-in-the-mod). The part docs were written by six parallel reverse-engineering passes over
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

## Fixes in the mod

`src/Network/LobbyFixes.cpp`, setting `LobbyFixes` (default 1; 0 = vanilla). Nothing changes
on the wire: a delivered message is byte-identical to the one the peer would have sent, so
players with and without the mod still play together.

| Fix | Bug it closes | How | Verified |
|---|---|---|---|
| Searches skip full lobbies | "Failed to connect" from full rooms and ranked hosts already in a match (D bug 6) | `AddRequestLobbyListFilterSlotsAvailable(1)` before the game's only `RequestLobbyList` (0x0041C270, ranked and rooms) | In-game: the filter goes out with the ranked search and the list comes back (4 and 7 hosts, all 1/2) |
| Handshake messages only from the host | A late 0x34 reject / 0x35 room info / 0x37 room closed from a previous host kicks you out of, or overwrites, the room you are in now (C) | The P2P read drops these three (matched on type and exact length; on the wire every message is `u16 wire length, u16 length, u16 type`, from 0x0041D260, and the mod's own are type 0x95) unless the sender owns a Steam lobby we are in; the next packet is read in its place | Code path runs on live channel-0 traffic; a real drop needs a second player |
| Ranked rooms cannot be hijacked by an old room | "Failed to connect" to ranked hosts whose Steam lobby holds only themselves: their game refuses every join with 0x34 reason 2 (room full) for as long as they stay listed | `Msg2_RoomSnapshot` (0x0049C000) applies any room snapshot (msg 2, length 0x3A8) with no sender or role check, and a new room's revision 0 always passes `Room_ApplySnapshot` (0x004A01E0). A player who leaves a room before its host drops them (the host keeps broadcasting msg 2 to them about once a second for up to 5 s, or indefinitely while its +0xAAC is 0 after a match) and then hosts ranked gets the old room copied over the new one: count 2, the old host as host, themselves a plain member. They are no longer the host, so nothing ever closes or repairs the room. Msg 2 joins the host-only messages: dropped unless the sender owns a Steam lobby we are in | Log evidence 2026-10-09 (2 hosts, 3 refusals); mechanism from the code, unverified in-game. A vanilla zombie host cannot be fixed from the joining side; the ranked list filter hides it after one failure, and the refusal now says "room full" instead of "failed to connect" |
| Missed leaves delivered | Members that never leave, a guest stuck in a room whose host crashed (C, "Membership via Steam callbacks") | Listens to `LobbyChatUpdate_t`. When Steam says a peer left and the game still has them 2 s later, on the menus only (never mid-match, where GGPO handles it), it hands the game the peer's own message: 0x36 to a host, 0x37 to a guest whose host left. The host matches 0x36 by sender address `{id64, 1000}`, which the injected packet carries like a real one | Needs a second player |
| Join failures say why | Ranked shows every `LobbyEnter_t` failure as "failed to connect" | Notification with the Steam reason (full, gone, blocked, limited...) | Needs a failing join |
| Every lobby gets a connection rating | Ranked/room list Delay ratings that stay blank until a manual refresh (C bug 5) | Each result needs 5 ping/pong samples and `QoS_PumpPings` (0x004702E0) never re-sends, so one lost ping (typically the first, sent while Steam opens the P2P session) blanks that row for good. A ping unanswered after 1 s is re-armed (`ts[k] = 0`) so the pump re-sends it; a late pong for the old one matches nothing. Up to 15 re-sends per row. The listener also has only 20 rows (`QoS_AddRow` 0x0046F720 drops the rest): on the ranked and player match lists, finished rows are handed to results still waiting | In-game: a host missed ping 1 twice, was re-sent, and got rated (7 out / 7 in). The past-20 path needs a list of more than 20 |
| Ranked upload timeout | State 0x37 tests +0x20 instead of +0x14, so its 3000-frame timeout never fires (B bug 2) | `83 7F 20 00` -> `83 7F 14 00` at 0x004AF78F, byte-checked | Patch installs in-game |
| No profile re-send on a live token | "Dropped your sign-in", failed profile uploads and empty D-code cards after a slow server answer (A bug 5's re-request, with a token race) | The TUS workers (download 0x00422B00, upload 0x00423A50) give up after 300 x Sleep(10), about 3 s, and re-submit with the same session token while curl still runs the first request (10 s limit). The server mints a new token with each answer, so when the first lands the re-send is refused with `param.status` 3. `cmp edi,300` -> `cmp edi,1100` at 0x00422BC2 and 0x00423B10, byte-checked: a re-send only follows a finished request. Also no use-after-free of the abandoned descriptor on this path | Log evidence (2026-10-09, 9 player matches over 2 h): 6 of 6 answers slower than 3 s were followed by a refusal; every refusal but one (a cooldown gap, fixed in NetworkStallDiagnostics) followed one. Untested in-game |
| No freeze on a full message queue | Channel-0 drain spins forever (`jmp $`, 0x0041D1E0) when the game's queue is full (D, E) | `EB FE` -> `EB 00`: leave the drain loop; that one packet is lost, the rest wait in Steam's queue for the next frame | Patch installs in-game |

`src/Network/LobbyTrace.cpp`, setting `LobbyTrace` (default 1), writes `[LobbyTrace]` lines to
DEBUG.txt:
- every lobby call (who called, the filters, the call handle), its result and how long Steam
  took, calls Steam never answers (warnings at 10, 30 and 120 s), and calls the game abandons;
- the lobby lists with members/limit and keys;
- Steam's membership and P2P events, flagged when they disagree with the game's room table;
- P2P traffic per peer and channel (10 s summaries, silences over 2 s);
- the game's lobby messages by type;
- `GetP2PSessionState` for room members;
- the network task's states, dialogs and toasts, the automatch steps, the net server's state
  bytes and the room table;
- GGPO assertions (0x00781890);
- Arc server requests with endpoint, HTTP code and time (0x00438BB0).

Lobby membership is tracked whenever either setting is on, because the fixes need it.

### What testers should do

Two players, both on this build, `GenerateDebugLogs=1`. Each scenario, then send DEBUG.txt:
1. **Ranked:** join from the list a few times; also leave a host waiting in the list until it
   auto-matches. Expected: no full hosts listed; any failure shows "Could not join: ...".
2. **Room, guest quits hard:** guest joins a player room, then kills the game (Task Manager).
   Expected on the host: within about 2 s of Steam noticing, `[LobbyFixes] ... delivering their
   leave` and the member disappears from the room.
3. **Room, host quits hard:** same with the host killed. Expected on the guest:
   `[LobbyFixes] the host ... left ... delivering "room closed"` and the guest is returned to the
   room list instead of sitting in a dead room.
4. **Plain play for an hour** in ranked and rooms. Grep for `FAILED`, `STILL no answer`,
   `abandoned`, `silence`, `GGPO assert`, `ignored "`. These are the counts the open questions
   below need.

## Proposed fix order

Done items are in [Fixes in the mod](#fixes-in-the-mod).

1. **Pure client bugs with no protocol change:**
   - capture the lobby ID at click time (the race is about one click in 600 frames; low priority);
   - ~~give each failure its own specific message~~ (notification; the game's own dialog is
     unchanged);
   - ~~add a slots-available filter~~;
   - ~~make the ranked 0x37 timer real~~;
   - always `LeaveLobby` on failure (the trace flags `JoinLobby ... while still in lobby`, which
     measures how often this matters);
   - ~~drop 0x34/0x35/0x37 from non-hosts~~;
   - fix `Socket_PollConnect` (2D Lobby only).
2. **Watchdogs:** a timeout and a reset for every create, search, join and leave wait, and for
   wedged session objects, with a visible error instead of an endless spinner. Wait for the
   trace: `STILL no answer` lines say which waits actually hang, and for how long.
3. **Membership:** ~~register `LobbyChatUpdate_t` and remove members Steam says have left~~;
   `LobbyKicked_t`; clear the room table when a guest role ends; never host and join at once.
4. **Connection:**
   - ~~reject P2P sessions from non-members~~ **dropped**: the trace shows the game pinging every
     listed host (message 0x10, plus channel 200) before joining any lobby, to rate connections.
     Those peers are not members, so rejecting them breaks the list's connection ratings;
   - react to `P2PSessionConnectFail_t`;
   - check the P2P session state before READY;
   - add a both-sides barrier before the match.

   A change that needs both players running the mod must degrade to vanilla against a vanilla
   peer.
