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
        int width{ 800 };
        int height{ 600 };
        const char *title{ "Poker [Croupier]" };
} ctx;

static std::map<int, Card> themed_cards;

static void
draw_card(const phevaluator::Card &pc, int x, int y)
{
        themed_cards.at(int(pc)).draw(x, y);
}

static void
draw_player(const Table &table, const Game_State &state, size_t idx,
            int x, int y, int screen_h, double now, const std::string &waiting_name)
{
        const Player &p = table.players[idx];
        int cw          = default_card_size.width;
        int ch          = default_card_size.height;
        int margin      = 8;
        int banner_w    = 2 * cw + margin;

        bool is_winner = (state.tournament_status == T_FINISHED && !p.busted);

        if (p.busted) {
                // empty seat (lobby) or eliminated player: two card outlines + name (OUT)
                std::string label = p.name.empty() ? "Empty" : (p.name + " (OUT)");
                int tx            = x + (banner_w - MeasureText(label.c_str(), 20)) / 2;
                DrawText(label.c_str(), tx, y - 4 - 20, 20, GRAY);

                DrawRectangleLinesEx({ .x = (float) x, .y = (float) y, .width = (float) cw, .height = (float) ch }, 2, DARKGRAY);
                DrawRectangleLinesEx({ .x = (float) (x + cw + margin), .y = (float) y, .width = (float) cw, .height = (float) ch }, 2, DARKGRAY);
                return;
        }

        if (!p.hand.has_value()) {
                std::string label = is_winner ? (p.name + " (WINNER)") : p.name;
                int nw = MeasureText(label.c_str(), 20);
                int tx = x + cw - nw / 2;
                DrawText(label.c_str(), tx, y - 4 - 20, 20, is_winner ? GOLD : WHITE);

                DrawRectangleLinesEx({ .x = (float) x, .y = (float) y, .width = (float) cw, .height = (float) ch }, 2, is_winner ? GOLD : DARKGRAY);
                DrawRectangleLinesEx({ .x = (float) (x + cw + margin), .y = (float) y, .width = (float) cw, .height = (float) ch }, 2, is_winner ? GOLD : DARKGRAY);

                char sbuf[64] = { 0 };
                snprintf(sbuf, sizeof(sbuf) - 1, "Chips: %d", p.stack);
                int stx = x + cw - MeasureText(sbuf, 20) / 2;
                DrawText(sbuf, stx, y + ch + 4 + 20, 20, is_winner ? GOLD : GRAY);
                return;
        }

        draw_card(p.hand->at(0), x, y);
        draw_card(p.hand->at(1), x + cw + margin, y);
        if (is_winner) {
                DrawRectangleLinesEx({ .x = (float) x - 2, .y = (float) y - 2, .width = (float) (2 * cw + margin + 4), .height = (float) (ch + 4) }, 2, GOLD);
        }

        std::string display_name = is_winner ? (p.name + " (WINNER)") : p.name;
        int nw = MeasureText(display_name.c_str(), 20);
        int tx = x + cw - nw / 2;
        DrawText(display_name.c_str(), tx, y - 4 - 20, 20, is_winner ? GOLD : WHITE);

        if (!waiting_name.empty()) {
                char wbuf[64] = { 0 };
                snprintf(wbuf, sizeof(wbuf) - 1, "Waiting: %s", waiting_name.c_str());
                int wtx = x + cw - MeasureText(wbuf, 20) / 2;
                DrawText(wbuf, wtx, y - 4 - 40, 20, ORANGE);
        }

        if (idx == (size_t) state.dealer) {
                int cx = x - 13;
                int cy = y - 13;
                DrawCircleLines(cx, cy, 12, WHITE);
                DrawText("D", cx - MeasureText("D", 20) / 2, cy - 10, 20, WHITE);
        }

        std::pair<int, int> bl = table.blind_seats(state.dealer);
        size_t sb = (size_t) bl.first;
        size_t bb = (size_t) bl.second;
        if (idx == sb) DrawText("SB", tx - 34, y - 4 - 20, 20, RED);
        if (idx == bb) DrawText("BB", tx + nw + 12, y - 4 - 20, 20, RED);

        char buf[64] = { 0 };

        if (p.hand_value() > 0) {
                snprintf(buf, sizeof(buf) - 1, "%s: %s",
                         p.describe_category().c_str(),
                         p.describe_rank().c_str());
                tx = x + cw - MeasureText(buf, 20) / 2;
                DrawText(buf, tx, y + ch + 4, 20, GRAY);
        }

        snprintf(buf, sizeof(buf) - 1, "Chips: %d", p.stack);
        tx = x + cw - MeasureText(buf, 20) / 2;
        DrawText(buf, tx, y + ch + 4 + 20, 20, GRAY);

        int betmargin = 10;
        snprintf(buf, sizeof(buf) - 1, "Bet: %d", p._bet);
        tx = x + cw - MeasureText(buf, 20) / 2;
        if (y > screen_h / 2) {
                DrawText(buf, tx, y - (4 + 20) * 2 - betmargin, 20, ORANGE);
        } else {
                DrawText(buf, tx, y + ch + 4 * 3 + 20 * 3 + betmargin, 20, ORANGE);
        }

        // banners span both cards, including the margin between them
        int banner_cx = x + (2 * cw + margin) / 2;

        if (p._fold) {
                // soft dark tint so hole cards/card backs remain 100% visible underneath
                DrawRectangle(x, y, banner_w, ch, Color{ 0, 0, 0, 110 });
                tx = banner_cx - MeasureText("Fold", 20) / 2;
                DrawRectangle(x, y + ch / 2 - 15, banner_w, 30, Color{ 0, 0, 0, 180 });
                DrawText("Fold", tx, y + ch / 2 - 10, 20, WHITE);
                DrawRectangleLines(x, y + ch / 2 - 15, banner_w, 30, WHITE);
        } else if (p.is_my_turn) {
                float len = banner_w * (table.action_timeout - (now - p.action_start)) /
                            table.action_timeout;
                Color bg  = GREEN;
                Color bg1 = BLACK;
                bg.a      = bg.a * 0.75;
                bg1.a     = bg1.a * 0.75;
                tx        = banner_cx - MeasureText("Action", 20) / 2;
                DrawRectangle(x, y + ch / 2 - 15, banner_w, 30, bg1);
                if (len > 0) DrawRectangle(x, y + ch / 2 - 15, len, 30, bg);
                DrawText("Action", tx, y + ch / 2 - 10, 20, WHITE);
                DrawRectangleLines(x, y + ch / 2 - 15, banner_w, 30, WHITE);
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
                tx   = banner_cx - MeasureText(buf, 20) / 2;
                DrawRectangle(x, y + ch / 2 - 15, banner_w, 30, bg);
                DrawText(buf, tx, y + ch / 2 - 10, 20, WHITE);
                DrawRectangleLines(x, y + ch / 2 - 15, banner_w, 30, WHITE);
        }
}

static void
draw_table(const Table &table, const Game_State &state, int w, int h, double now,
           const std::vector<std::string> &waiting)
{
        assert(table.players.size() <= (size_t) MaxPlayers);

        float cw = default_card_size.width;
        float ch = default_card_size.height;

        int px[MaxPlayers];
        int py[MaxPlayers];
        px[0] = cw / 2;
        py[0] = ch / 2;
        px[1] = w / 2 - cw;
        py[1] = ch / 2;
        px[2] = w - cw * 2 - cw / 2;
        py[2] = ch / 2;
        px[3] = w - cw * 2 - cw / 2;
        py[3] = h - ch - ch / 2;
        px[4] = w / 2 - cw;
        py[4] = h - ch - ch / 2;
        px[5] = cw / 2;
        py[5] = h - ch - ch / 2;

        for (size_t i = 0; i < table.players.size(); i++) {
                draw_player(table, state, i, px[i], py[i], h, now,
                            i < waiting.size() ? waiting[i] : std::string());
        }
        int margin = 8;
        float my   = (h - ch) / 2;
        float mx   = (w - cw * 5 - margin * 4) / 2;

        for (int i = 0; i < 5; i++) {
                DrawRectangleLinesEx({ .x = mx + (cw + margin) * i, .y = my, .width = cw, .height = ch }, 3, GRAY);
                if ((int) table.common.size() > i) {
                        draw_card(table.common[i], mx + (cw + margin) * i, my);
                }
        }

        char buf[128] = { 0 };
        snprintf(buf, sizeof(buf) - 1, "Pot: %d", table.pot);
        int tx = (w - MeasureText(buf, 20)) / 2;
        DrawText(buf, tx, my - 40, 20, ORANGE);

        if (state.current_bet > 0 && state.stage != OVER) {
                snprintf(buf, sizeof(buf) - 1, "Current bet: %d", state.current_bet);
                tx = (w - MeasureText(buf, 20)) / 2;
                DrawText(buf, tx, my - 64, 20, GRAY);
        }

        if (state.stage == OVER) {
                DrawText(table.result_text.c_str(),
                         (w - MeasureText(table.result_text.c_str(), 20)) / 2,
                         my - 100, 20, WHITE);
        }

        // tournament HUD: level, time left in the level, blinds, players left
        if (state.tournament) {
                int alive = table.alive_count();
                int secs = (int) state.level_remaining;

                int hud_x = 15;
                int hud_y = (h - 62) / 2;
                DrawRectangle(hud_x - 6, hud_y - 4, 122, 62, Color{ 20, 20, 20, 200 });
                DrawRectangleLines(hud_x - 6, hud_y - 4, 122, 62, DARKGRAY);

                snprintf(buf, sizeof(buf) - 1, "Level %d  %d:%02d", state.level, secs / 60,
                         secs % 60);
                DrawText(buf, hud_x, hud_y, 16, WHITE);

                if (state.ante > 0) {
                        snprintf(buf, sizeof(buf) - 1, "Blinds %d/%d A:%d",
                                 state.small_blind, state.big_blind, state.ante);
                } else {
                        snprintf(buf, sizeof(buf) - 1, "Blinds %d/%d", state.small_blind,
                                 state.big_blind);
                }
                DrawText(buf, hud_x, hud_y + 20, 14, GRAY);

                snprintf(buf, sizeof(buf) - 1, "Players %d/%d", alive, MaxPlayers);
                DrawText(buf, hud_x, hud_y + 38, 14, GRAY);

                // full-screen overlays for the tournament lifecycle
                if (state.tournament_status == T_LOBBY) {
                        snprintf(buf, sizeof(buf) - 1, "Waiting for players: %d/%d", alive,
                                 MaxPlayers);
                        DrawText(buf, (w - MeasureText(buf, 40)) / 2, h / 2 - 60, 40, YELLOW);
                } else if (state.tournament_status == T_COUNTDOWN) {
                        snprintf(buf, sizeof(buf) - 1, "Tournament starts in %.1fs",
                                 state.countdown_remaining);
                        DrawText(buf, (w - MeasureText(buf, 40)) / 2, h / 2 - 60, 40, YELLOW);
                } else if (state.tournament_status == T_FINISHED) {
                        int box_w = 420;
                        int box_h = 170;
                        int box_x = (w - box_w) / 2;
                        int box_y = (h - box_h) / 2;

                        DrawRectangle(box_x, box_y, box_w, box_h, Color{ 15, 15, 20, 235 });
                        DrawRectangleLines(box_x, box_y, box_w, box_h, GOLD);

                        const char *hdr = "TOURNAMENT OVER";
                        DrawText(hdr, box_x + (box_w - MeasureText(hdr, 28)) / 2, box_y + 16, 28, GOLD);

                        const Player *winner = nullptr;
                        for (const Player &p : table.players) {
                                if (!p.busted) winner = &p;
                        }

                        std::string wname = winner ? winner->name : "Champion";
                        snprintf(buf, sizeof(buf) - 1, "Winner: %s", wname.c_str());
                        DrawText(buf, box_x + (box_w - MeasureText(buf, 24)) / 2, box_y + 54, 24, WHITE);

                        int award = winner ? winner->stack : 0;
                        if (award > 0) {
                                snprintf(buf, sizeof(buf) - 1, "Earnings: %d Chips", award);
                        } else {
                                snprintf(buf, sizeof(buf) - 1, "Winner takes all!");
                        }
                        DrawText(buf, box_x + (box_w - MeasureText(buf, 22)) / 2, box_y + 88, 22, ORANGE);

                        snprintf(buf, sizeof(buf) - 1, "Final Level: Level %d (Blinds %d/%d)", state.level, state.small_blind, state.big_blind);
                        DrawText(buf, box_x + (box_w - MeasureText(buf, 18)) / 2, box_y + 124, 18, GRAY);
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
        const char *export_dir = "hands";
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
                } else if ((strcmp(argv[i], "--max-players") == 0 || strcmp(argv[i], "--table-size") == 0) && i + 1 < argc) {
                        max_players = atoi(argv[++i]);
                        if (max_players < 2 || max_players > 6) {
                                fprintf(stderr, "error: --max-players must be in [2, 6]\n");
                                return 1;
                        }
                } else if ((strcmp(argv[i], "--export-dir") == 0 || strcmp(argv[i], "--hand-history") == 0) && i + 1 < argc) {
                        export_dir = argv[++i];
                } else {
                        fprintf(stderr,
                                "usage: %s [--port N] [--host IP] [--token SECRET]\n"
                                "       [--headless] [--tournament] [--level-seconds N]\n"
                                "       [--countdown-seconds N] [--start-stack N]\n"
                                "       [--max-players N] [--export-dir DIR]\n",
                                argv[0]);
                        return 1;
                }
        }

        Server srv(port, host, token, /*verbose=*/true, tournament, level_seconds,
                   countdown_seconds, start_stack, max_players, export_dir);

        if (headless) {
                srv.run();
                return 0;
        }

        build_themed_cards(themed_cards, "decks/jorels");

        SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
        SetTraceLogLevel(LOG_WARNING);
        InitWindow(ctx.width, ctx.height, ctx.title);

        // cd to the executable path so relative paths works fine; when built
        // into server/build/ the deck images live up to two levels up
        ChangeDirectory(GetApplicationDirectory());
        for (int i = 0; i < 4 && !DirectoryExists("decks"); i++) {
                ChangeDirectory("..");
        }

        while (!WindowShouldClose()) {
                if (IsWindowResized()) {
                        ctx.height = GetScreenHeight();
                        ctx.width  = GetScreenWidth();
                }

                double now = GetTime();
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
