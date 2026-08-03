// Human-play GUI client: raylib window on top of the bot/example WebSocket
// client (included as a library, see ../bot/example/bot.cpp) speaking the
// API.md protocol. Connects to the server like any bot and lets a human
// play with Fold / Call / Raise buttons at the bottom of the screen.
//
// The table area is inset above the bottom action bar (the bar shrinks the
// inner size). Our own seat is always drawn at the bottom centre; the other
// seats wrap around clockwise from there. Buttons work even when it is not
// our turn: clicking one arms a preselected action (the label shows the
// amount it would use), clicking it again disarms it, and the armed action
// is sent automatically when our turn starts.
//
// Run: ./build/client [--port N] [--host IP] [--token SECRET] [--name NAME]

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>

#include "bot.cpp"
#include "card.h"
#include "raylib.h"

struct {
        int width{ 1280 };
        int height{ 720 };
        const char *title{ "Poker [Client]" };
} win;

static std::map<int, Card> themed_cards;

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

// ---------------------------------------------------------------------------
// Client bookkeeping: seat, hole cards, timer start, hand-over text
// ---------------------------------------------------------------------------

struct ClientData {
        lws_context *ctx{ nullptr };
        Bot *bot{ nullptr };
        int seat{ -1 };
        std::string my_cards[2];
        double my_turn_since{ 0 };
        std::string result_text;
        std::string prev_stage;
        int prev_game_id{ -1 };
        int raise_inc{ 10 };
        Texture2D card_back{};

        // preselected action (armed while not our turn, sent at turn start):
        // "" = none, or the resolved action "fold" | "check" | "call" |
        // "call_all" | "bet" as the button read at click time; disarmed
        // automatically when it no longer matches what the button would do
        std::string pending_action;
        int pending_amount{ 0 };
        // where the armed "bet" amount came from: 0 = none, 1 = raise counter,
        // 2 = POT preset, 3 = ALL-IN preset (the counter keeps being adjusted,
        // but must not overwrite a preset once armed)
        int pending_source{ 0 };
        bool prev_my_turn{ false };
        bool leave_requested{ false };
        int turn_seat{ -1 };         // whose turn per the last state
        double turn_seat_since{ 0 }; // client clock when that seat's turn started
        std::string tournament_winner; // set by tournament_over
        int tournament_award{ 0 };     // set by tournament_over
        std::string tournament_event_text; // temporary banner text for events
        double tournament_event_since{ 0 };
};

static void
request_my_cards(ClientData &cd)
{
        cd.bot->send_json({ { "type", "query" }, { "id", 1 }, { "what", "my_cards" } });
}

// Draw a card by its two-character name ("As", "Td", ...), falling back to
// the card back on any unexpected value from the server. phevaluator::Card's
// constructor throws std::out_of_range on strings it cannot parse ("??"),
// so it is wrapped and the throw lands in the same fallback.
static void
draw_named_card(ClientData &cd, const std::string &name, int x, int y)
{
        if (name.size() == 2) {
                try {
                        auto it = themed_cards.find(int(phevaluator::Card(name)));
                        if (it != themed_cards.end()) {
                                it->second.draw(x, y);
                                return;
                        }
                } catch (const std::out_of_range &) {
                        // fall through to the card back
                }
        }
        DrawTextureEx(cd.card_back, Vector2{ (float) x, (float) y }, 0.0f,
                      ui_scale / 2.0f, WHITE);
}

static void
handle_messages(ClientData &cd)
{
        for (const nlohmann::json &m : cd.bot->take_messages()) {
                std::string type = jval<std::string>(m, "type", "");
                if (type == "welcome") {
                        cd.seat = jval<int>(m, "seat", -1);
                        printf("client: joined as seat %d\n", cd.seat);
                        request_my_cards(cd);
                } else if (type == "reply") {
                        if (jval<std::string>(m, "what", "") == "my_cards" && m.contains("data") &&
                            m["data"].is_object() && m["data"].contains("cards")) {
                                const nlohmann::json &cards = m["data"]["cards"];
                                if (cards.is_array() && cards.size() >= 2 &&
                                    cards[0].is_string() && cards[1].is_string()) {
                                        cd.my_cards[0] = cards[0].get<std::string>();
                                        cd.my_cards[1] = cards[1].get<std::string>();
                                        printf("client: hole cards %s %s\n", cd.my_cards[0].c_str(),
                                               cd.my_cards[1].c_str());
                                }
                        }
                } else if (type == "your_turn") {
                        cd.my_turn_since = Bot::now();
                } else if (type == "hand_over") {
                        cd.result_text = jval<std::string>(m, "result", "");
                } else if (type == "tournament_start") {
                        cd.tournament_event_text = "Tournament Started! Level " +
                                                   std::to_string(jval<int>(m, "level", 1));
                        cd.tournament_event_since = Bot::now();
                        printf("client: tournament started (level %d)\n", jval<int>(m, "level", 1));
                } else if (type == "level") {
                        cd.tournament_event_text = "Level Up! Level " +
                                                   std::to_string(jval<int>(m, "level", 1));
                        cd.tournament_event_since = Bot::now();
                        printf("client: level up -> level %d\n", jval<int>(m, "level", 1));
                } else if (type == "tournament_over") {
                        if (m.contains("winner") && m["winner"].is_object()) {
                                cd.tournament_winner = jval<std::string>(m["winner"], "name", "?");
                        }
                        cd.tournament_award = jval<int>(m, "award", 0);
                        cd.tournament_event_text = "Tournament Over!";
                        cd.tournament_event_since = Bot::now();
                } else if (type == "player_out") {
                        int seat = jval<int>(m, "seat", -1);
                        std::string reason = jval<std::string>(m, "reason", "?");
                        cd.tournament_event_text = "P" + std::to_string(seat) +
                                                   " Out (" + reason + ")";
                        cd.tournament_event_since = Bot::now();
                        printf("client: P%d out (%s)\n", seat, reason.c_str());
                } else if (type == "error") {
                        printf("client: error %s\n", jval<std::string>(m, "code", "?").c_str());
                }
        }
}

// What the CALL button resolves to right now: check (nothing to call),
// call_all (to_call >= stack) or call (with an amount).
static std::string
resolve_call(int to_call, int stack)
{
        if (to_call <= 0) return "check";
        if (to_call >= stack) return "call_all";
        return "call";
}

// Send the armed preselected action ("bet" is clamped to the legal range).
// check/call/call_all are only sent if they are still what the button would
// do now; otherwise the arm is dropped (the action is no longer valid).
// Called once when our turn starts.
static void
send_pending(ClientData &cd)
{
        if (cd.pending_action.empty()) return;
        const nlohmann::json &st = cd.bot->last_state();
        if (!st.contains("players")) return;

        std::string action = cd.pending_action;
        int amount         = cd.pending_amount;
        cd.pending_action.clear();
        cd.pending_amount = 0;
        cd.pending_source = 0;

        if (action == "fold") {
                cd.bot->send_action("fold");
                return;
        }

        int stack = 0, street_bet = 0, current_bet = 0, min_raise = 10;
        if (st.contains("players") && st["players"].is_array()) {
                for (const auto &pl : st["players"]) {
                        if (jval<int>(pl, "seat", -1) == cd.seat) {
                                stack      = jval<int>(pl, "stack", 0);
                                street_bet = jval<int>(pl, "street_bet", 0);
                        }
                }
        }
        current_bet = jval<int>(st, "current_bet", 0);
        min_raise   = jval<int>(st, "min_raise", 10);
        int to_call = std::max(0, current_bet - street_bet);

        if (action == "check" || action == "call" || action == "call_all") {
                if (action != resolve_call(to_call, stack)) return; // no longer valid
                cd.bot->send_action(action);
        } else if (action == "bet") {
                int lo = min_raise;
                int hi = std::max(lo, stack + street_bet - current_bet);
                cd.bot->send_action("bet", std::max(lo, std::min(amount, hi)));
        }
}

// Track whose turn it is (for the countdown bar) and detect new hands (to
// re-request our hole cards). When our turn starts, fire any armed action.
static void
update_turn_state(ClientData &cd)
{
        const nlohmann::json &st = cd.bot->last_state();
        if (st.empty() || !st.contains("players")) return;

        std::string stage = st.value("stage", "");
        int game_id       = jval<int>(st, "game_id", -1);
        bool hand_over    = jval<bool>(st, "hand_over", false);

        // New hand: re-request our hole cards. Only when seated — while
        // queued (waiting for a seat) the server has no seat for us and
        // answers my_cards queries with `unauthorized`.
        if (cd.seat >= 0 &&
            (game_id != cd.prev_game_id || (stage == "preflop" && cd.prev_stage != "preflop"))) {
                cd.prev_game_id = game_id;
                cd.my_cards[0].clear();
                cd.my_cards[1].clear();
                cd.result_text.clear();
                cd.pending_action.clear(); // preselections are per-hand
                cd.pending_amount = 0;
                cd.pending_source = 0;
                request_my_cards(cd);
        }
        cd.prev_stage = stage;

        // If we are seated in an active hand but missing hole cards, keep requesting them until received.
        // Skip if we are busted (tournament out) — the server will return nulls indefinitely.
        if (cd.seat >= 0 && !hand_over && stage != "over" && cd.my_cards[0].empty()) {
                bool i_am_busted = false;
                for (const auto &pl : st["players"]) {
                        if (jval<int>(pl, "seat", -1) == cd.seat &&
                            jval<bool>(pl, "busted", false)) {
                                i_am_busted = true;
                                break;
                        }
                }
                if (!i_am_busted) {
                        static double last_req = 0;
                        if (Bot::now() - last_req > 0.5) {
                                last_req = Bot::now();
                                request_my_cards(cd);
                        }
                }
        }

        bool my_turn = false;
        for (const auto &pl : st["players"]) {
                if (jval<int>(pl, "seat", -1) == cd.seat && jval<bool>(pl, "is_turn", false)) {
                        my_turn = true;
                        break;
                }
        }

        // track the current turn holder (for other players' countdown bars):
        // states are broadcast when the turn changes, so the arrival time is
        // close enough to the real turn start
        int turn_seat = -1;
        for (const auto &pl : st["players"]) {
                if (jval<bool>(pl, "is_turn", false)) {
                        turn_seat = jval<int>(pl, "seat", -1);
                        break;
                }
        }
        if (turn_seat >= 0 && turn_seat != cd.turn_seat) {
                cd.turn_seat        = turn_seat;
                cd.turn_seat_since  = Bot::now();
        } else if (turn_seat < 0) {
                cd.turn_seat = -1;
        }

        if (my_turn) {
                if (cd.my_turn_since == 0) cd.my_turn_since = Bot::now();
                if (!cd.prev_my_turn) send_pending(cd);
        } else {
                cd.my_turn_since = 0;
        }
        cd.prev_my_turn = my_turn;
}

// ---------------------------------------------------------------------------
// Table drawing (same layout as the server GUI, JSON-driven)
// ---------------------------------------------------------------------------



static void
draw_player_bet(const nlohmann::json &pl, int x, int y, int screen_h)
{
        const int cw = sc((int) default_card_size.width);
        const int ch = sc((int) default_card_size.height);
        if (jval<bool>(pl, "busted", false)) return;

        char buf[64] = { 0 };
        snprintf(buf, sizeof(buf) - 1, "Bet: %d", jval<int>(pl, "bet", 0));
        int tx = x + cw - MeasureText(buf, sc(20)) / 2;
        int by;
        if (y > screen_h / 2) {
                // bottom half: just above the name
                by = y - sc(48);
        } else {
                // top half: just below the chips line
                by = y + ch + sc(4) + sc(20) + sc(4);
        }
        DrawText(buf, tx, by, sc(20), ORANGE);
}

static void
draw_player(ClientData &cd, const nlohmann::json &state, const nlohmann::json &pl,
            int seat, int x, int y)
{
        const int cw        = sc((int) default_card_size.width);
        const int ch        = sc((int) default_card_size.height);
        const int margin    = sc(8);
        std::string name    = jval<std::string>(pl, "name", "");
        bool folded         = jval<bool>(pl, "folded", false);
        int stack           = jval<int>(pl, "stack", 0);
        bool is_me          = seat == cd.seat;
        bool busted         = jval<bool>(pl, "busted", false);

        if (busted) {
                // empty seat (lobby) or eliminated player: two card outlines + name (OUT)
                std::string label = name.empty() ? "Empty" : (name + " (OUT)");
                const int bw = 2 * cw + margin;
                int tx = x + (bw - MeasureText(label.c_str(), sc(20))) / 2;
                DrawText(label.c_str(), tx, y - sc(4) - sc(20), sc(20), GRAY);

                DrawRectangleLinesEx({ .x = (float) x, .y = (float) y, .width = (float) cw, .height = (float) ch }, 2, DARKGRAY);
                DrawRectangleLinesEx({ .x = (float) (x + cw + margin), .y = (float) y, .width = (float) cw, .height = (float) ch }, 2, DARKGRAY);
                return;
        }

        std::string tstatus = jval<std::string>(state, "status", "");
        bool is_winner      = (tstatus == "finished" && !busted) ||
                              (!cd.tournament_winner.empty() && name == cd.tournament_winner && tstatus == "finished");

        bool show_cards = false;
        std::string c1, c2;
        if (is_me && !cd.my_cards[0].empty()) {
                show_cards = true;
                c1         = cd.my_cards[0];
                c2         = cd.my_cards[1];
        } else if (pl.contains("cards") && pl["cards"].is_array() && pl["cards"].size() >= 2 &&
                   pl["cards"][0].is_string() && pl["cards"][1].is_string()) {
                show_cards = true;
                c1         = pl["cards"][0].get<std::string>();
                c2         = pl["cards"][1].get<std::string>();
        }

        if (show_cards) {
                draw_named_card(cd, c1, x, y);
                draw_named_card(cd, c2, x + cw + margin, y);
                if (is_winner) {
                        DrawRectangleLinesEx({ .x = (float) x - sc(2), .y = (float) y - sc(2), .width = (float) (2 * cw + margin + sc(4)), .height = (float) (ch + sc(4)) }, 2, GOLD);
                }
        } else {
                if (is_winner) {
                        DrawRectangleLinesEx({ .x = (float) x, .y = (float) y, .width = (float) cw, .height = (float) ch }, 2, GOLD);
                        DrawRectangleLinesEx({ .x = (float) (x + cw + margin), .y = (float) y, .width = (float) cw, .height = (float) ch }, 2, GOLD);
                } else {
                        // active or folded players' cards are shown reversed (card back)
                        DrawTextureEx(cd.card_back, Vector2{ (float) x, (float) y }, 0.0f,
                                      ui_scale / 2.0f, WHITE);
                        DrawTextureEx(cd.card_back, Vector2{ (float) (x + cw + margin), (float) y }, 0.0f,
                                      ui_scale / 2.0f, WHITE);
                }
        }

        std::string display_name = is_winner ? (name + " (WINNER)") : name;
        int nw = MeasureText(display_name.c_str(), sc(20));
        int tx = x + cw - nw / 2;
        DrawText(display_name.c_str(), tx, y - sc(4) - sc(20), sc(20), is_winner ? GOLD : WHITE);

        int dealer = jval<int>(state, "dealer", -1);
        size_t nplayers = (state.contains("players") && state["players"].is_array())
                              ? state["players"].size()
                              : 6;
        // blind seats come from the server (they skip empty seats and follow
        // the heads-up rule); fall back to the full-table math for old servers
        int sb = -1, bb = -1;
        if (state.contains("blinds") && state["blinds"].is_object()) {
                sb = jval<int>(state["blinds"], "small_seat", -1);
                bb = jval<int>(state["blinds"], "big_seat", -1);
        }
        if (nplayers > 0 && (sb < 0 || bb < 0 || (size_t) sb >= nplayers || (size_t) bb >= nplayers)) {
                sb = (dealer + 1) % (int) nplayers;
                bb = (dealer + 2) % (int) nplayers;
        }

        // dealer coin (circled) and blind buttons share the seat's top-left
        // corner; SB/BB are plain red text without a circle
        int cx = x - sc(13);
        int cy = y - sc(13);
        if (seat == dealer) {
                DrawCircleLines(cx, cy, sc(12), WHITE);
                DrawText("D", cx - MeasureText("D", sc(20)) / 2, cy - sc(10), sc(20), WHITE);
        } else if (seat == sb || seat == bb) {
                const char *label = seat == sb ? "SB" : "BB";
                DrawText(label, cx - MeasureText(label, sc(13)) / 2, cy - sc(13) / 2, sc(13), RED);
        }

        char buf[64] = { 0 };
        snprintf(buf, sizeof(buf) - 1, "Chips: %d", stack);
        tx = x + cw - MeasureText(buf, sc(20)) / 2;
        DrawText(buf, tx, y + ch + sc(4), sc(20), is_winner ? GOLD : GRAY);

        // banners span both cards, including the margin between them
        int banner_w  = 2 * cw + margin;
        int banner_cx = x + (2 * cw + margin) / 2;

        if (folded) {
                // soft dark tint so hole cards/card backs remain 100% visible underneath
                DrawRectangle(x, y, banner_w, ch, Color{ 0, 0, 0, 110 });
                tx = banner_cx - MeasureText("Fold", sc(20)) / 2;
                DrawRectangle(x, y + ch / 2 - sc(15), banner_w, sc(30), Color{ 0, 0, 0, 180 });
                DrawText("Fold", tx, y + ch / 2 - sc(10), sc(20), WHITE);
                DrawRectangleLines(x, y + ch / 2 - sc(15), banner_w, sc(30), WHITE);
        } else if (jval<bool>(pl, "is_turn", false) && !is_me &&
                   cd.turn_seat == seat && cd.turn_seat_since > 0) {
                // someone else is thinking: green bar over their cards that
                // shrinks with the remaining action time
                double timeout = jval<int>(state, "timeout_seconds", 20);
                double remain  = timeout - (Bot::now() - cd.turn_seat_since);
                remain         = std::max(0.0, std::min(remain, (double) timeout));
                DrawRectangle(x, y + ch / 2 - sc(15), banner_w, sc(30), Color{ 0, 0, 0, 180 });
                if (remain > 0) {
                        DrawRectangle(x, y + ch / 2 - sc(15), (int) (banner_w * remain / timeout),
                                      sc(30), GREEN);
                }
                DrawRectangleLines(x, y + ch / 2 - sc(15), banner_w, sc(30), WHITE);
                tx = banner_cx - MeasureText("Action", sc(20)) / 2;
                DrawText("Action", tx, y + ch / 2 - sc(10), sc(20), WHITE);
        } else if (pl.contains("last_action") && pl["last_action"].is_object()) {
                const auto &la = pl["last_action"];
                std::string type         = jval<std::string>(la, "type", "none");
                int amount               = jval<int>(la, "amount", 0);
                if (type != "none") {
                        Color bg = BLACK;
                        std::string text;
                        if (type == "fold") {
                                bg = BLACK;
                                text = "Fold";
                        } else if (type == "check") {
                                bg = DARKGRAY;
                                text = "Check";
                        } else if (type == "call") {
                                bg = BLUE;
                                text = "Call " + std::to_string(amount);
                        } else if (type == "call_all") {
                                bg = RED;
                                text = "All-in " + std::to_string(amount);
                        } else if (type == "bet") {
                                bg = ORANGE;
                                text = "Bet " + std::to_string(amount);
                        }
                        bg.a = bg.a * 0.75;
                        tx   = banner_cx - MeasureText(text.c_str(), sc(20)) / 2;
                        DrawRectangle(x, y + ch / 2 - sc(15), banner_w, sc(30), bg);
                        DrawText(text.c_str(), tx, y + ch / 2 - sc(10), sc(20), WHITE);
                        DrawRectangleLines(x, y + ch / 2 - sc(15), banner_w, sc(30), WHITE);
                }
        }
}

static bool
clicked(Rectangle r)
{
        return CheckCollisionPointRec(GetMousePosition(), r) &&
               IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

static void
button(Rectangle r, const char *label, bool enabled, Color col, bool armed = false,
       int fsize = 20)
{
        fsize       = sc(fsize);
        bool hover  = enabled && CheckCollisionPointRec(GetMousePosition(), r);
        Color bg    = enabled ? col : Color{ 60, 60, 60, 255 };
        if (hover || armed) bg = ColorBrightness(bg, -0.35f);
        DrawRectangleRec(r, bg);
        DrawRectangleLinesEx(r, 2, !enabled ? DARKGRAY : WHITE);
        Color tc = enabled ? WHITE : GRAY;
        DrawText(label, r.x + (r.width - MeasureText(label, fsize)) / 2,
                 r.y + (r.height - fsize) / 2, fsize, tc);
}

// Bottom action bar: FOLD | CALL/CHECK | - RAISE + | +.
// The raise increment is adjusted with the -/+ buttons and the mouse wheel
// or UP/DOWN keys over the raise area (clamped between min_raise and all-in).
// Buttons work even when it is not our turn: clicking arms a preselection
// (toggle), which is sent automatically when our turn starts.
static void
draw_buttons(ClientData &cd, const nlohmann::json &state, int w, int h)
{
        int y               = h - sc(50);
        Rectangle rfold     = { (float) (w / 2 - sc(265)), (float) y, (float) sc(110), (float) sc(44) };
        Rectangle rcall     = { (float) (w / 2 - sc(147)), (float) y, (float) sc(110), (float) sc(44) };
        Rectangle rminus    = { (float) (w / 2 - sc(29)), (float) y, (float) sc(26), (float) sc(44) };
        Rectangle rraise    = { (float) (w / 2 + sc(5)), (float) y, (float) sc(130), (float) sc(44) };
        Rectangle rplus     = { (float) (w / 2 + sc(143)), (float) y, (float) sc(26), (float) sc(44) };
        Rectangle rpot      = { (float) (w / 2 + sc(177)), (float) y, (float) sc(64), (float) sc(44) };
        Rectangle rallin    = { (float) (w / 2 + sc(249)), (float) y, (float) sc(84), (float) sc(44) };
        Rectangle rleave    = { (float) w - sc(68), (float) sc(8), (float) sc(60), (float) sc(22) };

        DrawRectangle(0, h - sc(60), w, sc(60), { 24, 24, 24, 255 });

        bool my_turn = false, folded = false;
        int stack = 0, street_bet = 0, current_bet = 0, min_raise = 10, pot = 0;
        std::string stage;
        if (state.contains("players") && state["players"].is_array()) {
                stage       = jval<std::string>(state, "stage", "");
                current_bet = jval<int>(state, "current_bet", 0);
                min_raise   = jval<int>(state, "min_raise", 10);
                pot         = jval<int>(state, "pot", 0);
                for (const auto &pl : state["players"]) {
                        if (jval<int>(pl, "seat", -1) == cd.seat) {
                                my_turn    = jval<bool>(pl, "is_turn", false);
                                folded     = jval<bool>(pl, "folded", false);
                                stack      = jval<int>(pl, "stack", 0);
                                street_bet = jval<int>(pl, "street_bet", 0);
                        }
                }
        }

        // turn countdown: a long green bar just above the buttons that
        // shrinks with the remaining time (textless)
        if (my_turn && cd.my_turn_since > 0 && state.contains("players")) {
                int timeout   = jval<int>(state, "timeout_seconds", 20);
                double remain = timeout - (Bot::now() - cd.my_turn_since);
                remain        = std::max(0.0, std::min(remain, (double) timeout));
                int bh        = sc(6);
                DrawRectangle(0, h - sc(58), (int) (w * remain / timeout), bh, GREEN);
        }
        // clickable whenever we have a seat and are still in the hand;
        // not-our-turn clicks just arm a preselection; in a tournament the
        // buttons are dead outside the running state
        bool tournament  = jval<std::string>(state, "mode", "") == "tournament";
        std::string tstatus = jval<std::string>(state, "status", "");
        bool waiting     = tournament && tstatus != "running";
        bool enabled     = cd.seat >= 0 && !folded && stage != "over" && !waiting;

        int to_call = std::max(0, current_bet - street_bet);

        // clamp the raise increment: min_raise .. all-in (server caps at the
        // stack, so an over-range increment is just an all-in)
        int lo = min_raise;
        int hi = std::max(lo, stack + street_bet - current_bet);
        cd.raise_inc = std::max(lo, std::min(cd.raise_inc, hi));

        // quick raise presets: pot-sized raise increment and all-in.
        // The server protocol expects "amount" to be the increment above
        // current_bet, NOT the total target. A pot-sized raise totals
        // pot + 2*to_call (pot includes our own street bet), so the
        // increment is pot + to_call - street_bet (== pot + to_call when
        // we have nothing committed this street, e.g. UTG preflop).
        int pot_amount    = std::max(lo, std::min(pot + to_call - street_bet, hi));
        int all_in_amount = std::max(lo, std::min(stack + street_bet - current_bet, hi));

        if (enabled) {
                // disarmed automatically if the armed action is no longer
                // what the same button would do now (e.g. a raise turned our
                // armed check into a call)
                if (!cd.pending_action.empty() && cd.pending_action != "fold" &&
                    cd.pending_action != "bet" &&
                    cd.pending_action != resolve_call(to_call, stack)) {
                        cd.pending_action.clear();
                        cd.pending_amount = 0;
                        cd.pending_source = 0;
                }

                bool raise_hover = CheckCollisionPointRec(GetMousePosition(), rminus) ||
                                   CheckCollisionPointRec(GetMousePosition(), rraise) ||
                                   CheckCollisionPointRec(GetMousePosition(), rplus);
                float wheel = GetMouseWheelMove();
                int step    = lo;
                if (wheel != 0 && raise_hover) cd.raise_inc += (int) wheel * step;
                if (IsKeyPressed(KEY_UP)) cd.raise_inc += step;
                if (IsKeyPressed(KEY_DOWN)) cd.raise_inc -= step;
                cd.raise_inc = std::max(lo, std::min(cd.raise_inc, hi));
                if (clicked(rminus)) cd.raise_inc -= step;
                if (clicked(rplus)) cd.raise_inc += step;
                cd.raise_inc = std::max(lo, std::min(cd.raise_inc, hi));
                // only a counter-armed bet follows the +/-/wheel adjustments;
                // a POT/ALL-IN arm keeps its preset amount
                if (cd.pending_action == "bet" && cd.pending_source == 1) {
                        cd.pending_amount = cd.raise_inc;
                }

                if (clicked(rpot)) {
                        cd.raise_inc = pot_amount;
                        if (my_turn) {
                                cd.bot->send_action("bet", pot_amount);
                        } else if (cd.pending_action == "bet" &&
                                   cd.pending_source == 2) {
                                cd.pending_action.clear();
                                cd.pending_amount = 0;
                                cd.pending_source = 0;
                        } else {
                                cd.pending_action = "bet";
                                cd.pending_amount = pot_amount;
                                cd.pending_source = 2;
                        }
                        cd.raise_inc = lo; // restore the counter after a big bet
                }
                if (clicked(rallin)) {
                        cd.raise_inc = all_in_amount;
                        if (my_turn) {
                                cd.bot->send_action("bet", all_in_amount);
                        } else if (cd.pending_action == "bet" &&
                                   cd.pending_source == 3) {
                                cd.pending_action.clear();
                                cd.pending_amount = 0;
                                cd.pending_source = 0;
                        } else {
                                cd.pending_action = "bet";
                                cd.pending_amount = all_in_amount;
                                cd.pending_source = 3;
                        }
                        cd.raise_inc = lo; // restore the counter after a big bet
                }

                if (my_turn) {
                        // acting now: clicks send the action immediately
                        if (clicked(rfold)) cd.bot->send_action("fold");
                        if (clicked(rcall)) {
                                if (to_call <= 0) cd.bot->send_action("check");
                                else if (to_call >= stack) cd.bot->send_action("call_all");
                                else cd.bot->send_action("call");
                        }
                        if (clicked(rraise)) {
                                cd.bot->send_action("bet", cd.raise_inc);
                                cd.raise_inc = lo; // restore after a big raise
                        }
                } else {
                        // preselection: click to arm, click again to disarm;
                        // the arm stores what the button means right now
                        if (clicked(rfold)) {
                                cd.pending_action = cd.pending_action == "fold" ? "" : "fold";
                                if (cd.pending_action != "fold") cd.pending_amount = 0;
                        }
                        if (clicked(rcall)) {
                                std::string resolved = resolve_call(to_call, stack);
                                cd.pending_action = cd.pending_action == resolved ? "" : resolved;
                                if (cd.pending_action != resolved) cd.pending_amount = 0;
                        }
                        if (clicked(rraise)) {
                                if (cd.pending_action == "bet" && cd.pending_source == 1) {
                                        cd.pending_action.clear();
                                        cd.pending_amount = 0;
                                        cd.pending_source = 0;
                                } else {
                                        cd.pending_action = "bet";
                                        cd.pending_amount = cd.raise_inc;
                                        cd.pending_source = 1;
                                }
                                cd.raise_inc = lo; // restore after a big raise
                        }
                }
        }

        if (clicked(rleave)) cd.leave_requested = true;

        std::string call_label = to_call <= 0     ? "CHECK"
                                 : to_call >= stack ? "CALL ALL"
                                                    : "CALL " + std::to_string(to_call);
        char rbuf[64]          = { 0 };
        snprintf(rbuf, sizeof(rbuf) - 1, "RAISE +%d", cd.raise_inc);

        bool armed_fold = enabled && cd.pending_action == "fold";
        bool armed_call = enabled && (cd.pending_action == "check" ||
                                      cd.pending_action == "call" ||
                                      cd.pending_action == "call_all");
        bool armed_bet  = enabled && cd.pending_action == "bet" && cd.pending_source == 1;
        bool armed      = armed_call && !my_turn; // when acting, calls go out instantly
        bool armed_pot  = enabled && cd.pending_action == "bet" && cd.pending_source == 2;
        bool armed_ai   = enabled && cd.pending_action == "bet" && cd.pending_source == 3;

        button(rfold, "FOLD", enabled, RED, armed_fold);
        button(rcall, call_label.c_str(), enabled, BLUE, armed);
        button(rminus, "-", enabled, DARKGRAY);
        button(rraise, rbuf, enabled, ORANGE, armed_bet);
        button(rplus, "+", enabled, DARKGRAY);
        button(rpot, "POT", enabled, PURPLE, armed_pot);
        button(rallin, "ALL-IN", enabled, MAROON, armed_ai);
        button(rleave, "LEAVE", true, MAROON, false, 10);

        std::string status;
        if (!cd.bot->connected()) {
                status = "Connecting...";
        } else if (tournament && tstatus == "lobby") {
                status = "Waiting for the tournament to start...";
        } else if (tournament && tstatus == "countdown") {
                char cbuf[64] = { 0 };
                snprintf(cbuf, sizeof(cbuf) - 1, "Tournament starts in %.1fs",
                         state.value("countdown_seconds_remaining", 0.0));
                status = cbuf;
        } else if (cd.seat < 0) {
                status = "Waiting for a seat...";
        } else if (!cd.pending_action.empty()) {
                status = "Current: ";
                if (cd.pending_action == "fold") {
                        status += "Fold";
                } else if (cd.pending_action == "check") {
                        status += "Check";
                } else if (cd.pending_action == "call") {
                        status += "Call " + std::to_string(to_call);
                } else if (cd.pending_action == "call_all") {
                        status += "Call all";
                } else {
                        status += "Raise " + std::to_string(cd.pending_amount);
                }
        }
        if (!status.empty()) {
                DrawText(status.c_str(), (w - MeasureText(status.c_str(), sc(20))) / 2,
                         h - sc(60) - sc(26), sc(20), WHITE);
        }
}

static void
draw_table(ClientData &cd, int w, int h)
{
        const nlohmann::json &state = cd.bot->last_state();
        if (state.empty() || !state.contains("players")) {
                draw_buttons(cd, state, w, h);
                return;
        }

        const float cw = scf(default_card_size.width);
        const float ch = scf(default_card_size.height);

        // the action bar shrinks the table area: layout against inner height
        const int bar_h = sc(60);
        const int ih    = h - bar_h;

        // seats around an ellipse, indexed by clockwise offset from our own
        // seat; our seat is always bottom centre (slot 0) and the others
        // wrap around from there. N comes from the server state. The ellipse
        // point is the seat's centre: half the seat block is subtracted so
        // (px, py) is the top-left corner draw_player wants. The ring is
        // deliberately wider than tall (flatter) so seats get more
        // horizontal and less vertical separation. The vertical radius uses
        // the window height (not ih) so the top seat clears the board text row.
        const int N        = state.value("max_players", 6);
        const float pad    = scf(40.0f); // room for the name label above each seat
        const float bw     = 2.0f * cw + scf(8.0f);
        const float rx     = w / 2.0f - cw - pad * 0.25f;
        const float ry     = h / 2.0f - ch - pad * 1.5f;
        int px[10];
        int py[10];
        for (int s = 0; s < N; s++) {
                float a = (90.0f + s * 360.0f / N) * DEG2RAD;
                px[s]   = (int) (w / 2.0f + rx * cosf(a) - bw / 2.0f);
                py[s]   = (int) (ih / 2.0f + ry * sinf(a) - ch / 2.0f);
        }

        // where each seat is actually drawn (slot-indexed when seated,
        // absolute when unseated), reused by the bet pass below
        int sx[10];
        int sy[10];
        if (state.contains("players") && state["players"].is_array()) {
                for (const auto &pl : state["players"]) {
                        int seat = jval<int>(pl, "seat", -1);
                        if (seat < 0 || seat >= N) continue;
                        int slot;
                        if (cd.seat >= 0) {
                                slot = (seat - cd.seat + N) % N;
                        } else {
                                // unseated: mirror the server GUI's absolute
                                // layout (seat 0 top, clockwise)
                                float a = (-90.0f + seat * 360.0f / N) * DEG2RAD;
                                sx[seat] = (int) (w / 2.0f + rx * cosf(a) - bw / 2.0f);
                                sy[seat] = (int) (ih / 2.0f + ry * sinf(a) - ch / 2.0f);
                                draw_player(cd, state, pl, seat, sx[seat], sy[seat]);
                                continue;
                        }
                        sx[seat] = px[slot];
                        sy[seat] = py[slot];
                        draw_player(cd, state, pl, seat, px[slot], py[slot]);
                }
        }

        // common card slots
        int margin  = sc(8);
        float my    = (ih - ch) / 2;
        float mx    = (w - cw * 5 - margin * 4) / 2;

        const nlohmann::json &common = state.value("common", nlohmann::json::array());
        for (int i = 0; i < 5; i++) {
                DrawRectangleLinesEx({ .x = mx + (cw + margin) * i, .y = my, .width = cw, .height = ch }, sc(3), GRAY);
                if (common.is_array() && (int) common.size() > i && common[i].is_string()) {
                        draw_named_card(cd, common[i].get<std::string>(),
                                        mx + (cw + margin) * i, my);
                }
        }

        char buf[128] = { 0 };
        snprintf(buf, sizeof(buf) - 1, "Pot: %d", state.value("pot", 0));
        int tx = (w - MeasureText(buf, sc(20))) / 2;
        DrawText(buf, tx, my - sc(40), sc(20), ORANGE);

        std::string stage = jval<std::string>(state, "stage", "");
        int current_bet   = jval<int>(state, "current_bet", 0);
        if (current_bet > 0 && stage != "over") {
                snprintf(buf, sizeof(buf) - 1, "Current bet: %d", current_bet);
                tx = (w - MeasureText(buf, sc(20))) / 2;
                DrawText(buf, tx, my - sc(64), sc(20), GRAY);
        }

        // bets are drawn after the board so the ones that land on its edge
        // (center-column seats) stay visible instead of being covered
        if (state.contains("players") && state["players"].is_array()) {
                for (const auto &pl : state["players"]) {
                        int seat = jval<int>(pl, "seat", -1);
                        if (seat < 0 || seat >= N) continue;
                        draw_player_bet(pl, sx[seat], sy[seat], ih);
                }
        }

        // tournament HUD: level, time left, blinds, players left
        std::string mode = jval<std::string>(state, "mode", "");
        if (mode == "tournament") {
                char tbuf[128] = { 0 };
                int level      = jval<int>(state, "level", 0);
                int secs       = (int) jval<double>(state, "level_seconds_remaining", 0.0);
                int alive      = jval<int>(state, "players_alive", 0);
                size_t num_players = (state.contains("players") && state["players"].is_array())
                                         ? state["players"].size()
                                         : 6;
                int bs = 0, bb2 = 0, ant = 0;
                if (state.contains("blinds") && state["blinds"].is_object()) {
                        bs = jval<int>(state["blinds"], "small", 0);
                        bb2 = jval<int>(state["blinds"], "big", 0);
                        ant = jval<int>(state["blinds"], "ante", 0);
                }

                int hud_x = sc(15);
                int hud_y = sc(8);
                snprintf(tbuf, sizeof(tbuf) - 1, "Level %d  %d:%02d", level, secs / 60, secs % 60);
                DrawText(tbuf, hud_x, hud_y, sc(16), WHITE);

                if (ant > 0) {
                        snprintf(tbuf, sizeof(tbuf) - 1, "Blinds %d/%d A:%d", bs, bb2, ant);
                } else {
                        snprintf(tbuf, sizeof(tbuf) - 1, "Blinds %d/%d", bs, bb2);
                }
                DrawText(tbuf, hud_x, hud_y + sc(20), sc(14), GRAY);

                snprintf(tbuf, sizeof(tbuf) - 1, "Players %d/%zu", alive, num_players);
                DrawText(tbuf, hud_x, hud_y + sc(38), sc(14), GRAY);

                // lifecycle overlays
                std::string tstatus = jval<std::string>(state, "status", "");
                if (tstatus == "lobby") {
                        snprintf(tbuf, sizeof(tbuf) - 1, "Waiting for players: %d/%zu", alive,
                                 num_players);
                        DrawText(tbuf, (w - MeasureText(tbuf, sc(40))) / 2, ih / 2 - sc(60), sc(40), YELLOW);
                } else if (tstatus == "countdown") {
                        snprintf(tbuf, sizeof(tbuf) - 1, "Tournament starts in %.1fs",
                                 jval<double>(state, "countdown_seconds_remaining", 0.0));
                        DrawText(tbuf, (w - MeasureText(tbuf, sc(40))) / 2, ih / 2 - sc(60), sc(40), YELLOW);
                } else if (tstatus == "finished") {
                        int box_w = sc(420);
                        int box_h = sc(170);
                        int box_x = (w - box_w) / 2;
                        int box_y = (ih - box_h) / 2;

                        DrawRectangle(box_x, box_y, box_w, box_h, Color{ 15, 15, 20, 235 });
                        DrawRectangleLines(box_x, box_y, box_w, box_h, GOLD);

                        const char *hdr = "TOURNAMENT OVER";
                        DrawText(hdr, box_x + (box_w - MeasureText(hdr, sc(28))) / 2, box_y + sc(16), sc(28), GOLD);

                        std::string wname = cd.tournament_winner.empty() ? "Champion" : cd.tournament_winner;
                        snprintf(tbuf, sizeof(tbuf) - 1, "Winner: %s", wname.c_str());
                        DrawText(tbuf, box_x + (box_w - MeasureText(tbuf, sc(24))) / 2, box_y + sc(54), sc(24), WHITE);

                        int award = cd.tournament_award;
                        if (award <= 0 && state.contains("players") && state["players"].is_array()) {
                                for (const auto &pl : state["players"]) {
                                        if (!jval<bool>(pl, "busted", false)) {
                                                award = jval<int>(pl, "stack", 0);
                                        }
                                }
                        }
                        if (award > 0) {
                                snprintf(tbuf, sizeof(tbuf) - 1, "Earnings: %d Chips", award);
                        } else {
                                snprintf(tbuf, sizeof(tbuf) - 1, "Winner takes all!");
                        }
                        DrawText(tbuf, box_x + (box_w - MeasureText(tbuf, sc(22))) / 2, box_y + sc(88), sc(22), ORANGE);

                        snprintf(tbuf, sizeof(tbuf) - 1, "Final Level: Level %d (Blinds %d/%d)", level, bs, bb2);
                        DrawText(tbuf, box_x + (box_w - MeasureText(tbuf, sc(18))) / 2, box_y + sc(124), sc(18), GRAY);
                }

                if (!cd.tournament_event_text.empty() && Bot::now() - cd.tournament_event_since < 4.0) {
                        int font_size = sc(24);
                        int tw = MeasureText(cd.tournament_event_text.c_str(), font_size);
                        DrawRectangle((w - tw - sc(20)) / 2, sc(80), tw + sc(20), font_size + sc(10), Fade(GOLD, 0.8f));
                        DrawText(cd.tournament_event_text.c_str(), (w - tw) / 2, sc(85), font_size, BLACK);
                }
        }

        if (!cd.result_text.empty()) {
                DrawText(cd.result_text.c_str(),
                         (w - MeasureText(cd.result_text.c_str(), sc(20))) / 2,
                         my - sc(100), sc(20), WHITE);
        }

        draw_buttons(cd, state, w, h);
}

// ---------------------------------------------------------------------------
// main: CLI, raylib window, and the lws loop (lws_service blocks since lws
// 3.2, so a ticker thread cancels it every 16 ms to keep the loop moving)
// ---------------------------------------------------------------------------

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
        int port = 9000;
        std::string host = "127.0.0.1", token, name;
        float scale_override = 0.0f; // 0 = auto-fit from the window size

        for (int i = 1; i < argc; i++) {
                if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
                        port = atoi(argv[++i]);
                } else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
                        host = argv[++i];
                } else if (strcmp(argv[i], "--token") == 0 && i + 1 < argc) {
                        token = argv[++i];
                } else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
                        name = argv[++i];
                } else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
                        scale_override = atof(argv[++i]);
                        if (scale_override < 0.5f || scale_override > 2.0f) {
                                fprintf(stderr, "error: --scale must be in [0.5, 2.0]\n");
                                return 1;
                        }
                } else {
                        fprintf(stderr,
                                "usage: %s [--port N] [--host IP] [--token SECRET] [--name NAME] [--scale N]\n",
                                argv[0]);
                        return 1;
                }
        }

        lws_context *ctx = bot_context();
        if (!ctx) {
                fprintf(stderr, "failed to create lws context\n");
                return 1;
        }

        setvbuf(stdout, nullptr, _IOLBF, 0); // live logs when stdout is a pipe/file

        const char *name_arg = name.empty() ? nullptr : name.c_str();
        const char *tok_arg  = token.empty() ? nullptr : token.c_str();
        Bot bot(ctx, host.c_str(), port, 0, false, true, tok_arg, name_arg, 0.0, true);
        printf("client: connecting to %s:%d as %s\n", host.c_str(), port, bot.name().c_str());

        std::atomic<bool> running{ true };
        std::thread ticker([&] {
                while (running.load()) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(16));
                        lws_cancel_service(ctx);
                }
        });

        SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
        SetTraceLogLevel(LOG_WARNING);
        InitWindow(win.width, win.height, win.title);

        if (scale_override > 0.0f) {
                ui_scale = scale_override;
        } else {
                auto_scale(win.width, win.height);
        }

        // cd to the executable path so relative paths work fine; when built
        // into client/build/ the deck images live two levels up
        ChangeDirectory(GetApplicationDirectory());
        for (int i = 0; i < 4 && !DirectoryExists("decks"); i++) {
                ChangeDirectory("..");
        }

        ClientData cd;
        cd.ctx = ctx;
        cd.bot = &bot;
        build_themed_cards(themed_cards, "decks/jorels");
        cd.card_back = LoadTexture("decks/jorels/card_back.png");

        while (!WindowShouldClose() && !cd.leave_requested) {
                if (IsWindowResized()) {
                        win.height = GetScreenHeight();
                        win.width  = GetScreenWidth();
                        if (scale_override <= 0.0f) auto_scale(win.width, win.height);
                }

                lws_service(ctx, 0);
                bot.pump(Bot::now());
                handle_messages(cd);
                update_turn_state(cd);

                // the server disconnected us (e.g. busted out): leave
                if (cd.seat >= 0 && !cd.bot->connected()) cd.leave_requested = true;

                BeginDrawing();
                {
                        ClearBackground(BLACK);
                        draw_table(cd, win.width, win.height);
                }
                EndDrawing();
        }

        running.store(false);
        lws_cancel_service(ctx);
        ticker.join();
        lws_context_destroy(ctx);
        CloseWindow();
        return 0;
}
