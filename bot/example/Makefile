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
FLAGS = -Wall -Wextra -ggdb
FLAGS += -DBOT_EXAMPLE_STANDALONE
FLAGS += -I../../thirdparty/nlohmann/single_include

LWS_LIBS = -lwebsockets -lcap -lsystemd

.PHONY: all clean

all: $(OUT)

$(OUT): $(OBJ)
	$(CXX) $(FLAGS) $(OBJ) $(LWS_LIBS) -o $@

$(OBJ): $(SRC)
	@mkdir -p $(dir $@)
	$(CXX) $(FLAGS) -c $< -o $@

clean:
	rm -rf build
