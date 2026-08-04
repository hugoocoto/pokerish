# Shared rules for the third-party static libraries (raylib, phevaluator).
# Included by the root Makefile, server/Makefile and client/Makefile. Paths
# are resolved from this file's own location, so it works regardless of
# which Makefile includes it.

# The raylib build is platform-configurable. RAYLIB_PLATFORM selects the
# backend: "wayland", "x11", "macos" or "windows". When unset it is
# auto-detected (command-line/environment assignments take precedence):
#   - MSYS2/MinGW shells on Windows        -> windows
#   - Darwin                                -> macos
#   - Linux with a Wayland session          -> wayland
#   - Linux otherwise                       -> x11
# Override with: make RAYLIB_PLATFORM=x11   (propagates to sub-makes).
UNAME_S := $(shell uname -s)
ifndef RAYLIB_PLATFORM
  ifeq ($(OS),Windows_NT)
    RAYLIB_PLATFORM := windows
  else ifeq ($(UNAME_S),Darwin)
    RAYLIB_PLATFORM := macos
  else
    ifeq ($(shell printenv XDG_SESSION_TYPE),wayland)
      RAYLIB_PLATFORM := wayland
    else ifneq ($(shell printenv WAYLAND_DISPLAY),)
      RAYLIB_PLATFORM := wayland
    else
      RAYLIB_PLATFORM := x11
    endif
  endif
endif

ifeq ($(filter $(RAYLIB_PLATFORM),wayland x11 macos windows),)
  $(error RAYLIB_PLATFORM must be one of: wayland x11 macos windows (got "$(RAYLIB_PLATFORM)"))
endif

POKER_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

PHEVAL_PATH = $(POKER_ROOT)/thirdparty/PokerHandEvaluator/cpp/build
PHEVAL_LIB  = $(PHEVAL_PATH)/libpheval.a

# Per-platform raylib cmake options. Linux picks the GLFW backend explicitly
# (GLFW_BUILD_WAYLAND/GLFW_BUILD_X11 are the raylib 6.0 options; the old
# USE_WAYLAND flag no longer exists). macOS uses the GLFW Cocoa backend (no
# GLFW_BUILD_* override needed on APPLE). Windows uses raylib's native Win32
# platform (no GLFW at all).
ifeq ($(RAYLIB_PLATFORM),wayland)
  RAYLIB_CMAKE_FLAGS = -DPLATFORM=Desktop -DGLFW_BUILD_X11=OFF -DGLFW_BUILD_WAYLAND=ON
else ifeq ($(RAYLIB_PLATFORM),x11)
  RAYLIB_CMAKE_FLAGS = -DPLATFORM=Desktop -DGLFW_BUILD_X11=ON -DGLFW_BUILD_WAYLAND=OFF
else ifeq ($(RAYLIB_PLATFORM),macos)
  RAYLIB_CMAKE_FLAGS = -DPLATFORM=Desktop
else ifeq ($(RAYLIB_PLATFORM),windows)
  # raylib's native Win32 backend calls Windows 10 DPI APIs (GetDpiForWindow,
  # AdjustWindowRectExForDpi, WM_GETDPISCALEDSIZE); MinGW only declares them
  # with _WIN32_WINNT/WINVER >= 0x0A00 and defaults lower. Only raylib needs
  # this: our own code never uses those APIs.
  RAYLIB_CMAKE_FLAGS = -DPLATFORM=Win32 -DCMAKE_C_FLAGS="-D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00"
  RAYLIB_CMAKE_GENERATOR = -G "MinGW Makefiles"
endif

# raylib is linked as a plain static archive, so the platform libraries it
# needs must be supplied at app link time. On Linux the GLFW/Wayland stack is
# dlopened at runtime, but raylib's X11 clipboard code calls libX11 directly,
# so the X11 build needs -lX11. macOS needs the Cocoa/OpenGL frameworks.
ifeq ($(RAYLIB_PLATFORM),x11)
  RAYLIB_LINK_LIBS = -lX11
else ifeq ($(RAYLIB_PLATFORM),macos)
  RAYLIB_LINK_LIBS = -framework Cocoa -framework IOKit -framework CoreFoundation -framework OpenGL
else ifeq ($(RAYLIB_PLATFORM),windows)
  RAYLIB_LINK_LIBS = -lopengl32 -lwinmm -lgdi32
else
  RAYLIB_LINK_LIBS =
endif

# libwebsockets include/link flags per platform (Linux keeps -lcap/-lsystemd,
# which do not exist on macOS/Windows). macOS needs the Homebrew include/lib
# dirs explicitly: they are not on the default clang path.
ifeq ($(RAYLIB_PLATFORM),macos)
  LWS_CPPFLAGS = -I$(shell brew --prefix)/include
  LWS_LIBS = -L$(shell brew --prefix)/lib -lwebsockets
else ifeq ($(RAYLIB_PLATFORM),windows)
  LWS_LIBS = -lwebsockets -lws2_32 -lcrypt32
else
  LWS_LIBS = -lwebsockets -lcap -lsystemd
endif

# One build directory per platform so switching backends never reuses a
# stale cmake cache.
RAYLIB_BUILD = $(POKER_ROOT)/thirdparty/raylib/build-$(RAYLIB_PLATFORM)
RAYLIB_LIB   = $(RAYLIB_BUILD)/raylib/libraylib.a

$(PHEVAL_LIB):
	cd $(POKER_ROOT)/thirdparty/PokerHandEvaluator/cpp && cmake -B build -DBUILD_TESTS=OFF
	cmake --build $(PHEVAL_PATH) --target pheval

$(RAYLIB_LIB):
	mkdir -p $(RAYLIB_BUILD)
	cmake -S $(POKER_ROOT)/thirdparty/raylib -B $(RAYLIB_BUILD) \
		-DCMAKE_BUILD_TYPE=Release \
		-DBUILD_LIBTYPE=STATIC \
		-DBUILD_EXAMPLES=OFF \
		$(RAYLIB_CMAKE_FLAGS) $(RAYLIB_CMAKE_GENERATOR)
	cmake --build $(RAYLIB_BUILD) --parallel
