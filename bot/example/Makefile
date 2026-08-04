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

# Per-platform libwebsockets flags: Homebrew include/lib dirs are not on the
# default macOS compiler path; Windows needs ws2_32/crypt32 (no libcap/
# libsystemd there).
UNAME_S := $(shell uname -s)
ifeq ($(OS),Windows_NT)
  LWS_LIBS = -lwebsockets -lws2_32 -lcrypt32
else ifeq ($(UNAME_S),Darwin)
  FLAGS += -I$(shell brew --prefix)/include
  LWS_LIBS = -L$(shell brew --prefix)/lib -lwebsockets
else
  LWS_LIBS = -lwebsockets -lcap -lsystemd
endif

.PHONY: all clean

all: $(OUT)

$(OUT): $(OBJ)
	$(CXX) $(FLAGS) $(OBJ) $(LWS_LIBS) -o $@

$(OBJ): $(SRC)
	@mkdir -p $(dir $@)
	$(CXX) $(FLAGS) -c $< -o $@

clean:
	rm -rf build
