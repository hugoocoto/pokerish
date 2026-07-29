OUT            = a.out
SRC            = $(wildcard src/*.cpp)
HEADERS        = $(wildcard src/*.h src/*.hpp)
OBJ_DIR        = build
OBJ            = $(SRC:src/%.cpp=$(OBJ_DIR)/%.o)

CXX            = g++
FLAGS          = -Wall -Wextra -ggdb

LIBPHEVAL_PATH = ./PokerHandEvaluator/cpp/build
LIBPHEVAL      = $(LIBPHEVAL_PATH)/libpheval.a
FLAGS         += -IPokerHandEvaluator/cpp/include

RAYLIB_SRC     = raylib
RAYLIB_BUILD   = $(RAYLIB_SRC)/build
RAYLIB_INC     = $(RAYLIB_BUILD)/include
RAYLIB_LIB     = $(RAYLIB_BUILD)/raylib/libraylib.a
FLAGS         += -I$(RAYLIB_INC)

all: $(OUT)

$(OBJ_DIR)/%.o: src/%.cpp $(HEADERS)
	@mkdir -p $(OBJ_DIR)
	$(CXX) $(FLAGS) -c $< -o $@

$(OUT): $(OBJ) $(LIBPHEVAL)
	$(CXX) $(FLAGS) $^ $(LIBPHEVAL) -o $@

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

$(RAYLIB_INC)/raylib.h: $(RAYLIB_LIB)
	mkdir -p $(RAYLIB_INC)
	cp $(RAYLIB_SRC)/src/raylib.h $(RAYLIB_INC)/
	cp $(RAYLIB_SRC)/src/raymath.h $(RAYLIB_INC)/
	cp $(RAYLIB_SRC)/src/rlgl.h $(RAYLIB_INC)/
 
