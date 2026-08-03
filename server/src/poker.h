#pragma once

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "phevaluator/card.h"
#include "phevaluator/phevaluator.h"

constexpr int SmallBlind    = 5;
constexpr int BigBlind      = 10;
constexpr int StartStack    = 1000;
constexpr int ActionTimeOut = 20; // in seconds
constexpr int MaxPlayers    = 10; // hard cap; default table is 6 (--max-players)

enum Stage {
        PREFLOP = 0,
        FLOP,
        TURN,
        RIVER,
        OVER,
};

enum TournamentStatus {
        T_LOBBY = 0,   // waiting for players to fill the seats
        T_COUNTDOWN,   // all seats filled, countdown to the first hand
        T_RUNNING,     // hands are being played
        T_FINISHED,    // one player left, winner announced
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

        // game configuration settings
        int start_stack{ StartStack };
        int max_players{ MaxPlayers };

        // blinds of the current level (cash mode: the fixed defaults)
        int small_blind{ SmallBlind };
        int big_blind{ BigBlind };
        int ante{ 0 };

        // tournament mode; cash mode keeps the defaults and never touches these
        bool tournament{ false };
        int level{ 0 };                // current blind level, 1-based; 0 = not started
        double level_seconds{ 300 };   // length of a level (seconds)
        double level_started_at{ 0 };  // monotonic clock
        double level_remaining{ 0 };   // seconds left in the level (server keeps it fresh)
        int tournament_status{ T_LOBBY };
        double countdown_seconds{ 0 };
        double countdown_started_at{ 0 };
        double countdown_remaining{ 0 };
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
        bool busted{ false };   // tournament only: eliminated / empty seat (cash: never)

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

        double action_timeout{ ActionTimeOut }; // seconds; timeout folds

        std::vector<int> winners; // indexes of the winning players (last hand)
        std::vector<int> win_amount; // per-winner winnings, parallel to winners
        int award{ 0 };           // total chips awarded (last hand)
        std::string result_text;

        Table(Deck *d) : deck(d) {};

        void add_player(Player p);
        int non_folded_count() const;

        // tournament helpers: alive = not busted
        int alive_count() const;
        int next_alive_from(int from) const;
        std::pair<int, int> blind_seats(int dealer) const; // { small, big } among alive seats

        // game engine steps
        void new_hand(Game_State *state);
        void end_hand(Game_State *state);
        void start_betting_round(Game_State *state);
        void step_betting_round(Game_State *state, double now);
        void step_game(Game_State *state, double now);
        void finish_hand(Game_State *state);
        void award_pots(int dealer);

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
