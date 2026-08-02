#include "poker.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <random>

static std::mt19937 g_rng(std::random_device{}());

double
poker_random()
{
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        return dist(g_rng);
}

void
poker_set_seed(unsigned seed)
{
        g_rng.seed(seed);
}

void
Deck::add(phevaluator::Card c)
{
        this->cards.push_back(c);
}

phevaluator::Card
Deck::pick()
{
        if (this->cards.empty()) {
                printf("Unreachable: %s:%d\n", __FILE__, __LINE__);
                exit(1);
        }

        phevaluator::Card c = this->cards.back();
        this->cards.pop_back();
        return c;
}

void
Deck::shuffle()
{
        std::shuffle(this->cards.begin(), this->cards.end(), g_rng);
}

void
Deck::reset()
{
        this->cards.clear();
        for (char suit : { 'H', 'S', 'D', 'C' }) {
                for (char rank : { 'A', '2', '3', '4', '5', '6', '7', '8', '9', 'T', 'J', 'Q', 'K' }) {
                        this->cards.emplace_back(std::string{ rank, suit });
                }
        }
}

void
Player::ask_for_action(const Game_State *state)
{
        this->clear_response();
        this->response.has_response = true;

        int to_call = state->current_bet - this->_street_bet;
        bool strong = this->rank.value() > 0 && this->rank.value() < 2000;

        if (to_call <= 0) {
                // free action: check or bet
                double r = poker_random();
                if (r < 0.75 || !strong) {
                        this->response.type = Response::CHECK;
                } else {
                        this->response.type          = Response::BET;
                        int min_bet                  = (state->current_bet == 0) ? BigBlind : state->current_bet + state->min_raise;
                        int raise                    = (int) (poker_random() * 3.0) * state->min_raise;
                        this->response.as.bet.amount = std::min(min_bet + raise,
                                                                this->stack + this->_street_bet);
                }
                return;
        }

        if (strong && to_call <= this->stack / 4) {
                double r = poker_random();
                if (r < 0.15) {
                        this->response.type          = Response::BET;
                        int min_bet                  = state->current_bet + state->min_raise;
                        int raise                    = (int) (poker_random() * 3.0) * state->min_raise;
                        this->response.as.bet.amount = std::min(min_bet + raise,
                                                                this->stack + this->_street_bet);
                } else {
                        this->response.type = Response::CALL;
                }
                return;
        }

        if (to_call * 3 > this->stack) {
                // too expensive for this stack
                if (poker_random() < 0.7) {
                        this->response.type = Response::FOLD;
                } else {
                        this->response.type = Response::CALL_ALL;
                }
                return;
        }

        if (poker_random() < 0.2) {
                this->response.type = Response::FOLD;
        } else {
                this->response.type = Response::CALL;
        }
}

void
Player::clear_response()
{
        this->response.has_response  = false;
        this->response.type          = Response::NONE;
        this->response.as.bet.amount = 0;
}

std::string
Player::describe_category() const
{
        return rank.describeCategory();
}

std::string
Player::describe_rank() const
{
        return rank.describeRank();
}

std::string
Player::describe_sample_hand() const
{
        return rank.describeSampleHand();
}

int
Player::hand_value() const
{
        return rank.value();
}

void
Table::add_player(Player p)
{
        this->players.push_back(p);
}

int
Table::non_folded_count() const
{
        int count = 0;
        for (const Player &p : this->players) {
                if (!p._fold) count++;
        }
        return count;
}

int
Table::next_active_player(int from) const
{
        size_t n = this->players.size();
        for (size_t i = 1; i <= n; i++) {
                int idx         = (int) ((from + (int) i) % (int) n);
                const Player &p = this->players[idx];
                if (!p._fold && !p.is_all_in) return idx;
        }
        return -1;
}

bool
Table::needs_action(const Game_State *state) const
{
        for (const Player &p : this->players) {
                if (!p._fold && !p.is_all_in) {
                        if (!p.has_acted || p._street_bet < state->current_bet) {
                                return true;
                        }
                }
        }
        return false;
}

void
Table::commit_chips(Player &p, int amount)
{
        amount = std::max(0, std::min(amount, p.stack));
        p.stack -= amount;
        p._bet += amount;
        p._street_bet += amount;
        this->pot += amount;
        if (p.stack == 0) p.is_all_in = true;
}

void
Table::post_blind(Player &p, int amount)
{
        this->commit_chips(p, amount);
}

void
Table::new_hand(Game_State *state)
{
        assert(this->players.size() == (size_t) MaxPlayers);
        this->deck->reset();
        state->hand_started = true;
        state->turn         = state->dealer;

        this->shuffle_and_deal();
        this->start_betting_round(state);
}

void
Table::end_hand(Game_State *state)
{
        state->dealer       = (state->dealer + 1) % (int) this->players.size();
        state->stage        = PREFLOP;
        state->turn         = state->dealer;
        state->current_bet  = 0;
        state->min_raise    = BigBlind;
        state->round_done   = false;
        state->hand_started = false;
        state->hand_over    = false;

        this->pot = 0;
        this->common.clear();
        this->winners.clear();
        this->win_amount.clear();
        this->award = 0;
        this->result_text.clear();
        this->deck->cards.clear();

        for (Player &p : this->players) {
                p._fold       = false;
                p._bet        = 0;
                p._street_bet = 0;
                p.has_acted   = false;
                p.is_all_in   = false;
                p.is_my_turn  = false;
                p.clear_response();
                p.last_action = {};
                p.hand.reset();
                p.set_rank(phevaluator::Rank(0));
                if (p.stack <= 0) p.stack = StartStack; // rebuy busted players
        }
}

void
Table::start_betting_round(Game_State *state)
{
        state->current_bet = 0;
        state->min_raise   = BigBlind;
        state->round_done  = false;

        for (Player &p : this->players) {
                p._street_bet = 0;
                p.has_acted   = false;
                p.last_action = {};
                p.clear_response();
        }

        int first = 0;
        if (state->stage == PREFLOP) {
                int sb = (state->dealer + 1) % (int) this->players.size();
                int bb = (state->dealer + 2) % (int) this->players.size();
                this->post_blind(this->players[sb], SmallBlind);
                this->post_blind(this->players[bb], BigBlind);
                state->current_bet = BigBlind;
                first              = (bb + 1) % (int) this->players.size();
        } else {
                first = (state->dealer + 1) % (int) this->players.size();
        }

        state->turn = this->next_active_player(first - 1);
        if (state->turn < 0 || !this->needs_action(state)) {
                state->round_done = true;
        }
}

void
Table::step_betting_round(Game_State *state, double now)
{
        if (state->round_done) return;
        if (state->turn < 0 || (size_t) state->turn >= this->players.size()) {
                state->round_done = true;
                return;
        }

        Player &p = this->players[state->turn];

        if (!p.is_my_turn) {
                p.is_my_turn   = true;
                p.action_start = now;
        }

        if (p.auto_play && p.response.type == Player::Response::NONE) {
                p.ask_for_action(state);
        }

        if (p.response.has_response && p.response.type != Player::Response::NONE) {
                this->process_action(state, p);
                if (this->non_folded_count() <= 1) {
                        state->round_done = true;
                        return;
                }
                state->turn = this->next_active_player(state->turn);
                if (state->turn < 0) {
                        state->round_done = true;
                        return;
                }
                state->round_done = !this->needs_action(state);
                return;
        }

        if (now - p.action_start > this->action_timeout) {
                printf("%s: timeout, folds\n", p.name.c_str());
                p._fold = true;
                p.clear_response();
                p.is_my_turn = false;
                if (this->non_folded_count() <= 1) {
                        state->round_done = true;
                        return;
                }
                state->turn = this->next_active_player(state->turn);
                if (state->turn < 0) {
                        state->round_done = true;
                        return;
                }
                state->round_done = !this->needs_action(state);
        }
}

void
Table::process_action(Game_State *state, Player &p)
{
        int to_call = state->current_bet - p._street_bet;

        switch (p.response.type) {
        case Player::Response::NONE:
                return;

        case Player::Response::FOLD:
                p._fold = true;
                break;

        case Player::Response::CHECK:
                assert(to_call <= 0);
                p.has_acted = true;
                break;

        case Player::Response::CALL:
        case Player::Response::CALL_ALL:
                this->commit_chips(p, to_call);
                p.has_acted = true;
                break;

        case Player::Response::BET: {
                int amount = p.response.as.bet.amount;
                if (amount < state->current_bet + state->min_raise && amount != p.stack + p._street_bet) {
                        amount = state->current_bet + state->min_raise;
                }
                amount           = std::min(amount, p.stack + p._street_bet);
                bool legal_raise = (amount - state->current_bet) >= state->min_raise;
                if (legal_raise) {
                        state->min_raise = amount - state->current_bet;
                        for (Player &q : this->players) {
                                if (!q._fold && !q.is_all_in) q.has_acted = false;
                        }
                }
                this->commit_chips(p, amount - p._street_bet);
                state->current_bet = std::max(state->current_bet, amount);
                p.has_acted        = true;
                break;
        }

        default:
                assert(0 && "Unreachable");
        }

        p.last_action.type   = p.response.type;
        p.last_action.amount = p._street_bet;
        p.clear_response();
        p.is_my_turn = false;
}

void
Table::recalc_player_hand_strength()
{
        for (Player &p : this->players) {
                if (!p.hand.has_value()) continue;
                const phevaluator::Card &h1 = p.hand->at(0);
                const phevaluator::Card &h2 = p.hand->at(1);

                switch (this->common.size()) {
                case 3:
                        p.set_rank(phevaluator::EvaluateCards(
                        h1, h2,
                        this->common.at(0),
                        this->common.at(1),
                        this->common.at(2)));
                        break;

                case 4:
                        p.set_rank(phevaluator::EvaluateCards(
                        h1, h2,
                        this->common.at(0),
                        this->common.at(1),
                        this->common.at(2),
                        this->common.at(3)));
                        break;

                case 5:
                        p.set_rank(phevaluator::EvaluateCards(
                        h1, h2,
                        this->common.at(0),
                        this->common.at(1),
                        this->common.at(2),
                        this->common.at(3),
                        this->common.at(4)));
                        break;
                }
        }
}

void
Table::add_common()
{
        this->common.push_back(this->deck->pick());
}

void
Table::shuffle_and_deal()
{
        this->deck->shuffle();

        for (Player &p : this->players) {
                phevaluator::Card c1 = this->deck->pick();
                phevaluator::Card c2 = this->deck->pick();
                p.hand.emplace(std::array<phevaluator::Card, 2>{ c1, c2 });
        }
}

void
Table::finish_hand(Game_State *state)
{
        state->hand_over  = true;
        state->stage      = OVER;
        state->round_done = true;

        this->winners.clear();
        this->win_amount.clear();
        this->award = 0;
        this->result_text.clear();
        // `pot` is left as the committed total so it stays visible during
        // the hand-over pause; end_hand() zeroes it for the next hand

        int survivor = -1;
        for (size_t i = 0; i < this->players.size(); i++) {
                if (!this->players[i]._fold) {
                        survivor = (int) i;
                        break;
                }
        }

        if (this->non_folded_count() == 1) {
                for (const Player &p : this->players) {
                        this->award += p._bet;
                }
                this->winners.push_back(survivor);
                this->win_amount.push_back(this->award);
                this->players[survivor].stack += this->award;
                this->result_text = this->players[survivor].name + " wins " +
                                    std::to_string(this->award) + " (fold)";
                return;
        }

        this->recalc_player_hand_strength();
        this->award_pots();

        if (this->winners.empty()) {
                this->result_text = "Hand over: no pot";
                return;
        }

        std::string names;
        for (size_t i = 0; i < this->winners.size(); i++) {
                if (i > 0) names += (this->winners.size() == 2 ? " and " : ", ");
                names += this->players[this->winners[i]].name;
        }
        this->result_text = names + (this->winners.size() > 1 ? " split " : " wins ") +
                            std::to_string(this->award) + " with " +
                            this->players[this->winners[0]].describe_rank();
}

void
Table::award_pots()
{
        std::vector<int> levels;
        for (const Player &p : this->players) {
                if (p._bet > 0) levels.push_back(p._bet);
        }
        std::sort(levels.begin(), levels.end());
        levels.erase(std::unique(levels.begin(), levels.end()), levels.end());

        int prev = 0;
        for (int v : levels) {
                int layer = 0;
                for (const Player &p : this->players) {
                        layer += std::min(p._bet, v) - std::min(p._bet, prev);
                }
                if (layer <= 0) {
                        prev = v;
                        continue;
                }

                std::vector<int> eligible;
                for (size_t i = 0; i < this->players.size(); i++) {
                        const Player &p = this->players[i];
                        if (!p._fold && p._bet >= v) eligible.push_back((int) i);
                }
                if (eligible.empty()) {
                        // dead money: only folded players covered this layer;
                        // award it to the best hand among the survivors
                        for (size_t i = 0; i < this->players.size(); i++) {
                                if (!this->players[i]._fold) eligible.push_back((int) i);
                        }
                }
                if (eligible.empty()) {
                        prev = v;
                        continue;
                }

                int best = eligible[0];
                for (int i : eligible) {
                        if (this->players[i].hand_value() < this->players[best].hand_value()) {
                                best = i;
                        }
                }
                int best_value = this->players[best].hand_value();

                std::vector<int> tied;
                for (int i : eligible) {
                        if (this->players[i].hand_value() == best_value) tied.push_back(i);
                }

                int split     = layer / (int) tied.size();
                int remainder = layer % (int) tied.size();
                for (int i : tied) {
                        int w = split;
                        if (i == tied[0]) w += remainder;
                        this->players[i].stack += w;
                        this->award += w;
                        auto it = std::find(this->winners.begin(), this->winners.end(), i);
                        if (it == this->winners.end()) {
                                this->winners.push_back(i);
                                this->win_amount.push_back(w);
                        } else {
                                this->win_amount[it - this->winners.begin()] += w;
                        }
                }

                prev = v;
        }
}

void
Table::step_game(Game_State *state, double now)
{
        switch (state->stage) {
        case PREFLOP:
                if (!state->hand_started) {
                        this->new_hand(state);
                } else {
                        this->step_betting_round(state, now);
                }
                if (state->round_done) {
                        if (this->non_folded_count() <= 1) {
                                this->finish_hand(state);
                        } else {
                                state->stage = FLOP;
                        }
                }
                break;

        case FLOP:
                if ((int) this->common.size() < 3) {
                        while ((int) this->common.size() < 3)
                                this->add_common();
                        this->recalc_player_hand_strength();
                        this->start_betting_round(state);
                } else {
                        this->step_betting_round(state, now);
                }
                if (state->round_done) {
                        if (this->non_folded_count() <= 1) {
                                this->finish_hand(state);
                        } else {
                                state->stage = TURN;
                        }
                }
                break;

        case TURN:
                if ((int) this->common.size() < 4) {
                        this->add_common();
                        this->recalc_player_hand_strength();
                        this->start_betting_round(state);
                } else {
                        this->step_betting_round(state, now);
                }
                if (state->round_done) {
                        if (this->non_folded_count() <= 1) {
                                this->finish_hand(state);
                        } else {
                                state->stage = RIVER;
                        }
                }
                break;

        case RIVER:
                if ((int) this->common.size() < 5) {
                        this->add_common();
                        this->recalc_player_hand_strength();
                        this->start_betting_round(state);
                } else {
                        this->step_betting_round(state, now);
                }
                if (state->round_done) {
                        this->finish_hand(state);
                }
                break;

        case OVER:
                break;
        }
}
