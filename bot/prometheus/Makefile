# Prometheus: single-file elite poker bot (libwebsockets + nlohmann/json +
# phevaluator). Advanced GTO-approximation strategy with opponent modeling,
# board texture analysis, range tracking, mixed strategies, and ICM awareness.

OUT   = build/prometheus
OBJ   = build/bot.o
SRC   = bot.cpp

CXX   = g++
FLAGS = -Wall -Wextra -ggdb -O3 -std=c++17
FLAGS += -I../../thirdparty/nlohmann/single_include
FLAGS += -I../../thirdparty/PokerHandEvaluator/cpp/include
FLAGS += $(LWS_CPPFLAGS)

include ../../deps.mk

.DEFAULT_GOAL := all

.PHONY: all clean

all: $(OUT)

$(OUT): $(OBJ) $(PHEVAL_LIB) $(LWS_LIB)
	$(CXX) $(FLAGS) $(OBJ) $(PHEVAL_LIB) $(LWS_LIB) $(LWS_LINK_LIBS) -o $@

$(OBJ): $(SRC) $(LWS_LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(FLAGS) -c $< -o $@

clean:
	rm -rf build
