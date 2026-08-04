# Titan: single-file competitive poker bot (libwebsockets + nlohmann/json +
# phevaluator). Same transport as bot/example, but with a handcrafted
# Slumbot-style strategy: preflop ranges + Monte-Carlo equity + pot odds.

OUT   = build/titan
OBJ   = build/bot.o
SRC   = bot.cpp

CXX   = g++
FLAGS = -Wall -Wextra -ggdb -O2 -std=c++17
FLAGS += -I../../thirdparty/nlohmann/single_include
FLAGS += -I../../thirdparty/PokerHandEvaluator/cpp/include
FLAGS += $(LWS_CPPFLAGS)

include ../../deps.mk

.DEFAULT_GOAL := all

.PHONY: all clean

all: $(OUT)

$(OUT): $(OBJ) $(PHEVAL_LIB)
	$(CXX) $(FLAGS) $(OBJ) $(PHEVAL_LIB) $(LWS_LIBS) -o $@

$(OBJ): $(SRC)
	@mkdir -p $(dir $@)
	$(CXX) $(FLAGS) -c $< -o $@

clean:
	rm -rf build
