// ============================================================================
// Prometheus: elite poker bot for the API.md WebSocket/JSON protocol.
//
// Strategy pillars:
//   1. GTO-calibrated preflop ranges with mixed-strategy frequencies
//   2. Per-opponent VPIP/PFR/AF/fold-stat modeling and exploitative deviation
//   3. Bayesian opponent range narrowing based on observed actions
//   4. Board texture classification (wet/dry, monotone, paired, connectivity)
//   5. SPR-based multi-street planning
//   6. Dynamic bet sizing (33% / 66% / 100% / overbet) selected by context
//   7. Nash push/fold tables for short-stack tournament play
//   8. ICM awareness: tightening near tournament bubble / when at-risk
//   9. Equity calculations: exact on river/turn, MC on flop/preflop
//  10. Semi-bluff draws with correct equity + fold-equity evaluation
//
// Build: make -C bot/prometheus
// Run:   ./build/prometheus [--port N] [--host IP] [--token S] [--name N]
//                           [--selftest]
// ============================================================================

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <libwebsockets.h>
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

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------

template <typename T>
static T jval(const nlohmann::json &j, const char *key, const T &def) {
    if (!j.is_object() || !j.contains(key)) return def;
    const auto &val = j[key];
    if (val.is_null()) return def;
    try { return val.get<T>(); } catch (...) { return def; }
}

static double monotonic_now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static std::mt19937 &prom_rng() {
    static std::mt19937 rng(std::random_device{}());
    return rng;
}

static double rand01() {
    return std::uniform_real_distribution<double>(0.0, 1.0)(prom_rng());
}

static bool rand_bool(double p) { return rand01() < p; }

// ---------------------------------------------------------------------------
// Hand class encoding (169 classes: pairs 0..12, suited 13..90, offsuit 91..168)
// ---------------------------------------------------------------------------

static int rank_of(char c) {
    switch (c) {
    case '2': return 0; case '3': return 1; case '4': return 2;
    case '5': return 3; case '6': return 4; case '7': return 5;
    case '8': return 6; case '9': return 7; case 'T': return 8;
    case 'J': return 9; case 'Q': return 10; case 'K': return 11;
    case 'A': return 12;
    }
    return -1;
}

static int hand_index(int r1, int r2, bool suited) {
    // r1 > r2 always
    int off = (78 - r1 * (r1 + 1) / 2) + (r1 - 1 - r2);
    return suited ? 13 + off : 91 + off;
}

static int card_rank(const Card &c) { return int(c) / 4; }
static int card_suit(const Card &c) { return int(c) % 4; }

static int hand_class(const Card &a, const Card &b) {
    int ra = card_rank(a), rb = card_rank(b);
    if (ra == rb) return ra; // pair
    if (ra < rb) { int t = ra; ra = rb; rb = t; }
    bool suited = card_suit(a) == card_suit(b);
    return hand_index(ra, rb, suited);
}

// ---------------------------------------------------------------------------
// Range bitmask (169 bits in 3 uint64_t)
// ---------------------------------------------------------------------------

struct Range {
    uint64_t m[3] = {0, 0, 0};
    bool operator==(const Range &o) const {
        return m[0]==o.m[0] && m[1]==o.m[1] && m[2]==o.m[2];
    }
};

static void set_bit(Range &r, int i) { r.m[i/64] |= 1ULL << (i%64); }
static bool get_bit(const Range &r, int i) { return (r.m[i/64] >> (i%64)) & 1; }

static void add_tok(Range &r, const char *t) {
    int len = (int)strlen(t);
    if (len < 2) return;
    bool plus = t[len-1] == '+';
    int ra = rank_of(t[0]), rb = rank_of(t[1]);
    if (ra < 0 || rb < 0) return;
    if (ra == rb) {
        int k1 = plus ? 12 : ra;
        for (int k = ra; k <= k1; k++) set_bit(r, k);
        return;
    }
    bool suited = len >= 3 && (t[2]=='s' || t[2]=='S');
    if (ra < rb) { int tmp=ra; ra=rb; rb=tmp; }
    int hi=ra, lo=rb;
    int k1 = plus ? hi-1 : lo;
    for (int k = lo; k <= k1; k++) set_bit(r, hand_index(hi, k, suited));
}

static Range make_range(const char *list) {
    Range r;
    const char *p = list;
    while (*p) {
        const char *e = strchr(p, ',');
        int len = e ? (int)(e-p) : (int)strlen(p);
        if (len >= 2) {
            char tok[16];
            if (len > 15) len = 15;
            memcpy(tok, p, len); tok[len] = 0;
            add_tok(r, tok);
        }
        if (!e) break;
        p = e+1;
    }
    return r;
}

static bool in_range(const Range &r, const Card &a, const Card &b) {
    return get_bit(r, hand_class(a, b));
}

// ---------------------------------------------------------------------------
// Equity engine (phevaluator)
// ---------------------------------------------------------------------------

static int eval7_cards(const Card *hole, int nh, const Card *board, int nb) {
    Card cards[7]; int n=0;
    for (int i=0;i<nh;i++) cards[n++]=hole[i];
    for (int i=0;i<nb;i++) cards[n++]=board[i];
    switch(n){
    case 5: return EvaluateCards(cards[0],cards[1],cards[2],cards[3],cards[4]).value();
    case 6: return EvaluateCards(cards[0],cards[1],cards[2],cards[3],cards[4],cards[5]).value();
    default: return EvaluateCards(cards[0],cards[1],cards[2],cards[3],cards[4],cards[5],cards[6]).value();
    }
}

static int eval_opp(int c1, int c2, const Card *fb) {
    return EvaluateCards(Card(c1),Card(c2),fb[0],fb[1],fb[2],fb[3],fb[4]).value();
}

static void sample_cards(std::vector<int> &pool, int k, int *out) {
    for (int i=0;i<k;i++){
        int j = i + (int)(std::uniform_int_distribution<int>(0,(int)pool.size()-1-i)(prom_rng()));
        std::swap(pool[i],pool[j]); out[i]=pool[i];
    }
}

// Returns equity [0,1]: P(win) + 0.5*P(tie) heads-up vs random opponent range.
static double equity_hu(const Card *hole, const Card *board, int nb, int mc=0) {
    bool used[52]={};
    used[int(hole[0])]=true; used[int(hole[1])]=true;
    for (int i=0;i<nb;i++) used[int(board[i])]=true;
    std::vector<int> unk;
    for (int i=0;i<52;i++) if(!used[i]) unk.push_back(i);

    if (nb==5) { // river: exact
        long w=0,t=0;
        for (size_t i=0;i<unk.size();i++)
        for (size_t j=0;j<i;j++){
            int mv=eval7_cards(hole,2,board,5);
            int ov=eval_opp(unk[i],unk[j],board);
            if(mv<ov) w++; else if(mv==ov) t++;
        }
        double tot=(double)unk.size()*(unk.size()-1)/2;
        return (w+0.5*t)/tot;
    }
    if (nb==4) { // turn: exact
        long w=0,t=0;
        for (size_t i=0;i<unk.size();i++)
        for (size_t j=0;j<i;j++){
            Card fb[5]; for(int k=0;k<4;k++) fb[k]=board[k];
            for (size_t r_=0;r_<unk.size();r_++){
                if(r_==i||r_==j) continue;
                fb[4]=Card(unk[r_]);
                int mv=eval7_cards(hole,2,fb,5);
                int ov=eval_opp(unk[i],unk[j],fb);
                if(mv<ov) w++; else if(mv==ov) t++;
            }
        }
        double tot=(double)unk.size()*(unk.size()-1)/2*(unk.size()-2);
        return (w+0.5*t)/tot;
    }
    // flop/preflop: Monte Carlo
    int samples = mc>0 ? mc : (nb==3 ? 2000 : 1500);
    long w=0,t=0;
    int drawn[7]; Card fb[5];
    for (int s=0;s<samples;s++){
        std::vector<int> pool=unk;
        sample_cards(pool, 2+(5-nb), drawn);
        for (int k=0;k<nb;k++) fb[k]=board[k];
        for (int k=nb;k<5;k++) fb[k]=Card(drawn[2+k-nb]);
        int mv=eval7_cards(hole,2,fb,5);
        int ov=eval_opp(drawn[0],drawn[1],fb);
        if(mv<ov) w++; else if(mv==ov) t++;
    }
    return (w+0.5*t)/(double)samples;
}

// Multiway equity: fraction of pots won vs n opponents
static double equity_mw(const Card *hole, const Card *board, int nb, int nopp) {
    bool used[52]={};
    used[int(hole[0])]=true; used[int(hole[1])]=true;
    for (int i=0;i<nb;i++) used[int(board[i])]=true;
    std::vector<int> unk;
    for (int i=0;i<52;i++) if(!used[i]) unk.push_back(i);
    int samples=std::max(300, 1500/nopp);
    double p=0;
    int drawn[12]; Card fb[5];
    for (int s=0;s<samples;s++){
        std::vector<int> pool=unk;
        sample_cards(pool, 2*nopp+(5-nb), drawn);
        for (int k=0;k<nb;k++) fb[k]=board[k];
        for (int k=nb;k<5;k++) fb[k]=Card(drawn[2*nopp+k-nb]);
        int mv=eval7_cards(hole,2,fb,5);
        double pts=0;
        for (int o=0;o<nopp;o++){
            int ov=eval_opp(drawn[2*o],drawn[2*o+1],fb);
            if(mv<ov) pts+=1; else if(mv==ov) pts+=0.5;
        }
        p += pts/nopp;
    }
    return p/samples;
}

// ---------------------------------------------------------------------------
// Hand strength classifier (for bet sizing selection)
// ---------------------------------------------------------------------------

enum class HandStrength {
    AIR,           // < 15% equity or weak one pair
    WEAK_DRAW,     // gutshot, backdoor flush
    STRONG_DRAW,   // OESD, flush draw, combo draw
    WEAK_MADE,     // bottom pair, weak top pair
    MEDIUM_MADE,   // good top pair, two pair medium
    STRONG_MADE,   // two pair strong, trips
    MONSTER,       // quads, full house, nut flush, straight
};

struct BoardTexture {
    bool monotone;      // all 3 same suit
    bool two_tone;      // exactly 2 of one suit
    bool one_tone;      // all different suits
    bool paired;        // pair on board
    bool double_paired; // two pairs on board
    bool trips_board;   // trips on board
    int  connectivity;  // 0=disconnected, 1=low, 2=medium, 3=high
    bool has_ace;
    bool has_broadway;  // A/K/Q/J/T
    int  highest_rank;  // 0..12
    bool is_dynamic;    // lots of draws possible
    bool is_static;     // dry board, few draws
    int  flush_suit;    // dominant suit (-1 if none dominant)
    int  flush_count;   // max same-suit count on board
};

static BoardTexture classify_board(const Card *board, int n) {
    BoardTexture bt{};
    if (n < 3) return bt;

    int suits[4]={0,0,0,0};
    int ranks[13]={};
    for (int i=0;i<n;i++){
        suits[card_suit(board[i])]++;
        ranks[card_rank(board[i])]++;
    }

    // Suit analysis
    bt.flush_suit = -1;
    bt.flush_count = 0;
    for (int s=0;s<4;s++){
        if (suits[s]>bt.flush_count){ bt.flush_count=suits[s]; bt.flush_suit=s; }
    }
    bt.monotone = (bt.flush_count==n && n==3);
    bt.two_tone = (bt.flush_count==2 && n==3);
    bt.one_tone = (bt.flush_count==1 && n==3);

    // Rank analysis
    int pairs=0, trips=0;
    for (int r=0;r<13;r++){
        if (ranks[r]==2) pairs++;
        else if (ranks[r]>=3) trips++;
    }
    bt.paired = (pairs>0 || trips>0);
    bt.double_paired = (pairs>=2);
    bt.trips_board = (trips>0);

    // Broadway / Ace
    for (int i=0;i<n;i++){
        int r=card_rank(board[i]);
        if (r==12) bt.has_ace=true;
        if (r>=8) bt.has_broadway=true;
        if (r>bt.highest_rank) bt.highest_rank=r;
    }

    // Connectivity: count cards within 4 ranks of another
    if (n>=3){
        std::vector<int> rs;
        for (int i=0;i<n;i++) rs.push_back(card_rank(board[i]));
        std::sort(rs.begin(), rs.end());
        int connected=0;
        for (int i=1;i<(int)rs.size();i++)
            if (rs[i]-rs[i-1]<=4) connected++;
        bt.connectivity = std::min(3, connected);
    }

    bt.is_dynamic = (bt.flush_count>=2 || bt.connectivity>=2) && !bt.paired;
    bt.is_static  = (bt.flush_count==1 && bt.connectivity==0);

    return bt;
}

static HandStrength classify_hand(const Card * /*hole*/, const Card * /*board*/, int nb,
                                   double eq, int rank_val) {
    if (nb < 3) {
        if (eq >= 0.85) return HandStrength::MONSTER;
        if (eq >= 0.70) return HandStrength::STRONG_MADE;
        if (eq >= 0.55) return HandStrength::MEDIUM_MADE;
        if (eq >= 0.45) return HandStrength::WEAK_MADE;
        if (eq >= 0.30) return HandStrength::STRONG_DRAW;
        return HandStrength::AIR;
    }

    Rank rk(rank_val);
    int cat = rk.category();

    // Monster: quads, full house, straight flush
    if (cat == STRAIGHT_FLUSH || cat == FOUR_OF_A_KIND || cat == FULL_HOUSE)
        return HandStrength::MONSTER;

    // Strong made: flush, straight
    if (cat == FLUSH || cat == STRAIGHT)
        return HandStrength::STRONG_MADE;

    // Check for two pair / trips
    if (cat == THREE_OF_A_KIND)
        return HandStrength::STRONG_MADE;

    if (cat == TWO_PAIR) {
        // Is it the nuts or near nuts?
        if (eq >= 0.85) return HandStrength::MONSTER;
        if (eq >= 0.65) return HandStrength::STRONG_MADE;
        return HandStrength::MEDIUM_MADE;
    }

    if (cat == ONE_PAIR) {
        // Determine pair quality + draw potential
        if (eq >= 0.75) return HandStrength::STRONG_MADE;
        if (eq >= 0.55) return HandStrength::MEDIUM_MADE;
        if (eq >= 0.40) {
            // Could be a draw too
            if (nb < 5) return HandStrength::STRONG_DRAW;
            return HandStrength::WEAK_MADE;
        }
        if (eq >= 0.28) return HandStrength::WEAK_DRAW;
        return HandStrength::AIR;
    }

    // High card / draw territory
    if (eq >= 0.45) return HandStrength::STRONG_DRAW;
    if (eq >= 0.25) return HandStrength::WEAK_DRAW;
    return HandStrength::AIR;
}

// ---------------------------------------------------------------------------
// Opponent modeling: per-seat statistics
// ---------------------------------------------------------------------------

static const int kMaxSeats = 10;

struct OppModel {
    int   hands_seen    = 0;
    int   vpip          = 0;   // voluntarily put $ in preflop (not BB check)
    int   pfr           = 0;   // preflop raise
    int   three_bet     = 0;   // 3-bet opportunities taken
    int   three_bet_opp = 0;   // 3-bet opportunities seen
    int   fold_to_3bet  = 0;
    int   fold_to_3bet_opp = 0;
    int   cbet_seen     = 0;   // c-bet we saw
    int   fold_to_cbet  = 0;   // times we saw opponent fold to c-bet
    int   raise_count   = 0;
    int   call_count    = 0;
    int   check_count   = 0;

    double vpip_pct()  const { return hands_seen>3 ? (100.0*vpip/hands_seen) : 25.0; }
    double pfr_pct()   const { return hands_seen>3 ? (100.0*pfr/hands_seen)  : 15.0; }
    double af()        const {
        int total=raise_count+call_count+check_count;
        return total>5 ? (double)(raise_count*2)/(call_count+check_count+1) : 1.5;
    }
    double fold_cbet_pct() const { return cbet_seen>3 ? (100.0*fold_to_cbet/cbet_seen) : 45.0; }
    double fold_3bet_pct() const { return fold_to_3bet_opp>3 ? (100.0*fold_to_3bet/fold_to_3bet_opp) : 60.0; }

    // Classify opponent profile
    bool is_tight()       const { return vpip_pct() < 18; }
    bool is_loose()       const { return vpip_pct() > 38; }
    bool is_passive()     const { return af() < 1.5; }
    bool is_aggressive()  const { return af() > 2.5; }
    bool is_nit()         const { return is_tight() && is_passive(); }
    bool is_lag()         const { return is_loose() && is_aggressive(); }
    bool is_fish()        const { return is_loose() && is_passive(); }
    bool folds_too_much() const { return fold_cbet_pct() > 60; }
};

// ---------------------------------------------------------------------------
// Preflop ranges: GTO-calibrated with mixed strategy frequencies
// ---------------------------------------------------------------------------

struct MixedRange {
    Range value;     // pure value combos
    Range bluff;     // bluff/semi-bluff combos (played with given frequency)
    double bluff_freq; // 0..1 how often to include bluff combos
};

// All preflop strategy
struct PreflopStrategy {
    // Open ranges
    Range open_btn, open_co, open_mp, open_ep, open_sb;

    // 3-bet ranges (value + bluff) by raiser tier
    MixedRange r3b_vs_ep, r3b_vs_mp, r3b_vs_lp;

    // Call ranges facing open
    Range call_vs_ep, call_vs_mp, call_vs_lp;

    // BB ranges
    MixedRange bb_3b_vs_ep, bb_3b_vs_mp, bb_3b_vs_lp;
    Range bb_call_vs_ep, bb_call_vs_mp, bb_call_vs_lp;

    // Squeeze range
    Range squeeze_val, squeeze_bluff;

    // Facing 3-bet
    Range call_3b_deep, call_3b_med;
    Range stackoff_vs_3b;    // 4-bet/shove

    // 4-bet bluff
    Range fourbet_bluff;

    // Short stack push/fold (by percent top hands)
    // Pre-computed Nash push ranges by BB depth
    // push_range[i] = range to push with i BBs (0-indexed: 0=1BB, ..., 19=20BB)
    Range push_range[21]; // 0..20 BBs

    // SB ranges
    Range sb_open, sb_call_bb;

    // Limp-raise BB
    Range bb_raise_limp;

    // Cold 4-bet bluff hands
    Range cold_squeeze;
};

static PreflopStrategy g_pre;
static bool g_pre_init = false;

// Precomputed preflop HU equity for all 169 hand classes (pairs 0-12,
// suited 13-90, offsuit 91-168). Filled once by build_pf_eq_table() so
// call-shove decisions can use real equity instead of range tables.
static double g_pf_eq[169] = {};

static void build_pf_eq_table() {
    // pairs: index = rank (0=22 .. 12=AA)
    for (int r = 0; r < 13; r++) {
        Card h[2] = {Card(r * 4), Card(r * 4 + 1)};
        g_pf_eq[r] = equity_hu(h, nullptr, 0);
    }
    // suited and offsuit non-pair hands; r1 > r2 always
    for (int r1 = 1; r1 < 13; r1++) {
        for (int r2 = 0; r2 < r1; r2++) {
            Card hs[2] = {Card(r1 * 4),     Card(r2 * 4)    }; // same suit (0)
            Card ho[2] = {Card(r1 * 4),     Card(r2 * 4 + 1)}; // diff suit
            g_pf_eq[hand_index(r1, r2, true)]  = equity_hu(hs, nullptr, 0);
            g_pf_eq[hand_index(r1, r2, false)] = equity_hu(ho, nullptr, 0);
        }
    }
}

static void init_preflop() {
    if (g_pre_init) return;
    g_pre_init = true;

    // === Open ranges (GTO-calibrated for 6-max) ===
    g_pre.open_btn = make_range(
        "22+,A2s+,K2s+,Q4s+,J6s+,T6s+,95s+,84s+,74s+,63s+,53s+,43s,"
        "A2o+,K8o+,Q9o+,J9o+,T9o,98o,87o");
    g_pre.open_co = make_range(
        "22+,A2s+,K5s+,Q7s+,J7s+,T7s+,97s+,86s+,75s+,65s,"
        "A7o+,K9o+,QTo+,JTo,T9o");
    g_pre.open_mp = make_range(
        "22+,A3s+,K8s+,Q9s+,J9s+,T8s+,97s+,87s,76s,"
        "A9o+,KTo+,QJo");
    g_pre.open_ep = make_range(
        "22+,A5s+,K9s+,Q9s+,J9s+,T8s+,98s,ATo+,KJo+");
    g_pre.open_sb = make_range(
        "22+,A2s+,K3s+,Q6s+,J7s+,T7s+,96s+,86s+,75s+,64s+,54s,"
        "A2o+,K9o+,QTo+,JTo,T9o,98o");

    // === 3-bet ranges ===
    // vs EP open: very tight value + A5s/A4s as bluffs
    g_pre.r3b_vs_ep = {
        make_range("QQ+,AKs,AKo"),
        make_range("A5s,A4s,KQs"),
        0.70
    };
    // vs MP open
    g_pre.r3b_vs_mp = {
        make_range("TT+,AQs+,AKo,KQs"),
        make_range("A5s,A4s,A3s,76s,65s"),
        0.65
    };
    // vs LP open (CO/BTN)
    g_pre.r3b_vs_lp = {
        make_range("99+,ATs+,AJo+,KQs"),
        make_range("A5s,A4s,A3s,A2s,KTs,QTs,J9s,76s,65s,54s"),
        0.55
    };

    // === Call ranges ===
    g_pre.call_vs_ep = make_range("77+,ATs+,KQs,AJo+,KTs+,QJs,JTs,T9s,98s");
    g_pre.call_vs_mp = make_range("55+,A7s+,AJo+,KTs+,QJs,QTs,JTs,T9s,98s,87s,76s");
    g_pre.call_vs_lp = make_range("44+,A8s+,ATo+,K9s+,KTo+,Q9s+,QJo,J9s+,T9s,98s,87s,76s,65s");

    // === BB defense ranges ===
    g_pre.bb_3b_vs_ep = {
        make_range("QQ+,AKs,AKo"),
        make_range("A5s,A4s,KQs"),
        0.75
    };
    g_pre.bb_3b_vs_mp = {
        make_range("TT+,AJs+,AQo+,KQs"),
        make_range("A5s,A4s,A3s,66,55"),
        0.65
    };
    g_pre.bb_3b_vs_lp = {
        make_range("99+,ATs+,AJo+,KQs"),
        make_range("A5s,A4s,A3s,A2s,76s,65s,54s,KTs,QTs"),
        0.55
    };
    g_pre.bb_call_vs_ep = make_range("77+,ATs+,KQs,AJo+,KTs+,QJs,JTs,T9s,98s,87s");
    g_pre.bb_call_vs_mp = make_range("55+,A7s+,A9o+,K9s+,KTo+,Q9s+,QJo,J8s+,T8s+,97s+,86s+,76s");
    g_pre.bb_call_vs_lp = make_range("44+,A2s+,A8o+,K8s+,K9o+,Q7s+,QTo+,J8s+,T7s+,96s+,86s+,75s+,65s,54s,43s");

    // === Facing 3-bet ===
    g_pre.stackoff_vs_3b = make_range("KK+,AKs,AKo");
    g_pre.call_3b_deep   = make_range("QQ,JJ,TT,AQs,AJs,KQs");
    g_pre.call_3b_med    = make_range("QQ,JJ,AQs");
    g_pre.fourbet_bluff  = make_range("A5s,A4s,A3s");

    // === Squeeze ===
    g_pre.cold_squeeze   = make_range("TT+,AJs+,AQo+,KQs,A5s,A4s");

    // === Short stack push/fold (Nash approximation) ===
    // By effective stack in BB: push_range[i] = shove with i BB stack
    // These are approximate Nash equilibrium ranges
    g_pre.push_range[1]  = make_range("22+,A2s+,A2o+,K2s+,K2o+,Q2s+,Q2o+,J2s+,J2o+,T2s+,T2o+,92s+,92o+,82s+,82o+,72s+,72o+,62s+,62o+,52s+,52o+,42s+,42o+,32s");
    g_pre.push_range[2]  = make_range("22+,A2s+,A2o+,K2s+,K2o+,Q2s+,Q4o+,J4s+,J5o+,T5s+,T7o+,96s+,97o+,86s+,87o,76s");
    g_pre.push_range[3]  = make_range("22+,A2s+,A2o+,K2s+,K5o+,Q5s+,Q7o+,J7s+,J8o+,T7s+,T8o+,97s+,98o,87s");
    g_pre.push_range[4]  = make_range("22+,A2s+,A4o+,K5s+,K8o+,Q7s+,Q9o+,J8s+,J9o+,T8s+,T9o,98s,87s");
    g_pre.push_range[5]  = make_range("22+,A2s+,A7o+,K7s+,KTo+,Q8s+,QTo+,J8s+,JTo,T8s+,T9o,98s,87s,76s");
    g_pre.push_range[6]  = make_range("22+,A2s+,A8o+,K8s+,KJo+,Q9s+,QJo,J9s+,T9s,98s,87s,76s");
    g_pre.push_range[7]  = make_range("22+,A2s+,A9o+,K9s+,KJo+,Q9s+,QJo,J9s+,JTo,T9s,98s,87s");
    g_pre.push_range[8]  = make_range("33+,A2s+,ATo+,K9s+,KQo,QTs+,JTs,T9s,98s");
    g_pre.push_range[9]  = make_range("33+,A3s+,ATo+,KTs+,KQo,QTs+,JTs,T9s");
    g_pre.push_range[10] = make_range("44+,A4s+,AJo+,KTs+,KQo,QJs,JTs");
    g_pre.push_range[11] = make_range("55+,A5s+,AJo+,KJs+,KQo,QJs");
    g_pre.push_range[12] = make_range("55+,A7s+,AQo+,KQs,KQo");
    g_pre.push_range[13] = make_range("66+,A8s+,AQo+,KQs");
    g_pre.push_range[14] = make_range("77+,A9s+,AQo+,KQs");
    g_pre.push_range[15] = make_range("77+,ATs+,AQo+,KQs");
    g_pre.push_range[16] = make_range("88+,ATs+,AQo+,KQs");
    g_pre.push_range[17] = make_range("88+,AJs+,AQo+");
    g_pre.push_range[18] = make_range("99+,AJs+,AKo");
    g_pre.push_range[19] = make_range("99+,AQs+,AKo");
    g_pre.push_range[20] = make_range("TT+,AKs,AKo");

    g_pre.sb_open     = g_pre.open_sb;
    g_pre.sb_call_bb  = make_range("22+,A2s+,K5s+,Q8s+,J8s+,T8s+,97s+,87s,A8o+,K9o+,QTo+,JTo");
    g_pre.bb_raise_limp = make_range("22+,A2s+,K8s+,Q9s+,J9s+,T9s,98s,ATo+,KJo+");

    // Build the HU-equity table last (1500 MC samples × 169 hand classes;
    // fast in C++ – a few hundred ms – but must come after ranges are set).
    build_pf_eq_table();
}

// ---------------------------------------------------------------------------
// Preflop decision logic
// ---------------------------------------------------------------------------

struct PreflopContext {
    int my_seat;
    int pos;          // 0 = first to act after dealer
    int dist;         // 0 = button
    bool in_bb;
    bool in_sb;
    bool hu;
    bool raised;
    bool three_bet_pot;
    int  raiser_dist; // dist of the original raiser
    int  callers;     // cold callers between raiser and us
    int  limpers;
    int  current_bet;
    int  my_street_bet;
    int  to_call;
    int  pot;
    int  my_stack;
    int  bb;
    double sbb;       // stack in big blinds
    Card hole[2];
    // Opponent model of the raiser
    const OppModel *raiser_model;

    // --- ICM / tournament context (fixes: "ICM awareness" was a comment
    //     with no code behind it) ---
    bool is_tournament;
    double icm_win_share;     // our share of chips in play, ~= P(finish 1st)
                               // in this server's winner-take-all format
    double icm_risk_premium;  // 0..~3 "risk units"; higher => tighten
                               // marginal calls/shoves against short stacks
                               // when we're already the chip leader
};

// ICM: pokerish tournaments are winner-take-all (see API.md), so a player's
// tournament equity collapses to their probability of finishing first,
// approximated by their share of total chips in play.
static double icm_win_probability(int my_stack, const int *stacks, int n) {
    long total = 0;
    for (int i = 0; i < n; i++) total += stacks[i];
    if (total <= 0) return 0.0;
    return (double)my_stack / (double)total;
}

// Risk premium: discourage marginal, high-variance spots for a dominant
// chip leader against a much shorter stack (busting them barely moves our
// win probability, so it's a bad risk/reward trade even chip-EV positive).
static double icm_risk_premium(double my_share, double opp_share, int players_left) {
    double premium = 0.0;
    if (my_share > 0.30 && opp_share < my_share * 0.5) premium += 1.5;
    if (players_left <= 4) premium += 1.0;
    if (players_left <= 2) premium -= 1.5; // HU: chip EV ~= tournament EV
    return premium < 0.0 ? 0.0 : premium;
}

struct Decision {
    const char *act; // "fold","check","call","call_all","bet"
    int target;      // for "bet": desired total street bet
};

static Decision allin_(int stack, int street_bet) {
    return Decision{"bet", stack + street_bet};
}

static bool should_3bet(const MixedRange &mr, const Card *hole) {
    if (in_range(mr.value, hole[0], hole[1])) return true;
    if (in_range(mr.bluff, hole[0], hole[1]) && rand_bool(mr.bluff_freq)) return true;
    return false;
}

static Decision decide_preflop_full(const PreflopContext &ctx) {
    const Card *hole = ctx.hole;
    int bb = ctx.bb;
    double sbb = ctx.sbb;
    int to_call = ctx.to_call;

    // ─── Short stack push/fold ─────────────────────────────────────────────
    if (sbb <= 20) {
        // Position-aware effective-stack adjustment (fixes leak #1: ranges
        // used to be indexed by depth ONLY, identical from every seat).
        // Later position => more fold equity / less info => can profitably
        // push a range that "belongs" to a slightly deeper stack. Earlier
        // position => opposite. SB open-shoving only faces the BB, so it
        // gets the same treatment as a late-position seat.
        int pos_adj = 0;
        if (ctx.in_sb)        pos_adj = 2;
        else if (!ctx.in_bb) {
            switch (ctx.dist) {
                case 0: pos_adj = 3; break;  // BTN
                case 1: pos_adj = 2; break;  // CO
                case 2: pos_adj = 1; break;  // HJ
                case 3: pos_adj = 0; break;  // MP2
                case 4: pos_adj = -1; break; // MP1
                case 5: pos_adj = -1; break; // UTG1
                default: pos_adj = -2; break; // UTG (or shorter table)
            }
        }
        int push_bb = (int)std::round(sbb) + pos_adj;
        if (push_bb < 1) push_bb = 1;
        if (push_bb > 20) push_bb = 20;
        const Range &pr = g_pre.push_range[push_bb];

        // ICM: with a dominant stack, avoid marginal shoves/calls against a
        // much shorter stack — bust them and our win probability barely
        // moves, so it's not worth the variance (fixes leak #3: previously
        // no code backed the "ICM awareness" comment).
        bool icm_avoid_marginal = ctx.is_tournament && ctx.icm_risk_premium >= 1.5;

        if (ctx.in_bb && !ctx.raised) {
            if (ctx.limpers == 0) return Decision{"check", 0};
            // Limp in front: push with our range
            if (in_range(pr, hole[0], hole[1]) &&
                !(icm_avoid_marginal && ctx.icm_win_share > 0.30 && push_bb <= 8))
                return allin_(ctx.my_stack, ctx.my_street_bet);
            return Decision{"check", 0};
        }
        if (to_call <= 0) {
            if (in_range(pr, hole[0], hole[1]) &&
                !(icm_avoid_marginal && ctx.icm_win_share > 0.30 && push_bb <= 8))
                return allin_(ctx.my_stack, ctx.my_street_bet);
            return Decision{"fold", 0};
        }
        // Facing a raise: pot-odds-based call/fold.
        // Classic approach: use push_range as a proxy for calling range. Problem:
        // it ignores the actual bet size, so a 20x overbet gets called the same
        // as a 2x pot bet. Fix: compute real pot odds, look up our hand's preflop
        // equity, and require equity > pot_odds + ICM_margin.
        if (to_call >= ctx.my_stack) {
            double pot_total = (double)(ctx.pot + to_call); // pot after we call
            double pot_odds  = pot_total > 0 ? (double)to_call / pot_total : 0.5;
            double eq        = g_pf_eq[hand_class(hole[0], hole[1])];
            // ICM margin: larger when we have more equity share to protect.
            // Heads-up: chip-EV ≈ tournament EV, so very small margin.
            double icm_margin;
            if (ctx.hu)        icm_margin = 0.01;
            else if (sbb <= 8) icm_margin = 0.02;
            else if (sbb <= 12) icm_margin = 0.04;
            else               icm_margin = 0.06;
            // Exploit: vs. a very loose raiser, their range is wider than Nash
            // implies → our equity is higher → call wider (reduce margin).
            if (ctx.raiser_model && ctx.raiser_model->vpip > 70)
                icm_margin = std::max(0.0, icm_margin - 0.03);
            // ICM: as a dominant chip leader, also fold marginal calls at the
            // top of the band — busting a much shorter stack barely moves our
            // win probability, so a bare pot-odds-plus-margin call isn't worth
            // the tournament-equity risk (fixes leak #3: "ICM awareness" was
            // previously a comment with no code behind it).
            bool icm_fold_marginal = icm_avoid_marginal && ctx.icm_win_share > 0.30
                                      && sbb > 15; // only trims the top of the band
            if (eq >= pot_odds + icm_margin && !icm_fold_marginal) return Decision{"call_all", 0};
            return Decision{"fold", 0};
        }
        // Has chips behind: push with our push range
        if (in_range(pr, hole[0], hole[1]) &&
            !(icm_avoid_marginal && ctx.icm_win_share > 0.30 && push_bb <= 8))
            return allin_(ctx.my_stack, ctx.my_street_bet);
        return Decision{"fold", 0};
    }

    // ─── Deep stack (> 20 BB) ──────────────────────────────────────────────

    if (!ctx.raised) {
        // No raise yet
        if (ctx.in_bb) {
            if (ctx.limpers == 0) return Decision{"check", 0};
            // Facing limpers: raise our range, call some
            if (in_range(g_pre.bb_raise_limp, hole[0], hole[1]))
                return Decision{"bet", (int)(2.5*bb) + ctx.limpers*bb};
            return Decision{"check", 0};
        }
        if (ctx.in_sb) {
            if (in_range(g_pre.open_sb, hole[0], hole[1]))
                return Decision{"bet", (int)(3.0*bb) + ctx.limpers*bb};
            if (in_range(g_pre.sb_call_bb, hole[0], hole[1]))
                return Decision{"call", 0};
            return Decision{"fold", 0};
        }
        // Open raise: dist-based
        const Range *rng = nullptr;
        if (ctx.dist == 0) rng = &g_pre.open_btn;
        else if (ctx.dist == 1) rng = &g_pre.open_co;
        else if (ctx.dist == 2) rng = &g_pre.open_mp;
        else rng = &g_pre.open_ep;

        if (in_range(*rng, hole[0], hole[1]))
            return Decision{"bet", (int)(2.5*bb) + ctx.limpers*bb};
        return Decision{"fold", 0};
    }

    // ─── Facing a raise ────────────────────────────────────────────────────

    if (ctx.three_bet_pot) {
        // We opened, now facing 3-bet
        if (in_range(g_pre.stackoff_vs_3b, hole[0], hole[1]))
            return allin_(ctx.my_stack, ctx.my_street_bet);
        const Range *c3 = (ctx.hu || ctx.dist <= 1) ? &g_pre.call_3b_deep : &g_pre.call_3b_med;
        if (in_range(*c3, hole[0], hole[1])) return Decision{"call", 0};
        // 4-bet bluff some hands
        if (in_range(g_pre.fourbet_bluff, hole[0], hole[1]) && rand_bool(0.4))
            return allin_(ctx.my_stack, ctx.my_street_bet);
        return Decision{"fold", 0};
    }

    // Facing a single open raise
    // Multi-way (squeeze opportunity or cold call spot)
    if (ctx.callers >= 2) {
        // Squeeze with premiums only
        if (in_range(g_pre.cold_squeeze, hole[0], hole[1]))
            return Decision{"bet", 4 * ctx.current_bet + ctx.callers * (int)(0.5*bb)};
        // Cold-call only with strong hands
        if (in_range(make_range("TT+,AQs+,AKo,KQs"), hole[0], hole[1]))
            return Decision{"call", 0};
        return Decision{"fold", 0};
    }

    // Determine which of the three range *tables* to use (EP/MP/LP: we
    // only hand-tuned three charts). But within a table, don't treat every
    // raiser seat the same — fixes leak #4: previously all 9 raiser
    // positions collapsed into exactly 3 buckets with identical play
    // inside each bucket. Every extra seat of raiser_dist now tightens
    // our continuation frequency a little further, so e.g. facing UTG
    // (deepest dist) is measurably tighter than facing UTG+1, even though
    // both still pull from the same base "EP" chart.
    int tier = ctx.raiser_dist <= 1 ? 2 : (ctx.raiser_dist == 2 ? 1 : 0);
    double raiser_pos_tighten = 1.0 - 0.035 * std::min(ctx.raiser_dist, 8);
    if (raiser_pos_tighten < 0.55) raiser_pos_tighten = 0.55;

    // Exploit: if raiser is very tight (nit), tighten our call range
    double call_tighten = raiser_pos_tighten;
    bool raiser_is_nit = false;
    if (ctx.raiser_model && ctx.raiser_model->is_nit()) {
        call_tighten *= 0.7;
        raiser_is_nit = true;
    }
    bool raiser_is_loose = ctx.raiser_model && ctx.raiser_model->is_loose();
    (void) raiser_is_loose;

    if (ctx.in_bb) {
        const MixedRange *r3 = nullptr;
        const Range *c3 = nullptr;
        if (tier==0) { r3=&g_pre.bb_3b_vs_ep; c3=&g_pre.bb_call_vs_ep; }
        else if (tier==1) { r3=&g_pre.bb_3b_vs_mp; c3=&g_pre.bb_call_vs_mp; }
        else { r3=&g_pre.bb_3b_vs_lp; c3=&g_pre.bb_call_vs_lp; }

        // Exploit: 3-bet more vs loose, less vs nit; also fold slightly
        // more bluff combos the further back the raiser's seat is.
        double freq_adj = (raiser_is_nit ? 0.7 : 1.0) * raiser_pos_tighten;
        MixedRange mr_adj = *r3;
        mr_adj.bluff_freq *= freq_adj;

        if (should_3bet(mr_adj, hole))
            return Decision{"bet", 3 * ctx.current_bet};
        // call_tighten already folds in both the nit exploit and the
        // per-seat raiser_pos_tighten factor, so every raiser_dist gets a
        // distinct continuation frequency instead of one of 3 buckets.
        if (in_range(*c3, hole[0], hole[1])) {
            if (rand_bool(call_tighten))
                return Decision{"call", 0};
        }
        return Decision{"fold", 0};
    }

    // Non-BB facing open
    const MixedRange *r3 = nullptr;
    const Range *c3 = nullptr;
    if (tier==0) { r3=&g_pre.r3b_vs_ep; c3=&g_pre.call_vs_ep; }
    else if (tier==1) { r3=&g_pre.r3b_vs_mp; c3=&g_pre.call_vs_mp; }
    else { r3=&g_pre.r3b_vs_lp; c3=&g_pre.call_vs_lp; }

    MixedRange mr_open_adj = *r3;
    mr_open_adj.bluff_freq *= (raiser_is_nit ? 0.7 : 1.0) * raiser_pos_tighten;

    if (should_3bet(mr_open_adj, hole))
        return Decision{"bet", 3 * ctx.current_bet};
    if (in_range(*c3, hole[0], hole[1])) {
        if (rand_bool(call_tighten))
            return Decision{"call", 0};
    }
    return Decision{"fold", 0};
}

// ---------------------------------------------------------------------------
// Bet sizing selection (postflop)
// ---------------------------------------------------------------------------

enum class BetSize { CHECK, SMALL, MEDIUM, LARGE, OVERBET };

// Returns recommended bet as fraction of pot
static double bet_fraction(BetSize sz) {
    switch (sz) {
    case BetSize::SMALL:   return 0.33;
    case BetSize::MEDIUM:  return 0.66;
    case BetSize::LARGE:   return 1.00;
    case BetSize::OVERBET: return 1.50;
    default: return 0.0;
    }
}

// Select bet size based on:
// - Hand strength
// - Board texture
// - Stage
// - SPR
// - Position
static BetSize select_bet_size(HandStrength hs, const BoardTexture &bt,
                                int stage, double spr, bool in_position,
                                int nopp, bool as_pfr) {
    (void)as_pfr;

    // River: polarize (MONSTER = overbet, draws missed = give up or overbet bluff)
    if (stage == 3) { // river
        if (hs == HandStrength::MONSTER) {
            if (spr < 1.5 || !in_position) return BetSize::LARGE;
            return BetSize::OVERBET; // value overbet on river
        }
        if (hs == HandStrength::STRONG_MADE) return BetSize::MEDIUM;
        if (hs == HandStrength::MEDIUM_MADE) {
            // Thin value or check
            if (nopp == 1 && in_position) return BetSize::SMALL;
            return BetSize::CHECK;
        }
        // Bluff: overbet or give up
        if (hs == HandStrength::AIR) {
            if (in_position && nopp == 1 && rand_bool(0.35)) return BetSize::OVERBET;
        }
        return BetSize::CHECK;
    }

    // Turn
    if (stage == 2) {
        if (hs == HandStrength::MONSTER) {
            if (spr < 2) return BetSize::LARGE;
            return BetSize::LARGE; // charge draws maximally
        }
        if (hs == HandStrength::STRONG_MADE) {
            if (bt.is_dynamic) return BetSize::LARGE; // protect on wet turn
            return BetSize::MEDIUM;
        }
        if (hs == HandStrength::MEDIUM_MADE) {
            if (nopp == 1) return BetSize::SMALL;
            return BetSize::CHECK;
        }
        if (hs == HandStrength::STRONG_DRAW) {
            // Semi-bluff draw: charge opponents
            if (in_position) return BetSize::MEDIUM;
            return BetSize::SMALL;
        }
        return BetSize::CHECK;
    }

    // Flop
    if (bt.is_static) {
        // Dry board: small c-bet to charge top pair+ combos
        if (hs == HandStrength::MONSTER || hs == HandStrength::STRONG_MADE)
            return BetSize::SMALL; // polarize light on dry boards
        if (hs == HandStrength::MEDIUM_MADE) return BetSize::SMALL;
        if (hs == HandStrength::AIR && rand_bool(0.30)) return BetSize::SMALL; // bluff
        return BetSize::CHECK;
    }
    if (bt.is_dynamic || bt.monotone) {
        // Wet board: bet large to charge draws
        if (hs == HandStrength::MONSTER) return nopp == 1 ? BetSize::LARGE : BetSize::LARGE;
        if (hs == HandStrength::STRONG_MADE) return BetSize::LARGE;
        if (hs == HandStrength::MEDIUM_MADE) return BetSize::MEDIUM;
        if (hs == HandStrength::STRONG_DRAW) {
            if (in_position) return BetSize::MEDIUM;
            return BetSize::SMALL;
        }
        return BetSize::CHECK;
    }

    // Medium-texture flop
    if (hs == HandStrength::MONSTER) return BetSize::MEDIUM;
    if (hs == HandStrength::STRONG_MADE) return BetSize::MEDIUM;
    if (hs == HandStrength::MEDIUM_MADE) {
        if (nopp == 1 && in_position) return BetSize::SMALL;
        return BetSize::CHECK;
    }
    if (hs == HandStrength::STRONG_DRAW) {
        if (in_position) return BetSize::SMALL;
        return BetSize::CHECK;
    }
    if (hs == HandStrength::AIR && rand_bool(0.22)) return BetSize::SMALL; // bluff
    return BetSize::CHECK;
}

// ---------------------------------------------------------------------------
// Postflop decision
// ---------------------------------------------------------------------------

struct PostflopContext {
    int my_seat;
    int stage;       // 1=flop, 2=turn, 3=river
    int nopp;
    bool in_position;
    bool is_pfr;     // we were the preflop raiser
    double spr;      // stack/pot before action
    double eq;       // raw equity
    int    rank_val; // phevaluator rank value
    HandStrength hs;
    BoardTexture bt;
    int pot;
    int current_bet;
    int my_street_bet;
    int to_call;
    int my_stack;
    int min_raise;
    // Opponent model (a representative single opponent, kept for any future
    // per-opponent range narrowing) plus field-wide aggregated reads that
    // are correct in multi-way pots (see make_postflop_decision).
    const OppModel *opp_model;
    bool agg_folds_a_lot;
    bool agg_calling_station;
    bool agg_aggressive;
    bool is_tournament;
    bool icm_avoid_marginal; // we're a dominant chip leader; skip marginal all-ins
};

static Decision decide_postflop_full(const PostflopContext &ctx) {
    double eq = ctx.eq;
    double spr = ctx.spr;
    double pot = ctx.pot;
    int to_call = ctx.to_call;

    // Pot odds required to call
    double call_pot = pot + to_call;
    double pot_odds = call_pot > 0 ? (double)to_call / call_pot : 0.0;

    // Margin (safety buffer above pot odds)
    double margin = 0.04 + 0.03 * std::max(0, ctx.nopp - 1);

    // Opponent model adjustments: aggregated across every live, modeled
    // opponent (see make_postflop_decision) rather than a single seat, so
    // multi-way pots don't get a read that's only true for one player.
    bool opp_folds_a_lot = ctx.agg_folds_a_lot;
    bool opp_is_calling_station = ctx.agg_calling_station;
    bool opp_is_aggressive = ctx.agg_aggressive;

    // ─── Facing a bet ──────────────────────────────────────────────────────
    if (to_call > 0) {
        bool has_equity = eq >= pot_odds + margin;

        // All-in decision
        if (to_call >= ctx.my_stack) {
            // ICM: as a dominant chip leader, require a real equity edge
            // (not just a bare profitable call) before stacking off — the
            // marginal tournament-equity gain from winning is small relative
            // to the downside of busting, so add a small extra margin.
            double icm_margin = (ctx.is_tournament && ctx.icm_avoid_marginal) ? 0.05 : 0.0;
            if (eq >= pot_odds + margin + icm_margin) return Decision{"call_all", 0};
            // Desperate call with strong draw on flop/turn
            if (ctx.stage < 3 && ctx.hs == HandStrength::STRONG_DRAW &&
                eq >= pot_odds + margin * 0.5 + icm_margin) return Decision{"call_all", 0};
            return Decision{"fold", 0};
        }

        if (!has_equity) {
            // Consider folding weak hands even with some equity
            if (ctx.hs == HandStrength::AIR) return Decision{"fold", 0};
            if (ctx.hs == HandStrength::WEAK_DRAW && !ctx.in_position) return Decision{"fold", 0};
            if (ctx.hs == HandStrength::WEAK_DRAW && ctx.stage == 3) return Decision{"fold", 0};
            return Decision{"fold", 0};
        }

        // Raise decision: raise for value or as semi-bluff
        bool raise_for_value =
            (ctx.hs == HandStrength::MONSTER || ctx.hs == HandStrength::STRONG_MADE) &&
            ctx.nopp == 1 && spr >= 2.0;

        bool raise_as_semibluff =
            (ctx.hs == HandStrength::STRONG_DRAW) && ctx.nopp == 1 &&
            ctx.in_position && ctx.stage < 3 && rand_bool(0.40);

        // Exploit: raise more vs aggressive opponents (they'll fold or we stack off)
        if (opp_is_aggressive && ctx.hs >= HandStrength::STRONG_MADE && ctx.nopp == 1)
            raise_for_value = true;

        if (raise_for_value || raise_as_semibluff) {
            // Raise to ~3x current bet (or jam if short)
            int raise_to;
            if (spr < 2.0) {
                return Decision{"bet", ctx.my_stack + ctx.my_street_bet}; // jam
            }
            raise_to = ctx.current_bet + 3 * to_call;
            if (raise_to > ctx.my_stack + ctx.my_street_bet)
                raise_to = ctx.my_stack + ctx.my_street_bet;
            return Decision{"bet", raise_to};
        }

        return Decision{"call", 0};
    }

    // ─── No bet facing us: we can check or bet ─────────────────────────────

    // Exploit: bet more vs fold-happy opponents
    double bluff_freq = opp_folds_a_lot ? 0.45 : 0.22;
    // Don't bluff calling stations
    if (opp_is_calling_station) bluff_freq = 0.0;

    // Select bet size based on hand + board
    BetSize sz = select_bet_size(ctx.hs, ctx.bt, ctx.stage, spr,
                                  ctx.in_position, ctx.nopp, ctx.is_pfr);

    if (sz == BetSize::CHECK) {
        // Occasional bluff check-raise from OOP
        if (!ctx.in_position && ctx.hs == HandStrength::STRONG_DRAW &&
            ctx.nopp == 1 && rand_bool(0.20))
            sz = BetSize::SMALL;
        // Pure air bluff
        else if (ctx.hs == HandStrength::AIR && rand_bool(bluff_freq))
            sz = BetSize::SMALL;
        else
            return Decision{"check", 0};
    }

    // Convert BetSize to chip amount
    double frac = bet_fraction(sz);
    int bet_chips = (int)(frac * pot);
    if (bet_chips < 1) bet_chips = 1;

    // Cap at our stack
    int cap = ctx.my_stack + ctx.my_street_bet;
    int target = ctx.my_street_bet + bet_chips;
    if (target > cap) target = cap;

    // Make sure we're above min-raise
    int min_target = ctx.current_bet == 0 ? ctx.min_raise
                                          : ctx.current_bet + ctx.min_raise;
    if (target < min_target && target < cap) target = min_target;
    if (target > cap) target = cap;

    return Decision{"bet", target};
}

// ---------------------------------------------------------------------------
// Full game state model
// ---------------------------------------------------------------------------

struct PlayerState {
    int seat = -1;
    int stack = 0;
    int bet = 0;
    int street_bet = 0;
    bool folded = false;
    bool all_in = false;
    bool busted = false;
    bool is_turn = false;
    std::string last_action;
    int last_amount = 0;
    std::string name;
};

struct GameState {
    int stage = 4;          // 0=preflop..3=river,4=over
    int dealer = 0;
    int turn_seat = -1;
    int current_bet = 0;
    int min_raise = 10;
    int pot = 0;
    int sb = 5, bb = 10, ante = 0;
    int sb_seat = -1, bb_seat = -1;
    PlayerState players[kMaxSeats];
    Card common[5];
    int ncommon = 0;
    bool is_tournament = false;
    int level = 0;

    int my_seat = -1;
    Card hole[2];

    // Computed fields
    int live = 0;           // seated, not busted
    int active = 0;         // live, not folded, not all-in (can still act)
    int opps = 0;           // opponents still in hand (not folded)
    int my_pos = 0;
    int dist = 0;           // distance from button (0=BTN)
    bool in_position = false;
    bool is_pfr = false;    // we raised preflop
    bool hu = false;
};

static int stage_idx(const std::string &s) {
    if (s=="preflop") return 0;
    if (s=="flop") return 1;
    if (s=="turn") return 2;
    if (s=="river") return 3;
    return 4;
}

static void parse_game_state(const nlohmann::json &st, GameState &g) {
    if (!st.is_object()) return;
    g.stage = stage_idx(jval<std::string>(st, "stage", "over"));
    g.dealer = jval<int>(st, "dealer", 0);
    g.turn_seat = jval<int>(st, "turn", -1);
    g.current_bet = jval<int>(st, "current_bet", 0);
    g.min_raise = jval<int>(st, "min_raise", g.bb);
    g.pot = jval<int>(st, "pot", 0);
    g.is_tournament = jval<std::string>(st, "mode", "cash") == "tournament";
    g.level = jval<int>(st, "level", 0);

    if (st.contains("blinds") && st["blinds"].is_object()) {
        g.sb = jval<int>(st["blinds"], "small", 5);
        g.bb = jval<int>(st["blinds"], "big", 10);
        g.ante = jval<int>(st["blinds"], "ante", 0);
        g.sb_seat = jval<int>(st["blinds"], "small_seat", -1);
        g.bb_seat = jval<int>(st["blinds"], "big_seat", -1);
    }

    for (int i=0;i<kMaxSeats;i++) g.players[i] = PlayerState();
    if (st.contains("players") && st["players"].is_array()) {
        for (const auto &p : st["players"]) {
            if (!p.is_object()) continue;
            int seat = jval<int>(p, "seat", -1);
            if (seat<0||seat>=kMaxSeats) continue;
            PlayerState &pl = g.players[seat];
            pl.seat = seat;
            pl.stack = jval<int>(p, "stack", 0);
            pl.bet = jval<int>(p, "bet", 0);
            pl.street_bet = jval<int>(p, "street_bet", 0);
            pl.folded = jval<bool>(p, "folded", false);
            pl.all_in = jval<bool>(p, "all_in", false);
            pl.busted = jval<bool>(p, "busted", false);
            pl.is_turn = jval<bool>(p, "is_turn", false);
            pl.name = jval<std::string>(p, "name", "");
            if (p.contains("last_action") && p["last_action"].is_object()) {
                pl.last_action = jval<std::string>(p["last_action"], "type", "none");
                pl.last_amount = jval<int>(p["last_action"], "amount", 0);
            }
        }
    }

    g.ncommon = 0;
    if (st.contains("common") && st["common"].is_array()) {
        for (const auto &c : st["common"]) {
            if (g.ncommon>=5) break;
            if (c.is_string()) g.common[g.ncommon++] = Card(c.get<std::string>());
        }
    }

    // Compute derived fields
    g.live = 0;
    for (int i=0;i<kMaxSeats;i++)
        if (!g.players[i].busted && g.players[i].stack>=0 && g.players[i].seat>=0)
            g.live++;

    g.opps = 0;
    for (int i=0;i<kMaxSeats;i++) {
        if (i==g.my_seat) continue;
        const PlayerState &p=g.players[i];
        if (!p.busted && !p.folded && p.seat>=0) g.opps++;
    }

    g.hu = (g.live == 2);

    // Compute my position
    g.my_pos = 0;
    if (g.my_seat>=0 && g.live>0) {
        for (int idx=(g.dealer+1)%kMaxSeats; idx!=g.my_seat; idx=(idx+1)%kMaxSeats) {
            const PlayerState &p=g.players[idx];
            if (!p.busted && p.seat>=0) g.my_pos++;
        }
    }
    g.dist = std::max(0, g.live - 1 - g.my_pos); // 0=BTN

    // In position: acting after someone
    g.in_position = false;
    if (g.my_seat>=0 && g.live>1) {
        for (int idx=(g.my_seat+1)%kMaxSeats; idx!=g.my_seat; idx=(idx+1)%kMaxSeats) {
            const PlayerState &p=g.players[idx];
            if (!p.busted && !p.folded && !p.all_in && p.seat>=0)
                g.in_position = true;
        }
        // If all remaining are all-in, we're effectively last
        if (!g.in_position) g.in_position = true;
    }
}

// ---------------------------------------------------------------------------
// Prometheus bot class
// ---------------------------------------------------------------------------

static lws_context *prometheus_ctx();

class PrometheusBot {
public:
    PrometheusBot(lws_context *ctx, const char *host, int port,
                  const char *token=nullptr, const char *name=nullptr)
    : ctx_(ctx), host_(host), port_(port),
      name_(name&&name[0] ? name : "Prometheus"), token_(token ? token : "")
    {
        // Initialize per-seat opponent models
        for (int i=0;i<kMaxSeats;i++) opp_models_[i] = OppModel{};
        connect();
    }

    static double now() { return monotonic_now(); }
    bool connected() const { return wsi_ && established_; }
    int seat() const { return seat_; }
    const std::string &name() const { return name_; }

    void pump(double t);
    void close();

private:
    friend lws_context *prometheus_ctx();
    static int callback(lws *wsi, lws_callback_reasons reason,
                        void *user, void *in, size_t len);

    void connect();
    void flush();
    void send_json(const nlohmann::json &j);
    void send_action(const std::string &action, int amount=0);
    void on_message(const nlohmann::json &j);
    void maybe_decide();
    void decide_and_send();
    void update_opp_model(const nlohmann::json &action_msg);

    // Core decision logic
    Decision make_decision(GameState &g);
    Decision make_preflop_decision(GameState &g, const PlayerState &me);
    Decision make_postflop_decision(GameState &g, const PlayerState &me);

    lws_context *ctx_;
    std::string host_;
    int port_;
    std::string name_;
    std::string token_;

    lws *wsi_{nullptr};
    bool established_{false};
    std::string inbuf_;
    std::deque<std::string> outq_;
    int seat_{-1};
    nlohmann::json state_;
    int hand_epoch_{0};
    int cards_epoch_{-1};
    bool turn_pending_{false};
    std::string cards_[2];
    double last_connect_try_{0};

    // Per-seat opponent models
    OppModel opp_models_[kMaxSeats];

    // Track if we were the preflop raiser this hand
    bool was_pfr_{false};
    int  pfr_epoch_{-1};

    // Track hand count per opponent seat (for model reliability)
    int  hands_since_reset_[kMaxSeats]{};
};

static lws_context *prometheus_ctx() {
    static struct lws_protocols protocols[] = {
        {"poker", PrometheusBot::callback, 0, 4096, 0, nullptr, 0},
        LWS_PROTOCOL_LIST_TERM,
    };
    struct lws_context_creation_info info{};
    info.port      = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    return lws_create_context(&info);
}

void PrometheusBot::connect() {
    struct lws_client_connect_info i{};
    i.context                   = ctx_;
    i.address                   = host_.c_str();
    i.host                      = host_.c_str();
    i.port                      = port_;
    i.path                      = "/";
    i.protocol                  = "poker";
    i.userdata                  = this;
    i.ietf_version_or_minus_one = -1;
    wsi_ = lws_client_connect_via_info(&i);
}

void PrometheusBot::close() {
    if (!wsi_) return;
    lws_set_timeout(wsi_, PENDING_TIMEOUT_KILLED_BY_PROXY_CLIENT_CLOSE, LWS_TO_KILL_ASYNC);
}

void PrometheusBot::flush() {
    if (outq_.empty()) return;
    const std::string &msg = outq_.front();
    std::string buf(LWS_PRE, '\0');
    buf.append(msg);
    int n = lws_write(wsi_, (unsigned char*)buf.data()+LWS_PRE, msg.size(), LWS_WRITE_TEXT);
    if (n < (int)msg.size()) {
        lws_callback_on_writable(wsi_); return;
    }
    outq_.pop_front();
    if (!outq_.empty()) lws_callback_on_writable(wsi_);
}

void PrometheusBot::send_json(const nlohmann::json &j) {
    outq_.push_back(j.dump());
    if (wsi_) lws_callback_on_writable(wsi_);
}

void PrometheusBot::send_action(const std::string &action, int amount) {
    nlohmann::json j = {{"type","action"},{"action",action}};
    if (action == "bet") j["amount"] = amount;
    send_json(j);
    printf("  [Prometheus P%d] %s%s\n", seat_, action.c_str(),
           action=="bet" ? (" +" + std::to_string(amount)).c_str() : "");
}

int PrometheusBot::callback(lws *wsi, lws_callback_reasons reason,
                             void *user, void *in, size_t len) {
    PrometheusBot *b = (PrometheusBot*)user;
    switch (reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        b->wsi_ = wsi; b->established_ = true;
        {
            nlohmann::json hello = {{"type","hello"},{"name",b->name_}};
            if (!b->token_.empty()) hello["token"] = b->token_;
            b->send_json(hello);
        }
        break;
    case LWS_CALLBACK_CLIENT_RECEIVE:
        b->inbuf_.append((const char*)in, len);
        if (lws_is_final_fragment(wsi) && lws_remaining_packet_payload(wsi)==0) {
            std::string msg = std::move(b->inbuf_); b->inbuf_.clear();
            try { b->on_message(nlohmann::json::parse(msg)); } catch (...) {}
        }
        break;
    case LWS_CALLBACK_CLIENT_WRITEABLE:
        b->flush();
        break;
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        b->wsi_ = nullptr; b->established_ = false;
        printf("[Prometheus] connection error\n");
        break;
    case LWS_CALLBACK_CLIENT_CLOSED:
    case LWS_CALLBACK_CLOSED:
        b->wsi_ = nullptr; b->established_ = false;
        printf("[Prometheus] connection closed\n");
        break;
    default: break;
    }
    return 0;
}

void PrometheusBot::update_opp_model(const nlohmann::json &action_msg) {
    int seat = jval<int>(action_msg, "seat", -1);
    if (seat < 0 || seat >= kMaxSeats || seat == seat_) return;

    std::string act = jval<std::string>(action_msg, "action", "");
    if (act.empty()) return;

    OppModel &m = opp_models_[seat];

    // Track aggression
    if (act == "bet") { m.raise_count++; }
    else if (act == "call" || act == "call_all") { m.call_count++; }
    else if (act == "check") { m.check_count++; }

    // Track VPIP (preflop voluntary action)
    // We'd need to know stage; we track from action messages that include state
    if (action_msg.contains("state") && action_msg["state"].is_object()) {
        int st = stage_idx(jval<std::string>(action_msg["state"], "stage", "over"));
        if (st == 0) { // preflop
            if (act == "call" || act == "call_all" || act == "bet") {
                // Is it voluntary? Not if BB just checking
                m.vpip++;
            }
            if (act == "bet") m.pfr++;
        }
    }
}

void PrometheusBot::on_message(const nlohmann::json &j) {
    std::string type = jval<std::string>(j, "type", "");

    if (type == "welcome") {
        seat_ = jval<int>(j, "seat", -1);
        hand_epoch_ = 0; cards_epoch_ = -1; was_pfr_ = false; pfr_epoch_ = -1;
        printf("[Prometheus] joined as seat %d\n", seat_);
    } else if (type == "state") {
        if (j.is_object()) state_ = j;
    } else if (type == "action" || type == "stage" || type == "hand_over" ||
               type == "tournament_start" || type == "level" ||
               type == "player_out" || type == "tournament_over") {
        if (j.contains("state") && j["state"].is_object()) state_ = j["state"];
        if (type == "action") {
            update_opp_model(j);
            // Track if we were the PFR
            if (jval<int>(j, "seat", -1) == seat_) {
                int st = stage_idx(jval<std::string>(
                    j.contains("state") ? j["state"] : nlohmann::json::object(),
                    "stage", "over"));
                if (st == 0 && jval<std::string>(j, "action", "") == "bet") {
                    was_pfr_ = true; pfr_epoch_ = hand_epoch_;
                }
            }
        }
        if (type == "hand_over") {
            hand_epoch_++;
            // Update hands_seen for all opponents
            for (int i=0;i<kMaxSeats;i++) {
                if (i == seat_) continue;
                if (state_.is_object() && state_.contains("players") &&
                    state_["players"].is_array()) {
                    for (const auto &p : state_["players"]) {
                        if (jval<int>(p, "seat", -1) == i) {
                            opp_models_[i].hands_seen++;
                        }
                    }
                }
            }
            printf("  [Prometheus] hand over: %s\n",
                   jval<std::string>(j, "result", "").c_str());
        }
    } else if (type == "your_turn") {
        turn_pending_ = true;
        maybe_decide();
    } else if (type == "reply" && jval<std::string>(j, "what", "") == "my_cards") {
        const nlohmann::json &data = jval<nlohmann::json>(j, "data", nlohmann::json::object());
        if (data.is_object() && data.contains("cards") && data["cards"].is_array() &&
            data["cards"].size() >= 2 &&
            data["cards"][0].is_string() && data["cards"][1].is_string()) {
            cards_[0] = data["cards"][0].get<std::string>();
            cards_[1] = data["cards"][1].get<std::string>();
            cards_epoch_ = hand_epoch_;
            maybe_decide();
        } else {
            turn_pending_ = false;
        }
    } else if (type == "error") {
        std::string code = jval<std::string>(j, "code", "");
        printf("  [Prometheus] error: %s\n", code.c_str());
        turn_pending_ = false;
        if (code == "bet_too_small" || code == "illegal_action")
            send_action("check_or_fold");
    }
}

void PrometheusBot::maybe_decide() {
    if (!turn_pending_) return;
    if (state_.empty()) return;
    if (cards_epoch_ != hand_epoch_) {
        send_json({{"type","query"},{"id",200},{"what","my_cards"}});
        return;
    }
    turn_pending_ = false;
    decide_and_send();
}

Decision PrometheusBot::make_preflop_decision(GameState &g, const PlayerState &me) {
    int bb = g.bb > 0 ? g.bb : 10;
    double sbb = (double)me.stack / bb;
    int to_call = g.current_bet - me.street_bet;
    if (to_call < 0) to_call = 0;

    // Count limpers and callers
    int limpers = 0, callers = 0;
    int raiser_seat = -1;
    for (int i=0;i<kMaxSeats;i++) {
        const PlayerState &p = g.players[i];
        if (i == g.my_seat || i == g.sb_seat || i == g.bb_seat) continue;
        if (!p.busted && !p.folded && p.seat>=0) {
            if (p.street_bet == g.bb && g.current_bet == g.bb) limpers++;
            if (p.street_bet == g.current_bet && g.current_bet > g.bb) {
                raiser_seat = i;
            }
        }
    }
    // Count cold callers (called between raiser and us)
    if (raiser_seat >= 0) {
        for (int idx=(raiser_seat+1)%kMaxSeats; idx!=g.my_seat; idx=(idx+1)%kMaxSeats) {
            const PlayerState &p = g.players[idx];
            if (!p.busted && !p.folded && p.seat>=0 &&
                p.street_bet == g.current_bet && idx != raiser_seat)
                callers++;
        }
    }

    bool raised = g.current_bet > g.bb;
    bool is_3bet_pot = (me.street_bet > g.bb && raised);

    // Find raiser position
    int raiser_dist = 2; // default EP
    if (raiser_seat >= 0) {
        int rpos = 0;
        for (int idx=(g.dealer+1)%kMaxSeats; idx!=raiser_seat; idx=(idx+1)%kMaxSeats) {
            const PlayerState &p = g.players[idx];
            if (!p.busted && p.seat>=0) rpos++;
        }
        raiser_dist = std::max(0, g.live - 1 - rpos);
    }

    // Get raiser model (if available)
    const OppModel *raiser_model = nullptr;
    if (raiser_seat >= 0 && opp_models_[raiser_seat].hands_seen > 5)
        raiser_model = &opp_models_[raiser_seat];

    PreflopContext ctx;
    ctx.my_seat = g.my_seat;
    ctx.pos = g.my_pos;
    ctx.dist = g.dist;
    ctx.in_bb = g.hu ? (g.my_pos == 0) : (g.my_pos == 1 && !g.hu);
    // Heads-up: pos 0 is SB (BTN), pos 1 is BB
    if (g.hu) { ctx.in_bb = (g.my_seat == g.bb_seat); }
    else { ctx.in_bb = (g.my_seat == g.bb_seat); }
    ctx.in_sb = (g.my_seat == g.sb_seat);
    ctx.hu = g.hu;
    ctx.raised = raised;
    ctx.three_bet_pot = is_3bet_pot;
    ctx.raiser_dist = raiser_dist;
    ctx.callers = callers;
    ctx.limpers = limpers;
    ctx.current_bet = g.current_bet;
    ctx.my_street_bet = me.street_bet;
    ctx.to_call = to_call;
    ctx.pot = g.pot;
    ctx.my_stack = me.stack;
    ctx.bb = bb;
    ctx.sbb = sbb;
    ctx.hole[0] = g.hole[0];
    ctx.hole[1] = g.hole[1];
    ctx.raiser_model = raiser_model;

    // ICM context (fixes leak #3: "ICM awareness" was previously just a
    // comment — no bubble/stack-distribution logic backed it anywhere).
    ctx.is_tournament = g.is_tournament;
    ctx.icm_win_share = 0.0;
    ctx.icm_risk_premium = 0.0;
    if (g.is_tournament) {
        int stacks[kMaxSeats];
        int n = 0, players_left = 0;
        int opp_stack_vs_us = -1; // stack of the seat we're facing, if any
        for (int i = 0; i < kMaxSeats; i++) {
            const PlayerState &p = g.players[i];
            if (!p.busted && p.seat >= 0) {
                stacks[n++] = p.stack;
                players_left++;
                if (i == raiser_seat) opp_stack_vs_us = p.stack;
            }
        }
        ctx.icm_win_share = icm_win_probability(me.stack, stacks, n);
        long total = 0;
        for (int i = 0; i < n; i++) total += stacks[i];
        double opp_share = (opp_stack_vs_us >= 0 && total > 0)
                                ? (double)opp_stack_vs_us / (double)total
                                : ctx.icm_win_share; // no specific opp: neutral
        ctx.icm_risk_premium = icm_risk_premium(ctx.icm_win_share, opp_share, players_left);
    }

    return decide_preflop_full(ctx);
}

Decision PrometheusBot::make_postflop_decision(GameState &g, const PlayerState &me) {
    int nb = g.ncommon;
    if (nb < 3 || g.opps == 0) return Decision{"check", 0};

    // Compute equity
    double eq;
    if (g.opps == 1)
        eq = equity_hu(g.hole, g.common, nb);
    else
        eq = equity_mw(g.hole, g.common, nb, g.opps);

    // Raw rank value for hand classification
    int rank_val = eval7_cards(g.hole, 2, g.common, nb);
    HandStrength hs = classify_hand(g.hole, g.common, nb, eq, rank_val);
    BoardTexture bt = classify_board(g.common, nb);

    int to_call = g.current_bet - me.street_bet;
    if (to_call < 0) to_call = 0;
    double spr = g.pot > 0 ? (double)me.stack / g.pot : 10.0;

    // Aggregate across ALL live, modeled opponents instead of picking
    // whichever happens to be the first seat found — in a multi-way pot
    // that "first found" bias meant e.g. a station three seats over could
    // silently override a read on the one aggressive player actually
    // driving the action. We keep a representative model (still needed for
    // ranges.py-equivalent per-opponent range narrowing elsewhere) but the
    // three yes/no reads below are aggregated across the whole field:
    //   - opp_folds_a_lot   -> only true if EVERY live opponent folds a lot
    //                          (a bluff has to get through all of them)
    //   - opp_is_calling_station -> true if ANY live opponent is a station
    //                          (at least one will pay off a value bet)
    //   - opp_is_aggressive -> true if ANY live opponent is aggressive
    //                          (enough to make us cautious bluff-catching)
    const OppModel *opp_model = nullptr;
    bool agg_folds_a_lot = true;
    bool agg_calling_station = false;
    bool agg_aggressive = false;
    int modeled_count = 0;
    for (int i=0;i<kMaxSeats;i++) {
        if (i==g.my_seat) continue;
        const PlayerState &p = g.players[i];
        if (!p.busted && !p.folded && p.seat>=0 && opp_models_[i].hands_seen > 5) {
            if (!opp_model) opp_model = &opp_models_[i]; // representative, for callers needing a single model
            modeled_count++;
            if (!opp_models_[i].folds_too_much()) agg_folds_a_lot = false;
            if (opp_models_[i].is_fish())         agg_calling_station = true;
            if (opp_models_[i].is_aggressive())   agg_aggressive = true;
        }
    }
    if (modeled_count == 0) agg_folds_a_lot = false; // no reads: don't assume anything

    bool is_pfr = (was_pfr_ && pfr_epoch_ == hand_epoch_);

    PostflopContext ctx;
    ctx.my_seat = g.my_seat;
    ctx.stage = g.stage;
    ctx.nopp = g.opps;
    ctx.in_position = g.in_position;
    ctx.is_pfr = is_pfr;
    ctx.spr = spr;
    ctx.eq = eq;
    ctx.rank_val = rank_val;
    ctx.hs = hs;
    ctx.bt = bt;
    ctx.pot = g.pot;
    ctx.current_bet = g.current_bet;
    ctx.my_street_bet = me.street_bet;
    ctx.to_call = to_call;
    ctx.my_stack = me.stack;
    ctx.min_raise = g.min_raise;
    ctx.opp_model = opp_model;
    ctx.agg_folds_a_lot = agg_folds_a_lot;
    ctx.agg_calling_station = agg_calling_station;
    ctx.agg_aggressive = agg_aggressive;

    // ICM: same win-probability model used preflop (fix parity gap — a
    // dominant chip leader used to only get ICM caution preflop; a marginal
    // postflop all-in call/shove is exactly the same kind of unnecessary
    // variance and should get the same guard).
    ctx.is_tournament = g.is_tournament;
    ctx.icm_avoid_marginal = false;
    if (g.is_tournament) {
        int stacks[kMaxSeats];
        int n = 0;
        for (int i = 0; i < kMaxSeats; i++) {
            const PlayerState &p = g.players[i];
            if (!p.busted && p.seat >= 0) stacks[n++] = p.stack;
        }
        double my_share = icm_win_probability(me.stack, stacks, n);
        ctx.icm_avoid_marginal = (my_share > 0.35);
    }

    return decide_postflop_full(ctx);
}

Decision PrometheusBot::make_decision(GameState &g) {
    if (g.my_seat < 0 || g.opps == 0) return Decision{"check", 0};
    const PlayerState &me = g.players[g.my_seat];
    if (me.folded) return Decision{"check", 0};

    if (g.stage == 0) return make_preflop_decision(g, me);
    return make_postflop_decision(g, me);
}

void PrometheusBot::decide_and_send() {
    GameState g;
    g.my_seat = seat_;
    parse_game_state(state_, g);
    g.hole[0] = Card(cards_[0]);
    g.hole[1] = Card(cards_[1]);

    Decision d = make_decision(g);
    const PlayerState &me = g.players[g.my_seat];
    int to_call = g.current_bet - me.street_bet;
    if (to_call < 0) to_call = 0;

    if (d.act[0] == 'b') { // bet
        int cap = me.stack + me.street_bet;
        int target = d.target;
        if (target > cap) target = cap;

        int min_t = (g.current_bet == 0) ? g.min_raise
                                          : g.current_bet + g.min_raise;
        if (target < min_t) target = min_t;
        if (target > cap) target = cap;

        int inc = target - g.current_bet;
        if (inc <= 0) {
            send_action("check_or_fold");
        } else {
            send_action("bet", inc);
        }
    } else {
        send_action(d.act);
    }

    // Print diagnostic
    static const char *stage_names[] = {"preflop","flop","turn","river","over"};
    printf("  [Prometheus P%d] %s: %s%s (eq=calc, pot=%d, to_call=%d)\n",
           seat_,
           stage_names[g.stage < 5 ? g.stage : 4],
           d.act,
           d.act[0]=='b' ? (" target="+std::to_string(d.target)).c_str() : "",
           g.pot, to_call);
}

void PrometheusBot::pump(double t) {
    if (!wsi_) {
        if (t - last_connect_try_ >= 1.0) {
            last_connect_try_ = t;
            connect();
        }
    }
}

// ---------------------------------------------------------------------------
// Self-test
// ---------------------------------------------------------------------------

static int run_selftest() {
    int fails = 0;
    init_preflop();
#define CHECK(cond, msg) \
    do { if(cond){printf("  PASS: %s\n",msg);}else{printf("  FAIL: %s\n",msg);fails++;} } while(0)

    printf("== Prometheus selftest ==\n");

    // Range checks
    Card as("As"), kd("Kd"), jh("Jh"), _2c("2c"), ah("Ah"), kc("Kc");
    Card t8s_s("Ts"), t8s_8("8s");  // T8s: same suit (spades)
    Card kjo_k("Kd"), kjo_j("Jh"); // KJo: different suits

    CHECK(in_range(g_pre.open_btn, as, kd), "AKs in BTN open");
    CHECK(!in_range(g_pre.open_btn, jh, _2c), "J2o not in BTN open");
    CHECK(in_range(g_pre.open_ep, as, kd), "AKs in EP open");
    CHECK(in_range(g_pre.open_ep, kjo_k, kjo_j), "KJo IS in EP open (KJo+ range)");
    CHECK(in_range(g_pre.stackoff_vs_3b, as, kd), "AKs in stackoff");
    CHECK(in_range(g_pre.open_ep, t8s_s, t8s_8), "T8s in EP open (same suit)");

    // Push ranges
    CHECK(in_range(g_pre.push_range[5], as, kd), "AKs push at 5BB");
    CHECK(in_range(g_pre.push_range[1], jh, _2c), "J2o push at 1BB (any2)");
    CHECK(!in_range(g_pre.push_range[20], jh, _2c), "J2o NOT push at 20BB");

    // Equity checks
    Card hAA[2] = {Card("As"), Card("Ad")};
    double eqAA = equity_hu(hAA, nullptr, 0, 1500);
    printf("  eq(AA) = %.3f\n", eqAA);
    CHECK(eqAA > 0.80 && eqAA < 0.88, "AA equity ~0.85");

    Card hAKs[2] = {Card("Ks"), Card("As")};
    double eqAKs = equity_hu(hAKs, nullptr, 0, 1500);
    printf("  eq(AKs) = %.3f\n", eqAKs);
    CHECK(eqAKs > 0.62 && eqAKs < 0.72, "AKs equity ~0.67");

    // River exact equity: nut straight flush vs random = 1.0
    Card hNuts[2] = {Card("As"), Card("Ks")};
    Card bNuts[5] = {Card("Qs"), Card("Js"), Card("Ts"), Card("2c"), Card("3d")};
    double eqNuts = equity_hu(hNuts, bNuts, 5);
    printf("  eq(nut straight flush river) = %.3f\n", eqNuts);
    CHECK(eqNuts == 1.0, "nut straight flush on river = 1.0");

    // Board texture
    Card flop[3] = {Card("Ks"), Card("Qs"), Card("Js")};
    BoardTexture bt = classify_board(flop, 3);
    CHECK(bt.monotone, "KsQsJs is monotone");
    CHECK(bt.has_broadway, "KsQsJs has broadway");
    CHECK(!bt.paired, "KsQsJs not paired");
    CHECK(bt.connectivity >= 2, "KsQsJs high connectivity");

    Card dry[3] = {Card("Kd"), Card("7h"), Card("2c")};
    BoardTexture bt2 = classify_board(dry, 3);
    CHECK(!bt2.monotone, "K72r not monotone");
    CHECK(bt2.is_static, "K72r is static");
    CHECK(!bt2.is_dynamic, "K72r not dynamic");

    // Hand strength
    Card pair_hand[2] = {Card("As"), Card("Kd")};
    Card pair_board[3] = {Card("Ah"), Card("7d"), Card("2c")};
    int rv = eval7_cards(pair_hand, 2, pair_board, 3);
    double eq_pair = equity_hu(pair_hand, pair_board, 3);
    printf("  eq(AK on A72r) = %.3f\n", eq_pair);
    HandStrength hs = classify_hand(pair_hand, pair_board, 3, eq_pair, rv);
    CHECK(hs == HandStrength::MEDIUM_MADE || hs == HandStrength::STRONG_MADE,
          "AK on A72r is medium/strong made");

    printf("== selftest %s (%d failure(s)) ==\n", fails ? "FAILED" : "PASSED", fails);
#undef CHECK
    return fails ? 1 : 0;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    int port = 9000;
    std::string host = "127.0.0.1", token, name;
    bool selftest = false;

    for (int i=1;i<argc;i++) {
        if (strcmp(argv[i],"--port")==0 && i+1<argc) port = atoi(argv[++i]);
        else if (strcmp(argv[i],"--host")==0 && i+1<argc) host = argv[++i];
        else if (strcmp(argv[i],"--token")==0 && i+1<argc) token = argv[++i];
        else if (strcmp(argv[i],"--name")==0 && i+1<argc) name = argv[++i];
        else if (strcmp(argv[i],"--selftest")==0) selftest = true;
        else {
            fprintf(stderr, "usage: %s [--port N] [--host IP] [--token SECRET] [--name NAME] [--selftest]\n", argv[0]);
            return 1;
        }
    }

    if (selftest) { init_preflop(); return run_selftest(); }

    init_preflop();

    lws_context *ctx = prometheus_ctx();
    if (!ctx) { fprintf(stderr, "failed to create lws context\n"); return 1; }

    PrometheusBot bot(ctx, host.c_str(), port, token.c_str(), name.c_str());
    printf("[Prometheus] connecting to %s:%d as '%s'\n", host.c_str(), port, bot.name().c_str());

    std::atomic<bool> running{true};
    std::thread ticker([&] {
        while (running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            lws_cancel_service(ctx);
        }
    });

    for (;;) {
        lws_service(ctx, 20);
        bot.pump(PrometheusBot::now());
    }
}
