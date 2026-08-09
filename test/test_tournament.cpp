// Tournament mode tests: drive a Server in tournament mode (--tournament)
// through the lobby, countdown, running and finished states, with clients
// that always act (check/call) instantly. The action timeout is set to a
// huge value so no player ever folds on timeout: the game only progresses
// through explicit client actions, making the tests deterministic.
//
// The server is driven on the main thread via tick() (like the GUI path)
// so tests can read and poke table_/state_ directly (e.g. force a bust).
// The client lws context needs its own ticker thread (lws_service blocks
// since lws 3.2, see AGENTS.md).

#include "../bot/example/bot.cpp"
#include "../server/src/server.h"
#include "../server/src/tournament.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <thread>
#include <vector>

#include "platform.h"

static int g_failures = 0;

#define CHECK(cond)                                                       \
        do {                                                              \
                if (!(cond)) {                                            \
                        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
                        g_failures++;                                     \
                }                                                         \
        } while (0)

#define CHECK_EQ(a, b)                                                          \
        do {                                                                    \
                long long _a = (long long)(a), _b = (long long)(b);             \
                if (_a != _b) {                                                 \
                        printf("FAIL %s:%d: %s == %s (%lld != %lld)\n",        \
                               __FILE__, __LINE__, #a, #b, _a, _b);             \
                        g_failures++;                                           \
                }                                                               \
        } while (0)

static int
find_free_port()
{
        socket_lib_init();
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a{};
        a.sin_family      = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port        = 0;
        if (bind(fd, (struct sockaddr *) &a, sizeof(a)) < 0) return 9000;
        socklen_t alen = sizeof(a);
        getsockname(fd, (struct sockaddr *) &a, &alen);
        int port = ntohs(a.sin_port);
        sock_close(fd);
        return port;
}

static lws_context *g_ctx = nullptr;

// the server driven on this (main) thread; set per test
static Server *g_srv = nullptr;
static std::vector<Bot *> g_clients;
// every message any client received, for assertions
static std::vector<nlohmann::json> g_msgs;
// chips destroyed by bust_seat() (zeroing a stack outside a pot): the engine
// itself conserves chips, so winner checks subtract this test-side loss
static int g_destroyed = 0;

static void
respond(Bot *b)
{
        for (auto &m : b->take_messages()) {
                g_msgs.push_back(m);
                std::string t = m.value("type", "");
                if (t == "your_turn") {
                        // act instantly: check when free, else call
                        const nlohmann::json &st = b->last_state();
                        int street              = 0;
                        if (st.contains("players")) {
                                for (auto &pl : st["players"]) {
                                        if (pl.value("seat", -1) == b->seat()) {
                                                street = pl.value("street_bet", 0);
                                        }
                                }
                        }
                        if (st.value("current_bet", 0) - street <= 0) {
                                b->send_action("check");
                        } else {
                                b->send_action("call");
                        }
                } else if (t == "error" && m.value("code", "") == "illegal_action") {
                        b->send_action("call");
                }
        }
}

static void
respond_all()
{
        for (Bot *b : g_clients) {
                respond(b);
        }
}

// one frame: server tick + client service + instant client actions
static void
frame()
{
        g_srv->tick(Bot::now());
        lws_service(g_ctx, 10);
        respond_all();
}

static void
advance(double seconds)
{
        double end = Bot::now() + seconds;
        while (Bot::now() < end) {
                frame();
        }
}

static bool
wait_until(std::function<bool()> f, double timeout)
{
        double end = Bot::now() + timeout;
        while (Bot::now() < end) {
                if (f()) return true;
                frame();
        }
        return f();
}

static std::vector<Bot *>
make_clients(int port, int n)
{
        std::vector<Bot *> cs;
        for (int i = 0; i < n; i++) {
                cs.push_back(new Bot(g_ctx, "127.0.0.1", port, i, false, true));
        }
        return cs;
}

static bool
all_seated(const std::vector<Bot *> &cs)
{
        for (Bot *b : cs) {
                if (b->seat() < 0) return false;
        }
        return true;
}

static bool
seen(const char *type, int *count = nullptr)
{
        int n = 0;
        for (auto &m : g_msgs) {
                if (m.value("type", "") == type) n++;
        }
        if (count) *count = n;
        return n > 0;
}

static bool
seen_error(const char *code)
{
        for (auto &m : g_msgs) {
                if (m.value("type", "") == "error" && m.value("code", "") == code) {
                        return true;
                }
        }
        return false;
}

// wait for a hand-over pause, then zero the seat's stack so end_hand
// eliminates them (their chips were already settled into the pot)
static void
bust_seat(int seat)
{
        CHECK(wait_until([&] { return g_srv->state().hand_over; }, 15.0));
        g_destroyed += g_srv->table().players[seat].stack;
        g_srv->table().players[seat].stack = 0;
}

// --- tests -----------------------------------------------------------------

static void
test_lobby_welcome()
{
        int port = find_free_port();
        Server srv(port, "127.0.0.1", nullptr, /*verbose=*/false,
                   /*tournament=*/true, /*level_seconds=*/2, /*countdown=*/0.5);
        srv.table().action_timeout = 1e9; // no timeout folds
        srv.hand_pause()           = 0.1; // wide enough for bust_seat to catch
        g_srv = &srv;
        g_clients = make_clients(port, 5);

        // clients seat immediately in the lobby: welcome, no queued
        CHECK(wait_until([&] { return all_seated(g_clients); }, 10.0));
        CHECK(wait_until([&] { return !seen("queued"); }, 2.0));
        CHECK(!seen("tournament_start"));

        const nlohmann::json &st = g_clients[0]->last_state();
        CHECK(st.value("mode", "") == "tournament");
        CHECK(st.value("status", "") == "lobby");
        CHECK_EQ(st["blinds"].value("small", 0), 5);
        CHECK_EQ(st["blinds"].value("big", 0), 10);
        CHECK_EQ(st["blinds"].value("ante", 0), 0);
        CHECK_EQ(st["players"].size(), 6);
        for (auto &pl : st["players"]) {
                CHECK_EQ(pl.value("stack", 0), StartStack);
        }

        // the state at hello time only saw one seated player; a fresh query
        // must reflect all five joined
        g_clients[0]->send_json({ { "type", "query" }, { "id", 42 }, { "what", "state" } });
        CHECK(wait_until([&] {
                for (auto &m : g_msgs) {
                        if (m.value("type", "") == "reply" && m.value("what", "") == "state" &&
                            m["data"].value("players_alive", 0) == 5) {
                                return true;
                        }
                }
                return false;
        }, 5.0));

        // no hands are dealt while waiting
        CHECK(!g_srv->state().hand_started);
        int msgs_before = (int) g_msgs.size();
        advance(1.0);
        CHECK(!g_srv->state().hand_started);
        CHECK(!seen("your_turn"));
        CHECK((int) g_msgs.size() < msgs_before + 20); // no broadcast storm

        // a 6th seat triggers the countdown; a 7th client is rejected
        g_clients.push_back(new Bot(g_ctx, "127.0.0.1", port, 5, false, true));
        CHECK(wait_until([&] { return g_clients[5]->seat() >= 0; }, 10.0));
        CHECK(wait_until([&] { return g_srv->state().tournament_status == T_COUNTDOWN; }, 5.0));
        g_clients.push_back(new Bot(g_ctx, "127.0.0.1", port, 99, false, true));
        CHECK(wait_until([&] { return seen_error("table_full"); }, 5.0));

        for (Bot *b : g_clients) delete b;
        g_clients.clear();
        g_msgs.clear();
        srv.stop();
}

static void
test_countdown_then_start()
{
        int port = find_free_port();
        Server srv(port, "127.0.0.1", nullptr, /*verbose=*/false,
                   /*tournament=*/true, /*level_seconds=*/2, /*countdown=*/0.5);
        srv.table().action_timeout = 1e9;
        srv.hand_pause()           = 0.1;
        g_srv = &srv;
        g_clients = make_clients(port, 5);

        CHECK(wait_until([&] { return all_seated(g_clients); }, 10.0));
        advance(1.0);
        CHECK(g_srv->state().tournament_status == T_LOBBY); // still waiting

        // the 6th seat triggers the countdown
        g_clients.push_back(new Bot(g_ctx, "127.0.0.1", port, 5, false, true));
        CHECK(wait_until([&] { return g_clients[5]->seat() >= 0; }, 10.0));
        CHECK(wait_until([&] { return g_srv->state().tournament_status == T_COUNTDOWN; }, 5.0));
        CHECK(!seen("tournament_start"));
        CHECK(wait_until([&] { return g_srv->state().tournament_status == T_RUNNING; }, 5.0));
        // the broadcast arrives a frame after the status flips: wait for it
        CHECK(wait_until([&] { return seen("tournament_start"); }, 5.0));
        CHECK_EQ(g_srv->state().level, 1);
        CHECK_EQ(g_srv->state().small_blind, 5);
        CHECK_EQ(g_srv->state().big_blind, 10);

        // hands flow: check/call bots complete hands
        CHECK(wait_until([&] { return g_srv->state().hand_started; }, 5.0));
        CHECK(wait_until([&] {
                int n = 0;
                seen("hand_over", &n);
                return n >= 2;
        }, 20.0));
        // never a timeout fold: the action timeout is effectively infinite
        for (auto &m : g_msgs) {
                CHECK(m.value("type", "") != "action" ||
                      !(m.contains("reason") && m["reason"].is_string() &&
                        m["reason"] == "timeout"));
        }

        for (Bot *b : g_clients) delete b;
        g_clients.clear();
        g_msgs.clear();
        srv.stop();
}

static void
test_countdown_aborted()
{
        int port = find_free_port();
        Server srv(port, "127.0.0.1", nullptr, /*verbose=*/false,
                   /*tournament=*/true, /*level_seconds=*/2, /*countdown=*/0.5);
        srv.table().action_timeout = 1e9;
        srv.hand_pause()           = 0.1;
        g_srv = &srv;
        g_clients = make_clients(port, 6);
        CHECK(wait_until([&] { return all_seated(g_clients); }, 10.0));
        CHECK(wait_until([&] { return g_srv->state().tournament_status == T_COUNTDOWN; }, 5.0));

        // a player leaving during the countdown sends the table back to lobby
        Bot *leaver = g_clients[5];
        int seat    = leaver->seat();
        leaver->close(); // the object must stay alive (lws callback), so just leak it
        g_clients.pop_back();
        CHECK(wait_until([&] { return g_srv->state().tournament_status == T_LOBBY; }, 5.0));
        CHECK(!seen("tournament_start"));
        CHECK(g_srv->table().players[seat].busted);
        CHECK_EQ(g_srv->table().alive_count(), 5);

        // the seat is joinable again; the countdown restarts
        g_clients.push_back(new Bot(g_ctx, "127.0.0.1", port, 5, false, true));
        CHECK(wait_until([&] { return all_seated(g_clients); }, 10.0));
        CHECK(wait_until([&] { return g_srv->state().tournament_status == T_COUNTDOWN; }, 5.0));

        for (Bot *b : g_clients) delete b;
        g_clients.clear();
        g_msgs.clear();
        srv.stop();
}

static void
test_blind_levels()
{
        int port = find_free_port();
        Server srv(port, "127.0.0.1", nullptr, /*verbose=*/false,
                   /*tournament=*/true, /*level_seconds=*/1.0, /*countdown=*/0.5);
        srv.table().action_timeout = 1e9;
        srv.hand_pause()           = 0.1;
        g_srv = &srv;
        g_clients = make_clients(port, 6);
        CHECK(wait_until([&] { return all_seated(g_clients); }, 10.0));
        CHECK(wait_until([&] { return g_srv->state().tournament_status == T_RUNNING; }, 10.0));

        // the schedule itself: fixed table plus open-ended doubling
        CHECK_EQ(tournament::level_blinds(1).big, 10);
        CHECK_EQ(tournament::level_blinds(5).big, 80);
        CHECK_EQ(tournament::level_blinds(5).ante, 5);
        CHECK_EQ(tournament::level_blinds(12).big, 2000);
        CHECK_EQ(tournament::level_blinds(13).big, 4000);
        CHECK_EQ(tournament::level_blinds(13).ante, 300);

        // play until level 2: blinds 10/20
        CHECK(wait_until([&] { return g_srv->state().level >= 2; }, 10.0));
        CHECK_EQ(g_srv->state().small_blind, 10);
        CHECK_EQ(g_srv->state().big_blind, 20);
        CHECK_EQ(g_srv->state().ante, 0);
        bool level2_event = false;
        CHECK(wait_until([&] {
                for (auto &m : g_msgs) {
                        if (m.value("type", "") == "level" && m.value("level", 0) == 2) {
                                level2_event = true;
                                return true;
                        }
                }
                return false;
        }, 5.0));
        for (auto &m : g_msgs) {
                if (m.value("type", "") == "level" && m.value("level", 0) == 2) {
                        CHECK_EQ(m["blinds"].value("big", 0), 20);
                }
        }
        CHECK(level2_event);

        // play until level 5: 40/80 with ante 5
        CHECK(wait_until([&] { return g_srv->state().level >= 5; }, 20.0));
        CHECK_EQ(g_srv->state().small_blind, 40);
        CHECK_EQ(g_srv->state().big_blind, 80);
        CHECK_EQ(g_srv->state().ante, 5);

        // every level broadcast carries the blinds of that level
        for (auto &m : g_msgs) {
                if (m.value("type", "") != "level" || !m.contains("state")) continue;
                int lvl = m.value("level", 0);
                CHECK_EQ(m["state"]["blinds"].value("small", 0),
                         tournament::level_blinds(lvl).small);
                CHECK_EQ(m["state"]["blinds"].value("big", 0),
                         tournament::level_blinds(lvl).big);
        }

        // level_remaining stays inside [0, level_seconds]
        CHECK(g_srv->state().level_remaining >= 0.0);
        CHECK(g_srv->state().level_remaining <= 1.0 + 0.01);

        for (Bot *b : g_clients) delete b;
        g_clients.clear();
        g_msgs.clear();
        srv.stop();
}

static void
test_elimination_no_rebuy()
{
        int port = find_free_port();
        Server srv(port, "127.0.0.1", nullptr, /*verbose=*/false,
                   /*tournament=*/true, /*level_seconds=*/2, /*countdown=*/0.5);
        srv.table().action_timeout = 1e9;
        srv.hand_pause()           = 0.1;
        g_srv = &srv;
        g_clients = make_clients(port, 6);
        CHECK(wait_until([&] { return all_seated(g_clients); }, 10.0));
        CHECK(wait_until([&] { return g_srv->state().tournament_status == T_RUNNING; }, 10.0));

        int victim = 0;
        bust_seat(victim);

        // end of the hand: the seat is eliminated, no rebuy, no bot
        CHECK(wait_until([&] { return g_srv->table().players[victim].busted; }, 10.0));
        CHECK_EQ(g_srv->table().players[victim].stack, 0);
        CHECK_EQ(g_srv->table().alive_count(), 5);
        // the player_out broadcast itself carries the matching snapshot: wait on
        // the message (g_msgs), not on one client's state_ (which client owns
        // which seat is a connection race, and a client's snapshot only
        // refreshes on state-bearing messages)
        bool out_event = false;
        bool alive_ok  = wait_until([&] {
                for (auto &m : g_msgs) {
                        if (m.value("type", "") == "player_out" &&
                            m.value("seat", -1) == victim &&
                            m.value("reason", "") == "busted") {
                                out_event = true;
                                return m["state"].value("players_alive", 0) == 5;
                        }
                }
                return false;
        }, 15.0);
        if (!alive_ok) {
                // diagnostics for the CI flake: which client owns which seat,
                // and what snapshot each still holds
                for (size_t i = 0; i < g_clients.size(); i++) {
                        Bot *b = g_clients[i];
                        printf("  [diag] client[%zu] \"%s\" seat=%d connected=%d alive=%d\n",
                               i, b->name().c_str(), b->seat(), b->connected() ? 1 : 0,
                               b->last_state().value("players_alive", -1));
                }
        }
        CHECK(alive_ok);
        CHECK(out_event);

        // the client is disconnected and the seat never gets a bot
        CHECK(wait_until([&] { return !g_clients[victim]->connected(); }, 15.0));
        CHECK(!g_srv->table().players[victim].auto_play); // no bot took over
        CHECK(g_srv->table().players[victim].name.rfind("Bot ", 0) != 0);

        // play on with 5 players: hands still complete, the seat stays out
        CHECK(wait_until([&] {
                int n = 0;
                seen("hand_over", &n);
                return n >= 3;
        }, 20.0));
        CHECK(g_srv->table().players[victim].busted);

        for (Bot *b : g_clients) delete b;
        g_clients.clear();
        g_msgs.clear();
        srv.stop();
}

static void
test_disconnect_eliminates()
{
        int port = find_free_port();
        Server srv(port, "127.0.0.1", nullptr, /*verbose=*/false,
                   /*tournament=*/true, /*level_seconds=*/2, /*countdown=*/0.5);
        srv.table().action_timeout = 1e9;
        srv.hand_pause()           = 0.1;
        g_srv = &srv;
        g_clients = make_clients(port, 6);
        CHECK(wait_until([&] { return all_seated(g_clients); }, 10.0));
        CHECK(wait_until([&] { return g_srv->state().tournament_status == T_RUNNING; }, 10.0));
        CHECK(wait_until([&] { return g_srv->state().hand_started; }, 10.0));

        // drop a player mid-hand: they are folded immediately but only
        // busted (eliminated) at the next round end — graceful disconnect.
        Bot *leaver = g_clients[5];
        int seat    = leaver->seat();
        leaver->close();
        g_clients.pop_back(); // the object stays alive (lws callback), just leak it

        // The connection closes immediately on the client side.
        CHECK(wait_until([&] { return !leaver->connected(); }, 2.0));

        // player_out(disconnected) fires at round end, not immediately.
        CHECK(wait_until([&] {
                for (auto &m : g_msgs) {
                        if (m.value("type", "") == "player_out" &&
                            m.value("seat", -1) == seat &&
                            m.value("reason", "") == "disconnected") {
                                return true;
                        }
                }
                return false;
        }, 15.0));
        CHECK(g_srv->table().players[seat].busted);
        CHECK_EQ(g_srv->table().alive_count(), 5);


        // the remaining 5 keep playing: hands complete, no stall
        CHECK(wait_until([&] {
                int n = 0;
                seen("hand_over", &n);
                return n >= 2;
        }, 20.0));

        for (Bot *b : g_clients) delete b;
        g_clients.clear();
        g_msgs.clear();
        srv.stop();
}

static void
test_heads_up_and_winner()
{
        int port = find_free_port();
        Server srv(port, "127.0.0.1", nullptr, /*verbose=*/false,
                   /*tournament=*/true, /*level_seconds=*/2, /*countdown=*/0.5);
        srv.table().action_timeout = 1e9;
        srv.hand_pause()           = 0.1;
        g_destroyed                = 0;
        g_srv = &srv;
        g_clients = make_clients(port, 6);
        CHECK(wait_until([&] { return all_seated(g_clients); }, 10.0));
        CHECK(wait_until([&] { return g_srv->state().tournament_status == T_RUNNING; }, 10.0));

        // eliminate four seats; the last two face each other heads-up
        for (int i = 0; i < 4; i++) {
                bust_seat(i);
                CHECK(wait_until([&] { return g_srv->table().players[i].busted; }, 10.0));
        }
        CHECK_EQ(g_srv->table().alive_count(), 2);
        int a = -1, b = -1;
        for (int i = 0; i < 6; i++) {
                if (!g_srv->table().players[i].busted) {
                        if (a < 0) {
                                a = i;
                        } else {
                                b = i;
                        }
                }
        }
        CHECK(a >= 0 && b >= 0);

        // heads-up rule: the button posts the small blind, the other the big
        CHECK(wait_until([&] { return g_srv->state().hand_started; }, 10.0));
        std::pair<int, int> bl = g_srv->table().blind_seats(g_srv->state().dealer);
        CHECK_EQ(bl.first, g_srv->state().dealer); // button = small blind
        CHECK(bl.first != bl.second);
        CHECK(bl.first == a || bl.first == b);
        // _street_bet includes the ante (posted before blinds) plus the blind
        CHECK_EQ(g_srv->table().players[bl.first]._street_bet, g_srv->state().small_blind + g_srv->state().ante);
        CHECK_EQ(g_srv->table().players[bl.second]._street_bet, g_srv->state().big_blind + g_srv->state().ante);

        // hands play out heads-up
        int overs_before = 0;
        seen("hand_over", &overs_before);
        CHECK(wait_until([&] {
                int n = 0;
                seen("hand_over", &n);
                return n >= overs_before + 1;
        }, 20.0));

        // bust one of the two: winner takes all, tournament over
        bust_seat(a);
        CHECK(wait_until([&] { return g_srv->state().tournament_status == T_FINISHED; }, 10.0));
        CHECK(g_srv->table().players[a].busted);
        CHECK(!g_srv->table().players[b].busted);
        // bust_seat zeroed stacks outside the pot: those chips are gone, the
        // engine conserves the rest
        CHECK_EQ(g_srv->table().players[b].stack, 6 * StartStack - g_destroyed);

        CHECK(wait_until([&] { return seen("tournament_over"); }, 5.0));
        bool over_event = false;
        for (auto &m : g_msgs) {
                if (m.value("type", "") == "tournament_over") {
                        over_event = true;
                        CHECK_EQ(m["winner"].value("seat", -1), b);
                        CHECK_EQ(m.value("award", 0), 6 * StartStack - g_destroyed);
                        CHECK(m["state"].value("status", "") == "finished");
                }
        }
        CHECK(over_event);

        // finished: no more hands are dealt
        int hand_overs_before = 0;
        seen("hand_over", &hand_overs_before);
        advance(1.5);
        CHECK(!g_srv->state().hand_started);
        int hand_overs_after = 0;
        seen("hand_over", &hand_overs_after);
        CHECK_EQ(hand_overs_after, hand_overs_before);

        // no late join after the tournament ended
        g_clients.push_back(new Bot(g_ctx, "127.0.0.1", port, 99, false, true));
        CHECK(wait_until([&] { return seen_error("table_full"); }, 5.0));

        for (Bot *b : g_clients) delete b;
        g_clients.clear();
        g_msgs.clear();
        srv.stop();
}

static void
test_tournament_end_modes()
{
        // RESTART: after the winner-screen hold everyone is kicked and the
        // table resets to a fresh lobby; reconnecting clients play a second
        // tournament to completion.
        {
                int port = find_free_port();
                Server srv(port, "127.0.0.1", nullptr, /*verbose=*/false,
                           /*tournament=*/true, /*level_seconds=*/2, /*countdown=*/0.5,
                           /*start_stack=*/1000, /*max_players=*/2);
                srv.table().action_timeout = 1e9;
                srv.hand_pause()           = 0.1;
                srv.set_tournament_end(Server::TournamentEndMode::RESTART, 0.3);
                g_destroyed = 0;
                g_srv = &srv;
                g_clients = make_clients(port, 2);
                CHECK(wait_until([&] { return all_seated(g_clients); }, 10.0));
                CHECK(wait_until([&] { return g_srv->state().tournament_status == T_RUNNING; }, 10.0));

                // heads-up: bust one seat, the other wins the tournament
                bust_seat(0);
                CHECK(wait_until([&] { return g_srv->state().tournament_status == T_FINISHED; }, 10.0));
                CHECK(wait_until([&] { return seen("tournament_over"); }, 5.0));

                // after the hold: both clients are kicked, the table is back
                // to an empty lobby
                CHECK(wait_until([&] { return !g_clients[0]->connected() && !g_clients[1]->connected(); }, 5.0));
                CHECK(wait_until([&] { return g_srv->state().tournament_status == T_LOBBY; }, 5.0));
                CHECK_EQ(g_srv->table().alive_count(), 0);
                for (int i = 0; i < 2; i++) CHECK(g_srv->table().players[i].busted);

                // fresh clients rejoin the lobby and a second tournament runs
                // to a winner (the harness never pumps, so no auto-reconnect)
                for (Bot *b : g_clients) delete b;
                g_clients.clear();
                g_clients = make_clients(port, 2);
                CHECK(wait_until([&] { return all_seated(g_clients); }, 15.0));
                CHECK(wait_until([&] { return g_srv->state().tournament_status == T_RUNNING; }, 10.0));
                int overs_before = 0;
                seen("hand_over", &overs_before);
                bust_seat(g_clients[1]->seat());
                CHECK(wait_until([&] { return g_srv->state().tournament_status == T_FINISHED; }, 15.0));
                // the broadcast arrives a frame after the status flips
                CHECK(wait_until([&] {
                        int n = 0;
                        seen("tournament_over", &n);
                        return n >= 2;
                }, 5.0));

                for (Bot *b : g_clients) delete b;
                g_clients.clear();
                g_msgs.clear();
                srv.stop();
        }

        // EXIT: the server raises exit_requested() once the winner is declared
        {
                int port = find_free_port();
                Server srv(port, "127.0.0.1", nullptr, /*verbose=*/false,
                           /*tournament=*/true, /*level_seconds=*/2, /*countdown=*/0.5,
                           /*start_stack=*/1000, /*max_players=*/2);
                srv.table().action_timeout = 1e9;
                srv.hand_pause()           = 0.1;
                srv.set_tournament_end(Server::TournamentEndMode::EXIT);
                g_destroyed = 0;
                g_srv = &srv;
                g_clients = make_clients(port, 2);
                CHECK(wait_until([&] { return all_seated(g_clients); }, 10.0));
                CHECK(wait_until([&] { return g_srv->state().tournament_status == T_RUNNING; }, 10.0));
                bust_seat(0);
                CHECK(wait_until([&] { return srv.exit_requested(); }, 15.0));
                CHECK(g_srv->state().tournament_status == T_FINISHED);

                for (Bot *b : g_clients) delete b;
                g_clients.clear();
                g_msgs.clear();
                srv.stop();
        }
}

static void
run_test(const char *name, void (*fn)())
{
        double t0 = Bot::now();
        fn();
        printf("  %-24s %.2f s\n", name, Bot::now() - t0);
}

static void
test_ten_player_tournament()
{
        int port = find_free_port();
        Server srv(port, "127.0.0.1", nullptr, /*verbose=*/false,
                   /*tournament=*/true, /*level_seconds=*/2, /*countdown=*/0.5,
                   /*start_stack=*/1000, /*max_players=*/10);
        srv.table().action_timeout = 1e9; // no timeout folds
        srv.hand_pause()           = 0.1;
        g_srv = &srv;
        g_clients = make_clients(port, 10);

        CHECK(wait_until([&] { return all_seated(g_clients); }, 10.0));
        CHECK_EQ(g_srv->state().max_players, 10);
        CHECK_EQ(g_srv->table().players.size(), 10);

        // all ten seats fill -> countdown -> running
        CHECK(wait_until([&] { return g_srv->state().tournament_status == T_COUNTDOWN; }, 5.0));
        CHECK(wait_until([&] { return g_srv->state().tournament_status == T_RUNNING; }, 5.0));

        // a full hand is dealt with all ten seats in
        CHECK(wait_until([&] { return g_srv->state().hand_started; }, 5.0));
        CHECK(wait_until([&] {
                for (auto &m : g_msgs) {
                        if (m.value("type", "") == "state" &&
                            m.value("players_alive", 0) == 10) {
                                return true;
                        }
                }
                return false;
        }, 5.0));
        CHECK(wait_until([&] { return g_srv->state().hand_over; }, 15.0));
        CHECK_EQ(g_srv->table().alive_count(), 10);

        // a disconnect busts to 9 (deferred to the next round end)
        Bot *leaver = g_clients[9];
        int seat    = leaver->seat();
        leaver->close(); // the object must stay alive (lws callback), so just leak it
        g_clients.pop_back();
        CHECK(wait_until([&] { return g_srv->table().alive_count() == 9; }, 15.0));
        CHECK(wait_until([&] { return g_srv->table().players[seat].busted; }, 15.0));

        for (Bot *b : g_clients) delete b;
        g_clients.clear();
        g_msgs.clear();
        srv.stop();
}

// A player who disconnects mid-hand AND has their stack zeroed in the same
// round-end (busted by chips, pending_bust_ set) must produce exactly ONE
// player_out broadcast, not two.
static void
test_no_duplicate_player_out()
{
        int port = find_free_port();
        Server srv(port, "127.0.0.1", nullptr, /*verbose=*/false,
                   /*tournament=*/true, /*level_seconds=*/2, /*countdown=*/0.5);
        srv.table().action_timeout = 1e9;
        srv.hand_pause()           = 0.2;
        g_srv    = &srv;
        g_msgs.clear();
        g_destroyed = 0;

        g_clients = make_clients(port, 3);
        CHECK(wait_until([&] { return all_seated(g_clients); }, 10.0));
        advance(0.6); // let countdown fire + tournament_start

        // Wait for the first hand to start running.
        CHECK(wait_until([&] { return g_srv->state().stage != OVER; }, 10.0));

        // Zero the third player's stack so they will be chip-busted at round end.
        int leaver_seat = g_clients[2]->seat();
        CHECK(leaver_seat >= 0);
        g_destroyed += g_srv->table().players[leaver_seat].stack;
        g_srv->table().players[leaver_seat].stack = 0;

        // Also close their connection — this sets pending_bust_.
        g_clients[2]->close();

        // Advance until the bust is resolved.
        CHECK(wait_until([&] {
                return g_srv->table().players[leaver_seat].busted;
        }, 15.0));
        advance(0.5); // let any extra broadcasts arrive

        // Count how many player_out events were fired for that seat.
        int player_out_count = 0;
        for (auto &m : g_msgs) {
                if (m.value("type", "") == "player_out" &&
                    m.value("seat", -1) == leaver_seat) {
                        player_out_count++;
                }
        }
        // Each client receives its own copy; we have 2 remaining clients, so
        // the total count should be exactly 2 (one per remaining client), not 4.
        CHECK(player_out_count <= 2);
        CHECK(player_out_count >= 1); // at least one was fired

        for (Bot *b : g_clients) delete b;
        g_clients.clear();
        g_msgs.clear();
        srv.stop();
}

int
main()
{
        g_ctx = bot_context();
        CHECK(g_ctx != nullptr);

        // lws_service() blocks since lws 3.2: wake the client context
        // periodically or the test loops would stall (AGENTS.md)
        std::atomic<bool> ticker_run{ true };
        std::thread ticker([&] {
                while (ticker_run.load()) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(16));
                        lws_cancel_service(g_ctx);
                }
        });

        run_test("lobby_welcome", test_lobby_welcome);
        run_test("countdown_then_start", test_countdown_then_start);
        run_test("countdown_aborted", test_countdown_aborted);
        run_test("blind_levels", test_blind_levels);
        run_test("elimination_no_rebuy", test_elimination_no_rebuy);
        run_test("disconnect_eliminates", test_disconnect_eliminates);
        run_test("heads_up_and_winner", test_heads_up_and_winner);
        run_test("tournament_end_modes", test_tournament_end_modes);
        run_test("ten_player_tournament", test_ten_player_tournament);
        run_test("no_duplicate_player_out", test_no_duplicate_player_out);

        ticker_run.store(false);
        ticker.join();
        lws_context_destroy(g_ctx);

        if (g_failures == 0) {
                printf("test_tournament: all tests passed\n");
                return 0;
        }
        printf("test_tournament: %d test(s) failed\n", g_failures);
        return 1;
}
