# Umbrella: delegates to the server/ and bot/ projects, builds and runs the
# tests (the only things that still live at the root).

MAKE    = make -j $$(getconf _NPROCESSORS_ONLN)

CXX     = g++
FLAGS   = -Wall -Wextra -ggdb -std=c++17
FLAGS  += -Iserver/src
FLAGS  += -Ithirdparty/PokerHandEvaluator/cpp/include
FLAGS  += -Ithirdparty/nlohmann/single_include
FLAGS  += $(LWS_CPPFLAGS)

.DEFAULT_GOAL := all

include deps.mk

TEST_POKER_BIN      = test/build/test_poker
TEST_PROTO_BIN      = test/build/test_proto
TEST_SERVER_BIN     = test/build/test_server
TEST_TOURNAMENT_BIN = test/build/test_tournament
TEST_POKERSTARS_BIN = test/build/test_pokerstars_export

.PHONY: all server bot client test clean distclean

all: server bot client

# $(PHEVAL_LIB) is built here once, before the parallel sub-makes start:
# running cmake into the same build dir from several sub-makes at once
# corrupts the configure (server and bot/titan both depend on it).
server: $(PHEVAL_LIB)
	$(MAKE) -C server

bot: $(PHEVAL_LIB)
	$(MAKE) -C bot/example
	$(MAKE) -C bot/titan

client: $(PHEVAL_LIB)
	$(MAKE) -C client

$(TEST_POKER_BIN): test/build/test_poker.o test/build/poker.o $(PHEVAL_LIB)
	$(CXX) $(FLAGS) $^ -o $@

$(TEST_PROTO_BIN): test/build/test_proto.o test/build/proto.o test/build/poker.o $(PHEVAL_LIB)
	$(CXX) $(FLAGS) $^ -o $@

$(TEST_SERVER_BIN): test/build/test_server.o test/build/server.o test/build/proto.o \
                    test/build/poker.o test/build/tournament.o test/build/pokerstars_export.o $(PHEVAL_LIB) $(LWS_LIB)
	$(CXX) $(FLAGS) $^ $(LWS_LINK_LIBS) -o $@

$(TEST_TOURNAMENT_BIN): test/build/test_tournament.o test/build/server.o test/build/proto.o \
                        test/build/poker.o test/build/tournament.o test/build/pokerstars_export.o $(PHEVAL_LIB) $(LWS_LIB)
	$(CXX) $(FLAGS) $^ $(LWS_LINK_LIBS) -o $@

$(TEST_POKERSTARS_BIN): test/build/test_pokerstars_export.o test/build/pokerstars_export.o test/build/proto.o test/build/poker.o $(PHEVAL_LIB)
	$(CXX) $(FLAGS) $^ -o $@

test/build/test_server.o: test/test_server.cpp bot/example/bot.cpp \
                          server/src/server.h server/src/proto.h \
                          server/src/poker.h server/src/tournament.h \
                          server/src/pokerstars_export.h $(LWS_LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(FLAGS) -c $< -o $@

test/build/test_tournament.o: test/test_tournament.cpp bot/example/bot.cpp \
                              server/src/server.h server/src/proto.h \
                              server/src/poker.h server/src/tournament.h \
                              server/src/pokerstars_export.h $(LWS_LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(FLAGS) -c $< -o $@

test/build/%.o: test/%.cpp $(LWS_LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(FLAGS) -c $< -o $@

test/build/%.o: server/src/%.cpp $(LWS_LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(FLAGS) -c $< -o $@

test: $(TEST_POKER_BIN) $(TEST_PROTO_BIN) $(TEST_SERVER_BIN) $(TEST_TOURNAMENT_BIN) $(TEST_POKERSTARS_BIN)
	./$(TEST_POKER_BIN)
	./$(TEST_PROTO_BIN)
	./$(TEST_SERVER_BIN)
	./$(TEST_TOURNAMENT_BIN)
	./$(TEST_POKERSTARS_BIN)

clean:
	rm -rf server/build bot/example/build bot/titan/build client/build test/build

distclean: clean
	rm -rf $(PHEVAL_PATH) $(POKER_ROOT)/thirdparty/raylib/build* $(LWS_BUILD)
