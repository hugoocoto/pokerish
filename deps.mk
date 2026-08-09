# Shared rules for the third-party static libraries (raylib, phevaluator,
# libwebsockets).  Included by the root Makefile, server/Makefile,
# client/Makefile and bot/example/Makefile.  Paths are resolved from this
# file's own location, so it works regardless of which Makefile includes it.

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

# --- libwebsockets (vendored, built from source) ---
LWS_PATH = $(POKER_ROOT)/thirdparty/libwebsockets
LWS_BUILD = $(LWS_PATH)/build
LWS_LIB   = $(LWS_BUILD)/lib/libwebsockets.a

# Minimal ws-only build: no TLS, no extensions, no HTTP/2, no test apps.
LWS_CMAKE_FLAGS = \
	-DLWS_WITH_SSL=OFF \
	-DLWS_WITHOUT_EXTENSIONS=ON \
	-DLWS_WITH_HTTP2=OFF \
	-DLWS_WITH_SECURE_STREAMS=OFF \
	-DLWS_WITHOUT_TESTAPPS=ON \
	-DLWS_WITH_STATIC=ON \
	-DLWS_WITH_SHARED=OFF \
	-DLWS_WITH_LIBCAP=OFF \
	-DLWS_WITHOUT_TEST_SERVER=ON \
	-DLWS_WITHOUT_TEST_PING=ON \
	-DLWS_WITHOUT_TEST_CLIENT=ON

ifeq ($(RAYLIB_PLATFORM),windows)
  # MinGW generates MinGW Makefiles by default; force the generator. Do NOT
  # set -DCMAKE_C_FLAGS here (unlike raylib above): lws already defines
  # WINVER/_WIN32_WINNT itself (add_definitions in its CMakeLists.txt), so a
  # second -D on the compile line trips -Werror "redefined" on every TU.
  LWS_CMAKE_GENERATOR = -G "MinGW Makefiles"
  # lws needs pthreads on this platform: it auto-detects winpthreads in the
  # MinGW toolchain (LWS_HAVE_PTHREAD_H) but only includes <pthread.h> when
  # LWS_MAX_SMP > 1 or LWS_WITH_SYS_SMD is on, and it disables both unless
  # LWS_EXT_PTHREAD_LIBRARIES is given (CMakeLists-implied-options.txt) --
  # leaving txpacer.c with undeclared pthread_* functions. Point lws at the
  # toolchain's winpthreads so the include is enabled and the lib is linked.
  LWS_PTHREAD_ROOT ?= $(shell dirname $(shell dirname $(shell command -v gcc)))
  LWS_CMAKE_FLAGS += -DLWS_EXT_PTHREAD_INCLUDE_DIR=$(LWS_PTHREAD_ROOT)/include
  LWS_CMAKE_FLAGS += -DLWS_EXT_PTHREAD_LIBRARIES=$(LWS_PTHREAD_ROOT)/lib/libwinpthread.a
  # On WIN32, LWS defaults LWS_WITH_SCHANNEL=ON (the native TLS backend), and
  # CMakeLists-implied-options.txt then forces LWS_WITH_SSL=1 whenever
  # LWS_WITH_SCHANNEL is set -- overriding our -DLWS_WITH_SSL=OFF flag and
  # pulling in the schannel TLS C files, which fail to compile under MinGW
  # GCC's -Werror. Explicitly disable SChannel so the SSL=OFF flag holds.
  LWS_CMAKE_FLAGS += -DLWS_WITH_SCHANNEL=OFF
  # LWS_WITH_STUB=ON (default) causes implied-options to force LWS_WITH_SPAWN=1,
  # which includes lib/plat/windows/windows-spawn.c. That file uses
  # lws_filefd_type (== int on MinGW) where Windows APIs expect HANDLE (void*)
  # -- a type mismatch that GCC 14 (MSYS2) now promotes to an error. Disable
  # STUB (and thus SPAWN) so that file is not compiled.
  LWS_CMAKE_FLAGS += -DLWS_WITH_STUB=OFF
  # lib/core-net/pollfd.c:437 compares lws_sockfd_type (SOCKET = unsigned long
  # long on Win64) with int -- a sign-compare that GCC -Werror catches. Pass
  # -Wno-sign-compare for the lws-only C build so this non-fatal warning does
  # not abort the compile.
  LWS_CMAKE_FLAGS += "-DCMAKE_C_FLAGS=-Wno-sign-compare"
endif

# lws is linked as a plain static archive, so platform link libs must be
# supplied at app link time. Windows needs winsock plus winpthreads (lws's
# txpacer is compiled against it); Linux needs pthreads.
ifeq ($(RAYLIB_PLATFORM),windows)
  LWS_LINK_LIBS = -lws2_32 -lcrypt32 -ladvapi32 -luser32 -lwinpthread
else ifeq ($(RAYLIB_PLATFORM),macos)
  LWS_LINK_LIBS =
else
  LWS_LINK_LIBS = -lpthread -lm
endif

# LWS_CPPFLAGS / LWS_LIBS are consumed by every Makefile that links lws.
LWS_CPPFLAGS = -I$(LWS_PATH)/include -I$(LWS_BUILD)/include
LWS_LIBS     = $(LWS_LIB) $(LWS_LINK_LIBS)

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

$(LWS_LIB):
	mkdir -p $(LWS_BUILD)
	cmake -S $(LWS_PATH) -B $(LWS_BUILD) \
		-DCMAKE_BUILD_TYPE=Release \
		$(LWS_CMAKE_FLAGS) $(LWS_CMAKE_GENERATOR)
	cmake --build $(LWS_BUILD) --parallel
