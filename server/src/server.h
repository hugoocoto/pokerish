#pragma once

#include <atomic>
#include <deque>
#include <string>
#include <thread>
#include <vector>

#include <libwebsockets.h>

#include "poker.h"
#include "pokerstars_export.h"
#include "proto.h"

// WebSocket poker server implementing API.md. Single-threaded: all engine
// stepping, message handling and broadcasting happen inside run().
class Server
{
    public:
        // token != nullptr enables hello authentication (API.md "hello").
        // tournament != false runs tournament mode (API.md "tournament mode"):
        // no bots, seats fill with clients, blinds rise every level_seconds
        // seconds, busted/disconnected seats stay empty, last player wins.
        Server(int port, const char *host, const char *token = nullptr,
               bool verbose = true, bool tournament = false,
               double level_seconds = 300, double countdown_seconds = 10,
               int start_stack = 1000, int max_players = 6,
               const char *export_dir = "hands");
        ~Server();

        void run();  // blocks until stop()
        void stop(); // wakes run() and exits it
        void tick(double now); // one non-blocking lws poll + engine step + event detection

        Table &table() { return table_; }
        Game_State &state() { return state_; }

        // seconds between hand_over and the next hand (tests shrink it)
        double &hand_pause() { return hand_pause_; }

        // seat -> name of the client queued to join that seat ("" = none)
        std::vector<std::string> waiting_names() const;

        static int game_id() { return 1; }

    private:
        struct Session {
                lws *wsi{ nullptr };
                int seat{ -1 };        // -1 = connected, not seated
                int pending_seat{ -1 }; // reserved bot seat, taken over at end of round
                std::string name;      // hello name, applied when the seat is taken
                std::string inbuf;
                std::deque<std::string> outq;
        };

        static constexpr size_t kMaxIn = 65536;
        static constexpr int kMaxConns = 64;       // hard cap on simultaneous clients
        static constexpr size_t kMaxOutqBytes = 512 * 1024; // per-client pending send queue
        static constexpr size_t kMaxNameLen = 24;  // hello names are truncated to this
        static constexpr int kHistoryMax = 200;
        static constexpr double kHandPause = 3.0; // seconds between hand_over and next hand

        int port_;
        const char *host_;
        std::string token_; // empty = no auth
        bool verbose_{ true };
        lws_context *ctx_{ nullptr };
        std::atomic<bool> running_{ true };
        std::thread ticker_;   // wakes lws_service periodically so the engine
                               // keeps stepping even with idle clients

        Deck deck_;
        Table table_{ &deck_ };
        Game_State state_;

        bool tournament_{ false };
        double countdown_seconds_{ 10 };
        int start_stack_{ 1000 };
        int max_players_{ 6 };
        std::string export_dir_{ "hands" };
        long long hand_id_counter_{ 1 };
        PokerStarsExporter exporter_;

        std::vector<lws *> conns_;              // all live connections
        std::vector<Session *> seat_session_;   // seat -> session (null = bot plays)

        // change detection against the previous tick
        std::string last_state_str_;
        std::vector<Player::LastAction> prev_last_action_;
        std::vector<bool> prev_fold_;
        std::vector<bool> prev_turn_;
        std::vector<bool> prev_busted_;
        std::vector<bool> pending_bust_; // tournament: disconnected mid-hand, bust at round end
        int prev_stage_{ -1 };
        int prev_level_{ 0 };
        bool prev_hand_started_{ false };
        bool prev_hand_over_{ false };

        double hand_pause_{ kHandPause }; // seconds between hand_over and next hand
        double hand_over_at_{ 0 };
        double last_now_{ 0 }; // last engine clock value, for deadline messages
        std::deque<nlohmann::json> history_;

        Session *session_of(lws *wsi) const;
        std::vector<lws *> &conns();
        int free_seat() const;
        static std::string sanitize_name(const std::string &name);

        void send_to(lws *wsi, const std::string &text);
        void broadcast(const std::string &text);
        void push_history(nlohmann::json j);

        void handle_message(Session *s, lws *wsi, const std::string &text);
        void handle_hello(Session *s, lws *wsi, const proto::ClientMessage &msg);
        void handle_action(Session *s, lws *wsi, const proto::ClientMessage &msg);
        void handle_query(Session *s, lws *wsi, const proto::ClientMessage &msg);

        void give_bot(int seat);
        void process_round_end(); // pending joins + bot takeovers at hand boundary
        void disconnect(Session *s, lws *wsi);

        void step(double now);
        void detect_events(double now);
        void print_table() const;
        void logf(const char *fmt, ...) __attribute__((format(printf, 2, 3)));

        friend int server_callback(lws *wsi, enum lws_callback_reasons reason,
                                   void *user, void *in, size_t len);
};
