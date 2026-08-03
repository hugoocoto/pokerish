#pragma once

#include <string>
#include <vector>

#include "poker.h"

struct PokerStarsPlayerInfo {
        int seat{ 0 }; // 0-indexed internally, seat + 1 in PokerStars text format
        std::string name;
        int stack{ 0 };
        bool busted{ false };
};

struct PokerStarsActionRecord {
        int seat{ 0 };
        std::string name;
        std::string action_type; // "fold", "check", "call", "bet", "raise"
        int amount{ 0 };
        int to_amount{ 0 };      // total street bet target (for raise)
        bool is_all_in{ false };
};

struct PokerStarsStreetRecord {
        int stage{ PREFLOP };
        std::string stage_name; // "preflop", "flop", "turn", "river"
        std::vector<std::string> board_cards;
        std::vector<PokerStarsActionRecord> actions;
};

class PokerStarsExporter
{
    public:
        void start_hand(long long hand_id, const Game_State &state, const Table &table,
                        const std::string &timestamp_str = "");
        void record_action(int seat, const std::string &name, const std::string &action_type,
                           int amount, int street_bet, bool is_all_in, int stage);
        void record_stage(int stage, const std::vector<phevaluator::Card> &common);
        void finish_hand(const Game_State &state, const Table &table);

        std::string format_hand_history() const;
        bool save_to_file(const std::string &dir_path, const std::string &filename = "pokerstars_hands.txt") const;

    private:
        long long hand_id_{ 1 };
        bool is_tournament_{ false };
        int level_{ 1 };
        int small_blind_{ 5 };
        int big_blind_{ 10 };
        int ante_{ 0 };
        int dealer_seat_{ 0 };
        int max_players_{ 6 };
        std::string timestamp_;

        std::vector<PokerStarsPlayerInfo> players_;
        std::vector<PokerStarsStreetRecord> streets_;
        std::vector<std::string> final_board_;
        int total_pot_{ 0 };
        std::vector<int> winners_;
        std::vector<int> win_amounts_;
        std::string result_text_;
};
