#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "card.h"
#include "poker.h"
#include "raylib.h"

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
            int x, int y, int screen_h, double now)
{
        const Player &p = table.players[idx];
        if (!p.hand.has_value()) return;

        const Card &c0 = themed_cards.at(int(p.hand->at(0)));
        int cw         = c0.get_w();
        int ch         = c0.get_h();
        int margin     = 8;

        draw_card(p.hand->at(0), x, y);
        draw_card(p.hand->at(1), x + cw + margin, y);

        int nw = MeasureText(p.name.c_str(), 20);
        int tx = x + cw - nw / 2;
        DrawText(p.name.c_str(), tx, y - 4 - 20, 20, WHITE);

        if (idx == (size_t) state.dealer) {
                int cx = x - 12;
                int cy = y - 12;
                DrawCircle(cx, cy, 10, WHITE);
                DrawText("D", cx - MeasureText("D", 20) / 2, cy - 10, 20, BLACK);
        }

        size_t sb = (state.dealer + 1) % table.players.size();
        size_t bb = (state.dealer + 2) % table.players.size();
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
        int banner_w  = 2 * cw + margin;
        int banner_cx = x + (2 * cw + margin) / 2;

        if (p._fold) {
                Color bg = BLACK;
                bg.a     = bg.a * 0.75;
                tx       = banner_cx - MeasureText("Fold", 20) / 2;
                DrawRectangle(x, y + ch / 2 - 15, banner_w, 30, bg);
                DrawText("Fold", tx, y + ch / 2 - 10, 20, WHITE);
                DrawRectangleLines(x, y + ch / 2 - 15, banner_w, 30, WHITE);
        } else if (p.is_my_turn) {
                float len = banner_w * (ActionTimeOut - (now - p.action_start)) / ActionTimeOut;
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
                case Player::Response::FOLD: bg = BLACK; snprintf(buf, sizeof(buf) - 1, "Fold"); break;
                case Player::Response::CHECK: bg = DARKGRAY; snprintf(buf, sizeof(buf) - 1, "Check"); break;
                case Player::Response::CALL: bg = BLUE; snprintf(buf, sizeof(buf) - 1, "Call %d", p.last_action.amount); break;
                case Player::Response::CALL_ALL: bg = RED; snprintf(buf, sizeof(buf) - 1, "All-in %d", p.last_action.amount); break;
                case Player::Response::BET: bg = ORANGE; snprintf(buf, sizeof(buf) - 1, "Bet %d", p.last_action.amount); break;
                }
                bg.a = bg.a * 0.75;
                tx   = banner_cx - MeasureText(buf, 20) / 2;
                DrawRectangle(x, y + ch / 2 - 15, banner_w, 30, bg);
                DrawText(buf, tx, y + ch / 2 - 10, 20, WHITE);
                DrawRectangleLines(x, y + ch / 2 - 15, banner_w, 30, WHITE);
        }
}

static void
draw_table(Table &table, Game_State &state, int w, int h, double now)
{
        assert(table.players.size() == (size_t) MaxPlayers);

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
                draw_player(table, state, i, px[i], py[i], h, now);
        }

        // common card slots
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
        DrawText(buf, tx, my - 40, 20, YELLOW);

        if (state.current_bet > 0 && state.stage != OVER) {
                snprintf(buf, sizeof(buf) - 1, "Current bet: %d", state.current_bet);
                tx = (w - MeasureText(buf, 20)) / 2;
                DrawText(buf, tx, my - 64, 20, GRAY);
        }

        static const char *stage_names[] = { "Preflop", "Flop", "Turn", "River", "Hand over" };
        if (state.stage >= PREFLOP && state.stage <= OVER) {
                DrawText(stage_names[state.stage], 10, 10, 20, DARKGRAY);
        }

        if (state.stage == OVER) {
                DrawText(table.result_text.c_str(),
                         (w - MeasureText(table.result_text.c_str(), 20)) / 2,
                         my - 100, 20, GREEN);
                const char *hint = "Press SPACE or N to start the next hand";
                DrawText(hint, (w - MeasureText(hint, 20)) / 2, my + ch + margin, 20, GRAY);
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

void
game_loop(Table *table)
{
        static Game_State state;

        while (!WindowShouldClose()) {
                if (IsWindowResized()) {
                        ctx.height = GetScreenHeight();
                        ctx.width  = GetScreenWidth();
                }

                double now = GetTime();

                if (state.stage == OVER && (IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_N))) {
                        table->end_hand(&state);
                }

                table->step_game(&state, now);

                BeginDrawing();
                {
                        ClearBackground(BLACK);
                        draw_table(*table, state, ctx.width, ctx.height, now);
                }
                EndDrawing();
        }
}

int
main(int argc, char **argv)
{
        Deck deck   = Deck();
        Table table = Table(&deck);

        table.add_player(Player("You"));
        table.add_player(Player("Bot 1"));
        table.add_player(Player("Bot 2"));
        table.add_player(Player("Bot 3"));
        table.add_player(Player("Bot 4"));
        table.add_player(Player("Bot 5"));

        build_themed_cards(themed_cards, "decks/jorels");

        SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
        SetTraceLogLevel(LOG_WARNING);
        InitWindow(ctx.width, ctx.height, ctx.title);

        // cd to the executable path so relative paths works fine
        ChangeDirectory(GetApplicationDirectory());

        game_loop(&table);

        return 0;
        (void) argc, (void) argv;
}
