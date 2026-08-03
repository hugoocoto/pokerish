#!/usr/bin/env python3
"""Single-file poker bot example, in Python.

Same behavior as the C++ bot in bot/example: connects to the server, says
hello, and plays random-ish hands with a ~1 s "thinking" delay and 40%
indecision per try, following the API.md WebSocket/JSON protocol.

Use it as a template: the strategy lives in Bot.choose_action(), so a
smarter bot only needs to change that one method.

Run (from this directory, after creating the venv):
    python3 -m venv venv
    venv/bin/pip install -r requirements.txt
    venv/bin/python bot.py [--port N] [--host IP] [--token SECRET] [--name NAME]
"""

import argparse
import asyncio
import json
import random
import sys

import websockets

NAMES = [
    "Bob", "John", "Alice", "Emma", "Liam", "Olivia",
    "Noah", "Ava", "Ethan", "Mia", "Lucas", "Sofia",
]


class Bot:
    def __init__(self, host, port, token=None, name=None):
        self.host = host
        self.port = port
        self.token = token
        self.name = name or random.choice(NAMES)
        self.ws = None
        self.seat = -1
        self.state = {}
        self.thinking = False
        self.decide_at = 0.0

    # -- networking -------------------------------------------------------

    async def send(self, msg):
        await self.ws.send(json.dumps(msg))

    async def send_action(self, action, amount=0):
        msg = {"type": "action", "action": action}
        if action == "bet":
            msg["amount"] = amount
        await self.send(msg)

    async def run(self):
        uri = f"ws://{self.host}:{self.port}/"
        while True:
            try:
                async with websockets.connect(uri) as self.ws:
                    print(f"{self.name}: connected, saying hello")
                    await self.send_hello()
                    await self.listen()
            except (OSError, websockets.exceptions.ConnectionClosed) as e:
                print(f"{self.name}: disconnected ({e}); retrying in 2 s")
                await asyncio.sleep(2)

    async def send_hello(self):
        hello = {"type": "hello", "name": self.name}
        if self.token:
            hello["token"] = self.token
        await self.send(hello)

    async def listen(self):
        loop = asyncio.get_running_loop()
        while True:
            # Wait for the next message, but no longer than until the
            # "thinking" deadline, so we can act on our turn.
            timeout = None
            if self.thinking:
                timeout = max(0.0, self.decide_at - loop.time())
            try:
                msg = await asyncio.wait_for(self.ws.recv(), timeout)
            except asyncio.TimeoutError:
                await self.maybe_decide()
                continue
            try:
                await self.on_message(json.loads(msg))
            except json.JSONDecodeError:
                print(f"{self.name}: unparseable message")

    # -- protocol handling ------------------------------------------------

    async def on_message(self, msg):
        msg_type = msg.get("type")

        if msg_type == "welcome":
            self.seat = msg.get("seat", -1)
            print(f"{self.name}: joined as seat {self.seat}")
        elif msg_type == "your_turn":
            self.thinking = True
            self.decide_at = asyncio.get_running_loop().time() + 1.0
        elif msg_type == "state":
            self.state = msg
        elif msg_type in ("action", "stage", "hand_over", "tournament_start", "level", "player_out", "tournament_over"):
            if "state" in msg:
                self.state = msg["state"]
            if msg_type == "action":
                reason = msg.get("reason") or ""
                print(f"   {self.name}: P{msg.get('seat', -1)} {msg.get('action', '?')}"
                      + (f" ({reason})" if reason else ""))
            elif msg_type == "hand_over":
                print(f"   {self.name}: {msg.get('result', '')}")
            elif msg_type == "tournament_start":
                blinds = msg.get("blinds", {})
                sb = blinds.get("small", 5)
                bb = blinds.get("big", 10)
                ante = blinds.get("ante", 0)
                print(f"   {self.name}: == Tournament Started! Level {msg.get('level', 1)} (Blinds {sb}/{bb}, Ante {ante}) ==")
            elif msg_type == "level":
                blinds = msg.get("blinds", {})
                sb = blinds.get("small", 5)
                bb = blinds.get("big", 10)
                ante = blinds.get("ante", 0)
                print(f"   {self.name}: == Level Up! Level {msg.get('level', 1)} (Blinds {sb}/{bb}, Ante {ante}) ==")
            elif msg_type == "player_out":
                print(f"   {self.name}: == P{msg.get('seat', -1)} eliminated ({msg.get('reason', 'out')}) ==")
            elif msg_type == "tournament_over":
                winner = msg.get("winner", {})
                winner_name = winner.get("name", "?") if isinstance(winner, dict) else "?"
                print(f"   {self.name}: == Tournament Over! {winner_name} wins {msg.get('award', 0)} chips! ==")
        elif msg_type == "error":
            code = msg.get("code", "")
            print(f"   {self.name}: error {code}")
            self.thinking = False
            if code in ("bet_too_small", "illegal_action"):
                await self.send_action("fold")  # always legal fallback

    async def maybe_decide(self):
        if not self.thinking:
            return
        if random.random() < 0.4:
            self.decide_at = asyncio.get_running_loop().time() + 1.0  # indecision
            return
        self.thinking = False
        await self.choose_and_send()

    # -- the actual strategy ----------------------------------------------

    async def choose_and_send(self):
        """Read the public state and pick an action. Change this method
        to make a smarter bot."""
        players = self.state.get("players")
        if not players:
            return

        current_bet = self.state.get("current_bet", 0)
        min_raise = self.state.get("min_raise", 10)
        street, stack = 0, 0
        for pl in players:
            if pl.get("seat") == self.seat:
                street = pl.get("street_bet", 0)
                stack = pl.get("stack", 0)
                break
        to_call = current_bet - street
        r = random.random()

        if to_call <= 0:
            if r < 0.75:
                await self.send_action("check")
            else:
                inc = min_raise * (1 + int(random.random() * 2.9))  # 1..3x min_raise
                if current_bet + inc >= stack + street:
                    inc = stack + street - current_bet  # all-in
                if inc <= 0:
                    await self.send_action("check")
                else:
                    await self.send_action("bet", inc)
        elif to_call * 3 > stack:
            if r < 0.7:
                await self.send_action("fold")
            else:
                await self.send_action("call_all")
        elif r < 0.7:
            await self.send_action("call")
        elif r < 0.8:
            inc = min_raise * (1 + int(random.random() * 1.9))  # 1..2x min_raise
            if current_bet + inc >= stack + street:
                inc = stack + street - current_bet  # all-in
            if inc <= 0:
                await self.send_action("call")
            else:
                await self.send_action("bet", inc)
        else:
            await self.send_action("fold")


def main():
    parser = argparse.ArgumentParser(description="Simple poker bot (API.md protocol)")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--token", default=None)
    parser.add_argument("--name", default=None)
    args = parser.parse_args()
    try:
        asyncio.run(Bot(args.host, args.port, args.token, args.name).run())
    except KeyboardInterrupt:
        sys.exit(0)


if __name__ == "__main__":
    main()
