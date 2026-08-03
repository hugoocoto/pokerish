#include "pokerstars_export.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

static int g_failures = 0;

#define CHECK(cond)                                                             \
        do {                                                                    \
                if (!(cond)) {                                                  \
                        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                                #cond);                                         \
                        g_failures++;                                           \
                }                                                               \
        } while (0)

#define CHECK_EQ(a, b)                                                                  \
        do {                                                                            \
                if ((a) != (b)) {                                                       \
                        fprintf(stderr, "FAIL %s:%d: %s != %s (%s != %s)\n", __FILE__,  \
                                __LINE__, #a, #b, std::to_string(a).c_str(),            \
                                std::to_string(b).c_str());                             \
                        g_failures++;                                                   \
                }                                                                       \
        } while (0)

static void
test_exporter_cash_format()
{
        Deck deck;
        Table t(&deck);
        for (int i = 0; i < 6; i++) {
                t.add_player(Player("Player_" + std::to_string(i)));
        }

        Game_State s;
        s.tournament  = false;
        s.small_blind = 5;
        s.big_blind   = 10;
        s.dealer      = 0;
        s.max_players = 6;

        PokerStarsExporter exp;
        exp.start_hand(101, s, t, "2026/08/03 00:00:00 ET");
        exp.record_action(3, "Player_3", "call", 10, 10, false, PREFLOP);
        exp.record_action(4, "Player_4", "raise", 20, 30, false, PREFLOP);
        exp.record_action(5, "Player_5", "fold", 0, 0, false, PREFLOP);

        phevaluator::Card c1 = phevaluator::Card("Ah");
        phevaluator::Card c2 = phevaluator::Card("Kd");
        phevaluator::Card c3 = phevaluator::Card("7c");
        t.common = { c1, c2, c3 };
        exp.record_stage(FLOP, t.common);

        t.pot         = 85;
        t.winners     = { 4 };
        t.win_amount  = { 85 };
        t.result_text = "Player_4 wins 85";
        exp.finish_hand(s, t);

        std::string txt = exp.format_hand_history();
        CHECK(txt.find("PokerStars Hand #101: Hold'em No Limit (5/10 USD)") != std::string::npos);
        CHECK(txt.find("Table '1' 6-max Seat #1 is the button") != std::string::npos);
        CHECK(txt.find("Player_3: calls 10") != std::string::npos);
        CHECK(txt.find("Player_4: raises 20 to 30") != std::string::npos);
        CHECK(txt.find("*** FLOP *** [Ah Kd 7c]") != std::string::npos);
        CHECK(txt.find("Player_4 collected 85 from pot") != std::string::npos);
        CHECK(txt.find("*** SUMMARY ***") != std::string::npos);
}

static void
test_exporter_tournament_file_save()
{
        Deck deck;
        Table t(&deck);
        for (int i = 0; i < 4; i++) {
                t.add_player(Player("Bot_" + std::to_string(i)));
        }

        Game_State s;
        s.tournament  = true;
        s.level       = 2;
        s.small_blind = 10;
        s.big_blind   = 20;
        s.dealer      = 1;
        s.max_players = 4;

        PokerStarsExporter exp;
        exp.start_hand(202, s, t, "2026/08/03 00:05:00 ET");
        exp.record_action(2, "Bot_2", "fold", 0, 0, false, PREFLOP);
        exp.record_action(3, "Bot_3", "call", 20, 20, false, PREFLOP);

        t.pot         = 50;
        t.winners     = { 3 };
        t.win_amount  = { 50 };
        t.result_text = "Bot_3 wins 50";
        exp.finish_hand(s, t);

        std::string txt = exp.format_hand_history();
        CHECK(txt.find("PokerStars Hand #202: Tournament #1, Hold'em No Limit - Level II (10/20)") != std::string::npos);

        std::string test_dir = "test_hands_tmp";
        bool saved = exp.save_to_file(test_dir, "test_hand.txt");
        CHECK(saved);

        std::ifstream ifs(test_dir + "/test_hand.txt");
        CHECK(ifs.is_open());
        std::string file_content((std::istreambuf_iterator<char>(ifs)),
                                 std::istreambuf_iterator<char>());
        CHECK(file_content.find("PokerStars Hand #202:") != std::string::npos);
}

int
main()
{
        test_exporter_cash_format();
        test_exporter_tournament_file_save();

        if (g_failures == 0) {
                printf("test_pokerstars_export: all tests passed\n");
                return 0;
        } else {
                printf("test_pokerstars_export: %d test(s) failed\n", g_failures);
                return 1;
        }
}
