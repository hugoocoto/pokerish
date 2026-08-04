// Titan: a single-file competitive poker bot for the API.md WebSocket/JSON
// protocol. Handcrafted, Slumbot-style strategy:
//   - position-based preflop ranges (169 hand classes, bitmask tables)
//   - Monte-Carlo equity estimation via the phevaluator submodule
//   - pot-odds / bet-sizing decisions, short-stack push/fold, multiway care
// Transport adapted from bot/example/bot.cpp (same CLI + lws loop, but the
// strategy is deterministic-ish and decisions are instant).
//
// Run: ./build/titan [--port N] [--host IP] [--token SECRET] [--name NAME] [--selftest]

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <libwebsockets.h>
// Tripwire: the vendored libwebsockets build is minimal ws-only (deps.mk),
// and if its generated build/include/lws_config.h is missing at compile time
// the compiler silently falls back to /usr/include/lws_config.h, changing
// lws_context_creation_info's layout (runtime crash). Only the system config
// defines LWS_WITH_HTTP2.
#if defined(LWS_WITH_HTTP2)
#error "compiled against the system libwebsockets config: missing thirdparty/libwebsockets/build/include/lws_config.h"
#endif
#include <nlohmann/json.hpp>

#include <phevaluator/phevaluator.h>
#include <phevaluator/card.h>
#include <phevaluator/rank.h>

using phevaluator::Card;
using phevaluator::Rank;
using phevaluator::EvaluateCards;

template <typename T>
static T jval(const nlohmann::json &j, const char *key, const T &def) {
        if (!j.is_object() || !j.contains(key)) return def;
        const auto &val = j[key];
        if (val.is_null()) return def;
        try {
                return val.get<T>();
        } catch (...) {
                return def;
        }
}

static double
monotonic_now()
{
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static std::mt19937 &
titan_rng()
{
        static std::mt19937 rng(std::random_device{}());
        return rng;
}

static double
rand01()
{
        return std::uniform_real_distribution<double>(0.0, 1.0)(titan_rng());
}

// ---------------------------------------------------------------------------
// Preflop ranges: 169 hand classes as bitmasks.
// Pairs 0..12 (rank), suited 13..90, offsuit 91..168 (r1 > r2), ordered with
// the strongest hand first (AKs = 13).
// ---------------------------------------------------------------------------

struct Range {
        uint64_t m[3] = { 0, 0, 0 };
};

static int
rank_of(char c)
{
        switch (c) {
        case '2': return 0;  case '3': return 1;  case '4': return 2;
        case '5': return 3;  case '6': return 4;  case '7': return 5;
        case '8': return 6;  case '9': return 7;  case 'T': return 8;
        case 'J': return 9;  case 'Q': return 10; case 'K': return 11;
        case 'A': return 12;
        }
        return -1;
}

static int
hand_index(int r1, int r2, bool suited)
{
        int off = (78 - r1 * (r1 + 1) / 2) + (r1 - 1 - r2);
        return suited ? 13 + off : 91 + off;
}

static void
set_bit(Range &r, int i)
{
        r.m[i / 64] |= 1ULL << (i % 64);
}

static bool
get_bit(const Range &r, int i)
{
        return (r.m[i / 64] >> (i % 64)) & 1;
}

// Tokens: "22+" (all pairs 2..A), "A5s", "A5s+" (A5s..AKs), "A2o", "A2o+",
// "T9o" ...
static void
add_tok(Range &r, const char *t)
{
        int len = (int) strlen(t);
        if (len < 2) return;
        bool plus = t[len - 1] == '+';
        int ra = rank_of(t[0]);
        int rb = rank_of(t[1]);
        if (ra < 0 || rb < 0) return;
        if (ra == rb) { // pair; class index == rank
                int k1 = plus ? 12 : ra;
                for (int k = ra; k <= k1; k++) set_bit(r, k);
                return;
        }
        bool suited = len >= 3 && (t[2] == 's' || t[2] == 'S');
        if (ra < rb) {
                int tmp = ra;
                ra = rb;
                rb = tmp;
        }
        int hi = ra, lo = rb;
        int k1 = plus ? hi - 1 : lo;
        for (int k = lo; k <= k1; k++) set_bit(r, hand_index(hi, k, suited));
}

static Range
make_range(const char *list)
{
        Range r;
        const char *p = list;
        while (*p) {
                const char *e = strchr(p, ',');
                int len = e ? (int) (e - p) : (int) strlen(p);
                if (len >= 2) {
                        char tok[16];
                        if (len > 15) len = 15;
                        memcpy(tok, p, len);
                        tok[len] = 0;
                        add_tok(r, tok);
                }
                if (!e) break;
                p = e + 1;
        }
        return r;
}

static bool
in_range(const Range &r, const Card &a, const Card &b)
{
        int ia = int(a), ib = int(b);
        int ra = ia / 4, rb = ib / 4;
        if (ra == rb) return get_bit(r, ra);
        if (ra < rb) {
                int tmp = ra;
                ra = rb;
                rb = tmp;
        }
        bool suited = (ia % 4) == (ib % 4);
        return get_bit(r, hand_index(ra, rb, suited));
}

static bool g_ranges_init = false;
static Range R_BTN, R_CO, R_MP, R_EP, R_SB;
static Range R3_EP, R3_MP, R3_LP;          // 3-bet value+bluff vs opener tier
static Range CALL_EP, CALL_MP, CALL_LP;    // call vs opener tier (non-BB)
static Range BBB_EP, BBB_MP, BBB_LP;       // BB 3-bet vs opener tier
static Range BBC_EP, BBC_MP, BBC_LP;       // BB call vs opener tier
static Range STACKOFF, CALL3B, CALL3B_LP;  // facing a 3-bet
static Range CALL_MW, RAISE_LIMP, SB_CALL;

static void
init_ranges()
{
        if (g_ranges_init) return;
        g_ranges_init = true;

        // Open ranges, distance from button: 0=BTN 1=CO 2=MP 3=EP.
        R_BTN = make_range(
                "22+,A2s+,K2s+,Q5s+,J7s+,T6s+,95s+,84s+,74s+,63s+,53s+,43s,"
                "A2o+,K7o+,Q9o+,J9o+,T9o,98o");
        R_CO = make_range(
                "22+,A2s+,K6s+,Q8s+,J8s+,T7s+,96s+,85s+,75s+,64s+,"
                "A8o+,K9o+,QTo+,JTo");
        R_MP = make_range(
                "22+,A3s+,K8s+,Q9s+,J9s+,T8s+,97s+,86s+,76s,"
                "A9o+,KTo+,QJo");
        R_EP = make_range(
                "22+,A5s+,K9s+,Q9s+,J9s+,T8s+,97s+,87s,ATo+,KJo+");
        R_SB = make_range(
                "22+,A2s+,K3s+,Q7s+,J8s+,T8s+,97s+,86s+,75s+,64s+,54s,"
                "A2o+,K9o+,QTo+,JTo,T9o");

        // 3-bet / call vs an open raise, by opener tier.
        R3_EP   = make_range("JJ+,AKs,AQs,AKo");
        CALL_EP = make_range("77+,ATs+,KQs,AJo+,KTs+,QJs,JTs,T9s,98s,87s");
        R3_MP   = make_range("TT+,AJs+,AQo+,KQs,A5s,A4s");
        CALL_MP = make_range("55+,A7s+,AJo+,KTs+,QJs,QTs,JTs,T9s,98s,87s,76s");
        R3_LP   = make_range("99+,ATs+,AJo+,KQs,AQo,A5s,A4s,A3s,A2s,KTs,QTs");
        CALL_LP = make_range("44+,A8s+,ATo+,K9s+,KTo+,Q9s+,QJo,J9s+,T9s,98s,87s,76s,65s");

        // Big blind defense.
        BBB_EP   = make_range("JJ+,AKs,AKo,AQs");
        BBC_EP   = make_range("77+,ATs+,KQs,AJo+,KTs+,QJs,JTs,T9s,98s,87s");
        BBB_MP   = make_range("TT+,AJs+,AQo+,KQs,A5s,A4s");
        BBC_MP   = make_range("55+,A7s+,A9o+,K9s+,KTo+,Q9s+,QJo,J8s+,T8s+,97s+,86s+,76s");
        BBB_LP   = make_range("TT+,AJs+,AQo+,KQs,A5s,A4s,KTs");
        BBC_LP   = make_range("44+,A2s+,A8o+,K8s+,K9o+,Q8s+,QTo+,J8s+,T8s+,97s+,86s+,75s+,65s,54s");

        // Facing a 3-bet after opening.
        STACKOFF  = make_range("KK+,AKs,AKo");
        CALL3B    = make_range("QQ,JJ,TT,AQs");
        CALL3B_LP = make_range("QQ,JJ,TT,AQs,AJs,KQs");

        // Cold multiway spots.
        CALL_MW = make_range("JJ,TT,AQs,AJs,KQs");

        // BB raise over limpers; SB completion.
        RAISE_LIMP = make_range("22+,A2s+,K8s+,Q9s+,J9s+,T9s,98s,ATo+,KJo+");
        SB_CALL    = make_range("22+,A2s+,K5s+,Q7s+,J8s+,T8s+,97s+,86s+,75s+,64s+,"
                                "A8o+,K9o+,QTo+,JTo");
}

static const Range &
open_range(int dist)
{
        if (dist == 0) return R_BTN;
        if (dist == 1) return R_CO;
        if (dist == 2) return R_MP;
        return R_EP;
}

// ---------------------------------------------------------------------------
// Equity estimation (phevaluator): exact enumeration on turn/river heads-up,
// Monte-Carlo everywhere else. Returns P(win) + 0.5*P(tie).
// ---------------------------------------------------------------------------

static int
eval7(const Card *hole, int nhole, const Card *board, int nboard)
{
        Card cards[7];
        int n = 0;
        for (int i = 0; i < nhole; i++) cards[n++] = hole[i];
        for (int i = 0; i < nboard; i++) cards[n++] = board[i];
        switch (n) {
        case 5:
                return EvaluateCards(cards[0], cards[1], cards[2], cards[3], cards[4]).value();
        case 6:
                return EvaluateCards(cards[0], cards[1], cards[2], cards[3], cards[4], cards[5]).value();
        default:
                return EvaluateCards(cards[0], cards[1], cards[2], cards[3], cards[4], cards[5], cards[6]).value();
        }
}

static int
eval_7c(int c1, int c2, const Card *fb)
{
        return EvaluateCards(Card(c1), Card(c2), fb[0], fb[1], fb[2], fb[3], fb[4]).value();
}

// Pick k distinct ids from pool (destroys pool ordering).
static void
sample_cards(std::vector<int> &pool, int k, int *out, std::mt19937 &rng)
{
        for (int i = 0; i < k; i++) {
                int j = i + (int) (std::uniform_int_distribution<int>(0, (int) pool.size() - 1 - i)(rng));
                std::swap(pool[i], pool[j]);
                out[i] = pool[i];
        }
}

// Heads-up equity vs one random opponent hand.
static double
equity_heads_up(const Card *hole, const Card *board, int nboard, std::mt19937 &rng,
                int mc_samples = 0)
{
        bool used[52] = {};
        used[int(hole[0])] = true;
        used[int(hole[1])] = true;
        for (int i = 0; i < nboard; i++) used[int(board[i])] = true;

        std::vector<int> unk;
        for (int i = 0; i < 52; i++)
                if (!used[i]) unk.push_back(i);

        if (nboard == 5) { // river: exact over all opponent hands
                long wins = 0, ties = 0;
                for (size_t i = 0; i < unk.size(); i++) {
                        for (size_t j = 0; j < i; j++) {
                                int mv = eval7(hole, 2, board, 5);
                                int ov = eval_7c(unk[i], unk[j], board);
                                if (mv < ov) wins++;
                                else if (mv == ov) ties++;
                        }
                }
                double total = (double) unk.size() * (unk.size() - 1) / 2;
                return (wins + 0.5 * ties) / total;
        }
        if (nboard == 4) { // turn: exact over opponent hands x rivers
                long wins = 0, ties = 0;
                for (size_t i = 0; i < unk.size(); i++) {
                        for (size_t j = 0; j < i; j++) {
                                Card fb[5];
                                for (int k = 0; k < 4; k++) fb[k] = board[k];
                                for (size_t r = 0; r < unk.size(); r++) {
                                        if (r == i || r == j) continue;
                                        fb[4] = Card(unk[r]);
                                        int mv = eval7(hole, 2, fb, 5);
                                        int ov = eval_7c(unk[i], unk[j], fb);
                                        if (mv < ov) wins++;
                                        else if (mv == ov) ties++;
                                }
                        }
                }
                double total = (double) unk.size() * (unk.size() - 1) / 2 * (unk.size() - 2);
                return (wins + 0.5 * ties) / total;
        }
        // flop / preflop: Monte-Carlo (mc_samples overrides the default trial
        // count; the startup all-in table passes more samples because the
        // selftest and the push/fold ranges rely on a stable ordering)
        int samples = mc_samples > 0 ? mc_samples : (nboard == 3 ? 2000 : 1500);
        long wins = 0, ties = 0;
        int drawn[7];
        Card fb[5];
        for (int s = 0; s < samples; s++) {
                std::vector<int> pool = unk;
                sample_cards(pool, 2 + (5 - nboard), drawn, rng);
                for (int k = 0; k < nboard; k++) fb[k] = board[k];
                for (int k = nboard; k < 5; k++) fb[k] = Card(drawn[2 + k - nboard]);
                int mv = eval7(hole, 2, fb, 5);
                int ov = eval_7c(drawn[0], drawn[1], fb);
                if (mv < ov) wins++;
                else if (mv == ov) ties++;
        }
        return (wins + 0.5 * ties) / (double) samples;
}

// Equity vs n opponents (n >= 2), pairwise scoring against random hands.
static double
equity_multiway(const Card *hole, const Card *board, int nboard, int nopp, std::mt19937 &rng)
{
        bool used[52] = {};
        used[int(hole[0])] = true;
        used[int(hole[1])] = true;
        for (int i = 0; i < nboard; i++) used[int(board[i])] = true;

        std::vector<int> unk;
        for (int i = 0; i < 52; i++)
                if (!used[i]) unk.push_back(i);

        int samples = 1500 / nopp;
        if (samples < 250) samples = 250;
        double p = 0;
        int drawn[12];
        Card fb[5];
        for (int s = 0; s < samples; s++) {
                std::vector<int> pool = unk;
                sample_cards(pool, 2 * nopp + (5 - nboard), drawn, rng);
                for (int k = 0; k < nboard; k++) fb[k] = board[k];
                for (int k = nboard; k < 5; k++) fb[k] = Card(drawn[2 * nopp + k - nboard]);
                int mv = eval7(hole, 2, fb, 5);
                double pts = 0;
                for (int o = 0; o < nopp; o++) {
                        int ov = eval_7c(drawn[2 * o], drawn[2 * o + 1], fb);
                        if (mv < ov) pts += 1;
                        else if (mv == ov) pts += 0.5;
                }
                p += pts / nopp;
        }
        return p / samples;
}

// All-in push/fold tables: every one of the 169 hand classes ordered by
// preflop equity vs a random hand. Range masks for the common push widths.
static const int kNPct = 19;
static const int kPcts[kNPct] = { 5, 10, 15, 20, 25, 30, 35, 40, 45,
                                  50, 55, 60, 65, 70, 75, 80, 85, 90, 95 };
struct AllInTable {
        double eq[169];
        int order[169];       // class index, strongest first
        double boundary[kNPct]; // equity of the worst hand in top kPcts[i]
        Range top[kNPct];
};
static AllInTable g_ai;

static void
build_allin_table()
{
        for (int r1 = 0; r1 < 13; r1++) {
                for (int r2 = 0; r2 < r1; r2++) {
                        Card hole[2];
                        hole[0] = Card(r1 * 4);
                        hole[1] = Card(r2 * 4);
                        g_ai.eq[hand_index(r1, r2, true)] =
                                equity_heads_up(hole, nullptr, 0, titan_rng(), 6000);
                        hole[0] = Card(r1 * 4);
                        hole[1] = Card(r2 * 4 + 1);
                        g_ai.eq[hand_index(r1, r2, false)] =
                                equity_heads_up(hole, nullptr, 0, titan_rng(), 6000);
                }
        }
        for (int r = 0; r < 13; r++) {
                Card hole[2] = { Card(r * 4), Card(r * 4 + 1) };
                g_ai.eq[r] = equity_heads_up(hole, nullptr, 0, titan_rng(), 6000);
        }
        // sort by equity desc
        for (int i = 0; i < 169; i++) g_ai.order[i] = i;
        for (int i = 0; i < 169; i++) {
                for (int j = i + 1; j < 169; j++) {
                        if (g_ai.eq[g_ai.order[j]] > g_ai.eq[g_ai.order[i]]) {
                                int tmp = g_ai.order[i];
                                g_ai.order[i] = g_ai.order[j];
                                g_ai.order[j] = tmp;
                        }
                }
        }
        for (int p = 0; p < kNPct; p++) {
                int count = (169 * kPcts[p] + 50) / 100;
                if (count < 1) count = 1;
                g_ai.boundary[p] = g_ai.eq[g_ai.order[count - 1]];
                for (int i = 0; i < count; i++) set_bit(g_ai.top[p], g_ai.order[i]);
        }
}

static const Range &
top_range(int pct)
{
        for (int i = 0; i < kNPct; i++)
                if (kPcts[i] == pct) return g_ai.top[i];
        static Range none;
        return none;
}

static int
push_pct(double sbb)
{
        if (sbb <= 6) return 90;
        if (sbb <= 10) return 75;
        if (sbb <= 15) return 55;
        if (sbb <= 20) return 40;
        return 0;
}

// Tightest top-x% range whose worst hand still clears `needed` equity.
static int
call_pct(double needed)
{
        for (int i = 0; i < kNPct; i++)
                if (g_ai.boundary[i] >= needed) return kPcts[i];
        return 0;
}

// ---------------------------------------------------------------------------
// Game model (parsed from a state broadcast)
// ---------------------------------------------------------------------------

// The server seats up to MaxPlayers (10) players; never hardcode 6.
static const int kMaxSeats = 10;

struct Player {
        int seat = -1;
        int stack = 0;
        int street_bet = 0;
        bool folded = false;
        bool all_in = false;
        bool busted = false;
};

struct Game {
        int stage = 0;          // 0 preflop .. 3 river, 4 over
        int dealer = 0;
        int current_bet = 0;
        int min_raise = 10;
        int pot = 0;
        int sb = 5, bb = 10, ante = 0;
        int sb_seat = -1, bb_seat = -1;
        int my_seat = -1;
        Player players[kMaxSeats];
        Card hole[2];
        Card common[5];
        int ncommon = 0;
        int my_pos = 0;         // 0 = first live seat after the dealer
        int live = 0;           // seated, alive players (incl. me)
        int opps = 0;           // live, not folded (excl. me)
        bool hu = false;
        bool in_position = false;
};

static int
stage_index(const std::string &s)
{
        if (s == "preflop") return 0;
        if (s == "flop") return 1;
        if (s == "turn") return 2;
        if (s == "river") return 3;
        return 4;
}

static bool
parse_game(const nlohmann::json &st, Game &g)
{
        if (!st.is_object()) return false;
        g.stage = stage_index(jval<std::string>(st, "stage", "over"));
        g.dealer = jval<int>(st, "dealer", 0);
        g.current_bet = jval<int>(st, "current_bet", 0);
        g.min_raise = jval<int>(st, "min_raise", g.bb);
        g.pot = jval<int>(st, "pot", 0);
        if (st.contains("blinds") && st["blinds"].is_object()) {
                g.sb = jval<int>(st["blinds"], "small", 5);
                g.bb = jval<int>(st["blinds"], "big", 10);
                g.ante = jval<int>(st["blinds"], "ante", 0);
                g.sb_seat = jval<int>(st["blinds"], "small_seat", -1);
                g.bb_seat = jval<int>(st["blinds"], "big_seat", -1);
        }
        for (int i = 0; i < kMaxSeats; i++) g.players[i] = Player();
        if (st.contains("players") && st["players"].is_array()) {
                for (const auto &p : st["players"]) {
                        if (!p.is_object()) continue;
                        int seat = jval<int>(p, "seat", -1);
                        if (seat < 0 || seat >= kMaxSeats) continue;
                        Player &pl = g.players[seat];
                        pl.seat = seat;
                        pl.stack = jval<int>(p, "stack", 0);
                        pl.street_bet = jval<int>(p, "street_bet", 0);
                        pl.folded = jval<bool>(p, "folded", false);
                        pl.all_in = jval<bool>(p, "all_in", false);
                        pl.busted = jval<bool>(p, "busted", false);
                }
        }
        g.ncommon = 0;
        if (st.contains("common") && st["common"].is_array()) {
                for (const auto &c : st["common"]) {
                        if (g.ncommon >= 5) break;
                        if (c.is_string()) g.common[g.ncommon++] = Card(c.get<std::string>());
                }
        }
        g.live = 0;
        for (int i = 0; i < kMaxSeats; i++) {
                const Player &p = g.players[i];
                if (!p.busted && p.stack > 0) g.live++;
        }
        g.my_pos = 0;
        if (g.my_seat >= 0 && g.live > 0) {
                // pos = number of live players who act before us, i.e. the
                // live players from dealer+1 (first to act) up to our seat.
                int pos = 0;
                for (int idx = (g.dealer + 1) % kMaxSeats; idx != g.my_seat;
                     idx = (idx + 1) % kMaxSeats) {
                        const Player &p = g.players[idx];
                        if (!p.busted && p.stack > 0) pos++;
                }
                g.my_pos = pos;
        }
        g.opps = 0;
        for (int i = 0; i < kMaxSeats; i++) {
                const Player &p = g.players[i];
                if (i == g.my_seat) continue;
                if (!p.busted && p.stack > 0 && !p.folded) g.opps++;
        }
        g.hu = (g.live == 2);
        g.in_position = false;
        if (g.my_seat >= 0 && g.live > 1) {
                int idx = (g.my_seat + 1) % kMaxSeats;
                while (idx != g.dealer) {
                        const Player &p = g.players[idx];
                        if (!p.busted && p.stack > 0 && !p.folded && !p.all_in)
                                g.in_position = true;
                        idx = (idx + 1) % kMaxSeats;
                }
        }
        return true;
}

// ---------------------------------------------------------------------------
// The decision engine
// ---------------------------------------------------------------------------

struct Decision {
        const char *act; // "fold" "check" "call" "call_all" "bet"
        int target;      // for "bet": desired total street bet (chips)
};

static Decision
allin(const Player &me)
{
        return Decision{ "bet", me.stack + me.street_bet };
}

static int
count_limpers(const Game &g, const Player &me)
{
        int n = 0;
        for (int i = 0; i < kMaxSeats; i++) {
                const Player &p = g.players[i];
                if (i == g.my_seat || i == g.sb_seat || i == g.bb_seat) continue;
                if (!p.busted && p.stack > 0 && !p.folded && p.street_bet == g.bb) n++;
        }
        (void) me;
        return n;
}

static Decision
decide_preflop(const Game &g, const Player &me)
{
        double bb = g.bb > 0 ? g.bb : 10;
        double sbb = me.stack / bb;
        int pos = g.my_pos;
        int dist = g.live - 1 - pos;   // 0 = button
        bool in_bb = g.hu ? (pos == 0) : (pos == 1);
        bool in_sb = !g.hu && pos == 0;
        bool raised = g.current_bet > g.bb;
        int to_call = g.current_bet - me.street_bet;
        if (to_call < 0) to_call = 0;

        // -- short stack: push/fold --
        if (sbb <= 20) {
                if (in_bb && !raised) {
                        int limpers = count_limpers(g, me);
                        if (limpers == 0) return Decision{ "check", 0 };
                        int pct = push_pct(sbb);
                        if (pct > 0 && in_range(top_range(pct), g.hole[0], g.hole[1]))
                                return allin(me);
                        return Decision{ "check", 0 };
                }
                if (to_call <= 0) {
                        int pct = push_pct(sbb);
                        if (pct > 0 && in_range(top_range(pct), g.hole[0], g.hole[1]))
                                return allin(me);
                        return Decision{ "fold", 0 };
                }
                if (to_call >= me.stack) {
                        double need = (double) to_call / (double) (g.pot + 2.0 * to_call);
                        double margin = g.hu ? 0.05 : 0.10;
                        int pct = call_pct(need + margin);
                        if (pct > 0 && in_range(top_range(pct), g.hole[0], g.hole[1]))
                                return Decision{ "call_all", 0 };
                        return Decision{ "fold", 0 };
                }
                int pct = push_pct(sbb);
                if (pct > 0 && in_range(top_range(pct), g.hole[0], g.hole[1]))
                        return allin(me);
                return Decision{ "fold", 0 };
        }

        // -- deep stack --
        if (!raised) {
                if (in_bb) {
                        int limpers = count_limpers(g, me);
                        if (limpers == 0) return Decision{ "check", 0 };
                        if (in_range(RAISE_LIMP, g.hole[0], g.hole[1]))
                                return Decision{ "bet", (int) (2.5 * bb) + limpers * (int) bb };
                        return Decision{ "check", 0 };
                }
                if (in_sb) {
                        if (in_range(R_SB, g.hole[0], g.hole[1]))
                                return Decision{ "bet", (int) (3 * bb) };
                        if (in_range(SB_CALL, g.hole[0], g.hole[1]))
                                return Decision{ "call", 0 };
                        return Decision{ "fold", 0 };
                }
                int limpers = count_limpers(g, me);
                if (in_range(open_range(dist), g.hole[0], g.hole[1]))
                        return Decision{ "bet", (int) (2.5 * bb) + limpers * (int) bb };
                return Decision{ "fold", 0 };
        }

        // Facing a raise.
        if (me.street_bet > g.bb) {
                // We already raised: facing a 3-bet.
                if (in_range(STACKOFF, g.hole[0], g.hole[1]))
                        return allin(me);
                const Range &c3 = (g.hu || dist <= 1) ? CALL3B_LP : CALL3B;
                if (in_range(c3, g.hole[0], g.hole[1]))
                        return Decision{ "call", 0 };
                return Decision{ "fold", 0 };
        }

        // Facing an open raise.
        int raiser_seat = -1, raise_peers = 0;
        for (int i = 0; i < kMaxSeats; i++) {
                const Player &p = g.players[i];
                if (i == g.my_seat) continue;
                if (!p.busted && p.stack > 0 && !p.folded && p.street_bet == g.current_bet) {
                        raiser_seat = i;
                        raise_peers++;
                }
        }
        if (raise_peers >= 2 || count_limpers(g, me) > 0) {
                // Cold multiway: only premiums.
                if (in_range(R3_EP, g.hole[0], g.hole[1]))
                        return Decision{ "bet", 3 * g.current_bet };
                if (in_range(CALL_MW, g.hole[0], g.hole[1]))
                        return Decision{ "call", 0 };
                return Decision{ "fold", 0 };
        }
        int rpos = 0;
        if (raiser_seat >= 0) {
                for (int idx = (g.dealer + 1) % kMaxSeats; idx != raiser_seat;
                     idx = (idx + 1) % kMaxSeats) {
                        const Player &p = g.players[idx];
                        if (!p.busted && p.stack > 0) rpos++;
                }
        }
        int rdist = g.live - 1 - rpos;
        int tier = rdist <= 1 ? 2 : (rdist == 2 ? 1 : 0); // 2=LP 1=MP 0=EP
        const Range *r3, *c3;
        if (in_bb) {
                const Range *b3[3] = { &BBB_EP, &BBB_MP, &BBB_LP };
                const Range *bc[3] = { &BBC_EP, &BBC_MP, &BBC_LP };
                r3 = b3[tier];
                c3 = bc[tier];
        } else {
                const Range *r3t[3] = { &R3_EP, &R3_MP, &R3_LP };
                const Range *c3t[3] = { &CALL_EP, &CALL_MP, &CALL_LP };
                r3 = r3t[tier];
                c3 = c3t[tier];
        }
        if (in_range(*r3, g.hole[0], g.hole[1]))
                return Decision{ "bet", 3 * g.current_bet };
        if (in_range(*c3, g.hole[0], g.hole[1]))
                return Decision{ "call", 0 };
        return Decision{ "fold", 0 };
}

static Decision
decide_postflop(const Game &g, const Player &me)
{
        int nboard = g.ncommon;
        if (nboard < 3 || g.opps == 0) return Decision{ "check", 0 };
        double eq;
        if (g.opps == 1)
                eq = equity_heads_up(g.hole, g.common, nboard, titan_rng());
        else
                eq = equity_multiway(g.hole, g.common, nboard, g.opps, titan_rng());

        double pot = g.pot;
        double spr = pot > 0 ? me.stack / pot : 10.0;
        int to_call = g.current_bet - me.street_bet;
        if (to_call < 0) to_call = 0;
        // Pot odds: the equity we need to break even calling to_call chips.
        // Formula: to_call / (pot_after_call) = to_call / (pot + to_call).
        // Using 2*to_call in the denominator double-counts the bet and
        // artificially lowers the threshold, making Titan call too loosely.
        double needed = pot > 0 ? (double) to_call / (pot + to_call) : 0.0;

        // Draw flag: weak made hand but meaningful equity (flush/straight draws).
        int rv = eval7(g.hole, 2, g.common, nboard);
        Rank rk(rv);
        bool draw = eq >= 0.33 &&
                    (rk.category() == HIGH_CARD || rk.category() == ONE_PAIR);

        if (to_call <= 0) {
                // No bet to us: bet for value, semi-bluff draws, occasional bluff.
                if (g.opps >= 3) {
                        if (eq >= 0.85) return Decision{ "bet", (int) (0.7 * pot) };
                        if (eq >= 0.65) return Decision{ "bet", (int) (0.5 * pot) };
                        return Decision{ "check", 0 };
                }
                if (eq >= 0.85) {
                        if (spr <= 1.2) return allin(me);
                        return Decision{ "bet", (int) (0.8 * pot) };
                }
                if (eq >= 0.6) return Decision{ "bet", (int) (0.6 * pot) };
                if (eq >= 0.45) {
                        if (g.in_position || spr >= 5) return Decision{ "bet", (int) (0.4 * pot) };
                        return Decision{ "check", 0 };
                }
                if (draw) return Decision{ "bet", (int) (0.5 * pot) };
                if (eq < 0.3 && rand01() < 0.25) return Decision{ "bet", (int) (0.6 * pot) };
                return Decision{ "check", 0 };
        }

        // Facing a bet.
        double margin = 0.05 + 0.05 * (g.opps - 1);
        if (to_call >= me.stack) {
                if (eq >= needed + margin) return Decision{ "call_all", 0 };
                return Decision{ "fold", 0 };
        }
        if (eq >= needed + margin) {
                if (eq >= 0.75 && g.opps == 1 && spr >= 2)
                        return Decision{ "bet", g.current_bet + 3 * to_call };
                return Decision{ "call", 0 };
        }
        if (eq >= 0.9 && g.opps == 1)
                return Decision{ "bet", g.current_bet + 3 * to_call };
        return Decision{ "fold", 0 };
}

static Decision
decide(const Game &g)
{
        if (g.my_seat < 0 || g.opps == 0) return Decision{ "check", 0 };
        const Player &me = g.players[g.my_seat];
        if (me.folded) return Decision{ "check", 0 };
        if (g.stage == 0) return decide_preflop(g, me);
        return decide_postflop(g, me);
}

// ---------------------------------------------------------------------------
// Transport: one WebSocket connection = one player (adapted from bot/example)
// ---------------------------------------------------------------------------

static lws_context *bot_context();

class Bot
{
    public:
        Bot(lws_context *ctx, const char *host, int port,
            const char *token = nullptr, const char *name = nullptr)
        : ctx_(ctx), host_(host), port_(port),
          name_(name && name[0] ? name : "Titan"), token_(token ? token : "")
        {
                connect();
        }

        static double now() { return monotonic_now(); }

        bool connected() const { return wsi_ && established_; }
        int seat() const { return seat_; }
        const std::string &name() const { return name_; }

        void pump(double t);
        void close();

    private:
        friend lws_context *bot_context();
        static int callback(lws *wsi, enum lws_callback_reasons reason,
                            void *user, void *in, size_t len);

        void connect();
        void flush();
        void send_json(const nlohmann::json &j);
        void send_action(const std::string &action, int amount = 0);
        void on_message(const nlohmann::json &j);
        void maybe_decide();
        void decide_and_send();

        lws_context *ctx_;
        std::string host_;
        int port_;
        std::string name_;
        std::string token_;

        lws *wsi_{ nullptr };
        bool established_{ false };
        std::string inbuf_;
        std::deque<std::string> outq_;
        int seat_{ -1 };
        nlohmann::json state_;
        int hand_epoch_{ 0 };   // bumped at every hand_over
        int cards_epoch_{ -1 }; // hand_epoch_ our hole cards were fetched for
        bool turn_pending_{ false };
        std::string cards_[2];
        double last_connect_try_{ 0 };
};

static lws_context *
bot_context()
{
        static struct lws_protocols protocols[] = {
                { "poker", Bot::callback, 0, 4096, 0, nullptr, 0 },
                LWS_PROTOCOL_LIST_TERM,
        };
        struct lws_context_creation_info info{};
        info.port      = CONTEXT_PORT_NO_LISTEN;
        info.protocols = protocols;
        return lws_create_context(&info);
}

void
Bot::connect()
{
        struct lws_client_connect_info i{};
        i.context                   = ctx_;
        i.address                   = host_.c_str();
        i.host                      = host_.c_str(); // Host: header (required by lws 5)
        i.port                      = port_;
        i.path                      = "/";
        i.protocol                  = "poker";
        i.userdata                  = this;
        i.ietf_version_or_minus_one = -1;
        wsi_                        = lws_client_connect_via_info(&i);
}

void
Bot::close()
{
        if (!wsi_) return;
        lws_set_timeout(wsi_, PENDING_TIMEOUT_KILLED_BY_PROXY_CLIENT_CLOSE, LWS_TO_KILL_ASYNC);
}

void
Bot::flush()
{
        if (outq_.empty()) return;
        const std::string &msg = outq_.front();
        std::string buf(LWS_PRE, '\0');
        buf.append(msg);
        int n = lws_write(wsi_, (unsigned char *) buf.data() + LWS_PRE, msg.size(),
                          LWS_WRITE_TEXT);
        if (n < (int) msg.size()) {
                lws_callback_on_writable(wsi_); // retry the frame later
                return;
        }
        outq_.pop_front();
        if (!outq_.empty()) lws_callback_on_writable(wsi_);
}

void
Bot::send_json(const nlohmann::json &j)
{
        outq_.push_back(j.dump());
        if (wsi_) lws_callback_on_writable(wsi_);
}

void
Bot::send_action(const std::string &action, int amount)
{
        nlohmann::json j = { { "type", "action" }, { "action", action } };
        if (action == "bet") j["amount"] = amount;
        send_json(j);
}

int
Bot::callback(lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len)
{
        Bot *b = (Bot *) user;
        switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
                b->wsi_         = wsi;
                b->established_ = true;
                {
                        nlohmann::json hello = { { "type", "hello" }, { "name", b->name_ } };
                        if (!b->token_.empty()) hello["token"] = b->token_;
                        b->send_json(hello);
                }
                break;

        case LWS_CALLBACK_CLIENT_RECEIVE:
                b->inbuf_.append((const char *) in, len);
                if (lws_is_final_fragment(wsi) && lws_remaining_packet_payload(wsi) == 0) {
                        std::string msg = std::move(b->inbuf_);
                        b->inbuf_.clear();
                        try {
                                b->on_message(nlohmann::json::parse(msg));
                        } catch (...) {
                                printf("%s: unparseable (%zu bytes)\n", b->name_.c_str(),
                                       msg.size());
                        }
                }
                break;

        case LWS_CALLBACK_CLIENT_WRITEABLE:
                b->flush();
                break;

        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
                b->wsi_         = nullptr;
                b->established_ = false;
                printf("%s: connection error\n", b->name_.c_str());
                break;

        case LWS_CALLBACK_CLIENT_CLOSED:
        case LWS_CALLBACK_CLOSED:
                b->wsi_         = nullptr;
                b->established_ = false;
                printf("%s: connection closed\n", b->name_.c_str());
                break;

        default:
                break;
        }
        return 0;
}

static const char *
street_name(int stage)
{
        static const char *names[] = { "preflop", "flop", "turn", "river", "over" };
        return names[stage < 0 || stage > 4 ? 4 : stage];
}

void
Bot::on_message(const nlohmann::json &j)
{
        std::string type = jval<std::string>(j, "type", "");

        if (type == "welcome") {
                seat_ = jval<int>(j, "seat", -1);
                hand_epoch_ = 0;
                cards_epoch_ = -1;
                printf("%s: joined as seat %d\n", name_.c_str(), seat_);
        } else if (type == "state") {
                if (j.is_object()) state_ = j;
        } else if (type == "action" || type == "stage" || type == "hand_over" ||
                   type == "tournament_start" || type == "level" ||
                   type == "player_out" || type == "tournament_over") {
                if (j.contains("state") && j["state"].is_object()) state_ = j["state"];
                if (type == "hand_over") {
                        hand_epoch_++;
                        printf("   %s: %s\n", name_.c_str(),
                               jval<std::string>(j, "result", "").c_str());
                } else if (type == "action") {
                        printf("   %s: P%d %s\n", name_.c_str(), jval<int>(j, "seat", -1),
                               jval<std::string>(j, "action", "?").c_str());
                } else if (type == "player_out") {
                        printf("   %s: == P%d eliminated (%s) ==\n", name_.c_str(),
                               jval<int>(j, "seat", -1),
                               jval<std::string>(j, "reason", "out").c_str());
                } else if (type == "tournament_over") {
                        printf("   %s: == Tournament Over! ==\n", name_.c_str());
                }
        } else if (type == "your_turn") {
                turn_pending_ = true;
                maybe_decide();
        } else if (type == "reply" && jval<std::string>(j, "what", "") == "my_cards") {
                const nlohmann::json &data = jval<nlohmann::json>(j, "data", nlohmann::json::object());
                if (data.is_object() && data.contains("cards") && data["cards"].is_array() &&
                    data["cards"].size() >= 2 && data["cards"][0].is_string() &&
                    data["cards"][1].is_string()) {
                        cards_[0] = data["cards"][0].get<std::string>();
                        cards_[1] = data["cards"][1].get<std::string>();
                        cards_epoch_ = hand_epoch_;
                        maybe_decide();
                } else {
                        turn_pending_ = false; // hand effectively over
                }
        } else if (type == "error") {
                std::string code = jval<std::string>(j, "code", "");
                printf("   %s: error %s\n", name_.c_str(), code.c_str());
                turn_pending_ = false;
                if (code == "bet_too_small" || code == "illegal_action")
                        send_action("check_or_fold"); // always legal fallback
        }
}

void
Bot::maybe_decide()
{
        if (!turn_pending_) return;
        if (state_.empty()) return; // wait for the first state broadcast
        if (cards_epoch_ != hand_epoch_) {
                send_json({ { "type", "query" }, { "id", 100 }, { "what", "my_cards" } });
                return;
        }
        turn_pending_ = false;
        decide_and_send();
}

void
Bot::decide_and_send()
{
        Game g;
        g.my_seat = seat_;
        if (!parse_game(state_, g) || g.my_seat < 0) return;
        g.hole[0] = Card(cards_[0]);
        g.hole[1] = Card(cards_[1]);

        Decision d = decide(g);
        const Player &me = g.players[g.my_seat];
        int to_call = g.current_bet - me.street_bet;
        if (to_call < 0) to_call = 0;

        if (d.act[0] == 'b') { // bet/raise
                int cap = me.stack + me.street_bet;
                int target = d.target;
                if (target > cap) target = cap;
                int min_t = (g.current_bet == 0) ? g.min_raise
                                                 : g.current_bet + g.min_raise;
                if (target < min_t) target = min_t;
                if (target > cap) target = cap;
                int inc = target - g.current_bet;
                if (inc <= 0) {
                        send_action("check_or_fold"); // safety net
                } else {
                        send_action("bet", inc);
                }
        } else {
                send_action(d.act);
        }
        printf("   %s: P%d %s -> %s (to_call %d, pot %d)\n",
               name_.c_str(), seat_, street_name(g.stage), d.act, to_call, g.pot);
}

void
Bot::pump(double t)
{
        if (!wsi_) {
                if (t - last_connect_try_ >= 1.0) {
                        last_connect_try_ = t;
                        connect();
                }
        }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

static int
run_selftest()
{
        int fails = 0;
        init_ranges();
        build_allin_table();
#define CHECK(cond, msg)                                                       \
        do {                                                                   \
                if (cond) {                                                    \
                        printf("  PASS: %s\n", msg);                           \
                } else {                                                       \
                        printf("  FAIL: %s\n", msg);                           \
                        fails++;                                               \
                }                                                              \
        } while (0)

        printf("== selftest ==\n");

        // hand class indexing
        CHECK(hand_index(12, 11, true) == 13, "AKs index 13");
        CHECK(hand_index(12, 10, true) == 14, "AQs index 14");
        CHECK(hand_index(11, 10, true) == 25, "KQs index 25");
        CHECK(hand_index(12, 11, false) == 91, "AKo index 91");
        CHECK(hand_index(1, 0, true) == 90, "32s index 90");
        CHECK(hand_index(1, 0, false) == 168, "32o index 168");

        // range membership
        Card as("As"), kd("Kd"), ah("Ah"), kc("Kc");
        Card jh("Jh"), td("Td");
        Card th("Th"), sd("8h");
        CHECK(in_range(R_BTN, as, kd), "AKs in BTN open");
        CHECK(!in_range(R_BTN, jh, Card("2c")), "J2o not in BTN open");
        CHECK(in_range(R_EP, as, kd), "AKs in EP open");
        CHECK(!in_range(R_EP, jh, td), "JTo not in EP open");
        CHECK(in_range(R_EP, th, sd), "T8s in EP open");
        CHECK(in_range(R_BTN, ah, kc), "AKo in BTN open");
        CHECK(in_range(STACKOFF, as, kd), "AKs in STACKOFF");

        // equities
        Card holeAA[2] = { Card("As"), Card("Ad") };
        double eqAA = equity_heads_up(holeAA, nullptr, 0, titan_rng());
        printf("  eq(AA) = %.3f\n", eqAA);
        CHECK(eqAA > 0.82 && eqAA < 0.88, "AA equity ~0.85");
        Card holeAKs[2] = { as, kd };
        double eqAKs = equity_heads_up(holeAKs, nullptr, 0, titan_rng());
        printf("  eq(AKs) = %.3f\n", eqAKs);
        CHECK(eqAKs > 0.63 && eqAKs < 0.71, "AKs equity ~0.67");

        // exact river equity: nut flush vs random = 1.0
        Card holeNuts[2] = { as, Card("Ks") };
        Card boardNuts[5] = { Card("Qs"), Card("Js"), Card("Ts"), Card("2c"), Card("3d") };
        double eqNuts = equity_heads_up(holeNuts, boardNuts, 5, titan_rng());
        printf("  eq(nut flush river) = %.3f\n", eqNuts);
        CHECK(eqNuts == 1.0, "nut flush on river = 1.0");

        // all-in ordering: AA (class 12, pairs are indexed by rank) is strongest
        CHECK(g_ai.eq[12] >= 0.80, "AA in all-in table");
        int best = 0;
        for (int i = 1; i < 169; i++)
                if (g_ai.eq[i] > g_ai.eq[best]) best = i;
        CHECK(best == 12, "AA is the strongest all-in hand");

        printf("== selftest %s (%d failure(s)) ==\n", fails ? "FAILED" : "PASSED", fails);
#undef CHECK
        return fails ? 1 : 0;
}

int
main(int argc, char **argv)
{
        setvbuf(stdout, nullptr, _IOLBF, 0);
        int port = 9000;
        std::string host = "127.0.0.1", token, name;
        bool selftest = false;

        for (int i = 1; i < argc; i++) {
                if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
                        port = atoi(argv[++i]);
                } else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
                        host = argv[++i];
                } else if (strcmp(argv[i], "--token") == 0 && i + 1 < argc) {
                        token = argv[++i];
                } else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
                        name = argv[++i];
                } else if (strcmp(argv[i], "--selftest") == 0) {
                        selftest = true;
                } else {
                        fprintf(stderr,
                                "usage: %s [--port N] [--host IP] [--token SECRET] [--name NAME] [--selftest]\n",
                                argv[0]);
                        return 1;
                }
        }

        if (selftest) return run_selftest();
        init_ranges();
        build_allin_table();

        lws_context *ctx = bot_context();
        if (!ctx) {
                fprintf(stderr, "failed to create lws context\n");
                return 1;
        }

        Bot bot(ctx, host.c_str(), port, token.c_str(), name.c_str());
        printf("%s: connecting to %s:%d\n", bot.name().c_str(), host.c_str(), port);

        std::atomic<bool> running{ true };
        std::thread ticker([&] {
                while (running.load()) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(16));
                        lws_cancel_service(ctx);
                }
        });

        for (;;) {
                lws_service(ctx, 20);
                bot.pump(Bot::now());
        }
}
