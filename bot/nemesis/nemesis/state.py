from __future__ import annotations
from dataclasses import dataclass, field
from .ranges import POSITIONS_9MAX


@dataclass
class PlayerState:
    seat: int
    name: str = ""
    stack: int = 0
    bet: int = 0
    street_bet: int = 0
    folded: bool = False
    has_acted: bool = False
    all_in: bool = False
    busted: bool = False
    is_turn: bool = False


class GameState:
    def __init__(self):
        self.my_seat: int | None = None
        self.stage: str = "preflop"
        self.dealer: int = 0
        self.turn: int = 0
        self.current_bet: int = 0
        self.min_raise: int = 0
        self.pot: int = 0
        self.common: list[str | None] = [None] * 5
        self.blinds: dict = {}
        self.players: dict[int, PlayerState] = {}
        self.max_players: int = 9
        self.mode: str = "tournament"
        self.hole_cards: list[str] | None = None

    def update(self, state_payload: dict):
        self.stage = state_payload.get("stage", self.stage)
        self.dealer = state_payload.get("dealer", self.dealer)
        self.turn = state_payload.get("turn", self.turn)
        self.current_bet = state_payload.get("current_bet", 0)
        self.min_raise = state_payload.get("min_raise", 0)
        self.pot = state_payload.get("pot", 0)
        self.common = state_payload.get("common", self.common)
        self.blinds = state_payload.get("blinds", self.blinds)
        self.max_players = state_payload.get("max_players", self.max_players)
        self.mode = state_payload.get("mode", self.mode)

        self.players = {}
        for p in state_payload.get("players", []):
            self.players[p["seat"]] = PlayerState(
                seat=p["seat"], name=p.get("name", ""), stack=p.get("stack", 0),
                bet=p.get("bet", 0), street_bet=p.get("street_bet", 0),
                folded=p.get("folded", False), has_acted=p.get("has_acted", False),
                all_in=p.get("all_in", False), busted=p.get("busted", False),
                is_turn=p.get("is_turn", False),
            )

    # -- derived helpers -----------------------------------------------

    def active_seats(self) -> list[int]:
        """Seats still in the tournament (not busted), regardless of folded-this-hand."""
        return sorted(s for s, p in self.players.items() if not p.busted)

    def live_seats(self) -> list[int]:
        """Seats still contesting the current hand (not busted, not folded)."""
        return sorted(s for s, p in self.players.items() if not p.busted and not p.folded)

    def big_blind(self) -> int:
        return self.blinds.get("big", 1) or 1

    def effective_stack_bb(self, seat: int) -> float:
        my_stack = self.players[seat].stack + self.players[seat].bet
        others = [p.stack + p.bet for s, p in self.players.items()
                  if s != seat and not p.busted and not p.folded]
        if not others:
            return my_stack / self.big_blind()
        eff = min(my_stack, max(others))
        return eff / self.big_blind()

    def position_map(self) -> dict[int, str]:
        """Map seat -> position label ('UTG'..'BB'), robust to empty/busted
        seats and the heads-up dealer-posts-SB rule, by anchoring on the
        server-provided small_seat/big_seat rather than assuming fixed seat
        offsets from the dealer."""
        active = self.active_seats()
        n = len(active)
        if n < 2:
            return {s: "BTN" for s in active}

        big_seat = self.blinds.get("big_seat")
        if big_seat is None or big_seat not in active:
            # fallback: assume big_seat is 2 seats after dealer among active players
            big_seat = active[(active.index(self.dealer) + 2) % n] if self.dealer in active else active[0]

        N = self.max_players if self.max_players else max(active) + 1
        order = sorted(active, key=lambda s: (s - big_seat - 1) % N)
        labels = (POSITIONS_9MAX[-n:] if n <= 9 else
                  POSITIONS_9MAX + ["MP"] * (n - 9))
        return dict(zip(order, labels))

    def num_to_act_behind(self, seat: int) -> int:
        """How many live players act after `seat` this street (rough, for
        squeeze/steal-equity sizing) -- based on position_map order."""
        pm = self.position_map()
        order = [s for s, _ in sorted(pm.items(), key=lambda kv: POSITIONS_9MAX.index(kv[1])
                                       if kv[1] in POSITIONS_9MAX else 99)]
        if seat not in order:
            return 0
        idx = order.index(seat)
        return sum(1 for s in order[idx + 1:] if s in self.live_seats())
