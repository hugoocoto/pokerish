#include "proto.h"

#include <algorithm>
#include <climits>
#include <cstdint>

namespace proto {

static const struct {
        Err err;
        const char *code;
        const char *message;
} kErrors[] = {
        { Err::NONE,             "none",             "" },
        { Err::BAD_JSON,         "bad_json",         "message is not valid JSON" },
        { Err::BAD_REQUEST,      "bad_request",      "missing or unknown fields, or bad values" },
        { Err::UNKNOWN_TYPE,     "unknown_type",     "unknown message type" },
        { Err::UNKNOWN_ACTION,   "unknown_action",   "unknown action value" },
        { Err::UNKNOWN_QUERY,    "unknown_query",    "unknown query what" },
        { Err::NOT_YOUR_TURN,    "not_your_turn",    "it is not your turn" },
        { Err::ILLEGAL_ACTION,   "illegal_action",   "action impossible in the current situation" },
        { Err::BET_TOO_SMALL,    "bet_too_small",    "bet/raise below the legal minimum (and not all-in)" },
        { Err::ALREADY_FOLDED,   "already_folded",   "you already folded" },
        { Err::GAME_OVER,        "game_over",        "the hand is over" },
        { Err::TABLE_FULL,       "table_full",       "the table is full" },
        { Err::UNAUTHORIZED,     "unauthorized",     "message without a valid seat assignment" },
        { Err::BAD_TOKEN,        "bad_token",        "wrong or missing token" },
};

const char *
err_code(Err e)
{
        for (auto &k : kErrors) {
                if (k.err == e) return k.code;
        }
        return "unknown";
}

const char *
err_message(Err e)
{
        for (auto &k : kErrors) {
                if (k.err == e) return k.message;
        }
        return "unknown error";
}

Err
parse_message(const std::string &text, ClientMessage &out)
{
        nlohmann::json j;
        try {
                j = nlohmann::json::parse(text);
        } catch (...) {
                return Err::BAD_JSON;
        }

        if (!j.is_object()) return Err::BAD_REQUEST;
        if (!j.contains("type") || !j["type"].is_string()) return Err::BAD_REQUEST;
        if (j.contains("id")) {
                if (!j["id"].is_number_integer()) return Err::BAD_REQUEST;
                int64_t id = j["id"].get<int64_t>();
                out.id     = (id < INT_MIN || id > INT_MAX) ? 0 : (int) id;
        }

        std::string type = j["type"];

        if (type == "hello") {
                if (!j.contains("name") || !j["name"].is_string()) return Err::BAD_REQUEST;
                if (j.contains("token")) {
                        if (!j["token"].is_string()) return Err::BAD_REQUEST;
                        out.token = j["token"];
                }
                out.type = MsgType::HELLO;
                out.name = j["name"];
                return Err::NONE;
        }

        if (type == "action") {
                if (!j.contains("action") || !j["action"].is_string()) return Err::BAD_REQUEST;
                std::string a = j["action"];
                if (a == "bet") {
                        if (!j.contains("amount") || !j["amount"].is_number_integer()) {
                                return Err::BAD_REQUEST;
                        }
                        int64_t amount = j["amount"].get<int64_t>();
                        if (amount <= 0 || amount > kMaxBet) return Err::BAD_REQUEST;
                        out.amount = (int) amount;
                } else if (a != "fold" && a != "check" && a != "call" &&
                           a != "call_all" && a != "check_or_fold") {
                        return Err::UNKNOWN_ACTION;
                }
                out.type   = MsgType::ACTION;
                out.action = a;
                return Err::NONE;
        }

        if (type == "query") {
                if (!j.contains("what") || !j["what"].is_string()) return Err::BAD_REQUEST;
                std::string w = j["what"];
                if (w != "state" && w != "my_cards" && w != "history") {
                        return Err::UNKNOWN_QUERY;
                }
                out.type  = MsgType::QUERY;
                out.query = w;
                return Err::NONE;
        }

        if (type == "ping") {
                out.type = MsgType::PING;
                return Err::NONE;
        }

        return Err::UNKNOWN_TYPE;
}

Err
validate_action(const Game_State &state, const Player &p, int seat,
                const ClientMessage &msg, Player::Response &out)
{
        if (state.hand_over || state.stage == OVER || !state.hand_started) {
                return Err::GAME_OVER;
        }
        if (state.round_done) return Err::NOT_YOUR_TURN;
        if (state.turn != seat) return Err::NOT_YOUR_TURN;
        if (p._fold) return Err::ALREADY_FOLDED;

        int to_call = state.current_bet - p._street_bet;
        const std::string &a = msg.action;

        if (a == "fold") {
                out.type = Player::Response::FOLD;
        } else if (a == "check") {
                if (to_call > 0) return Err::ILLEGAL_ACTION;
                out.type = Player::Response::CHECK;
        } else if (a == "call") {
                out.type = Player::Response::CALL;
        } else if (a == "call_all") {
                out.type = Player::Response::CALL_ALL;
        } else if (a == "check_or_fold") {
                out.type = (to_call <= 0) ? Player::Response::CHECK : Player::Response::FOLD;
        } else if (a == "bet") {
                int target = (state.current_bet == 0) ? msg.amount
                                                      : state.current_bet + msg.amount;
                int max_target = p.stack + p._street_bet;
                if (target < max_target && target < state.current_bet + state.min_raise) {
                        return Err::BET_TOO_SMALL;
                }
                out.type          = Player::Response::BET;
                out.as.bet.amount = std::min(target, max_target);
        } else {
                return Err::UNKNOWN_ACTION;
        }

        out.has_response = true;
        return Err::NONE;
}

const char *
stage_name(int stage)
{
        switch (stage) {
        case PREFLOP: return "preflop";
        case FLOP: return "flop";
        case TURN: return "turn";
        case RIVER: return "river";
        case OVER: return "over";
        }
        return "unknown";
}

const char *
action_name(Player::Response::ResponseType t)
{
        switch (t) {
        case Player::Response::NONE: return "none";
        case Player::Response::FOLD: return "fold";
        case Player::Response::CHECK: return "check";
        case Player::Response::CALL: return "call";
        case Player::Response::CALL_ALL: return "call_all";
        case Player::Response::BET: return "bet";
        }
        return "unknown";
}

std::string
card_name(const phevaluator::Card &c)
{
        return c.describeCard();
}

nlohmann::json
common_json(const Table &table)
{
        nlohmann::json common = nlohmann::json::array();
        for (int i = 0; i < 5; i++) {
                if ((size_t) i < table.common.size()) {
                        common.push_back(card_name(table.common[i]));
                } else {
                        common.push_back(nullptr);
                }
        }
        return common;
}

nlohmann::json
state_json(const Table &table, const Game_State &state, int game_id)
{
        nlohmann::json j;
        j["type"]            = "state";
        j["game_id"]         = game_id;
        j["stage"]           = stage_name(state.stage);
        j["dealer"]          = state.dealer;
        j["turn"]            = state.turn;
        j["current_bet"]     = state.current_bet;
        j["min_raise"]       = state.min_raise;
        j["pot"]             = table.pot;
        j["round_done"]      = state.round_done;
        j["hand_over"]       = state.hand_over;
        j["common"]          = common_json(table);
        j["timeout_seconds"] = table.action_timeout;

        nlohmann::json players = nlohmann::json::array();
        for (size_t i = 0; i < table.players.size(); i++) {
                const Player &p = table.players[i];
                players.push_back({
                        { "seat", (int) i },
                        { "name", p.name },
                        { "stack", p.stack },
                        { "bet", p._bet },
                        { "street_bet", p._street_bet },
                        { "folded", p._fold },
                        { "has_acted", p.has_acted },
                        { "all_in", p.is_all_in },
                        { "last_action", { { "type", action_name(p.last_action.type) },
                                           { "amount", p.last_action.amount } } },
                        { "is_turn", p.is_my_turn },
                });
        }
        j["players"] = players;
        return j;
}

std::string
serialize_welcome(int game_id, int seat, const std::string &name)
{
        nlohmann::json j = {
                { "type", "welcome" },
                { "game_id", game_id },
                { "player_id", seat },
                { "seat", seat },
                { "name", name },
        };
        return j.dump();
}

std::string
serialize_queued(int seat)
{
        nlohmann::json j = {
                { "type", "queued" },
                { "seat", seat },
        };
        return j.dump();
}

std::string
serialize_your_turn(double timeout_seconds, double deadline)
{
        nlohmann::json j = {
                { "type", "your_turn" },
                { "timeout_seconds", timeout_seconds },
                { "deadline", deadline },
        };
        return j.dump();
}

std::string
serialize_error(int id, Err e)
{
        nlohmann::json j = {
                { "type", "error" },
                { "id", id },
                { "code", err_code(e) },
                { "message", err_message(e) },
        };
        return j.dump();
}

std::string
serialize_reply(int id, const std::string &what, const nlohmann::json &data)
{
        nlohmann::json j = {
                { "type", "reply" },
                { "id", id },
                { "what", what },
                { "data", data },
        };
        return j.dump();
}

std::string
serialize_pong()
{
        return nlohmann::json({ { "type", "pong" } }).dump();
}

} // namespace proto
