OUT     = build/server
SRC     = $(wildcard src/*.cpp)
HEADERS = $(wildcard src/*.h)

CXX     = g++
FLAGS   = -Wall -Wextra -ggdb

FLAGS  += -I../thirdparty/PokerHandEvaluator/cpp/include
FLAGS  += -I../thirdparty/raylib/src
FLAGS  += -I../thirdparty/nlohmann/single_include

LWS_LIBS = -lwebsockets -lcap -lsystemd

.DEFAULT_GOAL := all

include ../deps.mk

.PHONY: all

all: $(OUT)

$(OUT): $(SRC) $(HEADERS) $(PHEVAL_LIB) $(RAYLIB_LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(FLAGS) $(SRC) $(PHEVAL_LIB) $(RAYLIB_LIB) $(LWS_LIBS) -o $@
