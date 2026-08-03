# Building a Custom Poker Bot

This guide explains how to build a custom poker bot that connects to the poker server over WebSockets using JSON messages (as specified in [API.md](file:///home/hugo/code/poker/API.md)).

---

## 1. Architecture Overview

Your bot is an external process that connects via WebSocket. It communicates using standard JSON envelopes:

```
Bot (Client)  <--- WebSockets / JSON --->  Poker Server
```

### Key Messages Lifecycle
1. **`hello`**: Join the table and request a seat.
2. **`queued` / `welcome`**: The server confirms seat reservation, then binds the connection to a seat once the round ends.
3. **`state`**: Broadcast by the server whenever anything happens (cards dealt, player acted, stage changed).
4. **`your_turn`**: Targeted notification sent ONLY to the player whose turn it is. Contains the action timeout deadline.
5. **`query`**: (Optional) Ask the server for private info (`my_cards`) or public history/state.
6. **`action`**: Send your action (`fold`, `check`, `call`, `call_all`, `bet`).

---

## 2. Step-by-Step Connection Flow

### Step 1: Handshake (`hello`)
Send a `hello` message after connecting:

```json
{
  "type": "hello",
  "name": "MySmartBot",
  "token": "SECRET_IF_SET"
}
```

- **Cash mode**: Server replies with `{"type": "queued", "seat": X}`. At the next hand boundary, it sends `{"type": "welcome", "seat": X, ...}`.
- **Tournament mode**: Server replies directly with `{"type": "welcome", "seat": X, ...}` during lobby/countdown, or `table_full` error if running.

### Step 2: Tracking State & Cards
Listen for incoming JSON messages:
- When `state` arrives, update your local table view (pot, stage, community cards, player stacks, bets).
- When `your_turn` arrives, or when `state` indicates `is_turn == true` for your seat, fetch your cards if you haven't already:

```json
{
  "type": "query",
  "id": 1,
  "what": "my_cards"
}
```

Server replies:
```json
{
  "type": "reply",
  "id": 1,
  "what": "my_cards",
  "data": {
    "cards": ["As", "Kd"]
  }
}
```

*(Card format: `As` = Ace of spades, `Td` = Ten of diamonds, `2c` = Two of clubs, `null` = not dealt yet).*

### Step 3: Deciding & Sending an Action

When it's your turn, calculate your move and send:

```json
{
  "type": "action",
  "action": "call"
}
```

#### Valid Action Types:
- `fold`: Give up the hand.
- `check`: Pass action when `to_call == 0` (current_bet == street_bet).
- `call`: Match the current bet. Server calculates exact chip count.
- `call_all`: Go all-in.
- `bet`: Open or raise. **`amount` is the INCREMENT above `current_bet`**, NOT total chips!
  - Minimum raise increment = `state.min_raise`.
  - Example: `current_bet` is 20, `min_raise` is 10. To raise to 30 total street commitment, pass `"amount": 10`.

---

## 3. Complete Python Bot Example

Below is a complete, standalone Python bot using `asyncio` and `websockets`.

```python
import asyncio
import json
import websockets

SERVER_URI = "ws://127.0.0.1:9000"
BOT_NAME = "PythonBot"
TOKEN = ""  # Set if server uses --token

class PokerBot:
    def __init__(self):
        self.seat = -1
        self.my_cards = []
        self.state = {}

    async def run(self):
        async with websockets.connect(SERVER_URI) as ws:
            # 1. Send Hello
            hello_msg = {"type": "hello", "name": BOT_NAME}
            if TOKEN:
                hello_msg["token"] = TOKEN
            await ws.send(json.dumps(hello_msg))

            async for raw in ws:
                msg = json.loads(raw)
                mtype = msg.get("type")

                if mtype == "welcome":
                    self.seat = msg["seat"]
                    print(f"[{BOT_NAME}] Seated at seat {self.seat}")

                elif mtype == "state":
                    self.state = msg
                    # Reset cards on new hand
                    if msg.get("stage") == "preflop" and msg.get("current_bet") <= msg.get("blinds", {}).get("big", 10):
                        if not any(p["seat"] == self.seat and p["has_acted"] for p in msg.get("players", [])):
                            self.my_cards = []

                elif mtype == "your_turn":
                    # Query cards if not yet known
                    if not self.my_cards:
                        await ws.send(json.dumps({"type": "query", "id": 100, "what": "my_cards"}))
                    else:
                        await self.make_decision(ws)

                elif mtype == "reply" and msg.get("what") == "my_cards":
                    self.my_cards = msg.get("data", {}).get("cards", [])
                    await self.make_decision(ws)

    async def make_decision(self, ws):
        players = self.state.get("players", [])
        me = next((p for p in players if p["seat"] == self.seat), None)
        if not me:
            return

        current_bet = self.state.get("current_bet", 0)
        my_street_bet = me.get("street_bet", 0)
        to_call = current_bet - my_street_bet

        # Simple logic: check if free, call small bets, fold large bets
        if to_call <= 0:
            action = {"type": "action", "action": "check"}
        elif to_call <= me.get("stack", 0) // 4:
            action = {"type": "action", "action": "call"}
        else:
            action = {"type": "action", "action": "fold"}

        print(f"[{BOT_NAME}] Acting: {action['action']} (to_call: {to_call}, cards: {self.my_cards})")
        await ws.send(json.dumps(action))

if __name__ == "__main__":
    bot = PokerBot()
    asyncio.run(bot.run())
```

---

## 4. Quickstart: Testing Your Bot

1. **Build and start the server**:
   ```bash
   ./server/build/server --headless --port 9000
   ```
2. **Run existing example C++ bot**:
   ```bash
   ./bot/example/build/example_bot --port 9000
   ```
3. **Run your Python bot**:
   ```bash
   python3 bot/python/bot.py
   ```

For detailed protocol specifications, message formats, error codes, and tournament schedules, refer to [API.md](file:///home/hugo/code/poker/API.md).
