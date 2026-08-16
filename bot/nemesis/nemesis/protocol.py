from __future__ import annotations
import asyncio
import json
import logging

import websockets

from .state import GameState
from .opponent_model import OpponentModel
from .strategy import decide

log = logging.getLogger("nemesis")


class NemesisBot:
    def __init__(self, host: str, port: int, name: str, token: str | None = None):
        self.host = host
        self.port = port
        self.name = name
        self.token = token
        self.gs = GameState()
        self.om = OpponentModel()
        self.ws = None
        self._hand_preflop_acted: set[int] = set()  # seats that VPIP'd this hand
        self._hand_raised: set[int] = set()
        self._cbet_seat: int | None = None
        self._facing_cbet: set[int] = set()

    async def run(self):
        uri = f"ws://{self.host}:{self.port}"
        async for ws in websockets.connect(uri):
            self.ws = ws
            try:
                await self._send({"type": "hello", "name": self.name,
                                   **({"token": self.token} if self.token else {})})
                async for raw in ws:
                    try:
                        msg = json.loads(raw)
                    except json.JSONDecodeError:
                        continue
                    await self._handle(msg)
            except websockets.ConnectionClosed:
                log.warning("connection closed, reconnecting...")
                continue

    async def _send(self, obj: dict):
        await self.ws.send(json.dumps(obj))

    # -----------------------------------------------------------------

    async def _handle(self, msg: dict):
        t = msg.get("type")

        if t == "welcome":
            self.gs.my_seat = msg["seat"]
            log.info("seated at %s (seat %s)", self.name, self.gs.my_seat)

        elif t == "queued":
            log.info("queued for seat %s", msg.get("seat"))

        elif t == "state":
            self._on_new_state(msg)

        elif t == "stage":
            self._on_new_state(msg.get("state", msg))
            if msg.get("stage") == "flop":
                self._maybe_mark_cbet_opportunity()

        elif t == "action":
            self._track_opponent_action(msg)
            self._on_new_state(msg.get("state", msg))

        elif t == "your_turn":
            await self._act()

        elif t == "hand_over":
            self._on_new_state(msg.get("state", msg))
            self._reset_hand_trackers()

        elif t in ("tournament_start", "level", "player_out", "tournament_over"):
            self._on_new_state(msg.get("state", msg))
            if t == "tournament_over":
                log.info("tournament over: %s", msg.get("winner"))

        elif t == "error":
            log.warning("server error: %s (%s)", msg.get("code"), msg.get("message"))

        elif t == "reply":
            pass  # handled synchronously via _query where needed

    def _on_new_state(self, state_payload: dict):
        if "players" in state_payload:
            self.gs.update(state_payload)
        elif "state" in state_payload:
            self.gs.update(state_payload["state"])

    def _reset_hand_trackers(self):
        # count a "hand" for every seat that was dealt in, once, at hand end
        for seat in self.gs.live_seats() or self.gs.active_seats():
            if seat != self.gs.my_seat:
                self.om.get(seat).hands += 1
        self._hand_preflop_acted.clear()
        self._hand_raised.clear()
        self._cbet_seat = None
        self._facing_cbet.clear()

    # -- opponent stat collection from broadcasts ------------------------

    def _track_opponent_action(self, msg: dict):
        seat = msg.get("seat")
        action = msg.get("action")
        if seat is None or seat == self.gs.my_seat:
            return
        s = self.om.get(seat)
        stage = self.gs.stage  # stage BEFORE this action's resulting state overwrite

        if stage == "preflop":
            if action in ("call", "bet", "call_all") and seat not in self._hand_preflop_acted:
                s.vpip_hands += 1
                self._hand_preflop_acted.add(seat)
            if action == "bet" and seat not in self._hand_raised:
                s.pfr_hands += 1
                self._hand_raised.add(seat)
            if msg.get("all_in") and action == "bet":
                pos = self.gs.position_map().get(seat, "MP1")
                s.record_shove(pos)

        if action == "bet":
            s.raises += 1
        elif action in ("call", "check"):
            s.calls_or_checks += 1

        if self._cbet_seat is not None and seat in self._facing_cbet and action == "fold":
            s.cbet_faced += 1
            s.cbet_folded += 1
            self._facing_cbet.discard(seat)
        elif self._cbet_seat is not None and seat in self._facing_cbet:
            s.cbet_faced += 1
            self._facing_cbet.discard(seat)

    def _maybe_mark_cbet_opportunity(self):
        # crude: whoever was last preflop aggressor is "in position to c-bet";
        # mark all other live seats as facing a potential c-bet for fold-to-cbet stats
        if self._hand_raised:
            self._cbet_seat = next(iter(self._hand_raised))
            self._facing_cbet = set(self.gs.live_seats()) - {self._cbet_seat}

    # -- acting ------------------------------------------------------------

    async def _act(self):
        if self.gs.my_seat is None:
            return
        hole = await self._query_my_cards()
        if hole is None:
            await self._send({"type": "action", "action": "check_or_fold"})
            return

        d = decide(self.gs, self.om, self.gs.my_seat, hole)
        log.info("stage=%s pos=%s hole=%s -> %s %s",
                  self.gs.stage, self.gs.position_map().get(self.gs.my_seat),
                  hole, d.action, d.amount)

        payload = {"type": "action", "action": d.action}
        if d.action == "bet":
            payload["amount"] = max(0, d.amount)
        await self._send(payload)

    async def _query_my_cards(self) -> list[str] | None:
        req_id = 1
        await self._send({"type": "query", "id": req_id, "what": "my_cards"})
        try:
            while True:
                raw = await asyncio.wait_for(self.ws.recv(), timeout=5.0)
                msg = json.loads(raw)
                if msg.get("type") == "reply" and msg.get("id") == req_id and msg.get("what") == "my_cards":
                    return msg["data"]["cards"]
                # any other message that arrives while waiting: still process it
                await self._handle(msg)
        except (asyncio.TimeoutError, websockets.ConnectionClosed):
            return None
