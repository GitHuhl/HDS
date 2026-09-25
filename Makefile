CC ?= cc
CXX ?= g++
CFLAGS ?= -O2 -Wall -Wextra -std=gnu11
# WavPack recording is disabled for now (see HDS_WAVPACK_ENABLED in src/hds_standalone.c) --
# -lwavpack dropped from here to match; re-add it alongside flipping that flag back to 1.
LDLIBS := -lasound -lm -lpthread -llo

# --- Optional PipeWire backend (native libpipewire client, alongside the always-present ALSA
# backend -- see hds_standalone.c's "Audio platform shim" section) -- auto-detected via pkg-config
# rather than a manual flag to flip (see HDS_WAVPACK_ENABLED's own comment for that pattern), so a
# machine without libpipewire-0.3-dev (a plain apt-get libasound2-dev-only setup, or the Pi) still
# gets a normal ALSA-only build with nothing to change by hand, while nix-shell (which provides
# pipewire in shell.nix's buildInputs) picks it up automatically. --backend pipewire at runtime
# only exists in a build where this actually detected true -- see hds_standalone.c's own
# HDS_PIPEWIRE_ENABLED guard.
# Scoped to $(BIN) only: the CLAP plugin target never touches audio device I/O at all (the host
# owns that via CLAP's own audio-ports extension -- see clap_plugin.c's header comment), and the
# arm64 cross-build has no cross-compiled pipewire package wired up, so neither needs this.
PIPEWIRE_AVAILABLE := $(shell pkg-config --exists libpipewire-0.3 && echo 1 || echo 0)
ifeq ($(PIPEWIRE_AVAILABLE),1)
PIPEWIRE_CFLAGS := -DHDS_PIPEWIRE_ENABLED=1 $(shell pkg-config --cflags libpipewire-0.3)
PIPEWIRE_LDLIBS := $(shell pkg-config --libs libpipewire-0.3)
else
PIPEWIRE_CFLAGS := -DHDS_PIPEWIRE_ENABLED=0
PIPEWIRE_LDLIBS :=
endif
$(info PipeWire backend (--backend pipewire): $(if $(filter 1,$(PIPEWIRE_AVAILABLE)),enabled,disabled -- libpipewire-0.3 not found via pkg-config))

# SRC/BIN_OBJ point at hds_standalone.c (the platform I/O layer: ALSA/PipeWire audio, ALSA MIDI
# sequencer, recording, --program loading, the interactive CLI, and main() itself), NOT backend.c
# (the shared DSP/OSC/state core) -- backend.c is never compiled standalone, exactly like
# clap_plugin.c already treats it. hds_standalone.c #includes backend.c, so this is still a single
# translation unit per binary, same as before this split -- see hds_standalone.c's own header
# comment for the full rationale.
BIN := synth_engine
SRC := src/hds_standalone.c
BUILD_DIR := build
BIN_OBJ := $(BUILD_DIR)/hds_standalone.o
CLAP_OBJ := $(BUILD_DIR)/clap_plugin.o

# --- Neural Amp Modeler FX slot (FX_TYPE_NAM in backend.c) ---
# NeuralAmpModelerCore is a C++/Eigen library with no C API -- src/nam_bridge.cpp is the one
# extern "C" shim backend.c calls into (see its own header comment), and this is the ONLY C++ in
# HDS. Both Eigen and NeuralAmpModelerCore are vendored whole under third_party/ (not a system
# package like PipeWire -- there is nothing to auto-detect, so unlike HDS_PIPEWIRE_ENABLED's
# pkg-config probe, this is simply on by default. $(BIN)/$(CLAP_BIN) are therefore now built as
# C objects compiled by $(CC) and C++ objects compiled by $(CXX), linked together by $(CXX) (the
# only correct final link driver for a mixed C/C++ binary -- see $(BIN)/clap's own rules below).
#
# Also wired into the `arm64` target (see its own comment block for ARM64_CXX/ARM64_NAM_OBJS) --
# there never was a real "no C++ cross-compiler" problem, only a missing recipe: shell.nix's
# aarch64.stdenv.cc already installs a full aarch64-unknown-linux-gnu-g++ alongside its own -gcc,
# it just went unused. Eigen needs nothing extra either way (header-only, architecture-independent).
NAM_DIR := third_party/NeuralAmpModelerCore
EIGEN_DIR := third_party/eigen
NAM_BUILD_DIR := $(BUILD_DIR)/nam
# Defined here (not down by the arm64 target's own NAM pattern rules) specifically because the
# shared $(BUILD_DIR) $(NAM_BUILD_DIR) $(ARM64_NAM_BUILD_DIR): directory rule below references it --
# a := variable used in a target/prerequisite list expands to whatever value it holds AT THAT LINE,
# not its final value, so defining this any later left that combined rule silently missing the
# arm64-nam target entirely (confirmed directly: `make -n arm64` failed with "No rule to make
# target 'build/arm64-nam/activations.o'" until this moved up here).
ARM64_NAM_BUILD_DIR := $(BUILD_DIR)/arm64-nam
NAM_INC := -I$(NAM_DIR) -I$(NAM_DIR)/Dependencies/nlohmann -I$(EIGEN_DIR)
# -fPIC unconditionally, not just for the `clap` target's shared-object link: these objects are
# shared between $(BIN) (a plain executable) and $(CLAP_BIN) (a shared object, which REQUIRES
# every object linked into it to be position-independent or the link fails outright) -- PIC code
# runs correctly in a plain executable too (most distros build PIE executables by default anyway),
# so there is no correctness cost to always compiling it this way, only the ability to reuse one
# object set for both targets instead of building everything twice.
NAM_CXXFLAGS := -O2 -std=c++20 -fPIC $(NAM_INC)
NAM_SRCS := \
	$(NAM_DIR)/NAM/activations.cpp \
	$(NAM_DIR)/NAM/container.cpp \
	$(NAM_DIR)/NAM/conv1d.cpp \
	$(NAM_DIR)/NAM/convnet.cpp \
	$(NAM_DIR)/NAM/dsp.cpp \
	$(NAM_DIR)/NAM/get_dsp.cpp \
	$(NAM_DIR)/NAM/linear.cpp \
	$(NAM_DIR)/NAM/lstm.cpp \
	$(NAM_DIR)/NAM/nam_file.cpp \
	$(NAM_DIR)/NAM/ring_buffer.cpp \
	$(NAM_DIR)/NAM/sequential.cpp \
	$(NAM_DIR)/NAM/util.cpp \
	$(NAM_DIR)/NAM/wav.cpp \
	$(NAM_DIR)/NAM/wavenet/a2_fast.cpp \
	$(NAM_DIR)/NAM/wavenet/model.cpp \
	$(NAM_DIR)/NAM/wavenet/slimmable.cpp \
	src/nam_bridge.cpp
# Flattened into one directory (no name collisions between NAM/, NAM/wavenet/, and src/ -- verified
# by hand against the list above) so the three pattern rules below can all target it uniformly.
NAM_OBJS := $(addprefix $(NAM_BUILD_DIR)/,$(notdir $(NAM_SRCS:.cpp=.o)))

# --- Linux CLAP plugin ---
# Produces hds.clap (a plain ELF shared object under the required .clap extension -- real hosts
# scan for it at ~/.clap/, /usr/lib/clap/, or $CLAP_PATH). #include's the whole of backend.c (see
# src/clap_plugin.c's own header comment) -- backend.c is the shared DSP/OSC/state core ONLY,
# with every ALSA/PipeWire-touching line long since moved out to src/hds_standalone.c (see that
# file's own header comment), so this build never sees alsa/asoundlib.h or any device-I/O symbol
# at all, exactly as a plugin whose host owns the actual audio device should.
CLAP_INC := third_party/clap/include
CLAP_BIN := hds.clap
CLAP_SRC := src/clap_plugin.c

# --- HDS FX: standalone, synth-free CLAP effects plugin (make hds-fx) ---
# Produces hds_fx.clap. Unlike $(CLAP_BIN) above, this does NOT #include backend.c at all -- see
# src/hds_fx_plugin.c's own header comment for why (no OSC/networking, no synth, just src/fx_types.h
# + src/fx_chain.c, the same FX DSP backend.c's own embedded FX bus already runs). NAM
# (FX_TYPE_NAM) is compiled out entirely here (HDS_NAM_ENABLED 0 in hds_fx_plugin.c itself), so
# this is the one real target in this Makefile that needs neither $(CXX) nor any NAM_OBJS -- a
# plain $(CC) link, pure C, no liblo/-llo either. It DOES need Cairo + X11 now (src/hds_fx_gui_x11.c
# -- the real embedded GUI, see that file's own header comment): both are pulled in via pkg-config
# rather than hardcoded flags, same posture as the PipeWire auto-detection above.
HDS_FX_SRC := src/hds_fx_plugin.c
HDS_FX_BIN := hds_fx.clap
HDS_FX_OBJ := $(BUILD_DIR)/hds_fx_plugin.o
HDS_FX_CFLAGS := $(shell pkg-config --cflags cairo x11)
HDS_FX_LDLIBS := $(shell pkg-config --libs cairo x11) -lm

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
# before it was disabled -- see src/hds_standalone.c's HDS_WAVPACK_ENABLED). alsa-lib is deliberately
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
# Already installed by shell.nix's aarch64.stdenv.cc alongside ARM64_CC above (same package
# installs both under this prefix) -- used only for the NAM objects and the arm64 target's own
# final link (see NAM's own comment on why $(CXX)/$(ARM64_CXX), not the C compiler, has to be the
# final link driver for a mixed C/C++ binary).
ARM64_CXX ?= aarch64-unknown-linux-gnu-g++
ARM64_BIN := synth_engine-arm64
# Where liblo gets copied to, alongside the binary, so the Pi needs no apt-get for it (see above
# for why alsa-lib itself is excluded from this bundle on purpose). Copy this directory to the Pi
# together with the binary, keeping them in the same relative layout -- the RPATH below is
# $ORIGIN-relative, i.e. "next to me".
ARM64_LIBDIR := arm64-libs

.PHONY: all run-backend run-frontend clean clap hds-fx arm64

all: $(BIN)

$(BUILD_DIR) $(NAM_BUILD_DIR) $(ARM64_NAM_BUILD_DIR):
	mkdir -p $@

# backend.c/clap_plugin.c stay plain C, compiled by $(CC) exactly as before -- only the FINAL link
# step (below) needs $(CXX), to pull in libstdc++ for the NAM objects linked alongside them.
# src/backend.c and src/fx_chain.c are listed explicitly (not just $(SRC)/$(CLAP_SRC)) because
# every real source file #includes them rather than links against them -- without this, editing
# either alone wouldn't trigger a rebuild of any object depending on it. src/sequencer.c is the
# same story but ONLY for $(BIN_OBJ)/$(ARM64_BIN_OBJ): it's #include'd by hds_standalone.c alone
# (never reachable from a CLAP plugin instance -- see its own header comment), so it's deliberately
# left off $(CLAP_OBJ)'s own prerequisite list below.
$(BIN_OBJ): $(SRC) src/backend.c src/fx_types.h src/fx_chain.c src/sequencer.c src/nam_bridge.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(PIPEWIRE_CFLAGS) -c $(SRC) -o $@

$(CLAP_OBJ): $(CLAP_SRC) src/backend.c src/fx_types.h src/fx_chain.c src/nam_bridge.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -fPIC -I$(CLAP_INC) -c $(CLAP_SRC) -o $@

$(HDS_FX_OBJ): $(HDS_FX_SRC) src/fx_types.h src/fx_chain.c src/hds_fx_gui_draw.c src/hds_fx_gui_x11.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(HDS_FX_CFLAGS) -fPIC -I$(CLAP_INC) -c $(HDS_FX_SRC) -o $@

# Three source directories (NAM/, NAM/wavenet/, src/) flattened into one object directory -- see
# NAM_OBJS's own comment on why this is safe (no basename collisions).
$(NAM_BUILD_DIR)/%.o: $(NAM_DIR)/NAM/%.cpp | $(NAM_BUILD_DIR)
	$(CXX) $(NAM_CXXFLAGS) -c $< -o $@

$(NAM_BUILD_DIR)/%.o: $(NAM_DIR)/NAM/wavenet/%.cpp | $(NAM_BUILD_DIR)
	$(CXX) $(NAM_CXXFLAGS) -c $< -o $@

$(NAM_BUILD_DIR)/%.o: src/%.cpp | $(NAM_BUILD_DIR)
	$(CXX) $(NAM_CXXFLAGS) -c $< -o $@

$(BIN): $(BIN_OBJ) $(NAM_OBJS)
	$(CXX) -o $@ $(BIN_OBJ) $(NAM_OBJS) $(LDLIBS) $(PIPEWIRE_LDLIBS)

clap: $(CLAP_OBJ) $(NAM_OBJS)
	$(CXX) -shared -o $(CLAP_BIN) $(CLAP_OBJ) $(NAM_OBJS) $(LDLIBS)

hds-fx: $(HDS_FX_OBJ)
	$(CC) -shared -o $(HDS_FX_BIN) $(HDS_FX_OBJ) $(HDS_FX_LDLIBS)

# arm64's own NAM object set: SEPARATE .o files from $(NAM_OBJS) above (wrong ISA to reuse, even
# though the flags are identical -- NAM_CXXFLAGS needs nothing arm64-specific since NAM_INC is just
# header paths, architecture-independent either way), built by $(ARM64_CXX) into their own
# directory so a `make` and a `make arm64` in the same tree never fight over the same .o files.
# (ARM64_NAM_BUILD_DIR itself is defined up with NAM_BUILD_DIR near the top of this file, not here
# -- see that definition's own comment for why.)
ARM64_NAM_OBJS := $(addprefix $(ARM64_NAM_BUILD_DIR)/,$(notdir $(NAM_SRCS:.cpp=.o)))

$(ARM64_NAM_BUILD_DIR)/%.o: $(NAM_DIR)/NAM/%.cpp | $(ARM64_NAM_BUILD_DIR)
	$(ARM64_CXX) $(NAM_CXXFLAGS) -c $< -o $@

$(ARM64_NAM_BUILD_DIR)/%.o: $(NAM_DIR)/NAM/wavenet/%.cpp | $(ARM64_NAM_BUILD_DIR)
	$(ARM64_CXX) $(NAM_CXXFLAGS) -c $< -o $@

$(ARM64_NAM_BUILD_DIR)/%.o: src/%.cpp | $(ARM64_NAM_BUILD_DIR)
	$(ARM64_CXX) $(NAM_CXXFLAGS) -c $< -o $@

ARM64_INTERP := /lib/ld-linux-aarch64.so.1
ARM64_BIN_OBJ := $(BUILD_DIR)/hds_standalone-arm64.o

# hds_standalone.c itself stays plain C, compiled by $(ARM64_CC) exactly like the native
# $(BIN_OBJ) rule above -- only the FINAL link step needs $(ARM64_CXX), to pull in libstdc++ for
# the NAM objects linked alongside it, same reasoning as $(BIN)/clap's own rules.
$(ARM64_BIN_OBJ): $(SRC) src/backend.c src/fx_types.h src/fx_chain.c src/sequencer.c src/nam_bridge.h | $(BUILD_DIR)
	@test -n "$(ARM64_CFLAGS)" || (echo "ARM64_CFLAGS not set -- run this from inside 'nix-shell' at the repo root" >&2; exit 1)
	$(ARM64_CC) $(CFLAGS) $(ARM64_CFLAGS) -c $(SRC) -o $@

arm64: $(ARM64_BIN_OBJ) $(ARM64_NAM_OBJS)
	$(ARM64_CXX) -o $(ARM64_BIN) $(ARM64_BIN_OBJ) $(ARM64_NAM_OBJS) $(ARM64_LDFLAGS) -lasound -lm -lpthread -llo
	mkdir -p $(ARM64_LIBDIR)
	cp -Lf $(ARM64_BUNDLE_LIBS) $(ARM64_LIBDIR)/
	patchelf --set-rpath '$$ORIGIN/$(ARM64_LIBDIR)' --set-interpreter $(ARM64_INTERP) $(ARM64_BIN)

run-backend: $(BIN)
	./$(BIN)

run-frontend:
	python3 src/frontend.py

clean:
	rm -f $(BIN) $(CLAP_BIN) $(HDS_FX_BIN) $(ARM64_BIN)
	rm -rf $(ARM64_LIBDIR) $(BUILD_DIR)
