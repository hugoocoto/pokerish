# Umbrella: delegates to the server/ and bot/ projects, builds and runs the
# tests (the only things that still live at the root).

CXX     = g++
FLAGS   = -Wall -Wextra -ggdb
FLAGS  += -Iserver/src
FLAGS  += -Ithirdparty/PokerHandEvaluator/cpp/include
FLAGS  += -Ithirdparty/nlohmann/single_include

LWS_LIBS = -lwebsockets -lcap -lsystemd

.DEFAULT_GOAL := all

include deps.mk

TEST_POKER_BIN  = test/build/test_poker
TEST_PROTO_BIN  = test/build/test_proto
TEST_SERVER_BIN = test/build/test_server

.PHONY: all server bot test clean distclean

all: server

server:
	$(MAKE) -C server

bot:
	$(MAKE) -C bot/example

$(TEST_POKER_BIN): test/test_poker.cpp server/src/poker.cpp $(PHEVAL_LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(FLAGS) test/test_poker.cpp server/src/poker.cpp $(PHEVAL_LIB) -o $@

$(TEST_PROTO_BIN): test/test_proto.cpp server/src/proto.cpp server/src/poker.cpp $(PHEVAL_LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(FLAGS) test/test_proto.cpp server/src/proto.cpp server/src/poker.cpp $(PHEVAL_LIB) -o $@

$(TEST_SERVER_BIN): test/test_server.cpp server/src/server.cpp server/src/proto.cpp server/src/poker.cpp $(PHEVAL_LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(FLAGS) test/test_server.cpp server/src/server.cpp server/src/proto.cpp server/src/poker.cpp $(PHEVAL_LIB) $(LWS_LIBS) -o $@

test: $(TEST_POKER_BIN) $(TEST_PROTO_BIN) $(TEST_SERVER_BIN)
	./$(TEST_POKER_BIN)
	./$(TEST_PROTO_BIN)
	./$(TEST_SERVER_BIN)

clean:
	rm -rf server/build bot/example/build test/build

distclean: clean
	rm -rf $(PHEVAL_PATH) $(RAYLIB_BUILD)
