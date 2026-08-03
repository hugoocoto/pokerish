# Poker WebSocket API

Protocol for connecting remote clients to the poker server.

- Transport: WebSocket.
- Encoding: JSON (UTF-8), one JSON object per message.
- Keys: lowercase snake_case.
- One connection = one seat. Seat changes only happen at the end of a
  round: `hello` reserves a free (bot) seat and the server replies
  `queued`; when the current round ends the bot is replaced and the
  server sends `welcome`, after which the connection is bound to the
  seat. Clients leaving mid-hand are folded; a bot takes the seat over
  at the end of the round. (Tournament mode changes this: no bots, see
  `hello` and section 4.)

## Envelope

Every message has a `type` field. `id` is optional; when a client sends
an `id`, the server echoes it in the corresponding `reply` or `error`,
so the client can match responses to requests.

    { "type": "action", "id": 7, "action": "call" }

## Stages

`preflop`, `flop`, `turn`, `river`, `over`

## Cards

Two-letter strings, rank uppercase + suit lowercase, as produced by
phevaluator: `"As"`, `"Kd"`, `"Td"`, `"2c"`. Positions not yet dealt
are `null`.

---

# 1. Client -> server

## hello

Join the game and request a seat.

    { "type": "hello", "name": "Alice" }

The server replies with `queued` when a bot seat is free, or with
`error` `table_full` when the table is already full. The seat is
reserved, but the bot playing it finishes the current round; when the
round ends the server sends `welcome` and the seat is yours (with a
full entry stack, not the bot's chips). Until then the connection is
not bound to a seat: `action` and `query` are rejected with
`unauthorized`.

In tournament mode (`--tournament`) there are no bots: `hello` in the
`lobby`/`countdown` states seats the client immediately (the server
answers `welcome`, no `queued`), and once the tournament is `running`
or `finished` every `hello` is rejected with `table_full` (no late
registration, no re-entry).

When the server is started with `--token SECRET` (or a token is set in
tests), `hello` must carry the same token:

    { "type": "hello", "name": "Alice", "token": "SECRET" }

A missing or wrong token is rejected with `error` `bad_token`. Without
`--token` any token (or none) is accepted.

`name` is truncated to 24 characters and control characters are
stripped before it is stored or broadcast.

Human clients (the raylib play client) may mark themselves so the
server skips the simulated "thinking" delay (`--simulate`) for their
actions:

    { "type": "hello", "name": "Alice", "is_human": true }

`is_human` is optional and defaults to `false`; anything that is not a
boolean is rejected with `bad_request`. Bots should not send it — their
actions keep the simulated pacing.

## action

Take an action. `action` is one of:

| action        | meaning                                             |
|---------------|-----------------------------------------------------|
| `fold`        | fold, no condition                                  |
| `check`       | check (illegal if there is a bet to call)           |
| `call`        | call the current bet (amount computed by the server)|
| `call_all`    | call all-in with the whole stack                    |
| `bet`         | bet or raise; `amount` = raise increment, see below |
| `check_or_fold` | check if there is no bet to call, otherwise fold  |

Examples:

    { "type": "action", "action": "call" }
    { "type": "action", "action": "call_all" }
    { "type": "action", "action": "bet", "amount": 20 }

Bet amounts:

- `amount` is the **increment** on top of the current bet; with no
  current bet the increment is simply the full bet.
- The server converts it to the player's total street target:
  `target = current_bet + amount`, capped at `stack + street_bet`. A
  cap at the stack is an all-in and is always legal.
- If the target is below `current_bet + min_raise` and is not an
  all-in, the action is invalid: `error` `bet_too_small`.
- If it is not the client's turn: `error` `not_your_turn`.
- `check_or_fold` is resolved by the server (check if `current_bet ==
  street_bet`, else fold); it never appears as a broadcast `action`.
- Actions sent by a folded player or while the hand is over are
  rejected with `already_folded` / `game_over`.

## query

Ask for public information, at any time, even while other players are
acting. A query can never change the game.

    { "type": "query", "id": 5, "what": "state" }

| what       | reply payload                                   |
|------------|-------------------------------------------------|
| `state`    | full public state, same schema as the broadcast |
| `my_cards` | `{ "cards": ["As", "Kd"] }` (private; own seat only) |
| `history`  | list of events of the current hand (`action` / `stage` messages) |

The server answers with `reply` carrying the same `what` and the
request `id`.

## ping

Keepalive. The server answers `pong`.

    { "type": "ping" }

---

# 2. Server -> client

The server notifies **all** clients on every game change. Every
notification is self-contained: each `action`, `stage` and `hand_over`
message carries the full updated public state in its `state` field, so
clients stay in sync without extra round trips. In addition, every
client can `query` any public information at any time.

Tournament mode broadcasts four extra event types: `tournament_start`,
`level`, `player_out` and `tournament_over` (section 2bis).

## welcome

Sent when the client's reserved seat is actually taken over, at the end
of the round in which the client sent `hello`. Until this message
arrives the client is not seated.

    {
      "type": "welcome",
      "game_id": 1,
      "player_id": 0,
      "seat": 0,
      "name": "Alice"
    }

## queued

Replies to `hello` when a bot seat is free: the seat is reserved and
the bot plays out the rest of the current round. `welcome` follows at
the end of the round.

    {
      "type": "queued",
      "seat": 3
    }

## state

Full public snapshot of the game. Broadcast to all clients whenever
something changes; also the payload of `reply` to `query` `state`.

    {
      "type": "state",
      "game_id": 1,
      "stage": "preflop",
      "dealer": 2,
      "turn": 3,
      "current_bet": 10,
      "min_raise": 10,
      "pot": 15,
      "round_done": false,
      "hand_over": false,
      "common": [ null, null, null, null, null ],
      "mode": "cash",
      "blinds": { "small": 5, "big": 10, "ante": 0, "small_seat": 3, "big_seat": 4 },
      "players": [
        {
          "seat": 0,
          "name": "Alice",
          "stack": 985,
          "bet": 5,
          "street_bet": 5,
          "folded": false,
          "has_acted": false,
          "all_in": false,
          "busted": false,
          "last_action": { "type": "none", "amount": 0 },
          "is_turn": false
        }
      ],
      "timeout_seconds": 20
    }

Field mapping to the engine:

| field                    | engine                              |
|--------------------------|-------------------------------------|
| `stage`                  | `Game_State::stage`                 |
| `dealer`                 | `Game_State::dealer`                |
| `turn`                   | `Game_State::turn` (acting seat)    |
| `current_bet`            | `Game_State::current_bet`           |
| `min_raise`              | `Game_State::min_raise`             |
| `pot`                    | `Table::pot`                        |
| `round_done`             | `Game_State::round_done`            |
| `hand_over`              | `Game_State::hand_over`             |
| `common`                 | `Table::common` (null when not dealt) |
| `players[].seat`         | index in `Table::players`           |
| `players[].name`         | `Player::name`                      |
| `players[].stack`        | `Player::stack`                     |
| `players[].bet`          | `Player::_bet` (chips committed this hand) |
| `players[].street_bet`   | `Player::_street_bet`               |
| `players[].folded`       | `Player::_fold`                     |
| `players[].has_acted`    | `Player::has_acted`                 |
| `players[].all_in`       | `Player::is_all_in`                 |
| `players[].last_action`  | `Player::last_action` (kept for the whole betting round); `type` one of `none` `fold` `check` `call` `call_all` `bet` |
| `players[].is_turn`      | `Player::is_my_turn`                |
| `players[].busted`       | `Player::busted` (tournament: eliminated or empty seat; cash: always false) |
| `timeout_seconds`        | `ActionTimeOut` (20)                |
| `start_stack`            | starting stack for new/rebought seats (default 1000) |
| `max_players`            | table seat capacity (default 6, max 10) |
| `mode`                   | `"cash"` (infinite mode) or `"tournament"` |
| `blinds`                 | current blinds `{ small, big, ante, small_seat, big_seat }`; the seats skip empty seats and follow the heads-up rule (button on the small blind) |

In tournament mode the state additionally carries:

| field                        | meaning                                            |
|------------------------------|----------------------------------------------------|
| `level`                      | current blind level (1-based; 0 before the start)  |
| `level_seconds_remaining`    | seconds left until the blinds go up                |
| `countdown_seconds_remaining`| seconds left on the pre-start countdown            |
| `status`                     | `lobby` (waiting for players) / `countdown` / `running` / `finished` |
| `players_alive`              | players still in the tournament                    |

Hole cards are private: `my_cards` is never part of a broadcast and is
only returned to the owner by `query` `my_cards`.

## your_turn

Sent to the player whose turn it is, when the turn starts.

    { "type": "your_turn", "timeout_seconds": 20, "deadline": 1725.5 }

- `deadline` is a monotonic server timestamp in seconds; the player
  must act before it.
- If the player does not answer before the deadline, the server folds
  them automatically and broadcasts `action` with `reason` `"timeout"`.
- When the turn changes, the server also broadcasts a fresh `state`
  message to all clients, so everyone can track who is acting.

## action

Broadcast of a resolved action: every action any player takes, on
every street, including timeout folds.

    {
      "type": "action",
      "seat": 3,
      "action": "bet",
      "amount": 30,       // player's new street bet (total target)
      "all_in": false,    // true if the action put the player all-in
      "reason": null,     // "timeout" for automatic folds
      "pot": 60,
      "current_bet": 30,
      "state": { ... }    // full public snapshot after the action
    }

The `action` value is the engine `Response` type: `fold`, `check`,
`call`, `call_all`, `bet`. `check_or_fold` is resolved by the server
and never appears here.

## stage

Broadcast when the hand moves to a new street.

    {
      "type": "stage",
      "stage": "flop",
      "common": [ "Ah", "7d", "2c", null, null ],
      "blinds": { "small": 5, "big": 10, "small_seat": 3, "big_seat": 4 },
      "dealer": 2,
      "state": { ... }
    }

- `blinds` is present on `preflop` only (the posting itself is also
  announced as `action` events).
- `common` shows the dealt cards.
- `stage` `"over"` is not broadcast; the end of a hand is announced by
  `hand_over`.

## hand_over

Broadcast once per finished hand.

    {
      "type": "hand_over",
      "winners": [ { "seat": 1, "amount": 120 } ],
      "award": 120,
      "result": "P1 wins 120 with Flush",
      "state": { ... }
    }

- `winners` is an array of `{ seat, amount }`: side pots and splits
  produce several entries. `amount` is the exact per-winner winnings
  (with side pots, amounts differ per winner); `award` is their total.
- `state.pot` keeps the final pot value during the hand-over pause and
  is reset when the next hand starts.
- The next hand starts after a short pause; a new `state` broadcast
  announces it.

## 2bis. Tournament broadcasts

Only in tournament mode (`--tournament`). All four messages carry the
full `state` like every other broadcast.

## tournament_start

Broadcast once when the countdown ends and the first hand is about to
be dealt.

    {
      "type": "tournament_start",
      "level": 1,
      "blinds": { "small": 5, "big": 10, "ante": 0 },
      "state": { ... }
    }

## level

Broadcast when the blinds go up. Level changes only ever take effect
between hands, never mid-hand.

    {
      "type": "level",
      "level": 2,
      "blinds": { "small": 10, "big": 20, "ante": 0 },
      "state": { ... }
    }

## player_out

Broadcast when a player is eliminated: `"busted"` when their stack hit
0 at the end of a hand, `"disconnected"` when the connection dropped.
In either case the seat stays empty for the rest of the tournament (no
bots, no re-entry). A disconnected player is folded immediately (their
chips stay in the pot for the current hand) and the `player_out` event
is broadcast at the next round end — not at the moment the connection
drops.

    { "type": "player_out", "seat": 3, "reason": "busted", "state": { ... } }

## tournament_over

Broadcast once when one player is left. The server stops dealing, shows
the winner and rejects further `hello`s (`table_full`).

    {
      "type": "tournament_over",
      "winner": { "seat": 0, "name": "Alice" },
      "award": 6000,
      "state": { ... }
    }

- `award` is the winner's final stack (winner takes all).
- `state.status` is `"finished"`.

## reply

Answer to a `query`; echoes the request `id` and `what`.

    { "type": "reply", "id": 5, "what": "state", "data": { ... } }

## error

Answer to a request the server cannot or will not process. The request
is ignored, the game state is NOT changed, and the connection is never
closed because of it.

    { "type": "error", "id": 5, "code": "not_your_turn", "message": "It is not your turn" }

## pong

Reply to `ping`.

---

# 3. Error codes

| code            | meaning                                               |
|-----------------|-------------------------------------------------------|
| `bad_json`      | message is not valid JSON                             |
| `bad_request`   | valid JSON, but missing/unknown fields or bad values  |
| `unknown_type`  | unknown message `type`                                |
| `unknown_action`| unknown `action` value                                |
| `unknown_query` | unknown `query` `what`                                |
| `not_your_turn` | action sent when it is not the client's turn          |
| `illegal_action`| action impossible in the current situation (e.g. check with a bet to call) |
| `bet_too_small` | bet/raise below the legal minimum (and not all-in)    |
| `already_folded`| action sent by a player who already folded            |
| `game_over`     | action sent while the hand is over                    |
| `table_full`    | `hello` when the table is full                        |
| `unauthorized`  | message without a valid seat assignment               |
| `bad_token`     | `hello` with a wrong or missing token (server started with `--token`) |

---

# 4. Server behavior

- On every game change the server broadcasts a self-contained
  notification (`action`, `stage` or `hand_over`, each with the full
  `state`) to ALL connected clients.
- Every client can `query` any public information at any time.
- Invalid requests: the server answers with `error` and ignores the
  request. A client that sends garbage, impossible actions or nothing
  at all is never penalized beyond the timeout: if the acting player
  does not answer within `timeout_seconds`, the server folds them and
  broadcasts the fold with `reason` `"timeout"`. The timeout therefore
  protects the game from any kind of invalid or missing client
  response.
- Resource limits (protect the server from misbehaving clients):
  - at most 64 simultaneous connections; extra clients are closed at
    the handshake;
  - incoming messages are capped at 64 KB (larger ones are rejected
    with `bad_request`);
  - a client that stops reading gets dropped once ~512 KB of messages
    pile up in its send queue;
  - bet/raise `amount` values above 100,000,000 are rejected with
    `bad_request`.

# 5. Tournament mode

Run the server with `--tournament` (optionally
`--level-seconds N` for the level length, default 300;
`--countdown-seconds N`, default 10; and
`--max-players N` for the number of seats, 2–10, default 6).
Cash mode (the default, no flag)
behaves exactly as described above; the differences are:

- **No bots, no auto-play.** The seats start empty (2–10, configurable
  with `--max-players`). The game only starts when all seats are
  connected: the server seats clients immediately in the lobby, then
  runs a countdown once the table is full (`status: "countdown"`); a
  player leaving during the countdown returns the tournament to the
  lobby. No hands are dealt before the countdown ends.
- **Blinds rise with time.** Levels are fixed-length (see the schedule
  below); the blinds of the next level only apply from the next hand,
  never mid-hand. `level` broadcasts announce each increase.
- **No rebuys, no bots after a bust.** A player who busts (stack 0)
  is eliminated at the end of the hand: `player_out` (reason `"busted"`)
  is broadcast, the seat stays empty, and nobody joins mid-tournament.
  A player who disconnects is treated similarly but more gracefully:
  they are folded immediately (chips already committed to the pot stay
  in play), and `player_out` (reason `"disconnected"`) is broadcast at
  the next round end.
- **Heads-up rule.** With two players left, the button posts the small
  blind and acts first preflop; after the flop the big blind acts
  first. The button always moves to the next live player.
- **Winner takes all.** When one player is left, `tournament_over` is
  broadcast with the winner's full stack; the server stays on the
  winner screen and rejects new `hello`s. Restart the server to play
  again.

Blind schedule (starting stack 1000 = 100 BB, 6000 chips in play;
levels past 12 keep doubling):

| Level | SB | BB | Ante |
|-------|----|----|------|
| 1  | 5    | 10    | -   |
| 2  | 10   | 20    | -   |
| 3  | 15   | 30    | -   |
| 4  | 25   | 50    | -   |
| 5  | 40   | 80    | 5   |
| 6  | 60   | 120   | 10  |
| 7  | 100  | 200   | 15  |
| 8  | 150  | 300   | 25  |
| 9  | 250  | 500   | 40  |
| 10 | 400  | 800   | 60  |
| 11 | 600  | 1200  | 100 |
| 12 | 1000 | 2000  | 150 |

---

# 6. Worked example

A small hand, server "S", clients 0 (Alice) and 1 (Bob). State
payloads are abbreviated as `{ ... }`.

    C0: { "type": "hello", "name": "Alice" }
    S:  { "type": "queued", "seat": 0 }                      // bot finishes the round

    C1: { "type": "hello", "name": "Bob" }
    S:  { "type": "queued", "seat": 1 }

    ...current round ends; the bots are replaced at full entry stacks...

    S:  { "type": "welcome", "game_id": 1, "player_id": 0, "seat": 0, "name": "Alice" }
    S:  { "type": "welcome", "game_id": 1, "player_id": 1, "seat": 1, "name": "Bob" }

    S:  { "type": "state", "stage": "preflop", "dealer": 1, "turn": 0,
          "current_bet": 10, "min_raise": 10, "pot": 15, "common": [null,null,null,null,null],
          "players": [ ... ], "timeout_seconds": 20 }
    S:  { "type": "your_turn", "timeout_seconds": 20, "deadline": 5.0 }

    C0: { "type": "action", "action": "bet", "amount": 20 }
    S:  { "type": "action", "seat": 0, "action": "bet", "amount": 30, "all_in": false,
          "pot": 45, "current_bet": 30, "state": { ... } }

    C1: { "type": "action", "action": "bet", "amount": 5 }
    S:  { "type": "error", "code": "bet_too_small", "message": "raise below the legal minimum" }

    C1: { "type": "action", "action": "call" }
    S:  { "type": "action", "seat": 1, "action": "call", "amount": 30, "all_in": false,
          "pot": 75, "current_bet": 30, "state": { ... } }

    S:  { "type": "stage", "stage": "flop", "common": ["Ah","7d","2c",null,null], "state": { ... } }

    C0: { "type": "query", "id": 5, "what": "my_cards" }
    S:  { "type": "reply", "id": 5, "what": "my_cards", "data": { "cards": ["As", "Kd"] } }

    S:  { "type": "your_turn", "timeout_seconds": 20, "deadline": 12.0 }   // C0 again

    C0: { "type": "action", "action": "check" }
    S:  { "type": "action", "seat": 0, "action": "check", "amount": 0, "pot": 75,
          "current_bet": 0, "state": { ... } }

    ...C1 never answers; the server folds it after the deadline...

    S:  { "type": "action", "seat": 1, "action": "fold", "amount": 0, "reason": "timeout",
          "pot": 75, "current_bet": 0, "state": { ... } }
    S:  { "type": "hand_over", "winners": [ { "seat": 0, "amount": 75 } ], "award": 75,
          "result": "Alice wins 75", "state": { ... } }
