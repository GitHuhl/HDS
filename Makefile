CC ?= cc
CXX ?= g++
CFLAGS ?= -O2 -Wall -Wextra -std=gnu11
LDLIBS := -lasound -lm -lpthread -lwavpack -llo

BIN := synth_engine
SRC := src/backend.c

# --- Windows cross-build ---
# Produces synth_engine.exe via the mingw-w64 cross-compiler. Needs `tools/build_windows_deps.sh`
# run once first (fetches + cross-builds liblo/wavpack for Windows into third_party/win64-deps/ --
# audio/MIDI need nothing extra beyond mingw-w64 itself, see the platform-shim comments at the top
# of src/backend.c). This is a completely separate target from `all` above -- it never runs as
# part of a plain `make`, and touches none of the Linux build's flags/libs/output.
# `?=` (not `:=`): the standard x86_64-w64-mingw32-gcc name is right for Debian/Ubuntu's
# mingw-w64 package and the nix-shell in shell.nix at the repo root, but overridable
# (`make windows WIN_CC=...`) if some other toolchain names its cross compiler differently.
WIN_CC ?= x86_64-w64-mingw32-gcc
WIN_BIN := synth_engine.exe
WIN_DEPS := third_party/win64-deps
# Some mingw-w64 cross-toolchains (confirmed: some nixpkgs pkgsCross.mingwW64 builds) ship the
# "win32" threading model -- no winpthreads, no pthread.h at all -- unlike Debian/Ubuntu's
# mingw-w64 package, which defaults to "posix" (winpthreads bundled in). When that's missing,
# build_windows_deps.sh cross-builds winpthreads from source into $(WIN_DEPS)/winpthreads/ (see
# its own comments); this $(wildcard) check picks that up automatically -- add its include/lib
# path and link -lwinpthread instead of the plain -lpthread a posix-model toolchain already
# provides for free. Nothing to configure by hand either way.
WIN_HAS_WINPTHREADS := $(wildcard $(WIN_DEPS)/winpthreads/lib/libwinpthread.a)
ifneq ($(WIN_HAS_WINPTHREADS),)
WIN_CFLAGS := -O2 -Wall -Wextra -std=gnu11 -I$(WIN_DEPS)/liblo/include -I$(WIN_DEPS)/wavpack/include -I$(WIN_DEPS)/winpthreads/include
WIN_LDFLAGS := -L$(WIN_DEPS)/liblo/lib -L$(WIN_DEPS)/wavpack/lib -L$(WIN_DEPS)/winpthreads/lib
WIN_LDLIBS := -llo -lwavpack -lws2_32 -liphlpapi -lwinmm -lole32 -lm -lwinpthread
else
WIN_CFLAGS := -O2 -Wall -Wextra -std=gnu11 -I$(WIN_DEPS)/liblo/include -I$(WIN_DEPS)/wavpack/include
WIN_LDFLAGS := -L$(WIN_DEPS)/liblo/lib -L$(WIN_DEPS)/wavpack/lib
WIN_LDLIBS := -llo -lwavpack -lws2_32 -liphlpapi -lwinmm -lole32 -lm -lpthread
endif

# --- Linux CLAP plugin ---
# Produces hds.clap (a plain ELF shared object under the required .clap extension -- real hosts
# scan for it at ~/.clap/, /usr/lib/clap/, or $CLAP_PATH). #include's the whole of backend.c (see
# src/clap_plugin.c's own header comment), so every ALSA-touching symbol still needs linking even
# though the plugin's own vtable never calls open_pcm_stream/write_audio_block -- they're simply
# unreferenced-but-compiled-in dead code sharing the translation unit.
CLAP_INC := third_party/clap/include
CLAP_BIN := hds.clap
CLAP_SRC := src/clap_plugin.c

# --- Windows CLAP plugin (cross-built from Linux) ---
# Produces hds-windows.clap: a PE32+ DLL exporting clap_entry, built the same way synth_engine.exe
# is (mingw-w64 + the same third_party/win64-deps/ liblo/wavpack static libs -- see `windows`
# above; run tools/build_windows_deps.sh once first if that target hasn't been built yet). Needs
# no platform code of its own: clap_plugin.c never touches the audio/MIDI-device paths backend.c
# already abstracts per-OS (the host supplies buffers/note-events instead), so cross-compiling it
# is the exact same recipe as `clap` above, just with $(WIN_CC)/$(WIN_LDLIBS) instead of
# $(CC)/$(LDLIBS) and no -fPIC (meaningless for a Windows DLL). Distinct output name from
# $(CLAP_BIN) so `make clap clap-windows` in one checkout never overwrites either. To install on
# Windows: drop the .clap file into %COMMONPROGRAMFILES%\CLAP\ or %LOCALAPPDATA%\Programs\Common\CLAP\.
WIN_CLAP_BIN := hds-windows.clap

# --- Raspberry Pi / embedded touchscreen client (optional, not part of `all`) ---
# An ImGui + GLFW touch UI for a small (e.g. 720x720px 4") panel, talking to the real HDS backend
# over OSC -- see src/pi_touch_frontend.cpp's own header comment for scope/design. Needs GLFW3 +
# OpenGL + liblo dev packages (apt: libglfw3-dev liblo-dev); Dear ImGui itself is vendored into
# third_party/imgui/ (pinned to tag v1.91.5, MIT licensed, github.com/ocornut/imgui) rather than
# assumed to be a system package, mirroring this project's existing CLAP-SDK/miniaudio.h vendoring
# precedent. Compiles ImGui's own core + GLFW/OpenGL3 backend .cpp files directly alongside
# pi_touch_frontend.cpp rather than building a separate static lib -- ImGui's own documented build
# model (no configure step, just compile the handful of .cpp files a project actually uses), and
# simpler than adding a whole second build system for a small vendored dependency.
PI_TOUCH_BIN := hds_pi_touch
PI_TOUCH_INC := third_party/imgui
PI_TOUCH_SRC := src/pi_touch_frontend.cpp \
                $(PI_TOUCH_INC)/imgui.cpp $(PI_TOUCH_INC)/imgui_draw.cpp \
                $(PI_TOUCH_INC)/imgui_tables.cpp $(PI_TOUCH_INC)/imgui_widgets.cpp \
                $(PI_TOUCH_INC)/backends/imgui_impl_glfw.cpp $(PI_TOUCH_INC)/backends/imgui_impl_opengl3.cpp
PI_TOUCH_CFLAGS := $(shell pkg-config --cflags glfw3) -I$(PI_TOUCH_INC) -I$(PI_TOUCH_INC)/backends
PI_TOUCH_LDLIBS := $(shell pkg-config --libs glfw3) -llo -lGL -ldl

# --- Raspberry Pi 5 / aarch64 Linux cross-build (make arm64 / make pi-touch-arm64) ---
# Cross-compiles synth_engine and hds_pi_touch for aarch64 Linux (a Raspberry Pi 5, or any other
# aarch64 Linux host) from this x86_64 machine -- for the common case where the Pi itself runs
# plain Raspberry Pi OS rather than NixOS, so `nix-shell` isn't available to build directly on the
# device: build here instead, then copy the resulting binary over. Must be run from inside
# `nix-shell` at the repo root (see shell.nix's own aarch64 section) -- ARM64_CC/ARM64_CXX default
# to the prefixed cross-compiler names nix-shell puts on PATH (mirrors WIN_CC's exact convention
# above), and ARM64_CFLAGS/ARM64_LDFLAGS come from environment variables shell.nix's shellHook
# exports (cross-built libraries live under content-addressed Nix store paths, unlike $(WIN_DEPS)'s
# fixed relative directory, so they can't be hardcoded here). Unlike the Windows cross-build (which
# needs tools/build_windows_deps.sh to cross-build liblo/wavpack from source, since mingw builds of
# them aren't in nixpkgs), every dependency here -- alsa-lib, liblo, wavpack, glfw -- is already a
# real nixpkgs package for this exact target, so no separate deps-building script is needed.
ARM64_CC ?= aarch64-unknown-linux-gnu-gcc
ARM64_CXX ?= aarch64-unknown-linux-gnu-g++
ARM64_BIN := synth_engine-arm64
ARM64_PI_TOUCH_BIN := hds_pi_touch-arm64

# NOTE: `all` (immediately below) must stay the FIRST rule in this file -- Make's default goal
# (what runs when you type plain `make` with no target) is whichever rule appears first, and
# pi-touch is an opt-in target, never the default.
.PHONY: all run-backend run-frontend clean windows clean-windows clap clap-windows pi-touch arm64 pi-touch-arm64

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clap: $(CLAP_SRC)
	$(CC) $(CFLAGS) -fPIC -shared -I$(CLAP_INC) -o $(CLAP_BIN) $(CLAP_SRC) $(LDLIBS)

windows: $(WIN_DEPS)/liblo/lib/liblo.a $(WIN_DEPS)/wavpack/lib/libwavpack.a
	$(WIN_CC) $(WIN_CFLAGS) -o $(WIN_BIN) $(SRC) $(WIN_LDFLAGS) $(WIN_LDLIBS)

clap-windows: $(WIN_DEPS)/liblo/lib/liblo.a $(WIN_DEPS)/wavpack/lib/libwavpack.a $(CLAP_SRC)
	$(WIN_CC) $(WIN_CFLAGS) -shared -I$(CLAP_INC) -o $(WIN_CLAP_BIN) $(CLAP_SRC) $(WIN_LDFLAGS) $(WIN_LDLIBS)

pi-touch: $(PI_TOUCH_SRC)
	$(CXX) -O2 -Wall -Wextra -std=gnu++17 $(PI_TOUCH_CFLAGS) -o $(PI_TOUCH_BIN) $(PI_TOUCH_SRC) $(PI_TOUCH_LDLIBS)

$(WIN_DEPS)/liblo/lib/liblo.a $(WIN_DEPS)/wavpack/lib/libwavpack.a:
	@echo "Windows dependencies not found -- run tools/build_windows_deps.sh first" >&2
	@exit 1

arm64: $(SRC)
	@test -n "$(ARM64_CFLAGS)" || (echo "ARM64_CFLAGS not set -- run this from inside 'nix-shell' at the repo root" >&2; exit 1)
	$(ARM64_CC) $(CFLAGS) $(ARM64_CFLAGS) -o $(ARM64_BIN) $(SRC) $(ARM64_LDFLAGS) -lasound -lm -lpthread -lwavpack -llo

pi-touch-arm64: $(PI_TOUCH_SRC)
	@test -n "$(ARM64_CFLAGS)" || (echo "ARM64_CFLAGS not set -- run this from inside 'nix-shell' at the repo root" >&2; exit 1)
	$(ARM64_CXX) -O2 -Wall -Wextra -std=gnu++17 -I$(PI_TOUCH_INC) -I$(PI_TOUCH_INC)/backends $(ARM64_CFLAGS) -o $(ARM64_PI_TOUCH_BIN) $(PI_TOUCH_SRC) $(ARM64_LDFLAGS) -lglfw -llo -lGL -ldl

run-backend: $(BIN)
	./$(BIN)

run-frontend:
	python3 src/frontend.py

clean:
	rm -f $(BIN) $(CLAP_BIN) $(PI_TOUCH_BIN) $(ARM64_BIN) $(ARM64_PI_TOUCH_BIN)

clean-windows:
	rm -f $(WIN_BIN) $(WIN_CLAP_BIN)
