# Ranked "match found" with two strangers — stale host room

## Symptom (reporter, 2026-09-26)

In ranked, clicking a player occasionally produces a "match found" popup naming
**two other people**, both already marked OK with a grey square, instead of
yourself and the person you clicked. Nothing ever starts. Long-standing, thought
to be rare.

## The capture

Live log, session 15:49:24. The click is named:

```
15:50:49.162  click snapshot: cursorRow=76561199408150892(XxBigSwagHazamaFanxX)
15:50:49.179  JoinLobby lobby=109775242960256011 ownerSteamId=76561199408150892
15:50:49.179  join attempt ... pre-join liveness: members=1/2
15:50:49.479  [LobbyLink] current lobby = 109775242960256011 (via LobbyEnter)
15:50:49.479  lobby liveness ... members=2/2
15:50:49.795  connection to steamId=76561199408150892 confirmed (room member)
15:52:59.377  popup message "" -> "RankMatchLeaveMyself"
```

So the **Steam** side was entirely correct: the row clicked owned that lobby,
it went 1/2 → 2/2 on our join, and the host was found in the game's own room
struct 300 ms later. The popup showed "Raynne_" and "dqflynn" — neither name
appears anywhere in the log. Two minutes of `gameState=27`, then the kick.

The game-side room object tells the rest (`[NETCOLOR][Diag] localRoom`):

```
15:50:49  readable=1 steam=76561198103782817 member=4 matchPlayer=4 matchId=0
```

We were placed in **slot 4**, outside the match pair, with **matchId 0**. The
popup renders the pair in `matchPlayerIndex` 0/1 — the two strangers, whose OK
flags are stale from the host's previous match. No lobby or room existed on our
side before the click (0 prior `LobbyEnter`), so the stale entries are the
host's.

## The signature holds across 24 joins

First readable room after `LobbyEnter`, against outcome:

| outcome | our slot | count |
|---|---|---|
| match loaded | `matchPlayer` 0 or 1, `matchId` ≥ 1 | 21 / 21 |
| kicked, room readable | `matchPlayer` **4** or **6**, `matchId` **0** | 3 / 3 |
| kicked, room never readable | — | 2 |

The last row is a different failure (the host never delivered room state) and is
not this bug. Also: it is not rare right now — the 12:39 session had
`RankMatchLeaveMyself` on 9 of 12 joins, with zero RTT-check messages.

There is **no pre-join signal**: `PLAYER_SESSION_FLAG` / `_VALUE` were empty on
all 23 joins that logged liveness, good and bad alike. Detection has to be
post-join; the good news is the room is functional ~300 ms after `LobbyEnter`.

## Existing machinery this plugs into

`RankedListConnectionFilter::OnLeaveLobby` already names this the "zombie-room
class" ("join accepted, game backed out pre-match") and calls
`MarkLobbyBroken` + `MarkUnreachable`, and `reactiveFailCount > 0` gates the
list. It just only learns after the two-minute kick, and without a reason.

## What shipped

`InspectRoomAfterJoin()`, run at the top of `PollPendingConnectionConfirmation`
(before the existing confirmation clears the pending state), once per join, as
soon as our own entry is present:

1. **Dump every occupied slot** — index, steamId, persona name, `matchId`,
   `matchPlayerIndex`, `netcolor`, and the six unnamed bytes of
   `RoomMemberEntry` (+1..3, +0x35..0x37). One of those is the OK flag the
   popup draws; a healthy dump beside a stale one identifies it.
2. **Detect**: our `matchPlayerIndex >= 2` → `MarkLobbyBroken`,
   `MarkUnreachable` with a specific reason, and an on-screen notice.
3. **`RankedStaleRoomAutoLeave`** (default off): also `LeaveLobby`, after
   clearing the pending state so `OnLeaveLobby` does not double-mark. Nothing
   in the mod has ever left a lobby programmatically, so it is proven before it
   is defaulted on.

`RoomMemberEntry` (0x98): `memberIndex`@0, `steamId`@4, `matchId`@0x30,
`matchPlayerIndex`@0x34, `netcolor`@0x5A. Room: eight contiguous entries from
`Room::member1`@0x48; `Room::memberCount`@0x38.

## Root cause — host side, still open

The host's client keeps its previous match's entries in the room struct after
that match ends (or after the opponent drops), and broadcasts them to the next
joiner. Fixing it for real means fixing the *host*, which only helps when the
host runs the mod — but that is a large share of ranked.

Next RE, in order:
1. Where `RoomManager::m_pRoom` comes from (the game's room singleton) and the
   game functions that write `member1..8` — Ghidra xrefs on that array.
2. The clear-on-match-end / member-leave path, and the condition under which a
   slot is *not* released (opponent disconnect vs. clean leave is the obvious
   suspect).
3. Verify by hosting with the mod: dump our own room after every match end and
   look for entries that survive.
