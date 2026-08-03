#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "poker.h"

// API.md protocol layer, shared by the server and the bot clients.

namespace proto {

enum class MsgType {
        NONE,
        HELLO,
        ACTION,
        QUERY,
        PING,
};

struct ClientMessage {
        MsgType type{ MsgType::NONE };
        std::string name;   // hello
        std::string token;  // hello: optional auth token
        bool is_human{ false }; // hello: human client, no simulated "thinking" delay
        std::string action; // action: fold/check/call/call_all/check_or_fold/bet
        int amount{ 0 };    // bet: raise increment on top of current_bet
        std::string query;  // query: state/my_cards/history
        int id{ 0 };        // echoed in reply/error
};

// Largest accepted bet/raise amount (chips). Anything above is a bad request.
static constexpr int kMaxBet = 100'000'000;

enum class Err {
        NONE,
        BAD_JSON,
        BAD_REQUEST,
        UNKNOWN_TYPE,
        UNKNOWN_ACTION,
        UNKNOWN_QUERY,
        NOT_YOUR_TURN,
        ILLEGAL_ACTION,
        BET_TOO_SMALL,
        ALREADY_FOLDED,
        GAME_OVER,
        TABLE_FULL,
        UNAUTHORIZED,
        BAD_TOKEN,
};

const char *err_code(Err e);
const char *err_message(Err e);

// Parse a complete text frame into a client message. Returns the error
// code for invalid messages (out is untouched on error).
Err parse_message(const std::string &text, ClientMessage &out);

// Validate an action against the engine state and convert it into the
// engine response. `seat` is the player's index (must equal state.turn).
// check_or_fold is resolved here (check if free, else fold). Rejects
// anything the engine would assert on (src/poker.cpp process_action).
Err validate_action(const Game_State &state, const Player &p, int seat,
                    const ClientMessage &msg, Player::Response &out);

// Full public state snapshot (API.md "state" schema, engine mapping in
// API.md section 2). Only "state" key + data, no envelope.
nlohmann::json state_json(const Table &table, const Game_State &state, int game_id);

// The 5 common-card slots, "As"-style strings or null.
nlohmann::json common_json(const Table &table);

const char *stage_name(int stage);
const char *action_name(Player::Response::ResponseType t);
std::string card_name(const phevaluator::Card &c);

std::string serialize_welcome(int game_id, int seat, const std::string &name);
std::string serialize_queued(int seat);
std::string serialize_your_turn(double timeout_seconds, double deadline);
std::string serialize_error(int id, Err e);
std::string serialize_reply(int id, const std::string &what, const nlohmann::json &data);
std::string serialize_pong();

} // namespace proto
