CC ?= cc
CXX ?= g++
CFLAGS ?= -O2 -Wall -Wextra -std=gnu11
LDLIBS := -lasound -lm -lpthread -lwavpack -llo

BIN := synth_engine
SRC := src/backend.c

# --- Windows cross-build (CLAP plugin only -- standalone synth_engine.exe dropped from scope) ---
# `?=` (not `:=`): the standard x86_64-w64-mingw32-gcc name is right for Debian/Ubuntu's
# mingw-w64 package and the nix-shell in shell.nix at the repo root, but overridable
# (`make clap-windows WIN_CC=...`) if some other toolchain names its cross compiler differently.
WIN_CC ?= x86_64-w64-mingw32-gcc
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
# Produces hds-windows.clap: a PE32+ DLL exporting clap_entry, built with mingw-w64 + the
# third_party/win64-deps/ liblo/wavpack static libs (run tools/build_windows_deps.sh once first).
# Needs no platform code of its own: clap_plugin.c never touches the audio/MIDI-device paths
# backend.c already abstracts per-OS (the host supplies buffers/note-events instead), so
# cross-compiling it is the exact same recipe as `clap` above, just with $(WIN_CC)/$(WIN_LDLIBS)
# instead of $(CC)/$(LDLIBS) and no -fPIC (meaningless for a Windows DLL). To install on Windows:
# drop the .clap file into %COMMONPROGRAMFILES%\CLAP\ or %LOCALAPPDATA%\Programs\Common\CLAP\.
WIN_CLAP_BIN := hds-windows.clap

# --- Raspberry Pi 5 / aarch64 Linux cross-build (make arm64) ---
# Cross-compiles synth_engine (the backend engine only -- no GUI client for this target) for
# aarch64 Linux (a Raspberry Pi 5, or any other aarch64 Linux host) from this x86_64 machine --
# for the common case where the Pi itself runs plain Raspberry Pi OS rather than NixOS, so
# `nix-shell` isn't available to build directly on the device: build here instead, then copy the
# resulting binary over. Must be run from inside `nix-shell` at the repo root (see shell.nix's own
# aarch64 section) -- ARM64_CC defaults to the prefixed cross-compiler name nix-shell puts on PATH
# (mirrors WIN_CC's exact convention above), and ARM64_CFLAGS/ARM64_LDFLAGS come from environment
# variables shell.nix's shellHook exports (cross-built libraries live under content-addressed Nix
# store paths, unlike $(WIN_DEPS)'s fixed relative directory, so they can't be hardcoded here).
# Every dependency here -- alsa-lib, liblo, wavpack -- is already a real nixpkgs package for this
# exact target, so no separate deps-building script is needed.
#
# Deploying the output binary -- confirmed directly by inspecting a real built binary's own ELF
# headers, not assumed. Two separate Nix-specific paths get baked in by the cross linker, neither
# of which exists on the Pi:
#   1. An RPATH pointing at this build machine's own /nix/store/... library directories.
#   2. The ELF interpreter itself (PT_INTERP) -- literally the dynamic loader the kernel execve()s
#      to start the program -- set to this build machine's own /nix/store/...-glibc.../ld-linux-
#      aarch64.so.1. This is the more consequential of the two: a missing RPATH entry just means
#      the loader falls through to the next search path, but an interpreter path that doesn't
#      exist means the kernel can't start the binary AT ALL, and most shells report that as
#      "No such file or directory" -- easily (and reasonably) mistaken for the binary itself being
#      missing, even though `ls`/`file` show it's right there.
# The `arm64` rule runs `patchelf --set-rpath '$$ORIGIN/$(ARM64_LIBDIR)' --set-interpreter
# $(ARM64_INTERP)` on its output as a last step: `/lib/ld-linux-aarch64.so.1` is the standard,
# well-known path glibc-based aarch64 Debian/Ubuntu/Raspberry Pi OS systems already have their own
# real dynamic loader installed at, and the $ORIGIN-relative RPATH points at $(ARM64_LIBDIR)/
# (copied alongside the binary by this same rule -- see that variable's own comment) instead of
# this build machine's own /nix/store/... paths.
#
# $(ARM64_LIBDIR) bundles liblo/wavpack only -- pure protocol/codec libraries with no external
# config files or hardware coupling, confirmed safe to ship as plain .so files. alsa-lib is
# deliberately NOT bundled -- `strings` on the cross-built libasound.so.2 shows its compiled-in
# default config directory is THIS BUILD MACHINE's own Nix store path (where alsa.conf, card
# definitions, and dmix/plug setups live). That directory won't exist on the Pi, and even if it
# did, it would describe the wrong hardware -- the Pi's own apt-installed alsa-lib is compiled with
# the correct /usr/share/alsa path and is what actually knows how to talk to the Pi's real sound
# chip: `sudo apt-get install libasound2` on the Pi -- "cross compiles but the Pi complains about
# missing files" after confirming the interpreter/RPATH fix above almost always means this step was
# skipped, not a build problem. One more real caveat, not yet hit but worth stating plainly: this
# cross toolchain's glibc (2.39, from nixpkgs nixos-24.05) needs to be no newer than the Pi's own
# installed glibc, or the binary will refuse to start with a GLIBC_2.39-not-found error -- check
# the Pi's glibc version (`ldd --version`) against this if a binary that has the right interpreter
# and libraries still won't run.
ARM64_CC ?= aarch64-unknown-linux-gnu-gcc
ARM64_BIN := synth_engine-arm64
# Where alsa-lib/liblo/wavpack get copied to, alongside the binary, so the Pi needs no apt-get
# for liblo/wavpack (see above for why alsa-lib itself is excluded from this bundle on purpose).
# Copy this directory to the Pi together with the binary, keeping them in the same relative
# layout -- the RPATH below is $ORIGIN-relative, i.e. "next to me".
ARM64_LIBDIR := arm64-libs

.PHONY: all run-backend run-frontend clean clean-windows clap clap-windows arm64

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clap: $(CLAP_SRC)
	$(CC) $(CFLAGS) -fPIC -shared -I$(CLAP_INC) -o $(CLAP_BIN) $(CLAP_SRC) $(LDLIBS)

clap-windows: $(WIN_DEPS)/liblo/lib/liblo.a $(WIN_DEPS)/wavpack/lib/libwavpack.a $(CLAP_SRC)
	$(WIN_CC) $(WIN_CFLAGS) -shared -I$(CLAP_INC) -o $(WIN_CLAP_BIN) $(CLAP_SRC) $(WIN_LDFLAGS) $(WIN_LDLIBS)

$(WIN_DEPS)/liblo/lib/liblo.a $(WIN_DEPS)/wavpack/lib/libwavpack.a:
	@echo "Windows dependencies not found -- run tools/build_windows_deps.sh first" >&2
	@exit 1

ARM64_INTERP := /lib/ld-linux-aarch64.so.1

arm64: $(SRC)
	@test -n "$(ARM64_CFLAGS)" || (echo "ARM64_CFLAGS not set -- run this from inside 'nix-shell' at the repo root" >&2; exit 1)
	$(ARM64_CC) $(CFLAGS) $(ARM64_CFLAGS) -o $(ARM64_BIN) $(SRC) $(ARM64_LDFLAGS) -lasound -lm -lpthread -lwavpack -llo
	mkdir -p $(ARM64_LIBDIR)
	cp -Lf $(ARM64_BUNDLE_LIBS) $(ARM64_LIBDIR)/
	patchelf --set-rpath '$$ORIGIN/$(ARM64_LIBDIR)' --set-interpreter $(ARM64_INTERP) $(ARM64_BIN)

run-backend: $(BIN)
	./$(BIN)

run-frontend:
	python3 src/frontend.py

clean:
	rm -f $(BIN) $(CLAP_BIN) $(ARM64_BIN)
	rm -rf $(ARM64_LIBDIR)

clean-windows:
	rm -f $(WIN_CLAP_BIN)
