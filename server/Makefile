OUT     = build/server
SRCS    = $(wildcard src/*.cpp)
OBJS    = $(patsubst src/%.cpp,build/%.o,$(SRCS))
HEADERS = $(wildcard src/*.h)

CXX     = g++
FLAGS   = -Wall -Wextra -ggdb

FLAGS  += -I../thirdparty/PokerHandEvaluator/cpp/include
FLAGS  += -I../thirdparty/raylib/src
FLAGS  += -I../thirdparty/nlohmann/single_include

LWS_LIBS = -lwebsockets -lcap -lsystemd

.DEFAULT_GOAL := all

include ../deps.mk

.PHONY: all clean

all: $(OUT)

$(OUT): $(OBJS) $(PHEVAL_LIB) $(RAYLIB_LIB)
	$(CXX) $(FLAGS) $(OBJS) $(PHEVAL_LIB) $(RAYLIB_LIB) $(LWS_LIBS) -o $@

build/%.o: src/%.cpp $(HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) $(FLAGS) -c $< -o $@

clean:
	rm -rf build
