CC ?= cc
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

.PHONY: all run-backend run-frontend clean windows clean-windows clap clap-windows

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clap: $(CLAP_SRC)
	$(CC) $(CFLAGS) -fPIC -shared -I$(CLAP_INC) -o $(CLAP_BIN) $(CLAP_SRC) $(LDLIBS)

windows: $(WIN_DEPS)/liblo/lib/liblo.a $(WIN_DEPS)/wavpack/lib/libwavpack.a
	$(WIN_CC) $(WIN_CFLAGS) -o $(WIN_BIN) $(SRC) $(WIN_LDFLAGS) $(WIN_LDLIBS)

clap-windows: $(WIN_DEPS)/liblo/lib/liblo.a $(WIN_DEPS)/wavpack/lib/libwavpack.a $(CLAP_SRC)
	$(WIN_CC) $(WIN_CFLAGS) -shared -I$(CLAP_INC) -o $(WIN_CLAP_BIN) $(CLAP_SRC) $(WIN_LDFLAGS) $(WIN_LDLIBS)

$(WIN_DEPS)/liblo/lib/liblo.a $(WIN_DEPS)/wavpack/lib/libwavpack.a:
	@echo "Windows dependencies not found -- run tools/build_windows_deps.sh first" >&2
	@exit 1

run-backend: $(BIN)
	./$(BIN)

run-frontend:
	python3 src/frontend.py

clean:
	rm -f $(BIN) $(CLAP_BIN)

clean-windows:
	rm -f $(WIN_BIN) $(WIN_CLAP_BIN)
