#include "../src/poker.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <vector>

static int g_failures = 0;

#define CHECK(cond)                                                       \
        do {                                                              \
                if (!(cond)) {                                            \
                        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
                        g_failures++;                                     \
                }                                                         \
        } while (0)

#define CHECK_EQ(a, b)                                                          \
        do {                                                                    \
                long long _a = (long long)(a), _b = (long long)(b);             \
                if (_a != _b) {                                                 \
                        printf("FAIL %s:%d: %s == %s (%lld != %lld)\n",        \
                               __FILE__, __LINE__, #a, #b, _a, _b);             \
                        g_failures++;                                           \
                }                                                               \
        } while (0)

static void
add_six_players(Table &t)
{
        for (int i = 0; i < 6; i++) {
                t.add_player(Player("P" + std::to_string(i)));
        }
}

static size_t
count_unique(std::vector<int> ids)
{
        std::sort(ids.begin(), ids.end());
        return std::unique(ids.begin(), ids.end()) - ids.begin();
}

// Pre-set the current turn player's response, then step once.
static void
act(Table &t, Game_State &s, Player::Response::ResponseType type,
    int amount = 0, double now = 100.0)
{
        Player &p = t.players[s.turn];
        p.is_my_turn          = true;
        p.action_start        = now;
        p.response.has_response = true;
        p.response.type       = type;
        if (type == Player::Response::BET) {
                p.response.as.bet.amount = amount;
        }
        t.step_betting_round(&s, now);
}

static void
test_deck_reset()
{
        Deck deck;
        deck.reset();

        CHECK_EQ((int)deck.cards.size(), 52);

        std::vector<int> ids;
        for (const phevaluator::Card &c : deck.cards) {
                ids.push_back(int(c));
        }
        CHECK_EQ((int)count_unique(ids), 52);

        // dealing 12 cards leaves 40
        Deck d2;
        d2.reset();
        for (int i = 0; i < 12; i++) d2.pick();
        CHECK_EQ((int)d2.cards.size(), 40);
}

static void
test_new_hand_blinds()
{
        Deck deck;
        Table t(&deck);
        add_six_players(t);
        Game_State s{};
        t.new_hand(&s);

        CHECK_EQ(s.stage, PREFLOP);
        CHECK_EQ(s.dealer, 0);
        CHECK_EQ(s.turn, 3); // left of the big blind
        CHECK_EQ(s.current_bet, BigBlind);
        CHECK_EQ(t.players[1]._street_bet, SmallBlind);
        CHECK_EQ(t.players[2]._street_bet, BigBlind);
        CHECK_EQ(t.players[1]._bet, SmallBlind);
        CHECK_EQ(t.players[2]._bet, BigBlind);
        CHECK_EQ(t.pot, SmallBlind + BigBlind);
        CHECK(s.hand_started);
        CHECK(!s.round_done);

        for (const Player &p : t.players) {
                CHECK(p.hand.has_value());
                CHECK(!p._fold);
        }

        // no duplicate cards across all hands
        std::vector<int> ids;
        for (const Player &p : t.players) {
                ids.push_back(int(p.hand->at(0)));
                ids.push_back(int(p.hand->at(1)));
        }
        CHECK_EQ((int)count_unique(ids), 12);
}

static void
test_bb_option_full_call()
{
        Deck deck;
        Table t(&deck);
        add_six_players(t);
        Game_State s{};
        t.new_hand(&s);

        // everyone calls up to the big blind; the big blind gets the option
        // to check last: turn order 3,4,5,0,1(sb),2(bb)
        std::vector<int> expected = { 3, 4, 5, 0, 1, 2 };
        for (size_t i = 0; i < expected.size(); i++) {
                CHECK_EQ(s.turn, expected[i]);
                Player::Response::ResponseType action = (i == 5)
                        ? Player::Response::CHECK
                        : Player::Response::CALL;
                act(t, s, action);
        }

        CHECK(s.round_done);
        CHECK_EQ(t.pot, 6 * BigBlind);
        for (const Player &p : t.players) {
                CHECK_EQ(p._street_bet, BigBlind);
                CHECK(!p._fold);
        }
}

static void
test_raise_reopens_and_round_ends()
{
        Deck deck;
        Table t(&deck);
        add_six_players(t);
        Game_State s{};
        t.new_hand(&s);

        // P3 raises to 30 (legal: 10 + min_raise 10)
        act(t, s, Player::Response::BET, 30);
        CHECK_EQ(s.current_bet, 30);
        CHECK_EQ(s.min_raise, 20);
        CHECK_EQ(t.players[3]._street_bet, 30);
        CHECK(!s.round_done);
        CHECK_EQ(s.turn, 4);

        // the players who already acted must re-act
        for (int i = 0; i < 5; i++) {
                CHECK(!s.round_done);
                act(t, s, Player::Response::CALL);
        }

        CHECK(s.round_done);
        CHECK_EQ(t.pot, 6 * 30);
        for (const Player &p : t.players) {
                CHECK_EQ(p._street_bet, 30);
        }
}

static void
test_fold_walk_off()
{
        Deck deck;
        Table t(&deck);
        add_six_players(t);
        Game_State s{};
        t.new_hand(&s);

        // P3..P1 fold, leaving only P2 (the big blind)
        for (int i = 0; i < 5; i++) {
                act(t, s, Player::Response::FOLD);
        }
        CHECK(s.round_done);
        CHECK_EQ(t.non_folded_count(), 1);

        t.finish_hand(&s);
        CHECK_EQ(s.stage, OVER);
        CHECK_EQ((int)t.winners.size(), 1);
        CHECK_EQ(t.winners[0], 2);
        CHECK_EQ(t.award, SmallBlind + BigBlind);
        CHECK_EQ(t.players[2].stack, StartStack - BigBlind + SmallBlind + BigBlind);
}

static void
test_timeout_folds()
{
        Deck deck;
        Table t(&deck);
        add_six_players(t);
        Game_State s{};
        t.new_hand(&s);
        CHECK_EQ(s.turn, 3);

        Player &p = t.players[3];
        p.auto_play   = false; // human: does not auto-answer
        p.is_my_turn  = true;
        p.action_start = 0.0;  // way past the timeout
        p.response     = Player::Response{};

        t.step_betting_round(&s, 100.0);

        CHECK(p._fold);
        CHECK(!p.is_my_turn);
        CHECK_EQ(s.turn, 4);
        CHECK(!s.round_done);
}

static void
test_side_pots()
{
        Deck deck;
        Table t(&deck);
        add_six_players(t);
        Game_State s{};
        t.new_hand(&s);

        // fixed board and hands
        t.common = std::vector<phevaluator::Card>{
                phevaluator::Card("2s"), phevaluator::Card("3s"),
                phevaluator::Card("4s"), phevaluator::Card("5s"),
                phevaluator::Card("9h"),
        };
        t.players[0].hand.emplace(std::array<phevaluator::Card, 2>{
                phevaluator::Card("Ah"), phevaluator::Card("Ad") }); // 5-high straight flush
        t.players[1].hand.emplace(std::array<phevaluator::Card, 2>{
                phevaluator::Card("Kh"), phevaluator::Card("Kd") }); // pair of kings
        t.players[2].hand.emplace(std::array<phevaluator::Card, 2>{
                phevaluator::Card("Qh"), phevaluator::Card("Qd") }); // pair of queens

        // contributions: P0 all-in 50, P1/P2/P3/P4 put in 100, P5 nothing
        t.players[0]._bet  = 50;
        t.players[1]._bet  = 100;
        t.players[2]._bet  = 100;
        t.players[3]._bet  = 100;
        t.players[4]._bet  = 100;
        t.players[5]._bet  = 0;
        t.players[3]._fold = true;
        t.players[4]._fold = true;
        t.players[5]._fold = true;
        t.players[0].stack = 950;
        t.players[1].stack = 900;
        t.players[2].stack = 900;
        t.players[3].stack = 900;
        t.players[4].stack = 900;
        t.players[5].stack = 1000;
        t.pot              = 450;

        t.finish_hand(&s);
        CHECK_EQ(s.stage, OVER);

        // main pot (250) -> P0, side pot (200) -> P1
        CHECK_EQ(t.players[0].stack, 950 + 250);
        CHECK_EQ(t.players[1].stack, 900 + 200);
        CHECK_EQ(t.players[2].stack, 900);
        CHECK_EQ(t.award, 450);
        CHECK_EQ((int)t.winners.size(), 2);
        CHECK_EQ(t.pot, 0);
}

static void
test_tie_split()
{
        Deck deck;
        Table t(&deck);
        add_six_players(t);
        Game_State s{};
        t.new_hand(&s);

        // the board is a royal flush: both remaining players play the board
        t.common = std::vector<phevaluator::Card>{
                phevaluator::Card("As"), phevaluator::Card("Ks"),
                phevaluator::Card("Qs"), phevaluator::Card("Js"),
                phevaluator::Card("Ts"),
        };
        t.players[0].hand.emplace(std::array<phevaluator::Card, 2>{
                phevaluator::Card("2c"), phevaluator::Card("2d") });
        t.players[1].hand.emplace(std::array<phevaluator::Card, 2>{
                phevaluator::Card("3c"), phevaluator::Card("3d") });

        for (int i = 0; i < 6; i++) {
                t.players[i]._bet  = 100;
                t.players[i].stack = 900;
        }
        for (int i = 2; i < 6; i++) {
                t.players[i]._fold = true;
        }
        t.pot = 600;

        t.finish_hand(&s);
        CHECK_EQ(t.players[0].stack, 900 + 300);
        CHECK_EQ(t.players[1].stack, 900 + 300);
        CHECK_EQ(t.award, 600);
        CHECK_EQ((int)t.winners.size(), 2);
}

static void
test_full_hands()
{
        poker_set_seed(1234);
        Deck deck;
        Table t(&deck);
        add_six_players(t);
        Game_State s{};

        for (int hand = 0; hand < 5; hand++) {
                int guard = 0;
                while (s.stage != OVER && guard++ < 20000) {
                        t.step_game(&s, 0.0);
                }
                CHECK(s.stage == OVER);

                long long total = 0;
                for (const Player &p : t.players) {
                        total += p.stack;
                }
                CHECK_EQ(total, 6LL * StartStack);

                t.end_hand(&s);
                CHECK_EQ(s.stage, PREFLOP);
        }

        CHECK_EQ(s.dealer, 5); // dealer rotated once per hand
}

static void
test_busted_players()
{
        Deck deck;
        Table t(&deck);
        add_six_players(t);
        Game_State s{};

        // degenerate: everyone starts the hand broke (all-in, no chips)
        for (Player &p : t.players) {
                p.stack     = 0;
                p.is_all_in = true;
        }

        int guard = 0;
        while (s.stage != OVER && guard++ < 1000) {
                t.step_game(&s, 0.0);
        }
        CHECK(s.stage == OVER);
        CHECK_EQ(t.award, 0); // nothing was bet, nothing to award

        // busted players rebuy between hands
        t.end_hand(&s);
        CHECK_EQ(s.stage, PREFLOP);
        for (const Player &p : t.players) {
                CHECK_EQ(p.stack, StartStack);
                CHECK(!p._fold);
                CHECK(!p.is_all_in);
        }
}

int
main()
{
        test_deck_reset();
        test_new_hand_blinds();
        test_bb_option_full_call();
        test_raise_reopens_and_round_ends();
        test_fold_walk_off();
        test_timeout_folds();
        test_side_pots();
        test_tie_split();
        test_busted_players();
        test_full_hands();

        if (g_failures == 0) {
                printf("All tests passed\n");
                return 0;
        }
        printf("%d test(s) failed\n", g_failures);
        return 1;
}
