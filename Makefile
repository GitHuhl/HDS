CC ?= cc
CXX ?= g++
CFLAGS ?= -O2 -Wall -Wextra -std=gnu11
# WavPack recording is disabled for now (see HDS_WAVPACK_ENABLED in src/backend.c) -- -lwavpack
# dropped from here to match; re-add it alongside flipping that flag back to 1.
LDLIBS := -lasound -lm -lpthread -llo

BIN := synth_engine
SRC := src/backend.c

# --- Linux CLAP plugin ---
# Produces hds.clap (a plain ELF shared object under the required .clap extension -- real hosts
# scan for it at ~/.clap/, /usr/lib/clap/, or $CLAP_PATH). #include's the whole of backend.c (see
# src/clap_plugin.c's own header comment), so every ALSA-touching symbol still needs linking even
# though the plugin's own vtable never calls open_pcm_stream/write_audio_block -- they're simply
# unreferenced-but-compiled-in dead code sharing the translation unit.
CLAP_INC := third_party/clap/include
CLAP_BIN := hds.clap
CLAP_SRC := src/clap_plugin.c

# --- Raspberry Pi 5 / aarch64 Linux cross-build (make arm64) ---
# Cross-compiles synth_engine (the backend engine only -- no GUI client for this target) for
# aarch64 Linux (a Raspberry Pi 5, or any other aarch64 Linux host) from this x86_64 machine --
# for the common case where the Pi itself runs plain Raspberry Pi OS rather than NixOS, so
# `nix-shell` isn't available to build directly on the device: build here instead, then copy the
# resulting binary over. Must be run from inside `nix-shell` at the repo root (see shell.nix's own
# aarch64 section) -- ARM64_CC defaults to the prefixed cross-compiler name nix-shell puts on PATH,
# and ARM64_CFLAGS/ARM64_LDFLAGS come from environment variables shell.nix's shellHook exports
# (cross-built libraries live under content-addressed Nix store paths, so they can't be hardcoded
# here). Every dependency here -- alsa-lib, liblo -- is already a real nixpkgs package for this
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
# $(ARM64_LIBDIR) bundles liblo only -- a pure protocol library with no external config files or
# hardware coupling, confirmed safe to ship as a plain .so file (WavPack was bundled here too
# before it was disabled -- see src/backend.c's HDS_WAVPACK_ENABLED). alsa-lib is deliberately
# NOT bundled -- `strings` on the cross-built libasound.so.2 shows its compiled-in default config
# directory is THIS BUILD MACHINE's own Nix store path (where alsa.conf, card definitions, and
# dmix/plug setups live). That directory won't exist on the Pi, and even if it did, it would
# describe the wrong hardware -- the Pi's own apt-installed alsa-lib is compiled with the correct
# /usr/share/alsa path and is what actually knows how to talk to the Pi's real sound chip:
# `sudo apt-get install libasound2` on the Pi -- "cross compiles but the Pi complains about
# missing files" after confirming the interpreter/RPATH fix above almost always means this step
# was skipped, not a build problem. One more real caveat, not yet hit but worth stating plainly:
# this cross toolchain's glibc (2.39, from nixpkgs nixos-24.05) needs to be no newer than the
# Pi's own installed glibc, or the binary will refuse to start with a GLIBC_2.39-not-found error
# -- check the Pi's glibc version (`ldd --version`) against this if a binary that has the right
# interpreter and libraries still won't run.
ARM64_CC ?= aarch64-unknown-linux-gnu-gcc
ARM64_BIN := synth_engine-arm64
# Where liblo gets copied to, alongside the binary, so the Pi needs no apt-get for it (see above
# for why alsa-lib itself is excluded from this bundle on purpose). Copy this directory to the Pi
# together with the binary, keeping them in the same relative layout -- the RPATH below is
# $ORIGIN-relative, i.e. "next to me".
ARM64_LIBDIR := arm64-libs

.PHONY: all run-backend run-frontend clean clap arm64

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clap: $(CLAP_SRC)
	$(CC) $(CFLAGS) -fPIC -shared -I$(CLAP_INC) -o $(CLAP_BIN) $(CLAP_SRC) $(LDLIBS)

ARM64_INTERP := /lib/ld-linux-aarch64.so.1

arm64: $(SRC)
	@test -n "$(ARM64_CFLAGS)" || (echo "ARM64_CFLAGS not set -- run this from inside 'nix-shell' at the repo root" >&2; exit 1)
	$(ARM64_CC) $(CFLAGS) $(ARM64_CFLAGS) -o $(ARM64_BIN) $(SRC) $(ARM64_LDFLAGS) -lasound -lm -lpthread -llo
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
