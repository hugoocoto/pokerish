#include "../bot/example/bot.cpp"
#include "../server/src/server.h"

#include <atomic>
#include <cstdio>
#include <cstring>
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

static void
pump(double seconds)
{
        double end = Bot::now() + seconds;
        while (Bot::now() < end) {
                lws_service(g_ctx, 10);
        }
}

template <typename F>
static bool
wait_until(F f, double timeout)
{
        double end = Bot::now() + timeout;
        while (Bot::now() < end) {
                if (f()) return true;
                lws_service(g_ctx, 10);
        }
        return f();
}

// Server in a thread; short action timeout and hand pause for fast tests.
class ScopedServer
{
    public:
        explicit ScopedServer(int port, const char *token = nullptr)
                : srv_(port, "127.0.0.1", token, /*verbose=*/false)
        {
                srv_.table().action_timeout = 0.2;
                srv_.hand_pause()           = 0.01;
                th_ = std::thread([this] { srv_.run(); });
        }
        Server &srv() { return srv_; }
        ~ScopedServer()
        {
                srv_.stop();
                th_.join();
        }

    private:
        Server srv_;
        std::thread th_;
};

static std::vector<Bot *>
make_clients(int port, int n, bool auto_play, bool send_hello = true,
             double think_seconds = 0.0, bool is_human = false)
{
        std::vector<Bot *> cs;
        for (int i = 0; i < n; i++) {
                cs.push_back(new Bot(g_ctx, "127.0.0.1", port, i, auto_play, send_hello,
                                     nullptr, nullptr, think_seconds, is_human));
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
any_turn(const std::vector<Bot *> &cs)
{
        for (Bot *b : cs) {
                const nlohmann::json &st = b->last_state();
                if (st.contains("players")) {
                        for (auto &pl : st["players"]) {
                                if (pl.value("is_turn", false)) return true;
                        }
                }
        }
        return false;
}

static int
turn_seat(const std::vector<Bot *> &cs)
{
        for (Bot *b : cs) {
                const nlohmann::json &st = b->last_state();
                if (st.contains("players")) {
                        for (auto &pl : st["players"]) {
                                if (pl.value("is_turn", false)) return pl.value("seat", -1);
                        }
                }
        }
        return -1;
}

// The seat currently acting, or nullptr. The bots never act on their own in
// these tests, so a turn lasts until its short timeout; re-read it before
// each interaction.
static Bot *
current_turn(const std::vector<Bot *> &cs)
{
        int t = turn_seat(cs);
        if (t < 0) return nullptr;
        for (Bot *b : cs) {
                if (b->seat() == t) return b;
        }
        return nullptr;
}

// Drive the hand: on your_turn -> check if free, else call. Returns the
// number of hand_over messages seen.
static int
drive_hand(const std::vector<Bot *> &cs, double seconds, int *hand_over_events)
{
        double end = Bot::now() + seconds;
        int seen   = 0;
        while (Bot::now() < end) {
                lws_service(g_ctx, 10);
                for (Bot *b : cs) {
                        for (auto &m : b->take_messages()) {
                                std::string t = m.value("type", "");
                                if (t == "your_turn") {
                                        const nlohmann::json &st = b->last_state();
                                        int street = 0;
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
                                } else if (t == "error") {
                                        if (m.value("code", "") == "illegal_action") {
                                                b->send_action("call");
                                        }
                                } else if (t == "hand_over") {
                                        seen++;
                                }
                        }
                }
        }
        if (hand_over_events) *hand_over_events = seen;
        return seen;
}

static void
test_join_and_welcome()
{
        int port = find_free_port();
        ScopedServer ss(port);
        std::vector<Bot *> cs = make_clients(port, 6, false);

        CHECK(wait_until([&] { return all_seated(cs); }, 10.0));
        CHECK(all_seated(cs));
        for (size_t i = 0; i < cs.size(); i++) {
                CHECK(cs[i]->seat() >= 0 && cs[i]->seat() < 6);
                for (size_t j = i + 1; j < cs.size(); j++) {
                        CHECK(cs[i]->seat() != cs[j]->seat());
                }
                CHECK(cs[i]->last_state().contains("players"));
                CHECK_EQ(cs[i]->last_state()["players"].size(), 6);
        }
}

static void
test_protocol_errors()
{
        int port = find_free_port();
        ScopedServer ss(port);
        std::vector<Bot *> cs = make_clients(port, 6, false);

        CHECK(wait_until([&] { return all_seated(cs); }, 10.0));
        CHECK(wait_until([&] { return any_turn(cs); }, 15.0));

        // out-of-turn action (from a seated bot that is not the current turn)
        {
                Bot *turn = current_turn(cs);
                CHECK(turn != nullptr);
                Bot *other = nullptr;
                for (Bot *b : cs) {
                        if (b != turn) {
                                other = b;
                                break;
                        }
                }
                CHECK(other != nullptr);
                other->send_action("bet", 100);
                CHECK(wait_until([&] {
                        for (auto &m : other->take_messages()) {
                                if (m.value("type", "") == "error" &&
                                    m.value("code", "") == "not_your_turn") return true;
                        }
                        return false;
                }, 2.0));
        }

        // turn player: illegal bet
        {
                Bot *turn = current_turn(cs);
                CHECK(turn != nullptr);
                turn->send_action("bet", 5);
                CHECK(wait_until([&] {
                        for (auto &m : turn->take_messages()) {
                                if (m.value("type", "") == "error" &&
                                    m.value("code", "") == "bet_too_small") return true;
                        }
                        return false;
                }, 2.0));
        }

        // turn player: garbage JSON
        {
                Bot *turn = current_turn(cs);
                CHECK(turn != nullptr);
                turn->send_raw("this is not json");
                CHECK(wait_until([&] {
                        for (auto &m : turn->take_messages()) {
                                if (m.value("type", "") == "error" &&
                                    m.value("code", "") == "bad_json") return true;
                        }
                        return false;
                }, 2.0));
        }

        // ping / pong
        {
                Bot *turn = current_turn(cs);
                CHECK(turn != nullptr);
                turn->send_json({ { "type", "ping" } });
                CHECK(wait_until([&] {
                        for (auto &m : turn->take_messages()) {
                                if (m.value("type", "") == "pong") return true;
                        }
                        return false;
                }, 2.0));
        }

        // queries
        {
                Bot *turn = current_turn(cs);
                CHECK(turn != nullptr);
                turn->send_json({ { "type", "query" }, { "id", 7 }, { "what", "my_cards" } });
                CHECK(wait_until([&] {
                        for (auto &m : turn->take_messages()) {
                                if (m.value("type", "") == "reply" && m.value("id", 0) == 7) {
                                        return m["what"] == "my_cards" &&
                                               m["data"]["cards"].size() == 2;
                                }
                        }
                        return false;
                }, 2.0));
        }
        {
                Bot *turn = current_turn(cs);
                CHECK(turn != nullptr);
                turn->send_json({ { "type", "query" }, { "id", 8 }, { "what", "state" } });
                CHECK(wait_until([&] {
                        for (auto &m : turn->take_messages()) {
                                if (m.value("type", "") == "reply" && m.value("id", 0) == 8) {
                                        return m["data"].value("type", "") == "state";
                                }
                        }
                        return false;
                }, 2.0));
        }

        // illegal check when there is a bet to call (only when paid)
        {
                Bot *turn = current_turn(cs);
                CHECK(turn != nullptr);
                bool paid = false;
                const nlohmann::json &st = turn->last_state();
                if (st.contains("players")) {
                        for (auto &pl : st["players"]) {
                                if (pl.value("seat", -1) == turn->seat() &&
                                    pl.value("street_bet", 0) < st.value("current_bet", 0)) {
                                        paid = true;
                                }
                        }
                }
                if (paid) {
                        turn->send_action("check");
                        CHECK(wait_until([&] {
                                for (auto &m : turn->take_messages()) {
                                        if (m.value("type", "") == "error" &&
                                            m.value("code", "") == "illegal_action") return true;
                                }
                                return false;
                        }, 2.0));
                } else {
                        printf("   (skipped illegal check: street was free)\n");
                }
        }

        // the current turn player never answered: timeout fold broadcast
        {
                int tseat = turn_seat(cs);
                CHECK(tseat >= 0);
                CHECK(wait_until([&] {
                        for (Bot *b : cs) {
                                for (auto &m : b->take_messages()) {
                                        if (m.value("type", "") == "action" &&
                                            m.value("action", "") == "fold" &&
                                            m.contains("reason") && m["reason"].is_string() &&
                                            m["reason"] == "timeout" &&
                                            m.value("seat", -1) == tseat) return true;
                                }
                        }
                        return false;
                }, 8.0));
        }

        // unseated client: unauthorized, then table_full
        Bot *ghost = new Bot(g_ctx, "127.0.0.1", port, 99, false, false);
        CHECK(wait_until([&] { return ghost->connected(); }, 5.0));
        ghost->send_action("fold");
        CHECK(wait_until([&] {
                for (auto &m : ghost->take_messages()) {
                        if (m.value("type", "") == "error" &&
                            m.value("code", "") == "unauthorized") return true;
                }
                return false;
        }, 2.0));
        ghost->send_json({ { "type", "hello" }, { "name", "Ghost" } });
        CHECK(wait_until([&] {
                for (auto &m : ghost->take_messages()) {
                        if (m.value("type", "") == "error" &&
                            m.value("code", "") == "table_full") return true;
                }
                return false;
        }, 2.0));
}

static void
test_tick_timeout()
{
        // Drive a Server through tick() (the GUI path, no run() thread) and
        // verify a silent remote client is folded when its action times out.
        // The other 5 seats are auto bots that answer instantly.
        int port = find_free_port();
        Server srv(port, "127.0.0.1", nullptr, /*verbose=*/false);
        srv.table().action_timeout = 0.2;
        srv.hand_pause()           = 0.01;

        // The client context has no ticker, so lws_service(g_ctx) would block
        // indefinitely once the socket goes quiet and stall this loop. Wake it
        // periodically (like the server's own ticker) so the engine clock
        // keeps advancing through srv.tick().
        std::atomic<bool> ticker_run{ true };
        std::thread ticker([&] {
                while (ticker_run.load()) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(16));
                        lws_cancel_service(g_ctx);
                }
        });

        Bot *silent = new Bot(g_ctx, "127.0.0.1", port, 0, false);
        double end = Bot::now() + 10.0; // the first bot-only round precedes the join
        while (Bot::now() < end && silent->seat() < 0) {
                srv.tick(Bot::now());
                lws_service(g_ctx, 10);
        }
        CHECK(silent->seat() >= 0);

        double deadline = 0;
        bool fold_seen  = false;
        end = Bot::now() + 8.0;
        while (Bot::now() < end && !fold_seen) {
                srv.tick(Bot::now());
                lws_service(g_ctx, 10);
                for (auto &m : silent->take_messages()) {
                        std::string t = m.value("type", "");
                        if (t == "your_turn") {
                                deadline = m.value("deadline", 0.0);
                        } else if (t == "action" && m.value("action", "") == "fold" &&
                                   m.value("seat", -1) == silent->seat() &&
                                   m["reason"].is_string() && m["reason"] == "timeout") {
                                fold_seen = true;
                        }
                }
        }
        CHECK(fold_seen);

        // deadline uses the engine clock (same base as Bot::now()), so it must
        // sit roughly timeout_seconds in the future, not in the past
        CHECK(deadline > Bot::now() - 1.0);
        CHECK(deadline < Bot::now() + 5.0);

        ticker_run.store(false);
        ticker.join();
        srv.stop();
        delete silent;
}

static void
test_simulate_never_times_out()
{
        // --simulate: bots answer instantly, but the server holds every
        // action for a random "thinking" delay (capped just below the action
        // deadline). A bot that already answered must never be folded on
        // timeout, no matter how the random delay plays out.
        int port = find_free_port();
        Server srv(port, "127.0.0.1", nullptr, /*verbose=*/false,
                   /*tournament=*/false, 300, 10, 1000, /*max_players=*/2, "hands");
        CHECK(srv.hand_pause() == 0.0); // non-simulate: hands chain instantly
        srv.table().action_timeout = 0.2;
        srv.hand_pause()           = 0.01;
        srv.set_simulate(true, 1.0); // think = exactly 1.0 s, capped at 0.15 s

        std::atomic<bool> ticker_run{ true };
        std::thread ticker([&] {
                while (ticker_run.load()) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(16));
                        lws_cancel_service(g_ctx);
                }
        });

        // heads-up: the first (bot-only) round takes ~8 x 1 s holds, then the
        // instant auto bots take over and every action is capped at 0.15 s
        std::vector<Bot *> cs = make_clients(port, 2, true);

        double end = Bot::now() + 15.0;
        while (Bot::now() < end && !all_seated(cs)) {
                srv.tick(Bot::now());
                lws_service(g_ctx, 10);
                for (Bot *b : cs) b->pump(Bot::now());
        }
        CHECK(all_seated(cs));

        int actions = 0, timeouts = 0, hand_overs = 0;
        double min_delay = 1e9, turn_at = 0;
        end = Bot::now() + 10.0;
        while (Bot::now() < end && timeouts == 0) {
                srv.tick(Bot::now());
                lws_service(g_ctx, 10);
                for (Bot *b : cs) {
                        b->pump(Bot::now());
                        for (auto &m : b->take_messages()) {
                                std::string t = m.value("type", "");
                                if (t == "your_turn") {
                                        turn_at = Bot::now();
                                } else if (t == "action") {
                                        actions++;
                                        if (m["reason"].is_string() &&
                                            m["reason"] == "timeout") {
                                                timeouts++;
                                        }
                                        if (turn_at > 0 &&
                                            m.value("seat", -1) == b->seat()) {
                                                min_delay = std::min(min_delay,
                                                        Bot::now() - turn_at);
                                        }
                                } else if (t == "hand_over") {
                                        hand_overs++;
                                }
                        }
                }
        }

        // the game progressed at a simulated human pace, nobody was folded
        // on timeout, and the hold really delayed the actions
        CHECK(actions > 0);
        CHECK(timeouts == 0);
        CHECK(hand_overs > 0);
        CHECK(min_delay >= 0.10);

        ticker_run.store(false);
        ticker.join();
        srv.stop();
        for (Bot *b : cs) delete b;
}

static void
test_simulate_human_instant()
{
        // --simulate with a human client (hello "is_human": true): the
        // human's actions apply instantly, while a plain remote bot's are
        // still held for the simulated "thinking" delay (capped at 0.15 s).
        int port = find_free_port();
        Server srv(port, "127.0.0.1", nullptr, /*verbose=*/false,
                   /*tournament=*/false, 300, 10, 1000, /*max_players=*/2, "hands");
        srv.table().action_timeout = 0.2;
        srv.hand_pause()           = 0.01;
        srv.set_simulate(true, 1.0); // think = exactly 1.0 s, capped at 0.15 s

        std::atomic<bool> ticker_run{ true };
        std::thread ticker([&] {
                while (ticker_run.load()) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(16));
                        lws_cancel_service(g_ctx);
                }
        });

        // heads-up: one human client (is_human) + one plain bot
        std::vector<Bot *> cs = make_clients(port, 1, true);
        Bot *human = new Bot(g_ctx, "127.0.0.1", port, 1, true, true, nullptr, nullptr,
                             0.0, /*is_human=*/true);
        cs.push_back(human);

        double end = Bot::now() + 15.0;
        while (Bot::now() < end && !all_seated(cs)) {
                srv.tick(Bot::now());
                lws_service(g_ctx, 10);
                for (Bot *b : cs) b->pump(Bot::now());
        }
        CHECK(all_seated(cs));

        double human_delay = 1e9, bot_delay = 1e9;
        double human_turn_at = 0, bot_turn_at = 0;
        end = Bot::now() + 10.0;
        while (Bot::now() < end && (human_delay > 0.05 || bot_delay > 0.10)) {
                srv.tick(Bot::now());
                lws_service(g_ctx, 10);
                for (Bot *b : cs) {
                        b->pump(Bot::now());
                        for (auto &m : b->take_messages()) {
                                std::string t = m.value("type", "");
                                if (t == "your_turn") {
                                        if (b == human) {
                                                human_turn_at = Bot::now();
                                        } else {
                                                bot_turn_at = Bot::now();
                                        }
                                } else if (t == "action") {
                                        double at = (b == human) ? human_turn_at : bot_turn_at;
                                        if (at > 0) {
                                                double d = Bot::now() - at;
                                                if (b == human) {
                                                        human_delay = std::min(human_delay, d);
                                                } else {
                                                        bot_delay = std::min(bot_delay, d);
                                                }
                                        }
                                }
                        }
                }
        }

        // the human's actions were not held; the bot's were (capped at 0.15 s)
        CHECK(human_delay <= 0.05);
        CHECK(bot_delay >= 0.10);

        ticker_run.store(false);
        ticker.join();
        srv.stop();
        for (Bot *b : cs) delete b;
}

static void
test_hand_flow()
{
        int port = find_free_port();
        ScopedServer ss(port);
        std::vector<Bot *> cs = make_clients(port, 6, false);

        CHECK(wait_until([&] { return all_seated(cs); }, 10.0));
        CHECK(wait_until([&] { return any_turn(cs); }, 15.0));

        int dealer_before = -1;
        int hand_overs = 0;
        double end = Bot::now() + 40.0;
        while (Bot::now() < end && hand_overs == 0) {
                drive_hand(cs, 0.25, &hand_overs);
                for (Bot *b : cs) {
                        const nlohmann::json &st = b->last_state();
                        if (st.contains("dealer")) {
                                dealer_before = st.value("dealer", -1);
                        }
                }
        }
        CHECK(hand_overs > 0);

        // chips are conserved at the hand boundary (one snapshot: same for all);
        // during the hand-over pause `pot` shows the already-awarded chips, so
        // only add it while the hand is running
        int sum = 0;
        const nlohmann::json &st = cs[0]->last_state();
        if (st.contains("players")) {
                for (auto &pl : st["players"]) {
                        sum += pl.value("stack", 0);
                }
        }
        if (!st.value("hand_over", false)) sum += st.value("pot", 0);
        CHECK_EQ(sum, 6000);

        // the next hand rotates the dealer
        int dealer_now = dealer_before;
        end = Bot::now() + 10.0;
        while (Bot::now() < end && dealer_now == dealer_before) {
                drive_hand(cs, 0.25, &hand_overs);
                const nlohmann::json &st2 = cs[0]->last_state();
                if (st2.contains("dealer")) dealer_now = st2.value("dealer", -1);
        }
        CHECK_EQ(dealer_now, (dealer_before + 1) % 6);
}

static void
test_bot_e2e()
{
        int port = find_free_port();
        ScopedServer ss(port);
        std::vector<Bot *> cs = make_clients(port, 6, true); // auto bots

        CHECK(wait_until([&] { return all_seated(cs); }, 10.0));

        // stop as soon as two hands completed (no fixed wall-clock window)
        int hand_overs = 0;
        double end = Bot::now() + 10.0;
        while (Bot::now() < end && hand_overs < 2) {
                lws_service(g_ctx, 10);
                double t = Bot::now();
                for (Bot *b : cs) {
                        b->pump(t);
                        for (auto &m : b->take_messages()) {
                                if (m.value("type", "") == "hand_over") hand_overs++;
                        }
                }
        }
        CHECK(hand_overs > 0);

        // chips + pot stay constant (one snapshot: same for all), except that
        // busted players are rebought to StartStack; during the hand-over
        // pause `pot` mirrors already-awarded chips, so skip it
        int sum = 0;
        const nlohmann::json &st = cs[0]->last_state();
        if (st.contains("players")) {
                for (auto &pl : st["players"]) {
                        sum += pl.value("stack", 0);
                }
                if (!st.value("hand_over", false)) sum += st.value("pot", 0);
        }
        CHECK(sum >= 6000);
        CHECK((sum - 6000) % StartStack == 0);
}

static void
test_end_of_round_join()
{
        int port = find_free_port();
        ScopedServer ss(port);

        // A client joining mid-hand is queued, not seated: the bot plays
        // out the round, then the client takes the seat at a full entry.
        Bot *late = new Bot(g_ctx, "127.0.0.1", port, 0, false, false); // no auto hello
        CHECK(wait_until([&] { return late->connected(); }, 5.0));

        // watch a few hands go by (broadcasts reach unseated connections)
        // so the seat bot's stack drifts away from the entry value
        int overs    = 0;
        bool was_over = false;
        CHECK(wait_until([&] {
                const nlohmann::json &st = late->last_state();
                if (!st.is_object()) return false;
                bool over = st.value("hand_over", false);
                if (over && !was_over) overs++;
                was_over = over;
                return overs >= 3;
        }, 30.0));

        // ensure a hand is actively in progress so the join is queued mid-round
        CHECK(wait_until([&] {
                return ss.srv().state().hand_started && !ss.srv().state().hand_over;
        }, 5.0));

        late->send_json({ { "type", "hello" }, { "name", "Late" } });
        CHECK(wait_until([&] {
                for (auto &m : late->take_messages()) {
                        if (m.value("type", "") == "queued") return true;
                }
                return false;
        }, 5.0));
        CHECK_EQ(late->seat(), -1); // still queued, bot still playing

        // the client seats at the end of the round with a full entry stack
        int join_stack = -1;
        CHECK(wait_until([&] {
                if (late->seat() < 0) return false;
                const nlohmann::json &st = late->last_state();
                if (!st.contains("players")) return false;
                for (auto &pl : st["players"]) {
                        if (pl.value("seat", -1) == late->seat()) {
                                join_stack = pl.value("stack", -1);
                                return join_stack >= StartStack - BigBlind &&
                                       join_stack <= StartStack;
                        }
                }
                return false;
        }, 15.0));

        // a second client joins and plays; dropping mid-hand folds it
        // (it is on turn: response-fold, broadcast as a plain fold)
        Bot *leaver = new Bot(g_ctx, "127.0.0.1", port, 1, false);
        CHECK(wait_until([&] { return leaver->seat() >= 0; }, 15.0));
        CHECK(wait_until([&] {
                return turn_seat({ late, leaver }) == leaver->seat();
        }, 15.0));
        int leaver_seat = leaver->seat();

        leaver->close();
        CHECK(wait_until([&] {
                for (auto &m : late->take_messages()) {
                        if (m.value("type", "") == "action" &&
                            m.value("action", "") == "fold" &&
                            m.value("seat", -1) == leaver_seat &&
                            (!m.contains("reason") || !m["reason"].is_string())) {
                                return true;
                        }
                }
                return false;
        }, 5.0));

        // at the end of the round a bot takes the freed seat over
        CHECK(wait_until([&] {
                const nlohmann::json &st = late->last_state();
                if (!st.contains("players")) return false;
                for (auto &pl : st["players"]) {
                        if (pl.value("seat", -1) == leaver_seat) {
                                return pl.value("name", "").rfind("Bot ", 0) == 0;
                        }
                }
                return false;
        }, 25.0));
}

static void
test_bad_token()
{
        // with --token SECRET the server rejects hello without the token
        int port = find_free_port();
        ScopedServer ss(port, "secret");

        Bot *rogue = new Bot(g_ctx, "127.0.0.1", port, 0, false); // no token
        CHECK(wait_until([&] {
                for (auto &m : rogue->take_messages()) {
                        if (m.value("type", "") == "error" &&
                            m.value("code", "") == "bad_token") return true;
                }
                return false;
        }, 5.0));
        CHECK_EQ(rogue->seat(), -1); // never seated

        Bot *honest = new Bot(g_ctx, "127.0.0.1", port, 1, false, true, "secret");
        CHECK(wait_until([&] { return honest->seat() >= 0; }, 15.0));
}

static void
test_bet_amount_bounds()
{
        // out-of-range bet amounts are rejected at parse time with bad_request
        int port = find_free_port();
        ScopedServer ss(port);

        Bot *b = new Bot(g_ctx, "127.0.0.1", port, 0, false);
        CHECK(wait_until([&] { return b->seat() >= 0; }, 15.0));

        b->send_json({ { "type", "action" }, { "action", "bet" },
                       { "amount", 999999999 } }); // > kMaxBet
        b->send_json({ { "type", "action" }, { "action", "bet" },
                       { "amount", 4611686018427387904LL } }); // int64 overflow trap
        int bad_requests = 0;
        CHECK(wait_until([&] {
                for (auto &m : b->take_messages()) {
                        if (m.value("type", "") == "error" &&
                            m.value("code", "") == "bad_request") bad_requests++;
                }
                return bad_requests >= 2;
        }, 5.0));
}

static void
test_hello_name_sanitized()
{
        // control characters are stripped and long names are truncated
        int port = find_free_port();
        ScopedServer ss(port);

        Bot *weird = new Bot(g_ctx, "127.0.0.1", port, 0, false, false); // no auto hello
        CHECK(wait_until([&] { return weird->connected(); }, 5.0));
        weird->send_json({ { "type", "hello" },
                           { "name", "Bob\nEVILBob\nEVILBob\nEVIL" } }); // 24 chars, \n inside

        std::string seated_name;
        CHECK(wait_until([&] {
                if (weird->seat() < 0) return false;
                const nlohmann::json &st = weird->last_state();
                if (!st.contains("players")) return false;
                for (auto &pl : st["players"]) {
                        if (pl.value("seat", -1) == weird->seat()) {
                                seated_name = pl.value("name", "");
                        }
                }
                return seated_name == "Bob EVILBob EVILBob EVIL";
        }, 15.0));
        CHECK_EQ((int) seated_name.size(), 24);
        CHECK(seated_name.find('\n') == std::string::npos);
}

static void
run_test(const char *name, void (*fn)())
{
        double t0 = Bot::now();
        fn();
        printf("  %-24s %.2f s\n", name, Bot::now() - t0);
}

static void
test_bust_kick()
{
        // A client that busts (0 chips) is disconnected at the round end and
        // its seat reverts to an auto bot (rebought to StartStack).
        int port = find_free_port();
        Server srv(port, "127.0.0.1", nullptr, /*verbose=*/false);
        srv.table().action_timeout = 0.2;
        srv.hand_pause()           = 0.01;

        std::atomic<bool> ticker_run{ true };
        std::thread ticker([&] {
                while (ticker_run.load()) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(16));
                        lws_cancel_service(g_ctx);
                }
        });

        Bot *b = new Bot(g_ctx, "127.0.0.1", port, 0, false, true, nullptr, "Buster");
        double end = Bot::now() + 10.0;
        while (Bot::now() < end && b->seat() < 0) {
                srv.tick(Bot::now());
                lws_service(g_ctx, 10);
        }
        CHECK(b->seat() >= 0);
        int seat = b->seat();

        // force a bust: the seat's chips go to zero
        srv.table().players[seat].stack = 0;

        // the next round end must kick the client and hand the seat to a bot
        bool kicked = false;
        end = Bot::now() + 10.0;
        while (Bot::now() < end && !kicked) {
                srv.tick(Bot::now());
                lws_service(g_ctx, 10);
                kicked = !b->connected();
        }
        CHECK(kicked);
        CHECK(srv.table().players[seat].auto_play);
        CHECK_EQ(srv.table().players[seat].stack, StartStack);

        ticker_run.store(false);
        ticker.join();
        srv.stop();
        delete b;
}

static void
test_custom_config()
{
        int port = find_free_port();
        Server srv(port, "127.0.0.1", nullptr, /*verbose=*/false,
                   /*tournament=*/false, /*level_seconds=*/300, /*countdown=*/10,
                   /*start_stack=*/500, /*max_players=*/4);
        CHECK_EQ(srv.state().start_stack, 500);
        CHECK_EQ(srv.state().max_players, 4);
        CHECK_EQ(srv.table().players.size(), 4);
        for (const auto &p : srv.table().players) {
                CHECK_EQ(p.stack, 500);
        }
}

static void
test_ten_players()
{
        int port = find_free_port();
        Server srv(port, "127.0.0.1", nullptr, /*verbose=*/false, /*tournament=*/false,
                   /*level_seconds=*/300, /*countdown_seconds=*/10, /*start_stack=*/1000,
                   /*max_players=*/10);
        srv.table().action_timeout = 0.2;
        srv.hand_pause()           = 0.01;
        std::thread th([&] { srv.run(); });

        CHECK_EQ(srv.state().max_players, 10);
        CHECK_EQ(srv.table().players.size(), 10);

        // the hard cap applies even if the caller asks for more
        Server cap(port + 1, "127.0.0.1", nullptr, false, false, 300, 10, 1000,
                   /*max_players=*/12);
        CHECK_EQ(cap.state().max_players, MaxPlayers);

        std::vector<Bot *> cs = make_clients(port, 10, false);
        CHECK(wait_until([&] { return all_seated(cs); }, 10.0));
        for (Bot *b : cs) {
                CHECK(b->seat() >= 0 && b->seat() < 10);
                CHECK_EQ(b->last_state()["players"].size(), 10);
        }

        // one full hand with all ten seats dealt in and resolving; drive in
        // small windows so we exit as soon as the hand is over
        int hand_over = 0;
        int seen      = 0;
        double end    = Bot::now() + 40.0;
        while (Bot::now() < end && hand_over == 0) {
                seen = drive_hand(cs, 0.5, &hand_over);
        }
        CHECK(hand_over >= 1);
        CHECK(seen >= 1);

        for (Bot *b : cs) delete b;
        srv.stop();
        th.join();
}

// Sending a name that contains a multibyte UTF-8 sequence that would be
// sliced by naive resize() must not crash the server via a json::dump()
// exception.  The name is sanitised to a valid UTF-8 string on the server side.
static void
test_utf8_name_no_crash()
{
        int port = find_free_port();
        ScopedServer ss(port);

        // 🃁  is the 4-byte sequence F0 9F 83 81.  Embed it at the edge of the
        // 24-byte limit so a naive resize(24) would split it.
        // 20 ASCII chars + 4-byte emoji = 24 bytes exactly, but kMaxNameLen==24
        // means only 24 bytes survive, and the emoji starts at byte 20 so the
        // full codepoint is kept.  Add a second emoji so a 24-byte cut lands
        // inside the second one and the UTF-8-aware truncator drops the partial.
        std::string emoji_name = "12345678901234567890" // 20 bytes
                                 "\xF0\x9F\x83\x81"     // 4-byte emoji (24 total)
                                 "\xF0\x9F\x83\x82";    // another emoji (28 total, cut here)

        // Use send_hello=true so the Bot sends hello automatically inside
        // on_connect(), just like every other test — this avoids the race
        // where manual send_json fires before the WS handshake completes.
        Bot *b = new Bot(g_ctx, "127.0.0.1", port, 0, false, /*send_hello=*/true,
                         /*token=*/nullptr, emoji_name.c_str());

        // Wait for welcome or queued; crucially the server must still be alive.
        bool got_response = wait_until([&] {
                for (auto &m : b->take_messages()) {
                        std::string t = m.value("type", "");
                        if (t == "welcome" || t == "queued" || t == "error") return true;
                }
                return false;
        }, 5.0);
        CHECK(got_response); // server replied, did not crash

        // Verify the server is still processing new clients after the UTF-8 name.
        Bot *b2 = new Bot(g_ctx, "127.0.0.1", port, 1, false, /*send_hello=*/true,
                          /*token=*/nullptr, "NormalBot");
        bool b2_got = wait_until([&] {
                for (auto &m : b2->take_messages()) {
                        if (m.value("type", "") == "welcome" ||
                            m.value("type", "") == "queued") return true;
                }
                return false;
        }, 5.0);
        CHECK(b2_got); // server still alive and accepting connections

        delete b;
        delete b2;
}

// Sending a payload larger than kMaxIn must drop the connection (return -1
// in the callback), not keep it alive and cycle fill->clear indefinitely.
static void
test_dos_connection_dropped_on_overflow()
{
        int port = find_free_port();
        ScopedServer ss(port);

        // Connect a bot but do NOT send hello — we will send a raw oversized payload.
        Bot *b = new Bot(g_ctx, "127.0.0.1", port, 0, false, /*send_hello=*/false);
        // Wait for the connection to establish (WS handshake needs lws service).
        pump(0.3);
        CHECK(b->connected());

        // Send a payload larger than kMaxIn (65536 bytes) and pump so lws
        // delivers the write to the server.
        std::string giant(70000, 'x');
        b->send_raw(giant);
        pump(0.1); // let lws flush the outbound write

        // The server should drop the connection: the bot disconnects.
        bool disconnected = wait_until([&] {
                return !b->connected();
        }, 5.0);
        CHECK(disconnected); // connection was dropped, not kept alive

        delete b;
}

int
main()
{
        g_ctx = bot_context();
        CHECK(g_ctx != nullptr);

        run_test("join_and_welcome", test_join_and_welcome);
        run_test("protocol_errors", test_protocol_errors);
        run_test("tick_timeout", test_tick_timeout);
        run_test("simulate_never_times_out", test_simulate_never_times_out);
        run_test("simulate_human_instant", test_simulate_human_instant);
        run_test("hand_flow", test_hand_flow);
        run_test("bot_e2e", test_bot_e2e);
        run_test("end_of_round_join", test_end_of_round_join);
        run_test("bad_token", test_bad_token);
        run_test("bet_amount_bounds", test_bet_amount_bounds);
        run_test("hello_name_sanitized", test_hello_name_sanitized);
        run_test("bust_kick", test_bust_kick);
        run_test("custom_config", test_custom_config);
        run_test("ten_players", test_ten_players);
        run_test("utf8_name_no_crash", test_utf8_name_no_crash);
        run_test("dos_connection_dropped_on_overflow", test_dos_connection_dropped_on_overflow);

        lws_context_destroy(g_ctx);

        if (g_failures == 0) {
                printf("test_server: all tests passed\n");
                return 0;
        }
        printf("test_server: %d test(s) failed\n", g_failures);
        return 1;
}
