// Single-file poker bot: the whole client (transport, protocol, strategy,
// CLI) in one .cpp — libwebsockets + nlohmann/json, speaking the API.md
// WebSocket/JSON protocol. It connects to the server, says hello, and plays
// random-ish hands instantly (no "thinking" delay; use the server's
// --simulate flag to add one).
//
// The strategy lives in Bot::choose_and_send(): a smarter bot only needs to
// change that one method.
//
// Two build modes in one file:
//   - Standalone client:  make            -> build/example_bot
//     (the Makefile adds -DBOT_EXAMPLE_STANDALONE to enable main())
//   - Test client:        test/test_server.cpp includes this file directly
//     and drives the Bot class (no main). auto_play=false bots wait to be
//     told what to do; inbox/take_messages let tests read incoming messages.
//
// Run: ./build/example_bot [--port N] [--host IP] [--token SECRET] [--name NAME]

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <random>
#include <string>
#include <thread>
#include <vector>

// libwebsockets.h pulls in windows.h, whose names clash with raylib's
// (Rectangle, CloseWindow, ShowCursor, LoadImage, DrawText...). Rename them
// while windows.h is parsed, like raylib's own rcore_desktop_win32.c does,
// so this file can be included in the same TU as raylib.h (the GUI client).
#if defined(_WIN32)
    #define CloseWindow CloseWindowLws
    #define Rectangle  RectangleLws
    #define ShowCursor ShowCursorLws
    #define LoadImage  LoadImageLws
    #define DrawTextA  DrawTextALws
    #define DrawTextW  DrawTextWLws
    #define DrawTextExA DrawTextExALws
    #define DrawTextExW DrawTextExWLws
    #include <libwebsockets.h>
    #undef CloseWindow
    #undef Rectangle
    #undef ShowCursor
    #undef LoadImage
    #undef DrawTextA
    #undef DrawTextW
    #undef DrawTextExA
    #undef DrawTextExW
    #undef DrawText     // leftover winuser.h macros
    #undef DrawTextEx
#else
    #include <libwebsockets.h>
#endif
#include <nlohmann/json.hpp>

template <typename T>
static T jval(const nlohmann::json &j, const char *key, const T &def) {
        if (!j.is_object() || !j.contains(key)) return def;
        const auto &val = j[key];
        if (val.is_null()) return def;
        try {
                return val.get<T>();
        } catch (...) {
                return def;
        }
}

// ---------------------------------------------------------------------------
// Small helpers: monotonic clock, RNG, random person name
// ---------------------------------------------------------------------------

static double
monotonic_now()
{
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static std::mt19937 &
bot_rng()
{
        static std::mt19937 rng(std::random_device{}());
        return rng;
}

static double
rand01()
{
        return std::uniform_real_distribution<double>(0.0, 1.0)(bot_rng());
}

static const char *kNames[] = {
        "Bob", "John", "Alice", "Emma", "Liam", "Olivia",
        "Noah", "Ava", "Ethan", "Mia", "Lucas", "Sofia",
};

static std::string
random_name()
{
        return kNames[std::uniform_int_distribution<size_t>(
        0, sizeof(kNames) / sizeof(kNames[0]) - 1)(bot_rng())];
}

// ---------------------------------------------------------------------------
// Bot: one WebSocket connection = one player.
//
// Everything the connection does happens in the lws event loop (callback),
// while pump() drives reconnect + auto-actions from the main/test loop.
// ---------------------------------------------------------------------------

static lws_context *bot_context(); // forward decl: friend needs it

class Bot
{
    public:
        // `index` only tags the bot; `auto_play` bots decide on their own,
        // manual bots wait for take_messages().
        // `send_hello` bots say hello right after connecting.
        // `think_seconds` is a minimum delay before an auto_play bot acts;
        // 0 makes bots decide instantly (default, and what tests use).
        // `is_human` marks a human client in hello: the server then applies
        // its actions with no simulated "thinking" delay (--simulate).
        Bot(lws_context *ctx, const char *host, int port, int index,
            bool auto_play = true, bool send_hello = true,
            const char *token = nullptr, const char *name = nullptr,
            double think_seconds = 0.0, bool is_human = false)
        : ctx_(ctx), host_(host), port_(port), index_(index),
          name_(name && name[0] ? name : random_name()), token_(token ? token : ""),
          auto_play_(auto_play), send_hello_(send_hello), think_seconds_(think_seconds),
          is_human_(is_human)
        {
                connect();
        }

        // Drop the connection and detach the wsi's userdata so the late close
        // callback (it can fire well after this object is gone) skips it
        // instead of clobbering freed memory that a newer Bot may own.
        ~Bot()
        {
                if (wsi_) {
                        lws_set_wsi_user(wsi_, nullptr);
                        lws_set_timeout(wsi_, PENDING_TIMEOUT_KILLED_BY_PROXY_CLIENT_CLOSE,
                                        LWS_TO_KILL_ASYNC);
                }
        }

        static double now() { return monotonic_now(); }

        bool connected() const { return wsi_ && established_; }
        int seat() const { return seat_; }
        const std::string &name() const { return name_; }
        const nlohmann::json &last_state() const { return state_; }
        std::vector<nlohmann::json> take_messages();

        // Reconnect + auto-action logic; call once per lws_service() loop.
        void pump(double t);
        void send_action(const std::string &action, int amount = 0);
        void send_json(const nlohmann::json &j);
        void send_raw(const std::string &text);
        void close(); // drop the connection (tests)

    private:
        friend lws_context *bot_context();
        static int callback(lws *wsi, enum lws_callback_reasons reason,
                            void *user, void *in, size_t len);

        void connect();
        void flush();
        void on_message(const nlohmann::json &j);
        void choose_and_send();

        lws_context *ctx_;
        std::string host_;
        int port_;
        int index_;
        std::string name_;
        std::string token_;
        bool auto_play_;
        bool send_hello_;
        double think_seconds_;
        bool is_human_;

        lws *wsi_{ nullptr };
        bool established_{ false };
        std::string inbuf_;
        std::deque<std::string> outq_;
        int seat_{ -1 };
        nlohmann::json state_;
        bool thinking_{ false };
        double decide_at_{ 0 };
        double last_connect_try_{ 0 };
        std::vector<nlohmann::json> inbox_;
};

// Client lws context with the "poker" protocol registered.
static lws_context *
bot_context()
{
        static struct lws_protocols protocols[] = {
                { "poker", Bot::callback, 0, 4096, 0, nullptr, 0 },
                LWS_PROTOCOL_LIST_TERM,
        };
        struct lws_context_creation_info info{};
        info.port      = CONTEXT_PORT_NO_LISTEN;
        info.protocols = protocols;
        return lws_create_context(&info);
}

// ---------------------------------------------------------------------------
// lws glue: connection, send queue
// ---------------------------------------------------------------------------

void
Bot::connect()
{
        struct lws_client_connect_info i{};
        i.context                   = ctx_;
        i.address                   = host_.c_str();
        i.port                      = port_;
        i.path                      = "/";
        i.protocol                  = "poker";
        i.userdata                  = this;
        i.ietf_version_or_minus_one = -1;
        wsi_                        = lws_client_connect_via_info(&i);
}

void
Bot::close()
{
        if (!wsi_) return;
        lws_set_timeout(wsi_, PENDING_TIMEOUT_KILLED_BY_PROXY_CLIENT_CLOSE, LWS_TO_KILL_ASYNC);
}

void
Bot::flush()
{
        if (outq_.empty()) return;
        const std::string &msg = outq_.front();
        std::string buf(LWS_PRE, '\0');
        buf.append(msg);
        int n = lws_write(wsi_, (unsigned char *) buf.data() + LWS_PRE, msg.size(),
                          LWS_WRITE_TEXT);
        if (n < (int) msg.size()) {
                lws_callback_on_writable(wsi_); // retry the frame later
                return;
        }
        outq_.pop_front();
        if (!outq_.empty()) lws_callback_on_writable(wsi_);
}

void
Bot::send_json(const nlohmann::json &j)
{
        outq_.push_back(j.dump());
        if (wsi_) lws_callback_on_writable(wsi_);
}

void
Bot::send_raw(const std::string &text)
{
        outq_.push_back(text);
        if (wsi_) lws_callback_on_writable(wsi_);
}

void
Bot::send_action(const std::string &action, int amount)
{
        nlohmann::json j = { { "type", "action" }, { "action", action } };
        if (action == "bet") j["amount"] = amount;
        send_json(j);
}

std::vector<nlohmann::json>
Bot::take_messages()
{
        std::vector<nlohmann::json> v = std::move(inbox_);
        inbox_.clear();
        return v;
}

int
Bot::callback(lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len)
{
        // The wsi's userdata is nulled by ~Bot; the connection may outlive the
        // Bot object (e.g. killed with LWS_TO_KILL_ASYNC), so skip it.
        if (!user) return 0;
        Bot *b = (Bot *) user;
        switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
                b->wsi_         = wsi;
                b->established_ = true;
                if (b->send_hello_) {
                        nlohmann::json hello = { { "type", "hello" }, { "name", b->name_ } };
                        if (!b->token_.empty()) hello["token"] = b->token_;
                        if (b->is_human_) hello["is_human"] = true;
                        b->send_json(hello);
                }
                break;

        case LWS_CALLBACK_CLIENT_RECEIVE:
                b->inbuf_.append((const char *) in, len);
                if (lws_is_final_fragment(wsi) && lws_remaining_packet_payload(wsi) == 0) {
                        std::string msg = std::move(b->inbuf_);
                        b->inbuf_.clear();
                        try {
                                b->on_message(nlohmann::json::parse(msg));
                        } catch (...) {
                                printf("%s: unparseable (%zu bytes)\n", b->name_.c_str(),
                                       msg.size());
                        }
                }
                break;

        case LWS_CALLBACK_CLIENT_WRITEABLE:
                b->flush();
                break;

        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
                b->wsi_         = nullptr;
                b->established_ = false;
                printf("%s: connection error\n", b->name_.c_str());
                break;

        case LWS_CALLBACK_CLIENT_CLOSED:
                b->wsi_         = nullptr;
                b->established_ = false;
                printf("%s: connection closed\n", b->name_.c_str());
                break;

        case LWS_CALLBACK_CLOSED:
                b->wsi_         = nullptr;
                b->established_ = false;
                printf("%s: connection closed\n", b->name_.c_str());
                break;

        default:
                break;
        }
        return 0;
}

// ---------------------------------------------------------------------------
// Message handling: keep the latest state, log the game, act on our turn
// ---------------------------------------------------------------------------

void
Bot::on_message(const nlohmann::json &j)
{
        inbox_.push_back(j);
        std::string type = jval<std::string>(j, "type", "");

        if (type == "welcome") {
                seat_ = jval<int>(j, "seat", -1);
                printf("%s: joined as seat %d\n", name_.c_str(), seat_);
        } else if (type == "your_turn") {
                thinking_  = true;
                decide_at_ = now() + think_seconds_;
        } else if (type == "state") {
                if (j.is_object()) state_ = j;
        } else if (type == "action" || type == "stage" || type == "hand_over" ||
                   type == "tournament_start" || type == "level" ||
                   type == "player_out" || type == "tournament_over") {
                if (j.contains("state") && j["state"].is_object()) state_ = j["state"];
                if (type == "action") {
                        std::string reason = jval<std::string>(j, "reason", "");
                        printf("   %s: P%d %s%s\n", name_.c_str(), jval<int>(j, "seat", -1),
                               jval<std::string>(j, "action", "?").c_str(),
                               reason.empty() ? "" : (" (" + reason + ")").c_str());
                } else if (type == "hand_over") {
                        printf("   %s: %s\n", name_.c_str(),
                               jval<std::string>(j, "result", "").c_str());
                } else if (type == "tournament_start") {
                        int sb = 5, bb = 10, ante = 0;
                        if (j.contains("blinds") && j["blinds"].is_object()) {
                                sb   = jval<int>(j["blinds"], "small", 5);
                                bb   = jval<int>(j["blinds"], "big", 10);
                                ante = jval<int>(j["blinds"], "ante", 0);
                        }
                        printf("   %s: == Tournament Started! Level %d (Blinds %d/%d, Ante %d) ==\n",
                               name_.c_str(), jval<int>(j, "level", 1), sb, bb, ante);
                } else if (type == "level") {
                        int sb = 5, bb = 10, ante = 0;
                        if (j.contains("blinds") && j["blinds"].is_object()) {
                                sb   = jval<int>(j["blinds"], "small", 5);
                                bb   = jval<int>(j["blinds"], "big", 10);
                                ante = jval<int>(j["blinds"], "ante", 0);
                        }
                        printf("   %s: == Level Up! Level %d (Blinds %d/%d, Ante %d) ==\n",
                               name_.c_str(), jval<int>(j, "level", 1), sb, bb, ante);
                } else if (type == "player_out") {
                        printf("   %s: == P%d eliminated (%s) ==\n", name_.c_str(),
                               jval<int>(j, "seat", -1), jval<std::string>(j, "reason", "out").c_str());
                } else if (type == "tournament_over") {
                        std::string winner_name = "?";
                        if (j.contains("winner") && j["winner"].is_object()) {
                                winner_name = jval<std::string>(j["winner"], "name", "?");
                        }
                        printf("   %s: == Tournament Over! %s wins %d chips! ==\n",
                               name_.c_str(), winner_name.c_str(), jval<int>(j, "award", 0));
                }
        } else if (type == "error") {
                std::string code = jval<std::string>(j, "code", "");
                printf("   %s: error %s\n", name_.c_str(), code.c_str());
                thinking_ = false;
                if (auto_play_ && (code == "bet_too_small" || code == "illegal_action")) {
                        send_action("fold"); // always legal fallback
                }
        }
}

void
Bot::pump(double t)
{
        if (!wsi_) {
                if (t - last_connect_try_ >= 1.0) {
                        last_connect_try_ = t;
                        connect();
                }
                return;
        }
        if (!auto_play_ || !thinking_) return;
        if (t < decide_at_) return;

        thinking_ = false;
        choose_and_send();
}

// ---------------------------------------------------------------------------
// The actual strategy: read the public state, pick an action.
// This is the only method a smarter bot needs to change.
// ---------------------------------------------------------------------------

void
Bot::choose_and_send()
{
        if (!state_.is_object() || !state_.contains("players") || !state_["players"].is_array()) return;

        int current_bet = jval<int>(state_, "current_bet", 0);
        int min_raise   = jval<int>(state_, "min_raise", 10);
        int street      = 0;
        int stack       = 0;
        for (const auto &pl : state_["players"]) {
                if (pl.is_object() && jval<int>(pl, "seat", -1) == seat_) {
                        street = jval<int>(pl, "street_bet", 0);
                        stack  = jval<int>(pl, "stack", 0);
                        break;
                }
        }
        int to_call = current_bet - street;
        double r    = rand01();

        if (to_call <= 0) {
                if (r < 0.75) {
                        send_action("check");
                } else {
                        int inc = min_raise * (1 + (int) (rand01() * 2.9)); // 1..3x min_raise
                        if (current_bet + inc >= stack + street) {
                                inc = stack + street - current_bet; // all-in
                        }
                        if (inc <= 0) {
                                send_action("check");
                        } else {
                                send_action("bet", inc);
                        }
                }
        } else if (to_call * 3 > stack) {
                if (r < 0.7) {
                        send_action("fold");
                } else {
                        send_action("call_all");
                }
        } else if (r < 0.7) {
                send_action("call");
        } else if (r < 0.8) {
                int inc = min_raise * (1 + (int) (rand01() * 1.9)); // 1..2x min_raise
                if (current_bet + inc >= stack + street) {
                        inc = stack + street - current_bet; // all-in
                }
                if (inc <= 0) {
                        send_action("call");
                } else {
                        send_action("bet", inc);
                }
        } else {
                send_action("fold");
        }
}

// ---------------------------------------------------------------------------
// main: CLI, lws loop, and a ticker thread (only in the standalone build).
// lws_service() blocks (it ignores its timeout since lws 3.2), so the ticker
// wakes it every 16 ms to keep pump() advancing the "thinking" logic.
// ---------------------------------------------------------------------------

#ifdef BOT_EXAMPLE_STANDALONE
int
main(int argc, char **argv)
{
        setvbuf(stdout, nullptr, _IOLBF, 0); // live logs even when redirected
        int port = 9000;
        std::string host = "127.0.0.1", token, name;

        for (int i = 1; i < argc; i++) {
                if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
                        port = atoi(argv[++i]);
                } else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
                        host = argv[++i];
                } else if (strcmp(argv[i], "--token") == 0 && i + 1 < argc) {
                        token = argv[++i];
                } else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
                        name = argv[++i];
                } else {
                        fprintf(stderr,
                                "usage: %s [--port N] [--host IP] [--token SECRET] [--name NAME]\n",
                                argv[0]);
                        return 1;
                }
        }

        lws_context *ctx = bot_context();
        if (!ctx) {
                fprintf(stderr, "failed to create lws context\n");
                return 1;
        }

        Bot bot(ctx, host.c_str(), port, 0, true, true, token.c_str(), name.c_str());
        printf("%s: connecting to %s:%d\n", bot.name().c_str(), host.c_str(), port);

        std::atomic<bool> running{ true };
        std::thread ticker([&] {
                while (running.load()) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(16));
                        lws_cancel_service(ctx);
                }
        });

        for (;;) {
                lws_service(ctx, 20);
                bot.pump(Bot::now());
        }
}
#endif
