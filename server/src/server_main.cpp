#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include "card.h"
#include "poker.h"
#include "raylib.h"
#include "server.h"

struct {
        int width{ 1280 };
        int height{ 720 };
        const char *title{ "Poker [Croupier]" };
} ctx;

static std::map<int, Card> themed_cards;

static void
draw_card(const phevaluator::Card &pc, int x, int y)
{
        themed_cards.at(int(pc)).draw(x, y);
}

static void
draw_player_bet(const Table &table, size_t idx, int x, int y)
{
        const Player &p = table.players[idx];
        if (p.busted) return;
        int cw = sc((int) default_card_size.width);
        int ch = sc((int) default_card_size.height);

        char buf[64] = { 0 };
        snprintf(buf, sizeof(buf) - 1, "Bet: %d", p._bet);
        int tx = x + cw - MeasureText(buf, sc(20)) / 2;
        // Chips line sits at y + ch + sc(4) + sc(20); place the bet label
        // one full line (sc(20)) below that so it never overlaps the chips.
        int by = y + ch + sc(4) + sc(20) + sc(4) + sc(20);
        DrawText(buf, tx, by, sc(20), ORANGE);

}

static void
draw_player(const Table &table, const Game_State &state, size_t idx,
            int x, int y, double now, const std::string &waiting_name)
{
        const Player &p = table.players[idx];
        int cw          = sc((int) default_card_size.width);
        int ch          = sc((int) default_card_size.height);
        int margin      = sc(8);
        int banner_w    = 2 * cw + margin;

        bool is_winner = (state.tournament_status == T_FINISHED && !p.busted);

        if (p.busted) {
                // empty seat (lobby) or eliminated player: two card outlines + name (OUT)
                std::string label = p.name.empty() ? "Empty" : (p.name + " (OUT)");
                int tx            = x + (banner_w - MeasureText(label.c_str(), sc(20))) / 2;
                DrawText(label.c_str(), tx, y - sc(4) - sc(20), sc(20), GRAY);

                DrawRectangleLinesEx({ .x = (float) x, .y = (float) y, .width = (float) cw, .height = (float) ch }, 2, DARKGRAY);
                DrawRectangleLinesEx({ .x = (float) (x + cw + margin), .y = (float) y, .width = (float) cw, .height = (float) ch }, 2, DARKGRAY);
                return;
        }

        if (!p.hand.has_value()) {
                std::string label = is_winner ? (p.name + " (WINNER)") : p.name;
                int nw = MeasureText(label.c_str(), sc(20));
                int tx = x + cw - nw / 2;
                DrawText(label.c_str(), tx, y - sc(4) - sc(20), sc(20), is_winner ? GOLD : WHITE);

                DrawRectangleLinesEx({ .x = (float) x, .y = (float) y, .width = (float) cw, .height = (float) ch }, 2, is_winner ? GOLD : DARKGRAY);
                DrawRectangleLinesEx({ .x = (float) (x + cw + margin), .y = (float) y, .width = (float) cw, .height = (float) ch }, 2, is_winner ? GOLD : DARKGRAY);

                char sbuf[64] = { 0 };
                snprintf(sbuf, sizeof(sbuf) - 1, "Chips: %d", p.stack);
                int stx = x + cw - MeasureText(sbuf, sc(20)) / 2;
                DrawText(sbuf, stx, y + ch + sc(4) + sc(20), sc(20), is_winner ? GOLD : GRAY);
                return;
        }

        draw_card(p.hand->at(0), x, y);
        draw_card(p.hand->at(1), x + cw + margin, y);
        if (is_winner) {
                DrawRectangleLinesEx({ .x = (float) x - sc(2), .y = (float) y - sc(2), .width = (float) (2 * cw + margin + sc(4)), .height = (float) (ch + sc(4)) }, 2, GOLD);
        }

        std::string display_name = is_winner ? (p.name + " (WINNER)") : p.name;
        int nw = MeasureText(display_name.c_str(), sc(20));
        int tx = x + cw - nw / 2;
        DrawText(display_name.c_str(), tx, y - sc(4) - sc(20), sc(20), is_winner ? GOLD : WHITE);

        if (!waiting_name.empty()) {
                char wbuf[64] = { 0 };
                snprintf(wbuf, sizeof(wbuf) - 1, "Waiting: %s", waiting_name.c_str());
                int wtx = x + cw - MeasureText(wbuf, sc(20)) / 2;
                DrawText(wbuf, wtx, y - sc(4) - sc(40), sc(20), ORANGE);
        }

        if (idx == (size_t) state.dealer) {
                int cx = x - sc(13);
                int cy = y - sc(13);
                DrawCircleLines(cx, cy, sc(12), WHITE);
                DrawText("D", cx - MeasureText("D", sc(20)) / 2, cy - sc(10), sc(20), WHITE);
        }

        std::pair<int, int> bl = table.blind_seats(state.dealer);
        size_t sb = (size_t) bl.first;
        size_t bb = (size_t) bl.second;
        if (idx == sb) DrawText("SB", tx - sc(34), y - sc(4) - sc(20), sc(20), RED);
        if (idx == bb) DrawText("BB", tx + nw + sc(12), y - sc(4) - sc(20), sc(20), RED);

        char buf[64] = { 0 };

        if (p.hand_value() > 0) {
                snprintf(buf, sizeof(buf) - 1, "%s: %s",
                         p.describe_category().c_str(),
                         p.describe_rank().c_str());
                tx = x + cw - MeasureText(buf, sc(20)) / 2;
                DrawText(buf, tx, y + ch + sc(4), sc(20), GRAY);
        }

        snprintf(buf, sizeof(buf) - 1, "Chips: %d", p.stack);
        tx = x + cw - MeasureText(buf, sc(20)) / 2;
        DrawText(buf, tx, y + ch + sc(4) + sc(20), sc(20), GRAY);

        // banners span both cards, including the margin between them
        int banner_cx = x + (2 * cw + margin) / 2;

        if (p._fold) {
                // soft dark tint so hole cards/card backs remain 100% visible underneath
                DrawRectangle(x, y, banner_w, ch, Color{ 0, 0, 0, 110 });
                tx = banner_cx - MeasureText("Fold", sc(20)) / 2;
                DrawRectangle(x, y + ch / 2 - sc(15), banner_w, sc(30), Color{ 0, 0, 0, 180 });
                DrawText("Fold", tx, y + ch / 2 - sc(10), sc(20), WHITE);
                DrawRectangleLines(x, y + ch / 2 - sc(15), banner_w, sc(30), WHITE);
        } else if (p.is_my_turn) {
                float len = banner_w * (table.action_timeout - (now - p.action_start)) /
                            table.action_timeout;
                Color bg  = GREEN;
                Color bg1 = BLACK;
                bg.a      = bg.a * 0.75;
                bg1.a     = bg1.a * 0.75;
                tx        = banner_cx - MeasureText("Action", sc(20)) / 2;
                DrawRectangle(x, y + ch / 2 - sc(15), banner_w, sc(30), bg1);
                if (len > 0) DrawRectangle(x, y + ch / 2 - sc(15), len, sc(30), bg);
                DrawText("Action", tx, y + ch / 2 - sc(10), sc(20), WHITE);
                DrawRectangleLines(x, y + ch / 2 - sc(15), banner_w, sc(30), WHITE);
        } else if (p.last_action.type != Player::Response::NONE) {
                Color bg = BLACK;
                switch (p.last_action.type) {
                case Player::Response::NONE: break;
                case Player::Response::FOLD:
                        bg = BLACK;
                        snprintf(buf, sizeof(buf) - 1, "Fold");
                        break;
                case Player::Response::CHECK:
                        bg = DARKGRAY;
                        snprintf(buf, sizeof(buf) - 1, "Check");
                        break;
                case Player::Response::CALL:
                        bg = BLUE;
                        snprintf(buf, sizeof(buf) - 1, "Call %d", p.last_action.amount);
                        break;
                case Player::Response::CALL_ALL:
                        bg = RED;
                        snprintf(buf, sizeof(buf) - 1, "All-in %d", p.last_action.amount);
                        break;
                case Player::Response::BET:
                        bg = ORANGE;
                        snprintf(buf, sizeof(buf) - 1, "Bet %d", p.last_action.amount);
                        break;
                }
                bg.a = bg.a * 0.75;
                tx   = banner_cx - MeasureText(buf, sc(20)) / 2;
                DrawRectangle(x, y + ch / 2 - sc(15), banner_w, sc(30), bg);
                DrawText(buf, tx, y + ch / 2 - sc(10), sc(20), WHITE);
                DrawRectangleLines(x, y + ch / 2 - sc(15), banner_w, sc(30), WHITE);
        }
}

static void
draw_table(const Table &table, const Game_State &state, int w, int h, double now,
           const std::vector<std::string> &waiting)
{
        assert(table.players.size() <= (size_t) MaxPlayers);

        float cw = scf(default_card_size.width);
        float ch = scf(default_card_size.height);

        int px[MaxPlayers];
        int py[MaxPlayers];

        // seats around an ellipse: seat 0 at the top, going clockwise;
        // one formula covers every table size from 2 to MaxPlayers. The
        // ellipse point is the seat's centre: half the seat block is
        // subtracted so (px, py) is the top-left corner draw_player wants.
        // The ring is deliberately wider than tall (flatter) so seats get
        // more horizontal and less vertical separation.
        const int N     = (int) table.players.size();
        const float pad = scf(40.0f); // room for the name label above each seat
        const float bw  = 2.0f * cw + scf(8.0f);
        const float rx  = w / 2.0f - cw - pad * 0.25f;
        const float ry  = h / 2.0f - ch - pad * 1.5f;
        for (int i = 0; i < N; i++) {
                float a = (-90.0f + i * 360.0f / N) * DEG2RAD;
                px[i]   = (int) (w / 2.0f + rx * cosf(a) - bw / 2.0f);
                py[i]   = (int) (h / 2.0f + ry * sinf(a) - ch / 2.0f);
        }

        for (size_t i = 0; i < table.players.size(); i++) {
                draw_player(table, state, i, px[i], py[i], now,
                            i < waiting.size() ? waiting[i] : std::string());
        }
        int margin = sc(8);
        float my   = (h - ch) / 2;
        float mx   = (w - cw * 5 - margin * 4) / 2;

        for (int i = 0; i < 5; i++) {
                DrawRectangleLinesEx({ .x = mx + (cw + margin) * i, .y = my, .width = cw, .height = ch }, sc(3), GRAY);
                if ((int) table.common.size() > i) {
                        draw_card(table.common[i], mx + (cw + margin) * i, my);
                }
        }

        char buf[128] = { 0 };
        snprintf(buf, sizeof(buf) - 1, "Pot: %d", table.pot);
        int tx = (w - MeasureText(buf, sc(20))) / 2;
        DrawText(buf, tx, my - sc(40), sc(20), ORANGE);

        if (state.current_bet > 0 && state.stage != OVER) {
                snprintf(buf, sizeof(buf) - 1, "Current bet: %d", state.current_bet);
                tx = (w - MeasureText(buf, sc(20))) / 2;
                DrawText(buf, tx, my - sc(64), sc(20), GRAY);
        }

        if (state.stage == OVER) {
                DrawText(table.result_text.c_str(),
                         (w - MeasureText(table.result_text.c_str(), sc(20))) / 2,
                         my - sc(100), sc(20), WHITE);
        }

        // bets are drawn after the board so the ones that land on its edge
        // (center-column seats) stay visible instead of being covered
        for (size_t i = 0; i < table.players.size(); i++) {
                draw_player_bet(table, i, px[i], py[i]);
        }

        // tournament HUD: level, time left in the level, blinds, players left
        if (state.tournament) {
                int alive = table.alive_count();
                int secs = (int) state.level_remaining;

                int hud_x = sc(15);
                int hud_y = sc(8);
                snprintf(buf, sizeof(buf) - 1, "Level %d  %d:%02d", state.level, secs / 60,
                         secs % 60);
                DrawText(buf, hud_x, hud_y, sc(16), WHITE);

                if (state.ante > 0) {
                        snprintf(buf, sizeof(buf) - 1, "Blinds %d/%d A:%d",
                                 state.small_blind, state.big_blind, state.ante);
                } else {
                        snprintf(buf, sizeof(buf) - 1, "Blinds %d/%d", state.small_blind,
                                 state.big_blind);
                }
                DrawText(buf, hud_x, hud_y + sc(20), sc(14), GRAY);

                snprintf(buf, sizeof(buf) - 1, "Players %d/%d", alive,
                         (int) table.players.size());
                DrawText(buf, hud_x, hud_y + sc(38), sc(14), GRAY);

                // full-screen overlays for the tournament lifecycle
                if (state.tournament_status == T_LOBBY) {
                        snprintf(buf, sizeof(buf) - 1, "Waiting for players: %d/%d", alive,
                                 (int) table.players.size());
                        DrawText(buf, (w - MeasureText(buf, sc(40))) / 2, h / 2 - sc(60), sc(40), YELLOW);
                } else if (state.tournament_status == T_COUNTDOWN) {
                        snprintf(buf, sizeof(buf) - 1, "Tournament starts in %.1fs",
                                 state.countdown_remaining);
                        DrawText(buf, (w - MeasureText(buf, sc(40))) / 2, h / 2 - sc(60), sc(40), YELLOW);
                } else if (state.tournament_status == T_FINISHED) {
                        int box_w = sc(420);
                        int box_h = sc(170);
                        int box_x = (w - box_w) / 2;
                        int box_y = (h - box_h) / 2;

                        DrawRectangle(box_x, box_y, box_w, box_h, Color{ 15, 15, 20, 235 });
                        DrawRectangleLines(box_x, box_y, box_w, box_h, GOLD);

                        const char *hdr = "TOURNAMENT OVER";
                        DrawText(hdr, box_x + (box_w - MeasureText(hdr, sc(28))) / 2, box_y + sc(16), sc(28), GOLD);

                        const Player *winner = nullptr;
                        for (const Player &p : table.players) {
                                if (!p.busted) winner = &p;
                        }

                        std::string wname = winner ? winner->name : "Champion";
                        snprintf(buf, sizeof(buf) - 1, "Winner: %s", wname.c_str());
                        DrawText(buf, box_x + (box_w - MeasureText(buf, sc(24))) / 2, box_y + sc(54), sc(24), WHITE);

                        int award = winner ? winner->stack : 0;
                        if (award > 0) {
                                snprintf(buf, sizeof(buf) - 1, "Earnings: %d Chips", award);
                        } else {
                                snprintf(buf, sizeof(buf) - 1, "Winner takes all!");
                        }
                        DrawText(buf, box_x + (box_w - MeasureText(buf, sc(22))) / 2, box_y + sc(88), sc(22), ORANGE);

                        snprintf(buf, sizeof(buf) - 1, "Final Level: Level %d (Blinds %d/%d)", state.level, state.small_blind, state.big_blind);
                        DrawText(buf, box_x + (box_w - MeasureText(buf, sc(18))) / 2, box_y + sc(124), sc(18), GRAY);
                }
        }
}

static void
build_themed_cards(std::map<int, Card> &out, const std::string &path)
{
        for (std::string suit : { "H", "S", "D", "C" }) {
                for (std::string rank : { "A", "2", "3", "4", "5", "6", "7", "8", "9", "T", "J", "Q", "K" }) {
                        std::string name      = rank + suit;
                        std::string file_rank = rank.compare("T") == 0 ? "10" : rank;
                        Card card             = Card(name);
                        card.change_theme(path + "/" + file_rank + "-" + suit + ".png");
                        out.emplace(int(phevaluator::Card(name)), card);
                }
        }
}

// Pick the largest discrete UI level that fits the window. Level 2.0 is the
// current look (96x128 cards, fonts 20) at a 1280x720 window; the layout
// needs w >= 136*level and h >= 360*level (seat ring clears the board text).
static void
auto_scale(int w, int h)
{
        const float levels[] = { 2.0f, 1.5f, 1.25f, 1.0f, 0.75f, 0.5f };
        const float fit      = std::min(w / 136.0f, h / 360.0f);
        ui_scale             = 0.5f;
        for (float l : levels) {
                if (fit >= l - 0.02f) {
                        ui_scale = l;
                        break;
                }
        }
}

int
main(int argc, char **argv)
{
        int port         = 9000;
        const char *host = "127.0.0.1"; // loopback by default; --host 0.0.0.0 for LAN
        const char *token = nullptr;
        bool headless    = false;
        bool tournament  = false;
        int level_seconds = 300;
        int countdown_seconds = 10;
        int start_stack   = 1000;
        int max_players   = 6;
        int action_timeout = 20;
        double simulate_max = -1.0; // -1 = no --simulate
        double hand_pause = -1.0;   // -1 = not given (simulate default applies)
        const char *tournament_end = nullptr; // stay (default) | exit | restart
        double tournament_end_hold = 5.0;
        bool end_hold_given = false;
        const char *export_dir = "hands";
        float scale_override = 0.0f; // 0 = auto-fit from the window size
        for (int i = 1; i < argc; i++) {
                if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
                        port = atoi(argv[++i]);
                        if (port <= 0 || port > 65535) {
                                fprintf(stderr, "error: --port must be in [1, 65535]\n");
                                return 1;
                        }
                } else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
                        host = argv[++i];
                } else if (strcmp(argv[i], "--token") == 0 && i + 1 < argc) {
                        token = argv[++i];
                } else if (strcmp(argv[i], "--headless") == 0) {
                        headless = true;
                } else if (strcmp(argv[i], "--tournament") == 0) {
                        tournament = true;
                } else if (strcmp(argv[i], "--level-seconds") == 0 && i + 1 < argc) {
                        level_seconds = atoi(argv[++i]);
                        if (level_seconds < 1) {
                                fprintf(stderr, "error: --level-seconds must be >= 1\n");
                                return 1;
                        }
                } else if (strcmp(argv[i], "--countdown-seconds") == 0 && i + 1 < argc) {
                        countdown_seconds = atoi(argv[++i]);
                        if (countdown_seconds < 0) {
                                fprintf(stderr, "error: --countdown-seconds must be >= 0\n");
                                return 1;
                        }
                } else if (strcmp(argv[i], "--start-stack") == 0 && i + 1 < argc) {
                        start_stack = atoi(argv[++i]);
                        if (start_stack < 1) {
                                fprintf(stderr, "error: --start-stack must be >= 1\n");
                                return 1;
                        }
                } else if ((strcmp(argv[i], "--max-players") == 0 || strcmp(argv[i], "--table-size") == 0) &&
                           i + 1 < argc) {
                        max_players = atoi(argv[++i]);
                        if (max_players < 2 || max_players > MaxPlayers) {
                                fprintf(stderr, "error: --max-players must be in [2, %d]\n",
                                        MaxPlayers);
                                return 1;
                        }
                } else if (strcmp(argv[i], "--action-timeout") == 0 && i + 1 < argc) {
                        action_timeout = atoi(argv[++i]);
                        if (action_timeout < 1) {
                                fprintf(stderr, "error: --action-timeout must be >= 1\n");
                                return 1;
                        }
                } else if (strcmp(argv[i], "--simulate") == 0) {
                        // bare --simulate: random 1..3 s think per turn;
                        // --simulate N: random 1..N s
                        if (i + 1 < argc && argv[i + 1][0] != '-') {
                                simulate_max = atof(argv[++i]);
                                if (simulate_max < 1.0) {
                                        fprintf(stderr, "error: --simulate must be >= 1.0 seconds\n");
                                        return 1;
                                }
                        } else {
                                simulate_max = 3.0;
                        }
                } else if (strcmp(argv[i], "--hand-pause") == 0 && i + 1 < argc) {
                        hand_pause = atof(argv[++i]);
                        if (hand_pause < 0.0) {
                                fprintf(stderr, "error: --hand-pause must be >= 0\n");
                                return 1;
                        }
                } else if (strcmp(argv[i], "--tournament-end") == 0 && i + 1 < argc) {
                        tournament_end = argv[++i];
                        if (strcmp(tournament_end, "stay") != 0 &&
                            strcmp(tournament_end, "exit") != 0 &&
                            strcmp(tournament_end, "restart") != 0) {
                                fprintf(stderr, "error: --tournament-end must be stay, exit or restart\n");
                                return 1;
                        }
                } else if (strcmp(argv[i], "--tournament-end-hold") == 0 && i + 1 < argc) {
                        tournament_end_hold = atof(argv[++i]);
                        end_hold_given      = true;
                        if (tournament_end_hold < 0.0) {
                                fprintf(stderr, "error: --tournament-end-hold must be >= 0\n");
                                return 1;
                        }
                } else if ((strcmp(argv[i], "--export-dir") == 0 || strcmp(argv[i], "--hand-history") == 0) && i + 1 < argc) {
                        export_dir = argv[++i];
                } else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
                        scale_override = atof(argv[++i]);
                        if (scale_override < 0.5f || scale_override > 2.0f) {
                                fprintf(stderr, "error: --scale must be in [0.5, 2.0]\n");
                                return 1;
                        }
                } else {
                        fprintf(stderr,
                                "usage: %s [--port N] [--host IP] [--token SECRET]\n"
                                "       [--headless] [--tournament] [--level-seconds N]\n"
                                "       [--countdown-seconds N] [--start-stack N]\n"
                                "       [--max-players N] [--export-dir DIR]\n"
                                "       [--action-timeout N] [--simulate [MAX_SECONDS]]\n"
                                "       [--hand-pause N] [--tournament-end stay|exit|restart]\n"
                                "       [--tournament-end-hold N] [--scale N]\n",
                                argv[0]);
                        return 1;
                }
        }

        if (hand_pause >= 0 && simulate_max < 0) {
                fprintf(stderr, "error: --hand-pause requires --simulate "
                                "(without it the pause between hands is 0)\n");
                return 1;
        }
        if (tournament_end && !tournament) {
                fprintf(stderr, "error: --tournament-end requires --tournament\n");
                return 1;
        }
        if (end_hold_given && (!tournament_end || strcmp(tournament_end, "restart") != 0)) {
                fprintf(stderr, "error: --tournament-end-hold requires --tournament-end restart\n");
                return 1;
        }

        Server srv(port, host, token, /*verbose=*/true, tournament, level_seconds,
                   countdown_seconds, start_stack, max_players, export_dir);
        srv.table().action_timeout = action_timeout;
        if (simulate_max > 0) {
                srv.set_simulate(true, simulate_max);
                srv.hand_pause() = hand_pause >= 0 ? hand_pause : Server::kSimulateHandPause;
        }
        if (tournament_end) {
                Server::TournamentEndMode mode = Server::TournamentEndMode::STAY;
                if (strcmp(tournament_end, "exit") == 0) {
                        mode = Server::TournamentEndMode::EXIT;
                } else if (strcmp(tournament_end, "restart") == 0) {
                        mode = Server::TournamentEndMode::RESTART;
                }
                srv.set_tournament_end(mode, tournament_end_hold);
        }

        if (headless) {
#if !defined(_WIN32)
                // Install signal handlers so Ctrl-C / systemctl stop can
                // shut the server down cleanly (close frames sent to clients,
                // hand-history file flushed) rather than killing it instantly.
                // We store the Server pointer in a global so the handler can
                // call stop() without capturing a local by reference.
                static Server *g_srv_for_signal = nullptr;
                g_srv_for_signal = &srv;
                struct sigaction sa{};
                sa.sa_handler = [](int) { if (g_srv_for_signal) g_srv_for_signal->stop(); };
                sigemptyset(&sa.sa_mask);
                sa.sa_flags = 0;
                sigaction(SIGINT,  &sa, nullptr);
                sigaction(SIGTERM, &sa, nullptr);
#endif
                srv.run();
                return 0;
        }

        build_themed_cards(themed_cards, "decks/jorels");

        SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
        SetTraceLogLevel(LOG_WARNING);
        InitWindow(ctx.width, ctx.height, ctx.title);

        if (scale_override > 0.0f) {
                ui_scale = scale_override;
        } else {
                auto_scale(ctx.width, ctx.height);
        }

        // cd to the executable path so relative paths works fine; when built
        // into server/build/ the deck images live up to two levels up
        ChangeDirectory(GetApplicationDirectory());
        for (int i = 0; i < 4 && !DirectoryExists("decks"); i++) {
                ChangeDirectory("..");
        }

        while (!WindowShouldClose() && !srv.exit_requested()) {
                if (IsWindowResized()) {
                        ctx.height = GetScreenHeight();
                        ctx.width  = GetScreenWidth();
                        if (scale_override <= 0.0f) auto_scale(ctx.width, ctx.height);
                }

                double now = Server::now();
                srv.tick(now);

                BeginDrawing();
                {
                        ClearBackground(BLACK);
                        draw_table(srv.table(), srv.state(), ctx.width, ctx.height, now,
                                   srv.waiting_names());
                }
                EndDrawing();
        }

        return 0;
}
