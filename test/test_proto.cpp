#include "../server/src/proto.h"

#include <cstdio>
#include <string>

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

static void
test_parse()
{
        proto::ClientMessage m;

        CHECK(proto::parse_message("not json", m) == proto::Err::BAD_JSON);
        CHECK(proto::parse_message("{}", m) == proto::Err::BAD_REQUEST);
        CHECK(proto::parse_message("[]", m) == proto::Err::BAD_REQUEST);
        CHECK(proto::parse_message("{ \"type\": \"nope\" }", m) == proto::Err::UNKNOWN_TYPE);
        CHECK(proto::parse_message("{ \"id\": \"x\" }", m) == proto::Err::BAD_REQUEST);

        CHECK(proto::parse_message("{ \"type\": \"hello\" }", m) == proto::Err::BAD_REQUEST);
        CHECK(proto::parse_message("{ \"type\": \"hello\", \"name\": \"Al\" }", m) ==
              proto::Err::NONE);
        CHECK(m.type == proto::MsgType::HELLO);
        CHECK(m.name == "Al");

        CHECK(proto::parse_message("{ \"type\": \"action\", \"action\": \"allin\" }", m) ==
              proto::Err::UNKNOWN_ACTION);
        CHECK(proto::parse_message("{ \"type\": \"action\", \"action\": \"call\" }", m) ==
              proto::Err::NONE);
        CHECK(m.action == "call");
        CHECK(proto::parse_message("{ \"type\": \"action\", \"action\": \"bet\" }", m) ==
              proto::Err::BAD_REQUEST);
        CHECK(proto::parse_message("{ \"type\": \"action\", \"action\": \"bet\", \"amount\": 0 }",
                                   m) == proto::Err::BAD_REQUEST);
        CHECK(proto::parse_message(
                      "{ \"type\": \"action\", \"action\": \"bet\", \"amount\": 20, \"id\": 7 }",
                      m) == proto::Err::NONE);
        CHECK(m.action == "bet");
        CHECK_EQ(m.amount, 20);
        CHECK_EQ(m.id, 7);

        CHECK(proto::parse_message("{ \"type\": \"query\", \"what\": \"foo\" }", m) ==
              proto::Err::UNKNOWN_QUERY);
        CHECK(proto::parse_message("{ \"type\": \"query\", \"what\": \"state\" }", m) ==
              proto::Err::NONE);
        CHECK(m.query == "state");

        CHECK(proto::parse_message("{ \"type\": \"ping\" }", m) == proto::Err::NONE);
        CHECK(m.type == proto::MsgType::PING);
}

static void
test_validate()
{
        Game_State st;
        st.hand_started = true;
        st.current_bet  = 30;
        st.min_raise    = 10;
        st.turn         = 2;

        Player p("P");
        p._street_bet = 10;
        p.stack       = 500;

        proto::ClientMessage m;
        m.type   = proto::MsgType::ACTION;
        m.action = "check";
        Player::Response r;

        // check with a bet to call is illegal
        CHECK(proto::validate_action(st, p, 2, m, r) == proto::Err::ILLEGAL_ACTION);

        m.action = "fold";
        CHECK(proto::validate_action(st, p, 2, m, r) == proto::Err::NONE);
        CHECK(r.type == Player::Response::FOLD);

        m.action = "call";
        CHECK(proto::validate_action(st, p, 2, m, r) == proto::Err::NONE);
        CHECK(r.type == Player::Response::CALL);

        m.action = "call_all";
        CHECK(proto::validate_action(st, p, 2, m, r) == proto::Err::NONE);
        CHECK(r.type == Player::Response::CALL_ALL);

        // bet: target = current_bet + increment
        m.action = "bet";
        m.amount = 5; // target 35 < 40 (current_bet + min_raise)
        CHECK(proto::validate_action(st, p, 2, m, r) == proto::Err::BET_TOO_SMALL);
        m.amount = 10; // target 40, legal
        CHECK(proto::validate_action(st, p, 2, m, r) == proto::Err::NONE);
        CHECK(r.type == Player::Response::BET);
        CHECK_EQ(r.as.bet.amount, 40);
        m.amount = 999; // target 1029 >= stack + street (510): capped all-in
        CHECK(proto::validate_action(st, p, 2, m, r) == proto::Err::NONE);
        CHECK_EQ(r.as.bet.amount, 510);

        // opening bet: target = amount, min = min_raise
        st.current_bet = 0;
        p._street_bet  = 0;
        m.amount       = 5;
        CHECK(proto::validate_action(st, p, 2, m, r) == proto::Err::BET_TOO_SMALL);
        m.amount = 10;
        CHECK(proto::validate_action(st, p, 2, m, r) == proto::Err::NONE);
        CHECK_EQ(r.as.bet.amount, 10);

        // free check
        m.action = "check";
        CHECK(proto::validate_action(st, p, 2, m, r) == proto::Err::NONE);
        CHECK(r.type == Player::Response::CHECK);

        // check_or_fold resolves server-side
        m.action = "check_or_fold";
        CHECK(proto::validate_action(st, p, 2, m, r) == proto::Err::NONE);
        CHECK(r.type == Player::Response::CHECK);
        st.current_bet = 30;
        p._street_bet  = 10;
        CHECK(proto::validate_action(st, p, 2, m, r) == proto::Err::NONE);
        CHECK(r.type == Player::Response::FOLD);

        // turn / state checks
        st.current_bet = 30;
        p._street_bet  = 10;
        m.action       = "call";
        CHECK(proto::validate_action(st, p, 1, m, r) == proto::Err::NOT_YOUR_TURN);
        st.turn = 2;
        p._fold = true;
        CHECK(proto::validate_action(st, p, 2, m, r) == proto::Err::ALREADY_FOLDED);
        p._fold = false;
        st.round_done = true;
        CHECK(proto::validate_action(st, p, 2, m, r) == proto::Err::NOT_YOUR_TURN);
        st.round_done = false;
        st.hand_over = true;
        CHECK(proto::validate_action(st, p, 2, m, r) == proto::Err::GAME_OVER);
        st.hand_over  = false;
        st.hand_started = false;
        CHECK(proto::validate_action(st, p, 2, m, r) == proto::Err::GAME_OVER);
}

static void
test_state_json()
{
        Deck d;
        Table t(&d);
        for (int i = 0; i < 6; i++) {
                t.add_player(Player("P" + std::to_string(i)));
        }
        t.players[0].stack       = 985;
        t.players[0]._bet        = 15;
        t.players[0]._street_bet = 15;
        t.players[0].last_action.type   = Player::Response::CALL;
        t.players[0].last_action.amount = 15;
        t.players[1]._fold       = true;
        t.common.push_back(phevaluator::Card("As"));
        t.common.push_back(phevaluator::Card("Kd"));
        t.common.push_back(phevaluator::Card("2c"));

        Game_State st;
        st.stage       = FLOP;
        st.dealer      = 1;
        st.turn        = 2;
        st.current_bet = 30;
        st.min_raise   = 10;
        t.pot          = 45;

        nlohmann::json j = proto::state_json(t, st, 1);
        CHECK(j["type"] == "state");
        CHECK(j["game_id"] == 1);
        CHECK(j["stage"] == "flop");
        CHECK_EQ(j["dealer"], 1);
        CHECK_EQ(j["turn"], 2);
        CHECK_EQ(j["current_bet"], 30);
        CHECK_EQ(j["min_raise"], 10);
        CHECK_EQ(j["pot"], 45);
        CHECK(j["round_done"] == false);
        CHECK(j["hand_over"] == false);
        CHECK_EQ(j["timeout_seconds"], 20);
        CHECK_EQ(j["common"].size(), 5);
        CHECK(j["common"][0] == "As");
        CHECK(j["common"][1] == "Kd");
        CHECK(j["common"][2] == "2c");
        CHECK(j["common"][3].is_null());
        CHECK_EQ(j["players"].size(), 6);
        CHECK_EQ(j["players"][0]["stack"], 985);
        CHECK_EQ(j["players"][0]["street_bet"], 15);
        CHECK(j["players"][0]["last_action"]["type"] == "call");
        CHECK_EQ(j["players"][0]["last_action"]["amount"], 15);
        CHECK(j["players"][1]["folded"] == true);
        CHECK(j["players"][1]["all_in"] == false);
}

static void
test_serializers()
{
        std::string w = proto::serialize_welcome(1, 3, "Bob");
        nlohmann::json j = nlohmann::json::parse(w);
        CHECK(j["type"] == "welcome");
        CHECK_EQ(j["seat"], 3);
        CHECK(j["name"] == "Bob");

        std::string y = proto::serialize_your_turn(20, 1234.5);
        j = nlohmann::json::parse(y);
        CHECK(j["type"] == "your_turn");
        CHECK_EQ(j["timeout_seconds"], 20);
        CHECK(j["deadline"] == 1234.5);

        std::string e = proto::serialize_error(5, proto::Err::NOT_YOUR_TURN);
        j = nlohmann::json::parse(e);
        CHECK(j["type"] == "error");
        CHECK_EQ(j["id"], 5);
        CHECK(j["code"] == "not_your_turn");

        std::string r = proto::serialize_reply(5, "state", nlohmann::json({ { "pot", 7 } }));
        j = nlohmann::json::parse(r);
        CHECK(j["type"] == "reply");
        CHECK_EQ(j["id"], 5);
        CHECK(j["what"] == "state");
        CHECK_EQ(j["data"]["pot"], 7);

        j = nlohmann::json::parse(proto::serialize_pong());
        CHECK(j["type"] == "pong");

        CHECK(std::string(proto::err_code(proto::Err::BAD_JSON)) == "bad_json");
        CHECK(std::string(proto::err_code(proto::Err::TABLE_FULL)) == "table_full");
        CHECK(std::string(proto::err_code(proto::Err::ILLEGAL_ACTION)) == "illegal_action");
        CHECK(std::string(proto::stage_name(PREFLOP)) == "preflop");
        CHECK(std::string(proto::stage_name(RIVER)) == "river");
        CHECK(std::string(proto::action_name(Player::Response::CALL_ALL)) == "call_all");
        CHECK(proto::card_name(phevaluator::Card("As")) == "As");
}

int
main()
{
        test_parse();
        test_validate();
        test_state_json();
        test_serializers();

        if (g_failures == 0) {
                printf("test_proto: all tests passed\n");
                return 0;
        }
        printf("test_proto: %d test(s) failed\n", g_failures);
        return 1;
}
