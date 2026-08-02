# Shared rules for the third-party static libraries (raylib, phevaluator).
# Included by the root Makefile and server/Makefile. Paths are resolved
# from this file's own location, so it works regardless of which Makefile
# includes it.

POKER_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

PHEVAL_PATH = $(POKER_ROOT)/thirdparty/PokerHandEvaluator/cpp/build
PHEVAL_LIB  = $(PHEVAL_PATH)/libpheval.a

RAYLIB_BUILD = $(POKER_ROOT)/thirdparty/raylib/build
RAYLIB_LIB   = $(RAYLIB_BUILD)/raylib/libraylib.a

$(PHEVAL_LIB):
	cd $(POKER_ROOT)/thirdparty/PokerHandEvaluator/cpp && cmake -B build
	cmake --build $(PHEVAL_PATH) --target pheval

$(RAYLIB_LIB):
	mkdir -p $(RAYLIB_BUILD)
	cmake -S $(POKER_ROOT)/raylib -B $(RAYLIB_BUILD) \
		-DCMAKE_BUILD_TYPE=Release \
		-DPLATFORM=Desktop \
		-DUSE_WAYLAND=ON \
		-DGLFW_BUILD_WAYLAND=ON \
		-DGLFW_BUILD_X11=OFF \
		-DBUILD_LIBTYPE=STATIC
	make -C $(RAYLIB_BUILD) -j$$(nproc)
