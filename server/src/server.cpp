#include "server.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "proto.h"
#include "tournament.h"

static double
monotonic_now()
{
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
}

int
server_callback(lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len)
{
        Server *srv = (Server *) lws_context_user(lws_get_context(wsi));
        switch (reason) {
        case LWS_CALLBACK_ESTABLISHED: {
                if ((int) srv->conns().size() >= Server::kMaxConns) {
                        srv->logf("rejecting connection: too many clients\n");
                        return -1; // no Session allocated, nothing to leak
                }
                Server::Session *s = new Server::Session;
                s->wsi = wsi;
                lws_set_wsi_user(wsi, s);
                srv->conns().push_back(wsi);
                srv->logf("client connected\n");
                break;
        }

        case LWS_CALLBACK_RECEIVE: {
                Server::Session *s = (Server::Session *) lws_wsi_user(wsi);
                s->inbuf.append((const char *) in, len);
                if (s->inbuf.size() > Server::kMaxIn) {
                        srv->send_to(wsi, proto::serialize_error(0, proto::Err::BAD_REQUEST));
                        s->inbuf.clear();
                        break;
                }
                if (lws_is_final_fragment(wsi) && lws_remaining_packet_payload(wsi) == 0) {
                        std::string msg = std::move(s->inbuf);
                        s->inbuf.clear();
                        srv->handle_message(s, wsi, msg);
                }
                break;
        }

        case LWS_CALLBACK_SERVER_WRITEABLE: {
                Server::Session *s = (Server::Session *) lws_wsi_user(wsi);
                if (!s) break;
                // drain the whole queue: re-arming one message at a time with
                // lws_callback_on_writable() can stall the rest on idle
                // connections (no further events to re-arm from)
                while (!s->outq.empty()) {
                        const std::string &msg = s->outq.front();
                        std::string buf(LWS_PRE, '\0');
                        buf.append(msg);
                        if (lws_write(wsi, (unsigned char *) buf.data() + LWS_PRE,
                                      msg.size(), LWS_WRITE_TEXT) < (int) msg.size()) {
                                return -1; // dead socket: lws closes it, CLOSED cleans up
                        }
                        s->outq.pop_front();
                }
                break;
        }

        case LWS_CALLBACK_CLOSED: {
                Server::Session *s = (Server::Session *) lws_wsi_user(wsi);
                srv->disconnect(s, wsi);
                break;
        }

        default:
                break;
        }
        return 0;
}

Server::Server(int port, const char *host, const char *token, bool verbose,
               bool tournament, double level_seconds, double countdown_seconds,
               int start_stack, int max_players, const char *export_dir)
        : port_(port), host_(host), token_(token ? token : ""), verbose_(verbose),
          tournament_(tournament), countdown_seconds_(countdown_seconds),
          start_stack_(std::max(1, start_stack)),
          max_players_(std::max(2, std::min(max_players, 6))),
          export_dir_(export_dir ? export_dir : "")
{
        static struct lws_protocols protocols[] = {
                { "poker", server_callback, 0, 4096, 0, nullptr, 0 },
                LWS_PROTOCOL_LIST_TERM,
        };

        for (int i = 0; i < max_players_; i++) {
                if (tournament_) {
                        // empty seat: joinable by the first hello, no bot
                        table_.add_player(Player("Seat " + std::to_string(i + 1)));
                        table_.players.back().stack     = start_stack_;
                        table_.players.back().busted    = true;
                        table_.players.back().auto_play = false;
                } else {
                        table_.add_player(Player("Bot " + std::to_string(i + 1)));
                        table_.players.back().stack     = start_stack_;
                }
        }
        seat_session_.assign(max_players_, nullptr);
        prev_last_action_.resize(max_players_);
        prev_fold_.assign(max_players_, false);
        prev_turn_.assign(max_players_, false);
        prev_busted_.resize(max_players_);
        pending_bust_.assign(max_players_, false);
        for (int i = 0; i < max_players_; i++) {
                prev_busted_[i] = table_.players[i].busted;
        }

        state_.tournament     = tournament_;
        state_.level_seconds  = level_seconds;
        state_.start_stack    = start_stack_;
        state_.max_players    = max_players_;
        if (tournament_) {
                logf("tournament mode: %d seats, start stack %d, %g s levels, %g s countdown\n",
                     max_players_, start_stack_, level_seconds, countdown_seconds);
        }

        struct lws_context_creation_info info{};
        info.port      = port_;
        info.iface     = host_; // nullptr = all interfaces
        info.protocols = protocols;
        info.user      = this;
        ctx_ = lws_create_context(&info);
        if (!ctx_) {
                fprintf(stderr, "lws_create_context failed (port %d)\n", port_);
                exit(1);
        }
        this->logf("poker server listening on port %d\n", port_);

        // lws_service() blocks until an event is queued (the timeout argument
        // is ignored since lws 3.2); the ticker wakes it periodically so the
        // engine keeps stepping even with idle clients. Used by both run()
        // (headless) and tick() (GUI).
        ticker_ = std::thread([this] {
                while (running_.load()) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(16));
                        lws_cancel_service(ctx_);
                }
        });
}

Server::~Server()
{
        running_.store(false);
        if (ctx_) lws_cancel_service(ctx_);
        if (ticker_.joinable()) ticker_.join();
        if (ctx_) lws_context_destroy(ctx_);
}

void
Server::run()
{
        while (running_.load()) {
                lws_service(ctx_, 20);
                double now = monotonic_now();
                last_now_  = now;
                this->step(now);
                this->detect_events(now);
        }
}

void
Server::tick(double now)
{
        lws_service(ctx_, 0); // returns immediately: ticker wakes it regularly
        last_now_ = now;
        this->step(now);
        this->detect_events(now);
}

void
Server::stop()
{
        running_.store(false);
        if (ctx_) lws_cancel_service(ctx_);
}

std::vector<lws *> &
Server::conns()
{
        return conns_;
}

std::vector<std::string>
Server::waiting_names() const
{
        std::vector<std::string> out(max_players_);
        for (lws *wsi : conns_) {
                const Session *s = session_of(wsi);
                if (s && s->pending_seat >= 0 && (size_t) s->pending_seat < out.size()) {
                        out[s->pending_seat] = s->name;
                }
        }
        return out;
}

Server::Session *
Server::session_of(lws *wsi) const
{
        return (Session *) lws_wsi_user(wsi);
}

int
Server::free_seat() const
{
        for (int i = 0; i < max_players_; i++) {
                if (seat_session_[i] != nullptr) continue;
                bool reserved = false;
                for (lws *wsi : conns_) {
                        const Session *s = session_of(wsi);
                        if (s && s->pending_seat == i) {
                                reserved = true;
                                break;
                        }
                }
                if (!reserved) return i;
        }
        return -1;
}

std::string
Server::sanitize_name(const std::string &name)
{
        std::string out = name;
        if (out.size() > kMaxNameLen) out.resize(kMaxNameLen);
        for (char &c : out) {
                if ((unsigned char) c < 0x20) c = ' '; // strip control characters
        }
        if (out.empty()) out = "Player";
        return out;
}

void
Server::send_to(lws *wsi, const std::string &text)
{
        Session *s = session_of(wsi);
        if (!s) return;
        size_t pending = 0;
        for (auto &m : s->outq) pending += m.size();
        if (pending + text.size() > kMaxOutqBytes) {
                // client not draining its queue (slow or unread socket): drop it
                logf("dropping client: send queue overflow\n");
                lws_set_timeout(wsi, PENDING_TIMEOUT_KILLED_BY_PROXY_CLIENT_CLOSE,
                                LWS_TO_KILL_ASYNC);
                return;
        }
        s->outq.push_back(text);
        lws_callback_on_writable(wsi);
}

void
Server::broadcast(const std::string &text)
{
        for (lws *wsi : conns_) {
                this->send_to(wsi, text);
        }
}

void
Server::push_history(nlohmann::json j)
{
        j.erase("state"); // trimmed: events without the embedded snapshot
        history_.push_back(std::move(j));
        while ((int) history_.size() > kHistoryMax) {
                history_.pop_front();
        }
}

void
Server::handle_message(Session *s, lws *wsi, const std::string &text)
{
        proto::ClientMessage cm;
        proto::Err e = proto::parse_message(text, cm);
        if (e != proto::Err::NONE) {
                send_to(wsi, proto::serialize_error(cm.id, e));
                return;
        }
        switch (cm.type) {
        case proto::MsgType::HELLO: this->handle_hello(s, wsi, cm); break;
        case proto::MsgType::ACTION: this->handle_action(s, wsi, cm); break;
        case proto::MsgType::QUERY: this->handle_query(s, wsi, cm); break;
        case proto::MsgType::PING: send_to(wsi, proto::serialize_pong()); break;
        default: break;
        }
}

void
Server::handle_hello(Session *s, lws *wsi, const proto::ClientMessage &msg)
{
        if (s->seat >= 0 || s->pending_seat >= 0) {
                send_to(wsi, proto::serialize_error(msg.id, proto::Err::BAD_REQUEST));
                return;
        }
        if (!token_.empty() && msg.token != token_) {
                send_to(wsi, proto::serialize_error(msg.id, proto::Err::BAD_TOKEN));
                return;
        }

        if (tournament_) {
                // tournament: no bots, so a client seats immediately in the
                // lobby/countdown; once it is running (or over), no late join
                if (state_.tournament_status == T_RUNNING ||
                    state_.tournament_status == T_FINISHED) {
                        send_to(wsi, proto::serialize_error(msg.id, proto::Err::TABLE_FULL));
                        return;
                }
                int seat = -1;
                for (int i = 0; i < max_players_; i++) {
                        if (seat_session_[i] == nullptr && table_.players[i].busted) {
                                seat = i;
                                break;
                        }
                }
                if (seat < 0) {
                        send_to(wsi, proto::serialize_error(msg.id, proto::Err::TABLE_FULL));
                        return;
                }
                std::string name = sanitize_name(msg.name);
                s->seat          = seat;
                seat_session_[seat] = s;
                Player &p        = table_.players[seat];
                p.name           = name;
                p.stack          = start_stack_;
                p.busted         = false;
                p.auto_play      = false;
                logf("P%d %s joined the tournament\n", seat, name.c_str());

                send_to(wsi, proto::serialize_welcome(game_id(), seat, name));
                send_to(wsi, proto::state_json(table_, state_, game_id()).dump());
                return;
        }

        int seat = this->free_seat();
        if (seat < 0) {
                send_to(wsi, proto::serialize_error(msg.id, proto::Err::TABLE_FULL));
                return;
        }

        // the seat is reserved, but the occupying bot finishes the round;
        // the actual takeover (name, full entry stack) happens in
        // process_round_end() when the hand is over
        std::string name = sanitize_name(msg.name);

        s->pending_seat = seat;
        s->name         = name;
        logf("P%d %s waiting to join (end of round)\n", seat, name.c_str());

        send_to(wsi, proto::serialize_queued(seat));
}

void
Server::handle_action(Session *s, lws *wsi, const proto::ClientMessage &msg)
{
        if (s->seat < 0) {
                send_to(wsi, proto::serialize_error(msg.id, proto::Err::UNAUTHORIZED));
                return;
        }
        Player &p  = table_.players[s->seat];
        Player::Response resp;
        proto::Err e = proto::validate_action(state_, p, s->seat, msg, resp);
        if (e != proto::Err::NONE) {
                send_to(wsi, proto::serialize_error(msg.id, e));
                return;
        }
        p.response = resp;
}

void
Server::handle_query(Session *s, lws *wsi, const proto::ClientMessage &msg)
{
        if (s->seat < 0) {
                send_to(wsi, proto::serialize_error(msg.id, proto::Err::UNAUTHORIZED));
                return;
        }
        const Player &p = table_.players[s->seat];

        if (msg.query == "state") {
                send_to(wsi, proto::serialize_reply(
                        msg.id, "state", proto::state_json(table_, state_, game_id())));
        } else if (msg.query == "my_cards") {
                nlohmann::json cards = nlohmann::json::array();
                if (p.hand.has_value()) {
                        cards.push_back(proto::card_name(p.hand->at(0)));
                        cards.push_back(proto::card_name(p.hand->at(1)));
                } else {
                        cards.push_back(nullptr);
                        cards.push_back(nullptr);
                }
                send_to(wsi, proto::serialize_reply(msg.id, "my_cards", { { "cards", cards } }));
        } else if (msg.query == "history") {
                nlohmann::json hist = nlohmann::json::array();
                for (auto &h : history_) {
                        hist.push_back(h);
                }
                send_to(wsi, proto::serialize_reply(msg.id, "history", hist));
        }
}

void
Server::give_bot(int seat)
{
        Player &p = table_.players[seat];
        p.name     = "Bot " + std::to_string(seat + 1);
        p.auto_play = true;
}

void
Server::process_round_end()
{
        if (tournament_) {
                // Apply deferred busts for players who disconnected mid-hand.
                // Their session was already freed in disconnect(); we just mark
                // the seat as eliminated and broadcast the player_out event now.
                for (int i = 0; i < max_players_; i++) {
                        if (!pending_bust_[i]) continue;
                        pending_bust_[i] = false;
                        Player &p = table_.players[i];
                        if (p.busted) continue; // also busted by stack in end_hand
                        p.busted = true;
                        p._fold  = true;
                        // Pre-update prev_busted_ so detect_events doesn't fire
                        // a second player_out for this seat in the same tick.
                        if (i < (int) prev_busted_.size()) prev_busted_[i] = true;
                        nlohmann::json j = {
                                { "type", "player_out" },
                                { "seat", i },
                                { "reason", "disconnected" },
                                { "state", proto::state_json(table_, state_, game_id()) },
                        };
                        this->push_history(j);
                        this->broadcast(j.dump());
                        logf("P%d %s eliminated (disconnected)\n", i,
                             table_.players[i].name.c_str());
                }

                // Eliminate players busted by stack (end_hand set busted=true):
                // disconnect their session and leave the seat empty.
                for (int i = 0; i < max_players_; i++) {
                        Session *s = seat_session_[i];
                        if (!s || !table_.players[i].busted) continue;
                        logf("P%d %s busted, disconnecting\n", i,
                             table_.players[i].name.c_str());
                        s->seat         = -1;
                        s->pending_seat = -1;
                        seat_session_[i] = nullptr;
                        lws_set_timeout(s->wsi, PENDING_TIMEOUT_KILLED_BY_PROXY_CLIENT_CLOSE,
                                        LWS_TO_KILL_ASYNC);
                }
                return; // no bots, no pending joins in tournaments
        }

        // a player who lost everything is out: disconnect the client (if
        // any); the bot-revert loop below turns the seat into an auto bot
        // again (end_hand rebuys it to start_stack_)
        for (int i = 0; i < max_players_; i++) {
                if (table_.players[i].stack > 0) continue;
                Session *s = seat_session_[i];
                if (s) {
                        logf("P%d %s busted, disconnecting\n", i,
                             table_.players[i].name.c_str());
                        s->seat         = -1;
                        s->pending_seat = -1;
                        seat_session_[i] = nullptr;
                        lws_set_timeout(s->wsi, PENDING_TIMEOUT_KILLED_BY_PROXY_CLIENT_CLOSE,
                                        LWS_TO_KILL_ASYNC);
                }
        }

        // pending clients take their seats with a full entry stack,
        // replacing the bot that finished the round
        for (lws *wsi : conns_) {
                Session *s = session_of(wsi);
                if (!s || s->pending_seat < 0) continue;
                int seat     = s->pending_seat;
                s->pending_seat = -1;
                s->seat         = seat;
                seat_session_[seat] = s;
                Player &p       = table_.players[seat];
                p.name          = s->name;
                p.stack         = start_stack_; // fresh entry, not the bot's earnings
                p.auto_play     = false;
                logf("P%d %s joined (end of round)\n", seat, p.name.c_str());

                send_to(s->wsi, proto::serialize_welcome(game_id(), seat, p.name));
                // onboarding: current snapshot, then the turn if the seat must act now
                send_to(s->wsi, proto::state_json(table_, state_, game_id()).dump());
                if (p.is_my_turn) {
                        send_to(s->wsi, proto::serialize_your_turn(
                                table_.action_timeout, last_now_ + table_.action_timeout));
                }
        }

        // seats whose human left mid-hand revert to a bot (with the
        // remaining stack) for the next round
        for (int i = 0; i < max_players_; i++) {
                if (seat_session_[i] == nullptr && !table_.players[i].auto_play) {
                        this->give_bot(i);
                }
        }
}

void
Server::disconnect(Session *s, lws *wsi)
{
        if (!s) return;
        conns_.erase(std::remove(conns_.begin(), conns_.end(), wsi), conns_.end());

        if (s->seat >= 0) {
                int seat  = s->seat;
                Player &p = table_.players[seat];
                logf("P%d %s disconnected\n", seat, p.name.c_str());
                seat_session_[seat] = nullptr;
                if (tournament_) {
                        if (state_.tournament_status == T_RUNNING) {
                                if (!p.busted && !pending_bust_[seat]) {
                                        if (!state_.hand_over && !p._fold && !p.is_all_in) {
                                                if (p.is_my_turn) {
                                                        // engine will process the fold on next tick
                                                        p.response.type         = Player::Response::FOLD;
                                                        p.response.has_response = true;
                                                } else {
                                                        // force fold and record it so the action
                                                        // broadcast fires in detect_events
                                                        p._fold            = true;
                                                        p.last_action.type = Player::Response::FOLD;
                                                        p.last_action.amount = 0;
                                                }
                                        }
                                        // Defer the actual elimination until the round ends.
                                        // Committed chips stay in the pot; no bot takes the seat.
                                        pending_bust_[seat] = true;
                                        logf("P%d %s disconnected; will be eliminated at round end\n",
                                             seat, p.name.c_str());
                                }
                        } else if (state_.tournament_status == T_FINISHED) {
                                // Tournament is over: keep winner seat intact
                                logf("P%d %s disconnected after tournament finished\n", seat, p.name.c_str());
                        } else {
                                p.busted = true; // lobby/countdown: back to an empty seat
                        }
                        delete s;
                        return;
                }
                // leave mid-hand: fold if the player is still in the pot
                // (all-in players keep their committed chips)
                if (!state_.hand_over && !p._fold && !p.is_all_in) {
                        if (p.is_my_turn) {
                                p.response.type        = Player::Response::FOLD;
                                p.response.has_response = true;
                        } else {
                                p._fold = true;
                                p.last_action.type   = Player::Response::FOLD;
                                p.last_action.amount = 0;
                        }
                }
                // the bot takes over at the end of the round (process_round_end)
        } else if (s->pending_seat >= 0) {
                logf("P%d %s left while waiting\n", s->pending_seat, s->name.c_str());
                s->pending_seat = -1;
        } else {
                logf("client disconnected (not seated)\n");
        }
        delete s;
}

void
Server::step(double now)
{
        // keep the countdown / level clocks fresh for state_json and the GUI
        state_.level_remaining     = std::max(0.0, state_.level_started_at +
                                                    state_.level_seconds - now);
        state_.countdown_remaining = std::max(0.0, state_.countdown_started_at +
                                                    state_.countdown_seconds - now);

        if (tournament_) {
                switch (state_.tournament_status) {
                case T_LOBBY:
                        if (table_.alive_count() >= max_players_) {
                                state_.tournament_status    = T_COUNTDOWN;
                                state_.countdown_seconds    = countdown_seconds_;
                                state_.countdown_started_at = now;
                                logf("== tournament: table full, countdown (%gs) ==\n",
                                     countdown_seconds_);
                        }
                        return;

                case T_COUNTDOWN:
                        if (table_.alive_count() < max_players_) {
                                state_.tournament_status = T_LOBBY;
                                logf("== tournament: player left, back to lobby ==\n");
                                return;
                        }
                        if (now - state_.countdown_started_at >= state_.countdown_seconds) {
                                state_.tournament_status = T_RUNNING;
                                tournament::set_level(&state_, 1, now);
                                logf("== tournament started, level 1 (%d/%d) ==\n",
                                     state_.small_blind, state_.big_blind);
                                nlohmann::json j = {
                                        { "type", "tournament_start" },
                                        { "level", state_.level },
                                        { "blinds", { { "small", state_.small_blind },
                                                      { "big", state_.big_blind },
                                                      { "ante", state_.ante } } },
                                        { "state", proto::state_json(table_, state_, game_id()) },
                                };
                                this->push_history(j);
                                this->broadcast(j.dump());
                        }
                        return;

                case T_RUNNING:
                        if (state_.hand_over) {
                                if (now - hand_over_at_ < hand_pause_) return;
                                bool level_up = tournament::step(&state_, now);
                                table_.end_hand(&state_);
                                this->process_round_end();
                                if (level_up) {
                                        logf("== tournament: level %d, blinds %d/%d"
                                             " (ante %d) ==\n",
                                             state_.level, state_.small_blind,
                                             state_.big_blind, state_.ante);
                                }
                                if (table_.alive_count() <= 1) {
                                        state_.tournament_status = T_FINISHED;
                                        int winner = -1;
                                        for (int i = 0; i < max_players_; i++) {
                                                if (!table_.players[i].busted) winner = i;
                                        }
                                        if (winner >= 0) {
                                                nlohmann::json j = {
                                                        { "type", "tournament_over" },
                                                        { "winner", { { "seat", winner },
                                                                      { "name", table_.players[winner].name } } },
                                                        { "award", table_.players[winner].stack },
                                                        { "state", proto::state_json(table_, state_, game_id()) },
                                                };
                                                this->push_history(j);
                                                this->broadcast(j.dump());
                                                logf("== tournament over: %s wins the tournament ==\n",
                                                     table_.players[winner].name.c_str());
                                        } else {
                                                logf("== tournament over: no players left ==\n");
                                        }
                                }
                                logf("== next hand ==\n");
                                return;
                        }
                        table_.step_game(&state_, now);
                        return;

                case T_FINISHED:
                        return;
                }
        }

        if (state_.hand_over) {
                if (now - hand_over_at_ >= hand_pause_) {
                        this->process_round_end();
                        table_.end_hand(&state_);
                        logf("== next hand ==\n");
                }
                return;
        }
        table_.step_game(&state_, now);
}

void
Server::detect_events(double now)
{
        nlohmann::json st  = proto::state_json(table_, state_, game_id());
        std::string st_str = st.dump();
        bool changed       = (st_str != last_state_str_);
        if (changed) last_state_str_ = st_str;

        // new hand (preflop, blinds posted)
        if (state_.hand_started && !prev_hand_started_) {
                exporter_.start_hand(hand_id_counter_++, state_, table_);
                nlohmann::json j = {
                        { "type", "stage" },
                        { "stage", "preflop" },
                        { "blinds", { { "small", state_.small_blind },
                                      { "big", state_.big_blind },
                                      { "ante", state_.ante } } },
                        { "common", proto::common_json(table_) },
                        { "dealer", state_.dealer },
                        { "state", st },
                };
                history_.clear();
                this->push_history(j);
                this->broadcast(j.dump());
                logf("\n== new hand, dealer: P%d ==\n", state_.dealer);
        } else if (state_.stage >= FLOP && state_.stage <= RIVER &&
                   state_.stage != prev_stage_ && state_.hand_started) {
                exporter_.record_stage(state_.stage, table_.common);
                nlohmann::json j = {
                        { "type", "stage" },
                        { "stage", proto::stage_name(state_.stage) },
                        { "common", proto::common_json(table_) },
                        { "dealer", state_.dealer },
                        { "state", st },
                };
                this->push_history(j);
                this->broadcast(j.dump());
                logf("== stage: %s ==\n", proto::stage_name(state_.stage));
        }

        // per-seat events
        for (int i = 0; i < max_players_; i++) {
                const Player &p             = table_.players[i];
                Player::LastAction &prev    = prev_last_action_[i];
                if (p.busted) {
                        // eliminated seats emit nothing (end_hand marks them
                        // folded; that must not surface as a timeout fold)
                        prev.type = p.last_action.type;
                        prev.amount = p.last_action.amount;
                        prev_fold_[i] = p._fold;
                        prev_turn_[i] = p.is_my_turn;
                        continue;
                }

                if (p.last_action.type != Player::Response::NONE &&
                    (prev.type != p.last_action.type || prev.amount != p.last_action.amount)) {
                        exporter_.record_action(i, p.name, proto::action_name(p.last_action.type),
                                                p.last_action.amount, p._street_bet, p.is_all_in, state_.stage);
                        nlohmann::json j = {
                                { "type", "action" },
                                { "seat", i },
                                { "action", proto::action_name(p.last_action.type) },
                                { "amount", p.last_action.amount },
                                { "all_in", p.is_all_in },
                                { "reason", nullptr },
                                { "pot", table_.pot },
                                { "current_bet", state_.current_bet },
                                { "state", st },
                        };
                        this->push_history(j);
                        this->broadcast(j.dump());
                        logf("P%d %-12s %-8s %d (pot %d)\n", i, p.name.c_str(),
                             proto::action_name(p.last_action.type), p.last_action.amount,
                             table_.pot);
                } else if (!prev_fold_[i] && p._fold &&
                           p.last_action.type == Player::Response::NONE) {
                        // engine timeout path folds without a last_action
                        exporter_.record_action(i, p.name, "fold", 0, p._street_bet, false, state_.stage);
                        nlohmann::json j = {
                                { "type", "action" },
                                { "seat", i },
                                { "action", "fold" },
                                { "amount", 0 },
                                { "all_in", false },
                                { "reason", "timeout" },
                                { "pot", table_.pot },
                                { "current_bet", state_.current_bet },
                                { "state", st },
                        };
                        this->push_history(j);
                        this->broadcast(j.dump());
                        logf("P%d %-12s timeout, folds\n", i, p.name.c_str());
                }

                if (!prev_turn_[i] && p.is_my_turn) {
                        // the state now marks is_turn for the acting player;
                        // broadcast it so every client can track the turn
                        this->broadcast(st_str);
                        Session *s = seat_session_[i];
                        if (s && s->wsi) {
                                send_to(s->wsi, proto::serialize_your_turn(
                                        table_.action_timeout, now + table_.action_timeout));
                                logf("P%d %-12s your turn (%ds)\n", i, p.name.c_str(),
                                     (int) table_.action_timeout);
                        }
                }

                prev.type = p.last_action.type;
                prev.amount = p.last_action.amount;
                prev_fold_[i] = p._fold;
                prev_turn_[i] = p.is_my_turn;
        }

        // tournament: a seat went busted (by stack at the round end)
        if (tournament_) {
                for (int i = 0; i < max_players_; i++) {
                        if (!prev_busted_[i] && table_.players[i].busted) {
                                nlohmann::json j = {
                                        { "type", "player_out" },
                                        { "seat", i },
                                        { "reason", "busted" },
                                        { "state", st },
                                };
                                this->push_history(j);
                                this->broadcast(j.dump());
                                logf("P%d %-12s eliminated (busted)\n", i,
                                     table_.players[i].name.c_str());
                        }
                        prev_busted_[i] = table_.players[i].busted;
                }
        }

        // tournament: blind level went up (the step already changed the state)
        if (state_.level != prev_level_) {
                if (prev_level_ >= 1) {
                        nlohmann::json j = {
                                { "type", "level" },
                                { "level", state_.level },
                                { "blinds", { { "small", state_.small_blind },
                                              { "big", state_.big_blind },
                                              { "ante", state_.ante } } },
                                { "state", st },
                        };
                        this->push_history(j);
                        this->broadcast(j.dump());
                }
                prev_level_ = state_.level;
        }

        prev_stage_       = state_.stage;
        prev_hand_started_ = state_.hand_started;

        if (state_.hand_over && !prev_hand_over_) {
                exporter_.finish_hand(state_, table_);
                if (!export_dir_.empty()) {
                        exporter_.save_to_file(export_dir_);
                }
                nlohmann::json winners = nlohmann::json::array();
                // exact per-winner amounts (side pots: not an equal split)
                for (size_t k = 0; k < table_.winners.size() && k < table_.win_amount.size(); k++) {
                        winners.push_back({ { "seat", table_.winners[k] },
                                            { "amount", table_.win_amount[k] } });
                }
                nlohmann::json j = {
                        { "type", "hand_over" },
                        { "winners", winners },
                        { "award", table_.award },
                        { "result", table_.result_text },
                        { "state", st },
                };
                this->push_history(j);
                this->broadcast(j.dump());
                logf("== hand over: %s ==\n", table_.result_text.c_str());
                hand_over_at_ = now;
        }
        prev_hand_over_ = state_.hand_over;

        if (changed) {
                this->print_table();
        }
}

void
Server::logf(const char *fmt, ...)
{
        if (!verbose_) return;
        va_list ap;
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
}

void
Server::print_table() const
{
        if (!verbose_) return;
        printf("-- stage: %s, pot: %d, bet: %d, turn: P%d --\n", proto::stage_name(state_.stage),
               table_.pot, state_.current_bet, state_.turn);
        for (int i = 0; i < max_players_; i++) {
                const Player &p = table_.players[i];
                printf("   P%d %-12s stack %4d bet %3d %s%s last: %s %d\n", i, p.name.c_str(),
                       p.stack, p._street_bet, p._fold ? "FOLD " : "",
                       p.is_all_in ? "ALLIN " : "", proto::action_name(p.last_action.type),
                       p.last_action.amount);
        }
}
