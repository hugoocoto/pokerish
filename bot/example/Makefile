# Single-file poker bot example, in C++: libwebsockets + nlohmann/json,
# the same API.md WebSocket/JSON protocol as the old multi-file bot.
# Everything is in bot.cpp on purpose, so it is easy to read and copy.
#
# -DBOT_EXAMPLE_STANDALONE compiles in main(); without it bot.cpp is just a
# client library (test/test_server.cpp #includes it directly).

OUT   = build/example_bot
OBJ   = build/bot.o
SRC   = bot.cpp

CXX   = g++
FLAGS = -Wall -Wextra -ggdb -std=c++17
FLAGS += -DBOT_EXAMPLE_STANDALONE
FLAGS += -I../../thirdparty/nlohmann/single_include
FLAGS += $(LWS_CPPFLAGS)

include ../../deps.mk

.DEFAULT_GOAL := all
.PHONY: all clean

all: $(OUT)

$(OUT): $(OBJ) $(LWS_LIB)
	$(CXX) $(FLAGS) $(OBJ) $(LWS_LIB) $(LWS_LINK_LIBS) -o $@

$(OBJ): $(SRC) $(LWS_LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(FLAGS) -c $< -o $@

clean:
	rm -rf build
