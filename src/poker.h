#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "phevaluator/card.h"
#include "phevaluator/phevaluator.h"

constexpr int SmallBlind    = 5;
constexpr int BigBlind      = 10;
constexpr int StartStack    = 1000;
constexpr int ActionTimeOut = 20; // in seconds
constexpr int MaxPlayers    = 6;  // fixed for now

enum Stage {
        PREFLOP = 0,
        FLOP,
        TURN,
        RIVER,
        OVER,
};

struct Game_State {
        int stage{ PREFLOP }; // Stage
        int turn{ 0 };        // index of the player that has to do something
        int dealer{ 0 };      // index of the player that is the dealer

        int current_bet{ 0 }; // highest street commitment (the "to call" target)
        int min_raise{ 0 };   // legal raise increment

        bool round_done{ false };   // current street betting finished
        bool hand_started{ false }; // preflop dealt + blinds posted
        bool hand_over{ false };    // hand finished, result computed
};

double poker_random();
void poker_set_seed(unsigned seed);

class Deck
{
    public:
        std::vector<phevaluator::Card> cards;

        void add(phevaluator::Card c);
        phevaluator::Card pick();
        void shuffle();
        void reset(); // fresh 52 cards
};

class Player
{
    public:
        std::string name;
        std::optional<std::array<phevaluator::Card, 2>> hand;

        int stack{ StartStack };
        int _bet{ 0 };        // cumulative chips committed this hand
        int _street_bet{ 0 }; // chips committed in the current street
        bool _fold{ false };
        bool has_acted{ false };
        bool is_all_in{ false };

        bool is_my_turn{ false };
        double action_start{ 0 };
        bool auto_play{ true }; // bots answer instantly; humans don't (timeout)

        struct Response {
                bool has_response{ false };
                enum ResponseType {
                        NONE = 0,
                        FOLD,
                        CHECK,
                        CALL,
                        CALL_ALL,
                        BET,
                } type{ NONE };
                union {
                        struct {
                                int amount;
                        } bet;
                } as{};
        } response;

        struct LastAction {
                Response::ResponseType type{ Response::NONE };
                int amount{ 0 };
        } last_action; // action taken this betting round, kept like _fold

        Player(std::string name) : name(name) {};

        void set_rank(phevaluator::Rank r)
        {
                rank = r;
        }

        void ask_for_action(const Game_State *state);
        void clear_response();
        std::string describe_category() const;
        std::string describe_rank() const;
        std::string describe_sample_hand() const;
        int hand_value() const;

    private:
        phevaluator::Rank rank;
};

class Table
{
    public:
        std::vector<Player> players;
        Deck *deck{ nullptr };
        std::vector<phevaluator::Card> common;
        int pot{ 0 };

        std::vector<int> winners; // indexes of the winning players (last hand)
        int award{ 0 };           // total chips awarded (last hand)
        std::string result_text;

        Table(Deck *d) : deck(d) {};

        void add_player(Player p);
        int non_folded_count() const;

        // game engine steps
        void new_hand(Game_State *state);
        void end_hand(Game_State *state);
        void start_betting_round(Game_State *state);
        void step_betting_round(Game_State *state, double now);
        void step_game(Game_State *state, double now);
        void finish_hand(Game_State *state);
        void award_pots();

        void process_action(Game_State *state, Player &p);
        void recalc_player_hand_strength();
        void add_common();
        void shuffle_and_deal();

    private:
        int next_active_player(int from) const;
        bool needs_action(const Game_State *state) const;
        void commit_chips(Player &p, int amount);
        void post_blind(Player &p, int amount);
};
