OUT            = a.out
SRC            = $(wildcard src/*.cpp)
HEADERS        = $(wildcard src/*.h)
OBJ_DIR        = build
OBJ            = $(SRC:src/%.cpp=$(OBJ_DIR)/%.o)

CXX            = g++
FLAGS          = -Wall -Wextra -ggdb

LIBPHEVAL_PATH = ./PokerHandEvaluator/cpp/build
LIBPHEVAL      = $(LIBPHEVAL_PATH)/libpheval.a
FLAGS         += -IPokerHandEvaluator/cpp/include

RAYLIB_SRC     = raylib
RAYLIB_BUILD   = $(RAYLIB_SRC)/build
RAYLIB_LIB     = $(RAYLIB_BUILD)/raylib/libraylib.a
FLAGS         += -I$(RAYLIB_SRC)/src

TEST_SRC       = tests/test_poker.cpp
TEST_BIN       = $(OBJ_DIR)/test_poker

.PHONY: all test

all: $(OUT)

$(OBJ_DIR)/%.o: src/%.cpp $(HEADERS)
	@mkdir -p $(OBJ_DIR)
	$(CXX) $(FLAGS) -c $< -o $@

$(OUT): $(OBJ) $(LIBPHEVAL) $(RAYLIB_LIB)
	$(CXX) $(FLAGS) $^ -o $@

$(TEST_BIN): $(TEST_SRC) $(OBJ_DIR)/poker.o $(LIBPHEVAL)
	$(CXX) $(FLAGS) $(TEST_SRC) $(OBJ_DIR)/poker.o $(LIBPHEVAL) -o $@

test: $(TEST_BIN)
	./$(TEST_BIN)

$(LIBPHEVAL):
	cd ./PokerHandEvaluator/cpp && cmake -B build
	cmake --build $(LIBPHEVAL_PATH) --target pheval

$(RAYLIB_LIB):
	mkdir -p $(RAYLIB_BUILD)
	cmake -S $(RAYLIB_SRC) -B $(RAYLIB_BUILD) \
		-DCMAKE_BUILD_TYPE=Release \
		-DPLATFORM=Desktop \
		-DUSE_WAYLAND=ON \
		-DGLFW_BUILD_WAYLAND=ON \
		-DGLFW_BUILD_X11=OFF \
		-DBUILD_LIBTYPE=STATIC
	make -C $(RAYLIB_BUILD) -j$$(nproc)
