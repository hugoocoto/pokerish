# Human-play GUI client: raylib window + the bot/example WebSocket client
# (included as a library, no -DBOT_EXAMPLE_STANDALONE) speaking the API.md
# protocol. Connects to the server like any bot and lets a human play.

OUT   = build/client
SRC   = client.cpp

CXX   = g++
FLAGS = -Wall -Wextra -ggdb -std=c++17
FLAGS += -I../server/src
FLAGS += -I../bot/example
FLAGS += -I../thirdparty/PokerHandEvaluator/cpp/include
FLAGS += -I../thirdparty/raylib/src
FLAGS += -I../thirdparty/nlohmann/single_include
FLAGS += $(LWS_CPPFLAGS)

.PHONY: all clean

.DEFAULT_GOAL := all

include ../deps.mk

all: $(OUT)

$(OUT): $(SRC) $(PHEVAL_LIB) $(RAYLIB_LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(FLAGS) $(SRC) $(PHEVAL_LIB) $(RAYLIB_LIB) $(RAYLIB_LINK_LIBS) $(LWS_LIBS) -o $@

clean:
	rm -rf build
