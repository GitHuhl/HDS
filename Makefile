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

.PHONY: all run-backend run-frontend clean windows clean-windows

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

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
	rm -f $(BIN)

clean-windows:
	rm -f $(WIN_BIN)
