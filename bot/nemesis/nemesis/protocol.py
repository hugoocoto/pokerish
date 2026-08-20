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
        self._hand_preflop_acted: set[int] = set()   # seats that VPIP'd this hand
        self._hand_raised: set[int] = set()           # seats that raised preflop this hand
        self._hand_last_raiser_seat: int | None = None  # the seat whose RAISE we're facing
        self._cbet_seat: int | None = None
        self._facing_cbet: set[int] = set()
        # Per-position shove opportunity tracking: seat -> {position: had_opportunity}
        # Used to record "declined" shove opportunities at hand end.
        self._shove_opps: dict[int, list[tuple[str, bool]]] = {}  # seat -> [(pos, shoved)]
        # Track our own preflop aggression so postflop c-bet and 3-bet-pot logic works.
        self._i_am_pfr: bool = False
        # Monotonically increasing query request ID (wraps at 16-bit) to avoid
        # stale replies matching a new query in rapid re-action scenarios.
        self._req_id_counter: int = 0

    async def run(self):
        uri = f"ws://{self.host}:{self.port}"
        async for ws in websockets.connect(uri):
            self.ws = ws
            try:
                await self._send({"type": "hello", "name": self.name,
                                   **({("token"): self.token} if self.token else {})})
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
            # Track opponent stats BEFORE overwriting gs with the new state,
            # so stage / position_map reflect the moment of the action.
            self._track_opponent_action(msg)
            self._on_new_state(msg.get("state", msg))

        elif t == "your_turn":
            await self._act()

        elif t == "hand_over":
            self._on_new_state(msg.get("state", msg))
            self._flush_shove_opportunities()
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
        # Bug #3 fix: count a "hand" for every seated (non-busted) player once at
        # hand end, not just the players still live (non-folded) at showdown.
        # Using live_seats() meant folded opponents were never counted, keeping
        # almost every profile stuck at "unknown" and preventing the fingerprint
        # exploit from firing after 15+ hands.
        for seat in self.gs.active_seats():
            if seat != self.gs.my_seat:
                self.om.get(seat).hands += 1
        self._hand_preflop_acted.clear()
        self._hand_raised.clear()
        self._hand_last_raiser_seat = None
        self._cbet_seat = None
        self._facing_cbet.clear()
        self._shove_opps.clear()
        self._i_am_pfr = False

    # -- shove opportunity flushing at hand end ---------------------------

    def _flush_shove_opportunities(self):
        """At hand end, flush all recorded shove opportunities into opponent stats.
        A shove *opportunity* is any preflop decision made at <=20bb stack depth
        that was not a shove. We record these during the hand and flush at the end
        rather than inline so we don't count duplicate opportunities per street."""
        for seat, opps in self._shove_opps.items():
            s = self.om.get(seat)
            for pos, did_shove in opps:
                if did_shove:
                    s.record_shove(pos)
                else:
                    s.record_shove_opportunity_declined(pos)

    # -- opponent stat collection from broadcasts ------------------------

    def _track_opponent_action(self, msg: dict):
        seat = msg.get("seat")
        action = msg.get("action")
        if seat is None or seat == self.gs.my_seat:
            return
        s = self.om.get(seat)
        stage = self.gs.stage  # stage BEFORE this action's resulting state overwrite

        # ---- preflop VPIP / PFR ----
        if stage == "preflop":
            if action in ("call", "bet", "call_all") and seat not in self._hand_preflop_acted:
                s.vpip_hands += 1
                self._hand_preflop_acted.add(seat)

            if action == "bet" and seat not in self._hand_raised:
                s.pfr_hands += 1
                self._hand_raised.add(seat)
                # Remember this as the last aggressive raiser we're facing.
                self._hand_last_raiser_seat = seat

            # ---- shove opportunity tracking ----
            # Record any preflop action at <=20bb as a shove opportunity.
            # We defer flushing to hand end to avoid double-counting.
            player = self.gs.players.get(seat)
            if player is not None:
                stack_bb = self.gs.effective_stack_bb(seat) if seat in self.gs.players else 21
                if stack_bb <= 20:
                    pos = self.gs.position_map().get(seat, "MP1")
                    did_shove = (action in ("bet", "call_all") and msg.get("all_in", False))
                    # Record one opportunity per position per hand (avoid duplicates within hand)
                    existing = self._shove_opps.get(seat, [])
                    already_recorded = any(p == pos for p, _ in existing)
                    if not already_recorded:
                        if seat not in self._shove_opps:
                            self._shove_opps[seat] = []
                        self._shove_opps[seat].append((pos, did_shove))

        # ---- AF tracking (all streets) ----
        if action == "bet":
            s.raises += 1
        elif action in ("call", "check"):
            s.calls_or_checks += 1

        # ---- fold-to-cbet tracking ----
        if self._cbet_seat is not None and seat in self._facing_cbet and action == "fold":
            s.cbet_faced += 1
            s.cbet_folded += 1
            self._facing_cbet.discard(seat)
        elif self._cbet_seat is not None and seat in self._facing_cbet:
            s.cbet_faced += 1
            self._facing_cbet.discard(seat)

    def _maybe_mark_cbet_opportunity(self):
        # The preflop raiser is the likely c-bettor; mark everyone else as
        # "facing a potential c-bet" for fold-to-cbet stat collection.
        if self._hand_last_raiser_seat is not None:
            self._cbet_seat = self._hand_last_raiser_seat
            self._facing_cbet = set(self.gs.live_seats()) - {self._cbet_seat}
        elif self._hand_raised:
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

        d = decide(self.gs, self.om, self.gs.my_seat, hole,
                   last_raiser_seat=self._hand_last_raiser_seat,
                   i_am_pfr=self._i_am_pfr)
        log.info("stage=%s pos=%s hole=%s -> %s %s",
                  self.gs.stage, self.gs.position_map().get(self.gs.my_seat),
                  hole, d.action, d.amount)

        payload = {"type": "action", "action": d.action}
        if d.action == "bet":
            payload["amount"] = max(0, d.amount)
        await self._send(payload)

        # Record our own preflop raise so postflop c-bet and 3-bet-pot response
        # logic (is_pfr / i_am_pfr) know we were the aggressor.
        if self.gs.stage == "preflop" and d.action == "bet":
            self._i_am_pfr = True

    async def _query_my_cards(self) -> list[str] | None:
        # Bug #4 fix: use a monotonically incrementing counter instead of a
        # static hash of self, so rapid back-to-back queries don't collide.
        self._req_id_counter = (self._req_id_counter + 1) & 0xFFFF
        req_id = self._req_id_counter
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
