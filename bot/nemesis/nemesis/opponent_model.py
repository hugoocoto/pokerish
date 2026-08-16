"""
Opponent modeling. Tracks the same core stats Prometheus tracks (VPIP / PFR /
AF / fold-to-cbet) so Nemesis isn't behind on read quality, plus a couple of
things Prometheus's own code doesn't have:

  - per-position VPIP splits, not just an aggregate (Prometheus's model.cpp-
    equivalent stats are position-blind)
  - a lightweight signature check for "this seat's shove frequency barely
    changes with position" -- the fingerprint of Prometheus's confirmed
    leak #1. When flagged, Nemesis widens its own calling/3-bet-shove ranges
    specifically against that seat rather than applying the exploit blindly
    to everyone (guards against overfitting to Prometheus, per the plan's
    risk section).
"""
from __future__ import annotations
from dataclasses import dataclass, field


@dataclass
class SeatStats:
    hands: int = 0
    vpip_hands: int = 0
    pfr_hands: int = 0
    raises: int = 0
    calls_or_checks: int = 0
    cbet_faced: int = 0
    cbet_folded: int = 0
    shoves_by_position: dict = field(default_factory=dict)  # position -> [count, total_opportunities]

    @property
    def vpip(self) -> float:
        return self.vpip_hands / self.hands if self.hands else 0.30

    @property
    def pfr(self) -> float:
        return self.pfr_hands / self.hands if self.hands else 0.15

    @property
    def af(self) -> float:
        return self.raises / self.calls_or_checks if self.calls_or_checks else 1.0

    @property
    def fold_to_cbet(self) -> float:
        return self.cbet_folded / self.cbet_faced if self.cbet_faced else 0.5

    def record_shove(self, position: str):
        c, t = self.shoves_by_position.get(position, [0, 0])
        self.shoves_by_position[position] = [c + 1, t + 1]

    def record_shove_opportunity_declined(self, position: str):
        c, t = self.shoves_by_position.get(position, [0, 0])
        self.shoves_by_position[position] = [c, t + 1]

    def looks_position_blind(self) -> bool:
        """True if shove frequency varies suspiciously little across
        positions once we have enough samples -- Prometheus's signature."""
        rates = []
        for pos, (c, t) in self.shoves_by_position.items():
            if t >= 4:
                rates.append(c / t)
        if len(rates) < 3:
            return False
        spread = max(rates) - min(rates)
        return spread < 0.15  # real position-aware players vary a lot more than this


class OpponentModel:
    def __init__(self):
        self.stats: dict[int, SeatStats] = {}

    def get(self, seat: int) -> SeatStats:
        if seat not in self.stats:
            self.stats[seat] = SeatStats()
        return self.stats[seat]

    def classify(self, seat: int) -> str:
        s = self.get(seat)
        if s.hands < 15:
            return "unknown"
        if s.vpip < 0.18:
            return "nit"
        if s.vpip > 0.40 and s.af < 1.5:
            return "fish"
        if s.af > 2.5:
            return "aggro"
        if s.fold_to_cbet > 0.60:
            return "fold_happy"
        return "reg"
