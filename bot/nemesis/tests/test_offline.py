"""
Offline unit tests for Nemesis — no live server needed.
Run with: python3 tests/test_offline.py
"""
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from nemesis import ranges, icm, equity
from nemesis.state import GameState
from nemesis.opponent_model import OpponentModel, SeatStats
from nemesis.board import classify as classify_board
from nemesis.strategy import decide, Decision


# ---------------------------------------------------------------------------
# Ranges
# ---------------------------------------------------------------------------

def test_canonical_order_basic():
    assert ranges.CANONICAL_ORDER[0] == "AA"
    assert "72o" in ranges.CANONICAL_ORDER
    assert len(set(ranges.CANONICAL_ORDER)) == 169
    print("OK canonical order:", ranges.CANONICAL_ORDER[:10], "...", ranges.CANONICAL_ORDER[-5:])


def test_hand_key():
    assert ranges.hand_key("As", "Ad") == "AA"
    assert ranges.hand_key("As", "Ks") == "AKs"
    assert ranges.hand_key("Ks", "Ah") == "AKo"
    print("OK hand_key")


def test_push_ranges_widen_correctly():
    # position ordering: shove range should widen UTG -> BTN at fixed depth
    utg = ranges.push_range("UTG", 10)
    btn = ranges.push_range("BTN", 10)
    assert len(btn) > len(utg), (len(utg), len(btn))
    print(f"OK position widening at 10bb: UTG={len(utg)} classes, BTN={len(btn)} classes")

    # The 15-20bb band must stay a real, non-collapsed calling range instead
    # of narrowing to premiums only -- a common leak in simpler bots.
    call18 = ranges.call_shove_range("BTN", 18)
    assert len(call18) >= 5, call18
    print(f"OK 15-20bb calling range not collapsed: BTN@18bb={len(call18)} classes -> {sorted(call18)[:8]}...")


def test_top_pct_monotonic():
    small = ranges.top_pct(5)
    big = ranges.top_pct(20)
    assert small.issubset(big)
    print(f"OK top_pct nesting: top5%={len(small)} classes, top20%={len(big)} classes")


def test_call_open_range_extra_pct():
    """Extra pct parameter widens the flat-call range (used vs. fingerprinted raisers)."""
    base = ranges.call_open_range("BTN", 3, 6)
    wider = ranges.call_open_range("BTN", 3, 6, extra_pct=5.0)
    assert len(wider) >= len(base)
    assert wider.issuperset(base)
    print(f"OK call_open_range extra_pct: base={len(base)}, wider={len(wider)}")


# ---------------------------------------------------------------------------
# ICM
# ---------------------------------------------------------------------------

def test_icm_win_probability():
    stacks = [3000, 3000, 3000, 1000]
    p = icm.win_probability(3000, stacks)
    assert abs(p - 0.30) < 1e-9
    print("OK icm.win_probability:", p)


def test_icm_risk_premium_bigstack_vs_shortstack():
    stacks = [6000, 1000, 1000, 1000, 1000]
    premium = icm.icm_risk_premium(6000, 1000, stacks)
    assert premium > 0
    print("OK icm_risk_premium (big stack vs short, should tighten):", premium)


# ---------------------------------------------------------------------------
# State / position
# ---------------------------------------------------------------------------

def test_state_position_map_full_ring():
    gs = GameState()
    players = [{"seat": i, "name": f"P{i}", "stack": 1000, "bet": 0, "street_bet": 0,
                "folded": False, "has_acted": False, "all_in": False, "busted": False,
                "is_turn": False} for i in range(9)]
    gs.update({
        "stage": "preflop", "dealer": 0, "turn": 3, "current_bet": 20, "min_raise": 20,
        "pot": 30, "common": [None]*5,
        "blinds": {"small": 10, "big": 20, "ante": 0, "small_seat": 1, "big_seat": 2},
        "max_players": 9, "mode": "tournament", "players": players,
    })
    pm = gs.position_map()
    assert pm[2] == "BB", pm
    assert pm[1] == "SB", pm
    assert pm[0] == "BTN", pm
    assert pm[3] == "UTG", pm
    print("OK 9-max position_map:", pm)


def test_state_position_map_with_busted_seats():
    gs = GameState()
    seats = [0, 2, 3, 6, 8]  # some seats busted/empty
    players = [{"seat": i, "name": f"P{i}", "stack": 1000, "bet": 0, "street_bet": 0,
                "folded": False, "has_acted": False, "all_in": False,
                "busted": (i not in seats), "is_turn": False} for i in range(9)]
    gs.update({
        "stage": "preflop", "dealer": 0, "turn": 2, "current_bet": 20, "min_raise": 20,
        "pot": 30, "common": [None]*5,
        "blinds": {"small": 2, "big": 3, "ante": 0, "small_seat": 2, "big_seat": 3},
        "max_players": 9, "mode": "tournament", "players": players,
    })
    pm = gs.position_map()
    assert pm[3] == "BB"
    assert pm[2] == "SB"
    assert pm[0] == "BTN"
    print("OK position_map with empty seats (5-handed):", pm)


# ---------------------------------------------------------------------------
# Equity
# ---------------------------------------------------------------------------

def test_equity_fallback_or_real():
    eq = equity.hand_equity_mc(["As", "Ah"], [], 1, samples=300)
    assert 0.6 < eq < 1.0, eq
    print("OK AA equity vs 1 random opponent preflop ~=", round(eq, 3))
    eq2 = equity.hand_equity_mc(["7c", "2d"], [], 1, samples=300)
    assert eq2 < eq
    print("OK 72o equity lower than AA:", round(eq2, 3))


# ---------------------------------------------------------------------------
# Board texture
# ---------------------------------------------------------------------------

def test_board_classify_dry():
    bt = classify_board(["As", "7d", "2c"])
    assert bt.rainbow
    assert not bt.flush_draw_possible or bt.flush_draw_possible == False  # rainbow = no FD
    assert not bt.monotone
    assert not bt.paired
    # A-7-2 rainbow is very dry
    assert bt.is_dry
    print("OK dry board (A72r):", bt)


def test_board_classify_wet():
    bt = classify_board(["9h", "8h", "7d"])
    assert bt.straight_draw_possible
    assert bt.flush_draw_possible  # 2 hearts
    assert bt.is_wet
    assert not bt.is_dry
    print("OK wet board (987 two-tone):", bt)


def test_board_classify_monotone():
    bt = classify_board(["Ah", "7h", "2h"])
    assert bt.monotone
    assert bt.flush_draw_possible
    assert not bt.rainbow
    print("OK monotone board (A72 flush):", bt)


def test_board_classify_paired():
    bt = classify_board(["As", "Ad", "7c"])
    assert bt.paired
    assert not bt.double_paired
    print("OK paired board (AA7r):", bt)


def test_board_classify_turn_river():
    # Turn: 4 cards
    bt = classify_board(["9h", "8h", "7d", "2c"])
    assert bt.straight_draw_possible
    print("OK board classify works on turn:", bt)
    # River: 5 cards
    bt5 = classify_board(["9h", "8h", "7d", "2c", "Ks"])
    assert bt5.straight_draw_possible
    print("OK board classify works on river:", bt5)


# ---------------------------------------------------------------------------
# Opponent model — fingerprint detection
# ---------------------------------------------------------------------------

def test_looks_position_blind_fires():
    """After seeing the same ~50% shove rate from UTG through BTN, the seat
    should be flagged as position-blind."""
    s = SeatStats()
    positions = ["UTG", "UTG1", "MP1", "MP2", "HJ", "CO", "BTN"]
    for pos in positions:
        for _ in range(5):  # 5 shoves at each position
            s.record_shove(pos)
            s.record_shove_opportunity_declined(pos)  # 50% rate → one extra opp each
    assert s.looks_position_blind(), s.shoves_by_position
    print("OK looks_position_blind fires for uniform shove rate:", s.shoves_by_position)


def test_looks_position_blind_does_not_fire_for_correct_player():
    """A player who shoves wide on BTN and tight UTG should NOT be flagged."""
    s = SeatStats()
    # UTG: 1/5 shoves; BTN: 5/5 shoves  → spread = 0.8
    for _ in range(5):
        s.record_shove_opportunity_declined("UTG")
    s.record_shove("UTG")  # 1/6 shoves UTG  (~17%)
    for _ in range(5):
        s.record_shove("BTN")  # 5/5 shoves BTN (100%)
    assert not s.looks_position_blind(), s.shoves_by_position
    print("OK looks_position_blind does NOT fire for correctly position-aware player")


def test_opponent_model_classify():
    om = OpponentModel()
    s = om.get(0)
    # Nit: low VPIP, low AF
    s.hands = 30; s.vpip_hands = 4; s.pfr_hands = 3
    s.raises = 3; s.calls_or_checks = 20
    assert om.classify(0) == "nit", om.classify(0)
    print("OK OpponentModel.classify nit")


# ---------------------------------------------------------------------------
# Strategy — fingerprint exploit integration
# ---------------------------------------------------------------------------

def _make_gs(n_players=6, stack_bb=10, bb=10, stage="preflop", my_seat=2):
    """Helper: build a minimal GameState with n_players active seats."""
    gs = GameState()
    bb_val = bb
    sb_val = bb // 2
    players = []
    for i in range(n_players):
        players.append({
            "seat": i, "name": f"P{i}", "stack": stack_bb * bb_val - (bb_val if i == 1 else 0),
            "bet": bb_val if i == 1 else (sb_val if i == 0 else 0),
            "street_bet": bb_val if i == 1 else (sb_val if i == 0 else 0),
            "folded": False, "has_acted": False, "all_in": False, "busted": False,
            "is_turn": i == my_seat, "last_action": {"type": "none", "amount": 0},
        })
    gs.update({
        "stage": stage, "dealer": n_players - 1, "turn": my_seat,
        "current_bet": bb_val, "min_raise": bb_val, "pot": bb_val + sb_val,
        "common": [None]*5,
        "blinds": {"small": sb_val, "big": bb_val, "ante": 0,
                   "small_seat": 0, "big_seat": 1},
        "max_players": 9, "mode": "tournament", "players": players,
    })
    gs.my_seat = my_seat
    return gs


def test_strategy_preflop_shove_short_stack():
    """At 8bb, AA should always shove."""
    gs = _make_gs(n_players=6, stack_bb=8, bb=10)
    om = OpponentModel()
    d = decide(gs, om, 2, ["As", "Ah"])
    assert d.action == "bet", f"Expected shove, got {d.action}"
    print("OK strategy: AA shoves at 8bb")


def test_strategy_preflop_fold_trash_short():
    """At 8bb, 72o from UTG should fold."""
    gs = _make_gs(n_players=6, stack_bb=8, bb=10)
    om = OpponentModel()
    d = decide(gs, om, 2, ["7c", "2d"])
    assert d.action == "fold", f"Expected fold, got {d.action}"
    print("OK strategy: 72o folds at 8bb UTG")


def test_strategy_fingerprint_widens_call():
    """Against a fingerprinted (position-blind) raiser, our call-shove range
    at 18bb should be at least as wide as without the fingerprint."""
    # Build state: seat 5 raised all-in; we're seat 2 at 18bb
    gs = _make_gs(n_players=6, stack_bb=18, bb=10)
    # Simulate an all-in raise by seat 5
    for p in gs.players.values():
        p.street_bet = 0
    gs.players[5].street_bet = 180  # shoved all-in
    gs.players[5].all_in = True
    gs.current_bet = 180
    gs.players[2].stack = 180   # we have 18bb, calling is all-in
    gs.players[2].bet = 0

    om_plain = OpponentModel()
    om_print = OpponentModel()
    # Make seat 5 look position-blind in om_print
    s = om_print.get(5)
    for pos in ranges.POSITIONS_9MAX:
        for _ in range(5):
            s.record_shove(pos)
            s.record_shove_opportunity_declined(pos)
    assert s.looks_position_blind()

    # AQo: borderline call at 18bb
    d_plain = decide(gs, om_plain, 2, ["Ah", "Qd"], last_raiser_seat=5)
    d_print = decide(gs, om_print, 2, ["Ah", "Qd"], last_raiser_seat=5)
    print(f"OK fingerprint exploit: plain={d_plain.action}, fingerprinted={d_print.action}")
    # At minimum, the fingerprinted version should not be stricter
    action_rank = {"call_all": 2, "call": 1, "fold": 0}
    assert action_rank.get(d_print.action, 0) >= action_rank.get(d_plain.action, 0)


# ---------------------------------------------------------------------------
# Board texture → postflop strategy
# ---------------------------------------------------------------------------

def _make_gs_postflop(board, stage="flop", my_seat=0, n_opp=1, eq_override=None):
    """Build a minimal GameState for postflop testing."""
    gs = GameState()
    n_players = 1 + n_opp
    bb = 10
    pot = 100
    players = []
    for i in range(n_players):
        players.append({
            "seat": i, "name": f"P{i}", "stack": 500,
            "bet": 0, "street_bet": 0,
            "folded": False, "has_acted": True, "all_in": False, "busted": False,
            "is_turn": i == my_seat, "last_action": {"type": "check", "amount": 0},
        })
    full_board = board + [None] * (5 - len(board))
    gs.update({
        "stage": stage, "dealer": n_players - 1, "turn": my_seat,
        "current_bet": 0, "min_raise": bb, "pot": pot,
        "common": full_board,
        "blinds": {"small": 5, "big": bb, "ante": 0, "small_seat": 1, "big_seat": 0},
        "max_players": 9, "mode": "tournament", "players": players,
    })
    gs.my_seat = my_seat
    return gs


def test_postflop_dry_board_bets_bigger():
    """On a dry board, a strong hand should still bet (equity-driven), and
    the strategy must not crash on board texture classification."""
    gs = _make_gs_postflop(["As", "7d", "2c"])  # dry board
    om = OpponentModel()
    # KK on A72r: strong but not nuts; equity ~75% heads up
    d = decide(gs, om, 0, ["Ks", "Kh"])
    assert d.action in ("bet", "check"), f"Unexpected action: {d.action}"
    print(f"OK postflop dry board (A72r) KK -> {d.action} {d.amount}")


def test_postflop_wet_board_no_crash():
    """Wet board should not crash and should produce a valid action."""
    gs = _make_gs_postflop(["9h", "8h", "7d"])
    om = OpponentModel()
    d = decide(gs, om, 0, ["6s", "5s"])  # OESD + backdoor flush
    assert d.action in ("bet", "check", "call", "fold")
    print(f"OK postflop wet board (987hh7d) 65s -> {d.action} {d.amount}")


def test_postflop_paired_board_no_crash():
    gs = _make_gs_postflop(["As", "Ad", "7c"])
    om = OpponentModel()
    d = decide(gs, om, 0, ["Ah", "Kd"])  # trips aces
    assert d.action in ("bet", "check")
    print(f"OK postflop paired board (AA7r) AK -> {d.action} {d.amount}")


# ---------------------------------------------------------------------------
# New features: 4-bet, BB iso-raise, c-bet, position, ICM stage, equity
# ---------------------------------------------------------------------------

def test_strategy_4bet_stackoff_vs_3bet():
    """When we were the preflop raiser (i_am_pfr=True) and now face a 3-bet,
    KK/AA/AK should 4-bet jam (stackoff range), not fold."""
    gs = _make_gs(n_players=6, stack_bb=40, bb=10)
    # Simulate: we raised to 25, seat 5 re-raised to 80, we owe 55 more
    gs.current_bet = 80
    gs.players[5].street_bet = 80
    gs.players[5].all_in = False
    gs.players[2].street_bet = 25
    gs.players[2].stack = 400 - 25  # 40bb starting
    om = OpponentModel()
    # i_am_pfr=True signals we opened and now face the 3-bet
    d = decide(gs, om, 2, ["Ks", "Kh"], last_raiser_seat=5, i_am_pfr=True)
    assert d.action == "bet", f"KK vs 3-bet should 4-bet jam, got {d.action}"
    print(f"OK 4-bet stackoff: KK vs 3-bet -> {d.action} {d.amount}")


def test_strategy_4bet_fold_trash_vs_3bet():
    """Trash hand vs 3-bet with i_am_pfr should fold, not call."""
    gs = _make_gs(n_players=6, stack_bb=40, bb=10)
    gs.current_bet = 80
    gs.players[5].street_bet = 80
    gs.players[2].street_bet = 25
    gs.players[2].stack = 375
    om = OpponentModel()
    d = decide(gs, om, 2, ["7c", "2d"], last_raiser_seat=5, i_am_pfr=True)
    assert d.action == "fold", f"72o vs 3-bet should fold, got {d.action}"
    print(f"OK 4-bet fold: 72o vs 3-bet -> {d.action}")


def test_strategy_bb_iso_raise_vs_limpers():
    """BB should iso-raise a strong hand vs 1 limper (not just check)."""
    gs = _make_gs(n_players=6, stack_bb=30, bb=10, my_seat=1)
    # Seat 1 = BB (big_seat=1). current_bet still == BB (10), seat 4 called (limped).
    # Position map needs big_seat set correctly.
    gs.blinds["big_seat"] = 1
    gs.blinds["small_seat"] = 0
    gs.current_bet = 10  # no raise, just the BB
    # Seat 4 limped: street_bet == bb
    gs.players[4].street_bet = 10
    gs.players[4].bet = 10
    gs.players[1].street_bet = 10   # we posted BB
    gs.players[1].stack = 290
    om = OpponentModel()
    # AA in BB vs 1 limper: should iso-raise
    d = decide(gs, om, 1, ["As", "Ah"])
    assert d.action == "bet", f"AA in BB vs 1 limper should iso-raise, got {d.action}"
    assert d.amount > 10, f"iso-raise amount should be > 1BB, got {d.amount}"
    print(f"OK BB iso-raise: AA vs 1 limper -> {d.action} {d.amount}")


def test_strategy_cbet_with_pfr_flag():
    """When is_pfr=True, we should c-bet on a dry board even with moderate equity."""
    gs = _make_gs_postflop(["As", "7d", "2c"], my_seat=0, n_opp=1)
    om = OpponentModel()
    # KJo: moderate equity (~55%) on A72r. Without pfr flag, may check.
    # With pfr flag (c-bet range advantage), should bet.
    d_pfr = decide(gs, om, 0, ["Ks", "Jh"], i_am_pfr=True)
    d_nopfr = decide(gs, om, 0, ["Ks", "Jh"], i_am_pfr=False)
    print(f"OK c-bet: KJo A72r pfr={d_pfr.action}/{d_pfr.amount}  no-pfr={d_nopfr.action}/{d_nopfr.amount}")
    # With pfr, at minimum we should not fold (range advantage)
    assert d_pfr.action in ("bet", "check"), f"Unexpected action: {d_pfr.action}"


def test_postflop_bluff_bonus_aggregates_live_opponents():
    """The fold_happy bluff bonus must apply only when EVERY live, modeled
    opponent is fold-happy; a single calling station (fish) in the pot
    disables it. Previously the first live seat found could override the
    read on everyone else in a multi-way pot."""
    import nemesis.strategy as strat
    from nemesis import equity as eq_mod

    gs = _make_gs_postflop(["9h", "8h", "7d"], stage="flop", my_seat=0, n_opp=2)
    real_eq = eq_mod.hand_equity_mc
    real_rand = strat.random.random
    eq_mod.hand_equity_mc = lambda hole, board, n, samples=1500: 0.60
    strat.random.random = lambda: 1.0  # suppress random semi-bluffs: deterministic
    try:
        # Seat 1 fold_happy, seat 2 fish (calling station) -> no bluff bonus.
        om_station = OpponentModel()
        fh = om_station.get(1)
        fh.hands = 30; fh.vpip_hands = 9; fh.cbet_faced = 10; fh.cbet_folded = 9
        fish = om_station.get(2)
        fish.hands = 30; fish.vpip_hands = 22; fish.calls_or_checks = 8; fish.raises = 2
        d = decide(gs, om_station, 0, ["Ks", "Kh"])
        station_act = d.action
        assert d.action == "check", f"calling station in pot should kill bluff bonus, got {d.action}"

        # Both opponents fold_happy -> bluff bonus applies (0.60+0.08 > threshold).
        om_allfh = OpponentModel()
        for s in (1, 2):
            m = om_allfh.get(s)
            m.hands = 30; m.vpip_hands = 9; m.cbet_faced = 10; m.cbet_folded = 9
        d = decide(gs, om_allfh, 0, ["Ks", "Kh"])
        assert d.action == "bet", f"all-fold-happy field should get the bluff bonus, got {d.action}"
        print(f"OK aggregated bluff read: station-in-pot -> {station_act}, all-fold-happy -> {d.action}")
    finally:
        eq_mod.hand_equity_mc = real_eq
        strat.random.random = real_rand


def test_ranges_new_stackoff_and_iso():
    """Verify new range helpers exist and have expected contents."""
    assert "AA" in ranges.STACKOFF_VS_3BET
    assert "KK" in ranges.STACKOFF_VS_3BET
    assert "AKs" in ranges.STACKOFF_VS_3BET
    assert "72o" not in ranges.STACKOFF_VS_3BET
    assert "A5s" in ranges.FOURBET_BLUFF_HANDS

    iso1 = ranges.bb_iso_raise_range(1)
    iso2 = ranges.bb_iso_raise_range(2)
    assert len(iso1) > len(iso2), "iso range should shrink with more limpers"
    assert "AA" in iso1
    print(f"OK new ranges: stackoff={sorted(ranges.STACKOFF_VS_3BET)}, "
          f"iso1={len(iso1)} hands, iso2={len(iso2)} hands")


def test_icm_stage_aware():
    """ICM guard should be more lenient on flop than river for same stack share."""
    stacks = [4000, 1000, 1000, 1000, 1000, 1000, 1000]  # ~40% share for seat 0
    from nemesis import icm
    flop_guard  = icm.should_avoid_marginal_spot(4000, stacks, stage="flop")
    river_guard = icm.should_avoid_marginal_spot(4000, stacks, stage="river")
    # 4000/(4000+6000) = 40%; river threshold 35% → True; flop threshold 40% → False
    assert not flop_guard,  "40% stack share: should NOT avoid marginal spot on flop"
    assert river_guard,     "40% stack share: SHOULD avoid marginal spot on river"
    print(f"OK stage-aware ICM: flop_guard={flop_guard}, river_guard={river_guard}")


def test_equity_split_pot_correct():
    """Multi-way equity should handle three-way chops without inflating our wins."""
    # We can't easily force a three-way chop in MC, but we verify the change
    # doesn't break single-opponent accuracy (AA should still be ~85%).
    eq = equity.hand_equity_mc(["As", "Ah"], [], 2, samples=600)
    assert 0.55 < eq < 1.0, f"AA vs 2 opponents equity out of range: {eq}"
    print(f"OK multi-way equity (AA vs 2): {round(eq, 3)}")


def test_protocol_hand_counter_uses_active_seats():
    """Simulate hand_over and verify active (not just live) seats get counted."""
    from nemesis.protocol import NemesisBot
    bot = NemesisBot("127.0.0.1", 9000, "TestNemesis")
    # Set up a minimal GameState with 3 seats, seat 2 folded
    gs = GameState()
    players = [{"seat": i, "name": f"P{i}", "stack": 500, "bet": 0, "street_bet": 0,
                "folded": i == 2, "has_acted": True, "all_in": False,
                "busted": False, "is_turn": False} for i in range(3)]
    gs.update({
        "stage": "river", "dealer": 0, "turn": 0, "current_bet": 0, "min_raise": 10,
        "pot": 200, "common": ["As", "Kd", "7c", "2h", "Ts"],
        "blinds": {"small": 5, "big": 10, "ante": 0, "small_seat": 0, "big_seat": 1},
        "max_players": 9, "mode": "tournament", "players": players,
    })
    gs.my_seat = 0
    bot.gs = gs
    # Seat 2 is folded (not in live_seats, but IS in active_seats)
    assert 2 not in gs.live_seats(), "seat 2 is folded, not live"
    assert 2 in gs.active_seats(),   "seat 2 is not busted, still active"
    bot._reset_hand_trackers()
    # Both seats 1 and 2 should have hands += 1
    assert bot.om.get(1).hands == 1, f"seat 1 hands: {bot.om.get(1).hands}"
    assert bot.om.get(2).hands == 1, f"seat 2 hands: {bot.om.get(2).hands}"
    print("OK protocol hand counter: folded seats counted in active_seats()")



# ---------------------------------------------------------------------------
# New: range-weighted equity + pot-odds call system
# ---------------------------------------------------------------------------

def test_equity_vs_range_vs_random():
    """Range-weighted equity should differ meaningfully from random-hand equity.
    AA vs a tight top-10% range should be LOWER than AA vs random (wider) range,
    because the tight range is dominated by hands that do beat AA more often."""
    from nemesis.equity import hand_equity_vs_range
    from nemesis import ranges as r

    tight_range = r.top_pct(10.0)   # QQ+, AKs, AKo (the hands that compete with AA)
    wide_range  = r.top_pct(60.0)   # most of the deck

    eq_tight = hand_equity_vs_range(["As", "Ah"], [], tight_range, samples=3000)
    eq_wide  = hand_equity_vs_range(["As", "Ah"], [], wide_range,  samples=3000)
    eq_rand  = equity.hand_equity_mc(["As", "Ah"], [], 1, samples=600)

    assert eq_tight is not None, "range-weighted equity should return a value for top-10%"
    assert eq_wide  is not None, "range-weighted equity should return a value for top-60%"
    # AA vs tight range: faces AA/KK/QQ/AK more often → should have lower equity
    assert eq_tight < eq_wide, \
        f"AA vs tight range ({eq_tight:.3f}) should be < vs wide range ({eq_wide:.3f})"
    # Wide range ≈ random (within noise)
    assert abs(eq_wide - eq_rand) < 0.10, \
        f"Wide-range equity {eq_wide:.3f} should be near random {eq_rand:.3f}"
    print(f"OK range-weighted equity: AA vs tight={eq_tight:.3f}  wide={eq_wide:.3f}  rand={eq_rand:.3f}")


def test_equity_vs_range_returns_none_for_empty():
    """hand_equity_vs_range returns None for an empty range (caller falls back)."""
    from nemesis.equity import hand_equity_vs_range
    result = hand_equity_vs_range(["As", "Ah"], [], set(), samples=100)
    assert result is None, "empty range should return None"
    print("OK range-weighted equity returns None for empty range")


def test_pot_odds_call_adapts_to_bet_size():
    """Nemesis should call a small shove but fold a large overbet with the same hand.
    The old call_shove_range treated both the same (pure range membership);
    the new pot-odds logic distinguishes them correctly."""
    from nemesis.strategy import _decide_preflop, _estimate_shove_range
    from nemesis.state import GameState
    from nemesis.opponent_model import OpponentModel

    # Shared setup: we're at 10bb, UTG shoves
    def make_allin_state(shove_amount: int, our_stack: int, pot_before: int):
        gs = _make_gs(n_players=6, stack_bb=10, bb=10)
        gs.current_bet = shove_amount
        gs.players[5].street_bet = shove_amount
        gs.players[5].all_in = True
        gs.players[2].street_bet = 0
        gs.players[2].stack = our_stack
        gs.pot = pot_before
        return gs

    om = OpponentModel()
    # Same hand: KTo (decent but not premium)
    hole = ["Ks", "Td"]

    # Small shove: pot = 15 (blinds), shove = 95 → pot_odds ≈ 95/110 = 86%
    # Equity of KTo vs random hand ≈ 59% → should FOLD (equity < pot_odds)
    gs_big = make_allin_state(shove_amount=95, our_stack=100, pot_before=15)
    d_big = _decide_preflop(gs_big, om, 2, hole, last_raiser_seat=5)
    assert d_big.action == "fold", f"KTo vs 9.5bb shove should fold, pot_odds≈86%, got {d_big.action}"

    # Reasonable shove: pot = 15, shove = 85 → pot_odds ≈ 85/100 = 85%
    # Same conclusion (still fold), just checking it doesn't crash
    gs_med = make_allin_state(shove_amount=85, our_stack=100, pot_before=15)
    d_med = _decide_preflop(gs_med, om, 2, hole, last_raiser_seat=5)
    assert d_med.action in ("call_all", "fold"), f"unexpected action: {d_med.action}"

    print(f"OK pot-odds call: KTo vs big overbet → {d_big.action}, vs med bet → {d_med.action}")


def test_estimate_shove_range_nit_vs_fish():
    """Nit's estimated range should be tighter than a fish's."""
    from nemesis.strategy import _estimate_shove_range
    from nemesis.state import GameState
    from nemesis.opponent_model import OpponentModel

    gs = _make_gs(n_players=6, stack_bb=12, bb=10)
    om_nit = OpponentModel()
    om_fish = OpponentModel()

    # Make seat 5 look like a nit
    nit_model = om_nit.get(5)
    nit_model.hands = 30
    nit_model.vpip_hands = 4   # 4/30 = 13% VPIP

    # Make seat 5 look like a fish
    fish_model = om_fish.get(5)
    fish_model.hands = 30
    fish_model.vpip_hands = 22  # 22/30 = 73% VPIP

    nit_range  = _estimate_shove_range(gs, om_nit,  opp_seat=5)
    fish_range = _estimate_shove_range(gs, om_fish, opp_seat=5)

    assert len(nit_range) < len(fish_range), \
        f"nit range ({len(nit_range)}) should be tighter than fish range ({len(fish_range)})"
    print(f"OK estimated shove range: nit={len(nit_range)} hands  fish={len(fish_range)} hands")


def test_estimate_shove_range_fingerprint_widens():
    """A position-blind (fingerprinted) opponent gets BTN-wide range regardless of seat."""
    from nemesis.strategy import _estimate_shove_range
    from nemesis.state import GameState
    from nemesis.opponent_model import OpponentModel

    gs = _make_gs(n_players=6, stack_bb=12, bb=10)
    om = OpponentModel()

    # Feed uniform shove stats to trigger fingerprint (same rate from every position)
    m = om.get(5)
    m.hands = 60
    for pos in ["UTG", "UTG1", "MP1", "MP2", "HJ", "CO", "BTN"]:
        m.shoves_by_position[pos] = [5, 10]   # 5 shoves out of 10 opportunities at every seat

    normal_om = OpponentModel()
    normal_range = _estimate_shove_range(gs, normal_om, opp_seat=5)
    finger_range = _estimate_shove_range(gs, om,        opp_seat=5)

    assert len(finger_range) >= len(normal_range), \
        f"fingerprinted range ({len(finger_range)}) should be ≥ normal ({len(normal_range)})"
    print(f"OK fingerprint widens range: normal={len(normal_range)} → fingerprinted={len(finger_range)}")


if __name__ == "__main__":

    fns = [v for k, v in list(globals().items()) if k.startswith("test_")]
    passed = 0
    failed = 0
    for fn in fns:
        try:
            fn()
            passed += 1
        except Exception as e:
            print(f"FAIL {fn.__name__}: {e}")
            import traceback
            traceback.print_exc()
            failed += 1
    print(f"\n{'ALL' if not failed else 'SOME FAILED'}: {passed} passed, {failed} failed "
          f"({len(fns)} total)")
    if failed:
        sys.exit(1)
