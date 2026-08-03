#include "pokerstars_export.h"
#include "proto.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

static std::string
current_timestamp_str()
{
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf;
        localtime_r(&t, &tm_buf);
        std::ostringstream ss;
        ss << std::put_time(&tm_buf, "%Y/%m/%d %H:%M:%S ET");
        return ss.str();
}

static std::string
roman_numeral(int n)
{
        static const std::pair<int, const char *> kRoman[] = {
                { 1000, "M" }, { 900, "CM" }, { 500, "D" }, { 400, "CD" },
                { 100, "C" },  { 90, "XC" },  { 50, "L" },  { 40, "XL" },
                { 10, "X" },   { 9, "IX" },   { 5, "V" },   { 4, "IV" },
                { 1, "I" }
        };
        std::string result;
        for (const auto &pair : kRoman) {
                while (n >= pair.first) {
                        result += pair.second;
                        n -= pair.first;
                }
        }
        return result.empty() ? "I" : result;
}

void
PokerStarsExporter::start_hand(long long hand_id, const Game_State &state,
                              const Table &table, const std::string &timestamp_str)
{
        hand_id_       = hand_id;
        is_tournament_ = state.tournament;
        level_         = state.level > 0 ? state.level : 1;
        small_blind_   = state.small_blind;
        big_blind_     = state.big_blind;
        ante_          = state.ante;
        dealer_seat_   = state.dealer;
        max_players_   = state.max_players;
        timestamp_     = timestamp_str.empty() ? current_timestamp_str() : timestamp_str;

        players_.clear();
        streets_.clear();
        final_board_.clear();
        winners_.clear();
        win_amounts_.clear();
        total_pot_ = 0;
        result_text_.clear();

        for (size_t i = 0; i < table.players.size(); i++) {
                const Player &p = table.players[i];
                PokerStarsPlayerInfo info;
                info.seat   = (int) i;
                info.name   = p.name;
                info.stack  = p.stack;
                info.busted = p.busted;
                players_.push_back(info);
        }

        PokerStarsStreetRecord preflop;
        preflop.stage      = PREFLOP;
        preflop.stage_name = "preflop";
        streets_.push_back(preflop);
}

void
PokerStarsExporter::record_action(int seat, const std::string &name,
                                  const std::string &action_type, int amount,
                                  int street_bet, bool is_all_in, int stage)
{
        if (streets_.empty()) return;
        PokerStarsActionRecord rec;
        rec.seat        = seat;
        rec.name        = name;
        rec.action_type = action_type;
        rec.amount      = amount;
        rec.to_amount   = street_bet;
        rec.is_all_in   = is_all_in;

        // find or update street record
        for (auto &st : streets_) {
                if (st.stage == stage) {
                        st.actions.push_back(rec);
                        return;
                }
        }
        PokerStarsStreetRecord new_st;
        new_st.stage      = stage;
        new_st.stage_name = proto::stage_name(stage);
        new_st.actions.push_back(rec);
        streets_.push_back(new_st);
}

void
PokerStarsExporter::record_stage(int stage, const std::vector<phevaluator::Card> &common)
{
        std::vector<std::string> cards;
        for (const auto &c : common) {
                cards.push_back(proto::card_name(c));
        }

        for (auto &st : streets_) {
                if (st.stage == stage) {
                        st.board_cards = cards;
                        return;
                }
        }
        PokerStarsStreetRecord new_st;
        new_st.stage       = stage;
        new_st.stage_name  = proto::stage_name(stage);
        new_st.board_cards = cards;
        streets_.push_back(new_st);
}

void
PokerStarsExporter::finish_hand(const Game_State &state, const Table &table)
{
        (void) state;
        final_board_.clear();
        for (const auto &c : table.common) {
                final_board_.push_back(proto::card_name(c));
        }
        total_pot_   = table.pot > 0 ? table.pot : table.award;
        winners_     = table.winners;
        win_amounts_ = table.win_amount;
        result_text_ = table.result_text;
}

std::string
PokerStarsExporter::format_hand_history() const
{
        std::ostringstream ss;

        // Header
        if (is_tournament_) {
                ss << "PokerStars Hand #" << hand_id_ << ": Tournament #1, Hold'em No Limit - Level "
                   << roman_numeral(level_) << " (" << small_blind_ << "/" << big_blind_
                   << ") - " << timestamp_ << "\n";
        } else {
                ss << "PokerStars Hand #" << hand_id_ << ": Hold'em No Limit (" << small_blind_
                   << "/" << big_blind_ << " USD) - " << timestamp_ << "\n";
        }

        // Table Info
        ss << "Table '1' " << max_players_ << "-max Seat #" << (dealer_seat_ + 1)
           << " is the button\n";

        // Seat Listings
        for (const auto &p : players_) {
                if (!p.busted) {
                        ss << "Seat " << (p.seat + 1) << ": " << p.name << " (" << p.stack
                           << " in chips)\n";
                }
        }

        // Blinds and Antes Posting
        if (ante_ > 0) {
                for (const auto &p : players_) {
                        if (!p.busted) {
                                ss << p.name << ": posts ante " << ante_ << "\n";
                        }
                }
        }

        // SB and BB seats
        int sb_seat = -1, bb_seat = -1;
        int active_count = 0;
        for (const auto &p : players_) {
                if (!p.busted) active_count++;
        }
        if (active_count == 2) {
                sb_seat = dealer_seat_;
                for (const auto &p : players_) {
                        if (!p.busted && p.seat != dealer_seat_) bb_seat = p.seat;
                }
        } else {
                for (int i = 1; i <= (int) players_.size(); i++) {
                        int idx = (dealer_seat_ + i) % (int) players_.size();
                        if (!players_[idx].busted) {
                                if (sb_seat < 0) {
                                        sb_seat = idx;
                                } else if (bb_seat < 0) {
                                        bb_seat = idx;
                                        break;
                                }
                        }
                }
        }

        if (sb_seat >= 0 && sb_seat < (int) players_.size()) {
                ss << players_[sb_seat].name << ": posts small blind " << small_blind_ << "\n";
        }
        if (bb_seat >= 0 && bb_seat < (int) players_.size()) {
                ss << players_[bb_seat].name << ": posts big blind " << big_blind_ << "\n";
        }

        ss << "*** HOLE CARDS ***\n";

        // Street Actions
        for (const auto &st : streets_) {
                if (st.stage == FLOP) {
                        ss << "*** FLOP *** [";
                        for (size_t i = 0; i < st.board_cards.size() && i < 3; i++) {
                                if (i > 0) ss << " ";
                                ss << st.board_cards[i];
                        }
                        ss << "]\n";
                } else if (st.stage == TURN) {
                        ss << "*** TURN *** [";
                        for (size_t i = 0; i < st.board_cards.size() && i < 3; i++) {
                                if (i > 0) ss << " ";
                                ss << st.board_cards[i];
                        }
                        ss << "] [" << (st.board_cards.size() >= 4 ? st.board_cards[3] : "") << "]\n";
                } else if (st.stage == RIVER) {
                        ss << "*** RIVER *** [";
                        for (size_t i = 0; i < st.board_cards.size() && i < 4; i++) {
                                if (i > 0) ss << " ";
                                ss << st.board_cards[i];
                        }
                        ss << "] [" << (st.board_cards.size() >= 5 ? st.board_cards[4] : "") << "]\n";
                }

                for (const auto &act : st.actions) {
                        ss << act.name << ": ";
                        if (act.action_type == "fold") {
                                ss << "folds\n";
                        } else if (act.action_type == "check") {
                                ss << "checks\n";
                        } else if (act.action_type == "call") {
                                ss << "calls " << act.amount;
                                if (act.is_all_in) ss << " and is all-in";
                                ss << "\n";
                        } else if (act.action_type == "bet") {
                                ss << "bets " << act.amount;
                                if (act.is_all_in) ss << " and is all-in";
                                ss << "\n";
                        } else if (act.action_type == "bet_raise" || act.action_type == "raise") {
                                ss << "raises " << act.amount << " to " << act.to_amount;
                                if (act.is_all_in) ss << " and is all-in";
                                ss << "\n";
                        } else {
                                ss << act.action_type << "\n";
                        }
                }
        }

        // Collected amounts
        for (size_t k = 0; k < winners_.size() && k < win_amounts_.size(); k++) {
                int wseat = winners_[k];
                if (wseat >= 0 && wseat < (int) players_.size()) {
                        ss << players_[wseat].name << " collected " << win_amounts_[k] << " from pot\n";
                }
        }

        // Summary
        ss << "*** SUMMARY ***\n";
        ss << "Total pot " << total_pot_ << " | Rake 0\n";
        if (!final_board_.empty()) {
                ss << "Board [";
                for (size_t i = 0; i < final_board_.size(); i++) {
                        if (i > 0) ss << " ";
                        ss << final_board_[i];
                }
                ss << "]\n";
        }

        for (const auto &p : players_) {
                if (p.busted) continue;
                ss << "Seat " << (p.seat + 1) << ": " << p.name;
                if (p.seat == dealer_seat_) ss << " (button)";
                if (p.seat == sb_seat) ss << " (small blind)";
                if (p.seat == bb_seat) ss << " (big blind)";

                bool won = false;
                int amount_won = 0;
                for (size_t k = 0; k < winners_.size(); k++) {
                        if (winners_[k] == p.seat) {
                                won = true;
                                amount_won += win_amounts_[k];
                        }
                }
                if (won) {
                        ss << " collected (" << amount_won << ")";
                } else {
                        ss << " folded";
                }
                ss << "\n";
        }
        ss << "\n";

        return ss.str();
}

bool
PokerStarsExporter::save_to_file(const std::string &dir_path, const std::string &filename) const
{
        if (dir_path.empty()) return false;
        mkdir(dir_path.c_str(), 0755);

        std::string filepath = dir_path;
        if (filepath.back() != '/') filepath += "/";
        filepath += filename;

        std::ofstream ofs(filepath, std::ios::out | std::ios::app);
        if (!ofs.is_open()) return false;
        ofs << format_hand_history();
        return true;
}
