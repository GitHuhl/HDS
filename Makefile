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
WIN_CC := x86_64-w64-mingw32-gcc
WIN_BIN := synth_engine.exe
WIN_DEPS := third_party/win64-deps
WIN_CFLAGS := -O2 -Wall -Wextra -std=gnu11 -I$(WIN_DEPS)/liblo/include -I$(WIN_DEPS)/wavpack/include
WIN_LDFLAGS := -L$(WIN_DEPS)/liblo/lib -L$(WIN_DEPS)/wavpack/lib
WIN_LDLIBS := -llo -lwavpack -lws2_32 -liphlpapi -lwinmm -lole32 -lm -lpthread

# --- Linux CLAP plugin ---
# Produces hds.clap (a plain ELF shared object under the required .clap extension -- real hosts
# scan for it at ~/.clap/, /usr/lib/clap/, or $CLAP_PATH). #include's the whole of backend.c (see
# src/clap_plugin.c's own header comment), so every ALSA-touching symbol still needs linking even
# though the plugin's own vtable never calls open_pcm_stream/write_audio_block -- they're simply
# unreferenced-but-compiled-in dead code sharing the translation unit. Linux-only for now; see the
# plan doc's own stated Windows-out-of-scope boundary.
CLAP_INC := third_party/clap/include
CLAP_BIN := hds.clap
CLAP_SRC := src/clap_plugin.c

.PHONY: all run-backend run-frontend clean windows clean-windows clap

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clap: $(CLAP_SRC)
	$(CC) $(CFLAGS) -fPIC -shared -I$(CLAP_INC) -o $(CLAP_BIN) $(CLAP_SRC) $(LDLIBS)

windows: $(WIN_DEPS)/liblo/lib/liblo.a $(WIN_DEPS)/wavpack/lib/libwavpack.a
	$(WIN_CC) $(WIN_CFLAGS) -o $(WIN_BIN) $(SRC) $(WIN_LDFLAGS) $(WIN_LDLIBS)

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
	rm -f $(WIN_BIN)
