# Building HDS: background and rationale

These notes explain *why* the Makefile is set up the way it is. They were the Makefile's inline
comments until the build was consolidated (see git history); they are kept here verbatim, grouped
by topic. Variable names and object paths in them refer to the Makefile as it was when they were
written -- the reasoning still applies, but e.g. "$(NAM_OBJS)" or "build/nam/" are now per-variant
(build/<variant>/nam/), and individual compile rules are now one pattern rule per variant.

For the commands themselves, see the README's Building and Testing sections.

## General build settings

Context: `STRIPFLAGS := -s`

No -g anywhere in CFLAGS above (never was), so there's no DWARF debug info to omit -- but by
default the linker still keeps every binary's full symbol table (function/variable names), which
is exactly what makes a disassembler pleasant to read. STRIPFLAGS (-s, "strip at link time") on
every final link below removes that symbol table so a beta build doesn't hand a curious tester a
labeled map of the codebase for free. This is NOT real anti-tampering/anti-RE protection -- the
actual DSP logic, control flow, and strings are all still fully present and reversible with
enough effort (a stripped binary's function boundaries/args are still recoverable via a decompiler,
just unnamed) -- it just raises the floor past "nm/objdump immediately hands you every symbol".

Context: `LDLIBS := -lasound -lm -lpthread -llo -lwavpack`

WavPack recording (see HDS_WAVPACK_ENABLED in src/targets/standalone/hds_standalone.c) -- -lwavpack added back
here, same shared LDLIBS list $(BIN)/hds.clap both already link (see -lasound's own precedent
just above: hds.clap links it too despite never touching audio device I/O itself, since nothing
here is worth a second near-duplicate LDLIBS variable over). Only synth_engine's own translation
unit (hds_standalone.c) references any WavPack symbol at all.

## PipeWire auto-detection

Context: `PIPEWIRE_AVAILABLE := $(shell pkg-config --exists libpipewire-0.3 && echo 1 || echo 0)`

--- Optional PipeWire backend (native libpipewire client, alongside the always-present ALSA
backend -- see hds_standalone.c's "Audio platform shim" section) -- auto-detected via pkg-config
rather than a manual flag to flip (see HDS_WAVPACK_ENABLED's own comment for that pattern), so a
machine without libpipewire-0.3-dev (a plain apt-get libasound2-dev-only setup, or the Pi) still
gets a normal ALSA-only build with nothing to change by hand, while nix-shell (which provides
pipewire in shell.nix's buildInputs) picks it up automatically. --backend pipewire at runtime
only exists in a build where this actually detected true -- see hds_standalone.c's own
HDS_PIPEWIRE_ENABLED guard.
Scoped to $(BIN) only: the CLAP plugin target never touches audio device I/O at all (the host
owns that via CLAP's own audio-ports extension -- see clap_plugin.c's header comment), and the
arm64 cross-build has no cross-compiled pipewire package wired up, so neither needs this.

## Hidden visibility for objects linked into the plugins

Context: `FX_EFFECTS := fx_airwindows_mod fx_airwindows_tone fx_nam fx_eq_filter fx_compressors fx_limiters fx...`

fx_chain.o is shared by synth_engine, hds.clap and hds_fx.clap: built -fPIC and with hidden
visibility so the two .clap files never interpose each other's fx_chain symbols in one host
(same reason as NAM_CXXFLAGS' -fvisibility=hidden; CLAP_EXPORT keeps clap_entry visible).
The FX bus: fx_chain.c (slot lifecycle + process_fx_chain) plus one object per effect family in
src/fx/. FX_CHAIN_OBJ is the whole list, for linking.

Context: `ENGINE_UNITS := json tuning smf_import sfz_parser osc_protocol sample_engine`

backend.o (the shared synth engine) is linked into synth_engine and hds.clap; same -fPIC/hidden
visibility reasoning as fx_chain.o.
The shared engine: backend.c plus the subsystems split out of it into src/core/. BACKEND_OBJ is
the whole list, for linking.

## Neural Amp Modeler (NAM) build

Context: `NAM_DIR := third_party/NeuralAmpModelerCore`

--- Neural Amp Modeler FX slot (FX_TYPE_NAM in backend.c) ---
NeuralAmpModelerCore is a C++/Eigen library with no C API -- src/fx/nam_bridge.cpp is the one
extern "C" shim backend.c calls into (see its own header comment), and this is the ONLY C++ in
HDS. Both Eigen and NeuralAmpModelerCore are vendored whole under third_party/ (not a system
package like PipeWire -- there is nothing to auto-detect, so unlike HDS_PIPEWIRE_ENABLED's
pkg-config probe, this is simply on by default. $(BIN)/$(CLAP_BIN) are therefore now built as
C objects compiled by $(CC) and C++ objects compiled by $(CXX), linked together by $(CXX) (the
only correct final link driver for a mixed C/C++ binary -- see $(BIN)/clap's own rules below).

Also wired into the `arm64` target (see its own comment block for ARM64_CXX/ARM64_NAM_OBJS) --
there never was a real "no C++ cross-compiler" problem, only a missing recipe: shell.nix's
aarch64.stdenv.cc already installs a full aarch64-unknown-linux-gnu-g++ alongside its own -gcc,
it just went unused. Eigen needs nothing extra either way (header-only, architecture-independent).

Context: `ARM64_NAM_BUILD_DIR := $(BUILD_DIR)/arm64-nam`

Defined here (not down by the arm64 target's own NAM pattern rules) specifically because the
shared $(BUILD_DIR) $(NAM_BUILD_DIR) $(ARM64_NAM_BUILD_DIR): directory rule below references it --
a := variable used in a target/prerequisite list expands to whatever value it holds AT THAT LINE,
not its final value, so defining this any later left that combined rule silently missing the
arm64-nam target entirely (confirmed directly: `make -n arm64` failed with "No rule to make
target 'build/arm64-nam/activations.o'" until this moved up here).

Context: `WIN_NAM_BUILD_DIR := $(BUILD_DIR)/win-nam`

Same up-front-definition reasoning as ARM64_NAM_BUILD_DIR just above -- referenced by the shared
directory-creation rule below.

Context: `NAM_CXXFLAGS := -O2 -std=c++20 -fPIC -fvisibility=hidden -fvisibility-inlines-hidden -DNAM_SAMPLE_FL...`

-fPIC unconditionally, not just for the `clap` target's shared-object link: these objects are
shared between $(BIN) (a plain executable) and $(CLAP_BIN) (a shared object, which REQUIRES
every object linked into it to be position-independent or the link fails outright) -- PIC code
runs correctly in a plain executable too (most distros build PIE executables by default anyway),
so there is no correctness cost to always compiling it this way, only the ability to reuse one
object set for both targets instead of building everything twice.

-DNAM_SAMPLE_FLOAT here is load-bearing, not cosmetic: NAM_SAMPLE (dsp.h) is `double` unless this
is defined, and every real DSP class's actual compiled `process(NAM_SAMPLE**, ...)` signature --
WaveNet::process, ContainerModel::process, the Conv1D/RingBuffer internals, all of it -- lives in
the OTHER 16 files below, none of which ever defined this themselves. src/fx/nam_bridge.cpp used to
be the ONLY translation unit that flipped NAM_SAMPLE to `float` (a local #define before its NAM
includes), which affects nothing outside that one .cpp -- so nam_bridge.cpp called through the
vtable believing every process() took `float**`, while the actual compiled code on the other side
of that same call (container.cpp/wavenet/model.cpp/etc., built with this flag absent) took
`double**`. Pointers themselves are the same width either way, so this silently linked and ran:
every real sample write then read/wrote 8 bytes through a pointer backed by only 4 real bytes,
quietly corrupting whatever memory sat next to it on every single call. A confirmed, real crash --
a live DAW session hit "Stack smashing detected" after loading a genuine multi-submodel .nam file
(see nam_bridge.cpp's own comment, and README.md's own bug narrative) -- traced with ASan straight
to this: a stack-buffer-overflow READ of 8 bytes out of a 4-byte float local, inside
WaveNet::_set_condition_array(). Defining it here, once, for every NAM_SRCS file (nam_bridge.cpp's
own copy is now redundant but harmless) is the actual fix: one real, consistent NAM_SAMPLE=float
across the whole vendored library, matching what nam_bridge.cpp always assumed on its own side.

-fvisibility=hidden -fvisibility-inlines-hidden: load-bearing, not an optimization. Without this,
every one of NeuralAmpModelerCore's hundreds of C++ symbols (nam::DSP, nam::get_dsp, the whole
ConfigParserRegistry singleton, ...) gets DEFAULT visibility -- exported into the DYNAMIC symbol
table of whichever .so links these objects. hds.clap and hds_fx.clap BOTH statically link the
exact same 17 NAM object files into their own separate .so, so both used to export IDENTICAL
symbol names into the process-wide dynamic symbol table. A DAW that loads both plugins into one
process (a real, required use case -- running several instances of both together) can then have
the dynamic linker bind one plugin's calls to the OTHER plugin's copy of that same symbol (classic
ELF symbol interposition) -- confirmed directly: `nm -D` on both .clap files showed the exact same
`nam::ConfigParserRegistry::instance()` guard variable and every other nam:: symbol exported
identically from each. Since that registry holds real, mutable, stateful data (not just code),
this could corrupt model loading across the two plugins or double-free it at unload -- a real,
confirmed DAW crash from loading both together. Hiding these means each .so keeps its own,
genuinely independent copy of the whole NAM library; nothing outside a .so ever needed to call
these directly anyway (nam_bridge.cpp's own extern "C" bridge functions, the only things a C file
calls, aren't affected -- hidden visibility only hides a symbol from OTHER .so's, never from code
in the SAME .so), and each plugin's own required export (clap_entry) lives in a plain C file this
flag never touches, so it stays fully visible exactly as CLAP hosts require.

Context: `NAM_OBJS := $(addprefix $(NAM_BUILD_DIR)/,$(notdir $(NAM_SRCS:.cpp=.o)))`

Flattened into one directory (no name collisions between NAM/, NAM/wavenet/, and src/ -- verified
by hand against the list above) so the three pattern rules below can all target it uniformly.

Context: `NAM_FLAGS_STAMP := $(NAM_BUILD_DIR)/.cxxflags.stamp`

Make's own dependency tracking is purely file-mtime-based -- it has no idea NAM_CXXFLAGS even
exists, let alone that it changed, so editing NAM_CXXFLAGS alone (e.g. adding a -D flag) leaves
every already-built build/nam/*.o completely undisturbed: none of their .cpp sources changed, so
a plain `make hds-fx`/`make clap`/`make` silently RELINKS the OLD objects, built under the OLD
flags, against whatever DID get recompiled. This is not hypothetical: it is exactly how the
NAM_SAMPLE float/double ABI mismatch fix (see NAM_CXXFLAGS's own comment on -DNAM_SAMPLE_FLOAT)
could silently fail to actually take effect for anyone who ran a plain rebuild instead of `make
clean` first -- same crash, same symptoms, with the "fixed" commit already checked out.
NAM_FLAGS_STAMP/ARM64_NAM_FLAGS_STAMP close that gap: a small file holding the exact flags string
last used to build that directory's objects, rewritten (touching its mtime) only when the string
actually differs from before -- so the three pattern rules below, which list it as a real
prerequisite, transparently rebuild every NAM object whenever NAM_CXXFLAGS itself changes, with
zero cost (no rewrite, no rebuild) on any ordinary build where it hasn't.

Context: `$(BIN_OBJ): $(SRC) src/core/backend.h src/core/backend_types.h src/core/hds_config.h src/core/record...`

backend.c/clap_plugin.c stay plain C, compiled by $(CC) exactly as before -- only the FINAL link
step (below) needs $(CXX), to pull in libstdc++ for the NAM objects linked alongside them.
src/core/backend.c and src/fx/fx_chain.c are listed explicitly (not just $(SRC)/$(CLAP_SRC)) because
every real source file #includes them rather than links against them -- without this, editing
either alone wouldn't trigger a rebuild of any object depending on it. src/core/sequencer.c is the
same story but ONLY for $(BIN_OBJ)/$(ARM64_BIN_OBJ): it's #include'd by hds_standalone.c alone
(never reachable from a CLAP plugin instance -- see its own header comment), so it's deliberately
left off $(CLAP_OBJ)'s own prerequisite list below.
HDS_STANDALONE_GUI_ENABLED=1 turns on hds_standalone.c's own native-GUI include chain (Cairo/X11 --
see that macro's own comment there for why it's a BUILD-time flag, not just the --gui runtime flag's
concern: this same file is also compiled for the aarch64 cross-build below, which has no Cairo/X11
cross-built at all). Native $(BIN_OBJ) always turns it on (this machine has Cairo/X11 via nix, same
as $(CLAP_OBJ) already does), so it needs the matching cflags and the same draw-file prerequisite
list $(CLAP_OBJ) has below (plus src/gui/hds_engine_instance.h and src/gui/hds_clap_global_draw.c -- both
real prerequisites of $(CLAP_OBJ) too; their absence there is a pre-existing gap this same pass
fixes) so editing any of them triggers a rebuild here as well.

Context: `$(NAM_BUILD_DIR)/%.o: $(NAM_DIR)/NAM/%.cpp $(NAM_FLAGS_STAMP) | $(NAM_BUILD_DIR)`

Three source directories (NAM/, NAM/wavenet/, src/) flattened into one object directory -- see
NAM_OBJS's own comment on why this is safe (no basename collisions).

## Linux CLAP plugins

Context: `CLAP_INC := third_party/clap/include`

--- Linux CLAP plugin ---
Produces hds.clap (a plain ELF shared object under the required .clap extension -- real hosts
scan for it at ~/.clap/, /usr/lib/clap/, or $CLAP_PATH). #include's the whole of backend.c (see
src/targets/clap/clap_plugin.c's own header comment) -- backend.c is the shared DSP/OSC/state core ONLY,
with every ALSA/PipeWire-touching line long since moved out to src/targets/standalone/hds_standalone.c (see that
file's own header comment), so this build never sees alsa/asoundlib.h or any device-I/O symbol
at all, exactly as a plugin whose host owns the actual audio device should.

Context: `HDS_FX_SRC := src/targets/fx_clap/hds_fx_plugin.c`

--- HDS FX: standalone, synth-free CLAP effects plugin (make hds-fx) ---
Produces hds_fx.clap. Unlike $(CLAP_BIN) above, this does NOT #include backend.c at all -- see
src/targets/fx_clap/hds_fx_plugin.c's own header comment for why (no OSC/networking, no synth, just src/fx/fx_types.h
+ src/fx/fx_chain.c, the same FX DSP backend.c's own embedded FX bus already runs). NAM (FX_TYPE_NAM)
IS enabled here (HDS_NAM_ENABLED 1 in hds_fx_plugin.c itself, same nam_bridge.h/.cpp C++ bridge
$(CLAP_BIN) already links), so this target needs the same $(NAM_INC)/$(NAM_OBJS)/$(CXX) final-
link treatment $(BIN)/$(CLAP_BIN) already get below, not a plain $(CC) link. It ALSO needs Cairo +
X11 (src/platform/x11/hds_fx_gui_x11.c -- the real embedded GUI, see that file's own header comment): both are
pulled in via pkg-config rather than hardcoded flags, same posture as the PipeWire auto-detection
above.

Context: `GUI_SHARED_TABS := play combi filters lfos envelopes global`

Synth GUI (hds_clap_*_draw.c). Six tabs are identical in both hosts and built once; the four
that read HDS_STANDALONE_GUI_ENABLED (it changes their view-state layouts and pulls in
standalone-only device/recording code) are built once per host. See src/gui/hds_clap_gui.h.

## Raspberry Pi 5 / aarch64 cross-build (make arm64)

Context: `ARM64_CC ?= aarch64-unknown-linux-gnu-gcc`

--- Raspberry Pi 5 / aarch64 Linux cross-build (make arm64) ---
Cross-compiles synth_engine (the backend engine only -- no GUI client for this target) for
aarch64 Linux (a Raspberry Pi 5, or any other aarch64 Linux host) from this x86_64 machine --
for the common case where the Pi itself runs plain Raspberry Pi OS rather than NixOS, so
`nix-shell` isn't available to build directly on the device: build here instead, then copy the
resulting binary over. Must be run from inside `nix-shell` at the repo root (see shell.nix's own
aarch64 section) -- ARM64_CC defaults to the prefixed cross-compiler name nix-shell puts on PATH,
and ARM64_CFLAGS/ARM64_LDFLAGS come from environment variables shell.nix's shellHook exports
(cross-built libraries live under content-addressed Nix store paths, so they can't be hardcoded
here). Every dependency here -- alsa-lib, liblo -- is already a real nixpkgs package for this
exact target, so no separate deps-building script is needed.

Deploying the output binary -- confirmed directly by inspecting a real built binary's own ELF
headers, not assumed. Two separate Nix-specific paths get baked in by the cross linker, neither
of which exists on the Pi:
1. An RPATH pointing at this build machine's own /nix/store/... library directories.
2. The ELF interpreter itself (PT_INTERP) -- literally the dynamic loader the kernel execve()s
to start the program -- set to this build machine's own /nix/store/...-glibc.../ld-linux-
aarch64.so.1. This is the more consequential of the two: a missing RPATH entry just means
the loader falls through to the next search path, but an interpreter path that doesn't
exist means the kernel can't start the binary AT ALL, and most shells report that as
"No such file or directory" -- easily (and reasonably) mistaken for the binary itself being
missing, even though `ls`/`file` show it's right there.
The `arm64` rule runs `patchelf --set-rpath '$$ORIGIN/$(ARM64_LIBDIR)' --set-interpreter
$(ARM64_INTERP)` on its output as a last step: `/lib/ld-linux-aarch64.so.1` is the standard,
well-known path glibc-based aarch64 Debian/Ubuntu/Raspberry Pi OS systems already have their own
real dynamic loader installed at, and the $ORIGIN-relative RPATH points at $(ARM64_LIBDIR)/
(copied alongside the binary by this same rule -- see that variable's own comment) instead of
this build machine's own /nix/store/... paths.

$(ARM64_LIBDIR) bundles liblo only -- a pure protocol library with no external config files or
hardware coupling, confirmed safe to ship as a plain .so file (WavPack was bundled here too
before it was disabled -- see src/targets/standalone/hds_standalone.c's HDS_WAVPACK_ENABLED). alsa-lib is deliberately
NOT bundled -- `strings` on the cross-built libasound.so.2 shows its compiled-in default config
directory is THIS BUILD MACHINE's own Nix store path (where alsa.conf, card definitions, and
dmix/plug setups live). That directory won't exist on the Pi, and even if it did, it would
describe the wrong hardware -- the Pi's own apt-installed alsa-lib is compiled with the correct
/usr/share/alsa path and is what actually knows how to talk to the Pi's real sound chip:
`sudo apt-get install libasound2` on the Pi -- "cross compiles but the Pi complains about
missing files" after confirming the interpreter/RPATH fix above almost always means this step
was skipped, not a build problem. One more real caveat, not yet hit but worth stating plainly:
this cross toolchain's glibc (2.39, from nixpkgs nixos-24.05) needs to be no newer than the
Pi's own installed glibc, or the binary will refuse to start with a GLIBC_2.39-not-found error
-- check the Pi's glibc version (`ldd --version`) against this if a binary that has the right
interpreter and libraries still won't run.

Context: `ARM64_CXX ?= aarch64-unknown-linux-gnu-g++`

Already installed by shell.nix's aarch64.stdenv.cc alongside ARM64_CC above (same package
installs both under this prefix) -- used only for the NAM objects and the arm64 target's own
final link (see NAM's own comment on why $(CXX)/$(ARM64_CXX), not the C compiler, has to be the
final link driver for a mixed C/C++ binary).

Context: `ARM64_LIBDIR := arm64-libs`

Where liblo gets copied to, alongside the binary, so the Pi needs no apt-get for it (see above
for why alsa-lib itself is excluded from this bundle on purpose). Copy this directory to the Pi
together with the binary, keeping them in the same relative layout -- the RPATH below is
$ORIGIN-relative, i.e. "next to me".

Context: `ARM64_NAM_OBJS := $(addprefix $(ARM64_NAM_BUILD_DIR)/,$(notdir $(NAM_SRCS:.cpp=.o)))`

arm64's own NAM object set: SEPARATE .o files from $(NAM_OBJS) above (wrong ISA to reuse, even
though the flags are identical -- NAM_CXXFLAGS needs nothing arm64-specific since NAM_INC is just
header paths, architecture-independent either way), built by $(ARM64_CXX) into their own
directory so a `make` and a `make arm64` in the same tree never fight over the same .o files.
(ARM64_NAM_BUILD_DIR itself is defined up with NAM_BUILD_DIR near the top of this file, not here
-- see that definition's own comment for why.)

Context: `$(ARM64_BIN_OBJ): $(SRC) src/core/backend.h src/core/backend_types.h src/core/hds_config.h src/core/...`

hds_standalone.c itself stays plain C, compiled by $(ARM64_CC) exactly like the native
$(BIN_OBJ) rule above -- only the FINAL link step needs $(ARM64_CXX), to pull in libstdc++ for
the NAM objects linked alongside it, same reasoning as $(BIN)/clap's own rules.

## Windows CLAP plugins (make clap-windows / make hds-fx-windows)

Context: `WIN_CC ?= x86_64-w64-mingw32-gcc`

--- Windows CLAP plugins (cross-built from Linux, both hds.clap and hds_fx.clap) ---
Produces hds-windows.clap and hds_fx-windows.clap: real PE32+ DLLs exporting clap_entry (the
CLAP_EXPORT macro in third_party/clap/include/clap/private/macros.h already emits
__attribute__((dllexport)) under _WIN32 automatically -- nothing to change there). Same source
files as the Linux targets (clap_plugin.c/hds_fx_plugin.c), each including their own
#ifdef _WIN32 platform-shim swap (hds_clap_gui_win32.c/hds_fx_gui_win32.c in place of the X11
ones -- see either file's own header comment) -- so this section is purely new toolchain/link
plumbing, no third source-file variant of either plugin.

What's cross-built into $(WIN_DEPS) (third_party/win64-deps/, NOT checked into git -- see
tools/build_windows_deps.sh) and why:
- Cairo + pixman: no mingw-w64 package exists anywhere for either (checked apt, and upstream
ships no Windows cross build) -- built from source via meson. Cairo is configured with
-Dpng=disabled -Dfreetype=disabled -Dfontconfig=disabled -Dxlib=disabled -Dxcb=disabled: on
host_machine.system()=='windows' it auto-enables CAIRO_HAS_WIN32_SURFACE and
CAIRO_HAS_WIN32_FONT (native GDI fonts, gdi32/msimg32 only -- both ship with mingw-w64's own
import libraries) -- so freetype/fontconfig/libpng are never needed at all: nothing in this
GUI's own production code (hds_clap_gui_draw.c/hds_fx_gui_draw.c/either platform shim) ever
calls cairo_surface_write_to_png either, only this repo's own Linux-only screenshot test
scaffolding does. pixman is cairo's own genuinely-required low-level compositing dependency
-- no way around building it.
- liblo: cross-built via its own configure/make (autoreconf -fiv first -- ships only
configure.ac, needs autoconf/automake/libtool/gettext to generate a real ./configure).
Detects winsock2 automatically and links -lws2_32 -liphlpapi on its own.
- zlib: NOT built from source -- Ubuntu's own libz-mingw-w64-dev package already provides it
(apt-get install libz-mingw-w64-dev), the one real mingw-w64-cross-built dependency that
exists pre-packaged.

WIN_CC/WIN_CXX default to the standard mingw-w64 (POSIX threading model -- required for real
pthread_create/pthread_join support, since backend.c's own sample_disk_thread uses it; the
win32-threading-model variant mingw-w64 also ships silently lacks a real pthread implementation)
cross-compiler names `nix-shell`/apt both put on PATH once mingw-w64/g++-mingw-w64-x86-64 are
installed -- see shell.nix's own Windows section for exactly what it adds.

-static (not just -shared, which the final .clap link ALSO needs -- both together is a real,
correct mingw combination): statically links libgcc/libstdc++/libwinpthread INTO the DLL itself.
Without it, the DLL depends on libwinpthread-1.dll/libstdc++-6.dll/libgcc_s_seh-1.dll at load
time -- confirmed directly (a plain non-static test binary refused to start under Wine with
"Library libwinpthread-1.dll ... not found") -- none of which a real DAW's own installation
ships, so a real end user's Windows machine would refuse to load the plugin at all without
separately shipping those three DLLs alongside it. Static linking avoids that entirely: the
resulting .clap file is fully self-contained.

Context: `WIN_EXTRA_CFLAGS ?=`

apt's own gcc-mingw-w64-x86-64-posix package bundles a real pthread.h/libpthread.a straight into
its own cross sysroot (/usr/x86_64-w64-mingw32/{include,lib}), already on the default search path
with nothing extra needed here. nixpkgs' pkgsCross.mingwW64 does NOT: it splits winpthreads into
its own separate derivation (windows.mingw_w64_pthreads, confirmed directly against nixpkgs' own
pkgs/os-specific/windows/mingw-w64/pthreads.nix/default.nix -- NOT the same thing as nixpkgs' own
`windows.pthreads`, which is the unrelated, legacy pthreads-win32/pthread-w32 project and won't
match mingw-w64's own ABI), which the base mingw cross stdenv.cc never searches on its own --
confirmed directly: `#include <pthread.h>` in backend.c fails outright under a plain
`pkgsCross.mingwW64.stdenv.cc` with no extra include path. WIN_EXTRA_CFLAGS/WIN_PTHREAD_LDLIBS
exist so shell.nix's own Windows section (see there for the exact nixpkgs paths/library name)
can supply exactly that gap without this Makefile hardcoding a Nix store path itself; both stay
empty/default (a no-op) for the apt case, which needs nothing extra.

Context: `WIN_EXTRA_LDLIBS ?=`

nixpkgs' pkgsCross.mingwW64 gcc/g++ also unconditionally bakes a `-lmcfgthread` reference into its
own link spec (confirmed directly: visible as a `-plugin-opt=-pass-through=-lmcfgthread` in its
own collect2 invocation, regardless of whether the program even uses threads) -- nixpkgs' own
"mcf" gcc threading model, a completely different thing from the -lpthread/winpthreads story above
(that one is about backend.c's own explicit pthread_create/pthread_join calls; this one is the
compiler's own runtime requiring a library apt's posix-threaded gcc never needs at all, since it
doesn't use the mcf threading model). nixpkgs' own mcfgthread derivation ships ONLY
libmcfgthread.dll.a (an import library) + the actual runtime libmcfgthread-1.dll -- no true static
libmcfgthread.a at all, and unlike a plain missing static lib, this isn't a packaging gap: upstream
mcfgthread's own Makefile.am gives it a custom linker entry point
(-Wl,--entry,__MCF_dll_startup@@Z, -nostdlib, freestanding) that fully replaces normal CRT startup
-- it is BY DESIGN only ever loadable as its own standalone DLL, statically merging its objects into
another DLL would never invoke that startup logic and silently corrupt its own init/cleanup (a real
correctness bug, confirmed by reading mcfgthread's own upstream source, not assumed). So
WIN_EXTRA_LDLIBS/WIN_EXTRA_RUNTIME_DLLS below are a genuine, irreducible exception, not a
workaround to later remove: shell.nix supplies mcfgthread's real -L/-l here (needs no
-Wl,-Bdynamic/-Bstatic bracket at all -- see WIN_LDFLAGS's own comment on why this Makefile no
longer uses a blanket -static, which is what made bracketing necessary in an earlier version of
this file), and WIN_EXTRA_RUNTIME_DLLS copies the resulting DLL dependency next to the built .clap.
Both stay empty/default (a no-op) for the apt case, which needs neither and stays a genuinely
single-file-self-contained .clap.

Context: `WIN_CFLAGS := -O2 -Wall -Wextra -std=gnu11 -Isrc -DCAIRO_WIN32_STATIC_BUILD -I$(WIN_DEPS)/include -I...`

-DCAIRO_WIN32_STATIC_BUILD: cairo.h's own cairo_public macro defaults to __declspec(dllimport)
under _WIN32 unless this is defined -- correct for consuming cairo as a real system DLL, wrong
for our own statically-linked libcairo.a (produced every symbol as a plain, non-imported
definition), and without it the linker looks for __imp_cairo_* import-thunk symbols that were
never generated at all (confirmed directly: this is exactly the undefined-reference error a first
link attempt without this flag produces).

Context: `WIN_LDFLAGS := -static-libgcc -static-libstdc++ -L$(WIN_DEPS)/lib`

No blanket -static: on nixpkgs' own mingw cross gcc it forces plain -Bstatic across the ENTIRE
link, including gcc's own automatically-appended default-library tail (which mentions -lmcfgthread
TWICE more on top of our own explicit one below, un-interruptible by anything on our own command
line) -- confirmed directly, by hand, against nixpkgs' own real toolchain (not apt's): under a
blanket -static, mcfgthread's import library is unreachable no matter how it's bracketed, because
every one of ld's -Bstatic/-Bdynamic mode changes on our own command line is undone again by the
time gcc's own tail is reached, and a plain, unbracketed -lmcfgthread there always resolves in
whatever mode was last set. Dropping the blanket flag instead and reaching for the SPECIFIC,
dedicated tools each static-linking goal actually needs works cleanly with no such conflict:
-static-libgcc/-static-libstdc++ substitute direct paths to the real static libgcc.a/libstdc++.a
(immune to -Bstatic/-Bdynamic entirely, unlike a plain -lstdc++ -- confirmed: without a
competing blanket -static, these actually take effect, where they silently didn't when -static was
also present), and an explicit -Wl,-Bstatic/-Wl,-Bdynamic bracket around JUST
$(WIN_PTHREAD_LDLIBS) forces winpthread specifically static. mcfgthread itself is then linked with
no bracket at all, naturally resolving via its import library under ld's own default (dynamic)
mode -- exactly what it needs anyway. Verified end-to-end against a real nix-shell (not guessed):
the resulting DLL only imports from libmcfgthread-1.dll plus the same unavoidable Windows system
DLLs (KERNEL32/USER32/GDI32/msvcrt/WS2_32/IPHLPAPI) any Windows program needs regardless of how
it's linked -- no libstdc++-6.dll, no libwinpthread-1.dll. cairo/pixman/liblo are unaffected by
dropping the blanket flag: tools/build_windows_deps.sh builds them with no shared-library variant
at all, so there is no competing dynamic candidate for ld to prefer either way.

Context: `WIN_NAM_OBJS := $(addprefix $(WIN_NAM_BUILD_DIR)/,$(notdir $(NAM_SRCS:.cpp=.o)))`

Windows' own NAM object set: SEPARATE .o files from $(NAM_OBJS)/$(ARM64_NAM_OBJS) (wrong ABI/ISA
to reuse -- PE32+ vs ELF, plus the mingw name-mangling target), same NAM_CXXFLAGS otherwise
(architecture/OS-independent: just header search paths and the NAM_SAMPLE_FLOAT/visibility
fixes -- see NAM_CXXFLAGS's own comment).

## Android (currently disabled)

Android backend test (android/README.md): a real signed APK, built by hand with aapt2/javac/d8/
apksigner against the android SDK/NDK shell.nix's own androidSdk binding puts on PATH -- NOT a
compiled binary this Makefile tracks as a dependency the way every other target above is (no
.o/.c rule graph), since the actual work is cross-language (C -> .so, Java -> .dex, both zipped
together) and tools/build_android_manual.sh already encodes that whole pipeline -- this target
is just this project's usual `make <target>` entry point for it. Always re-runs (FORCE,
.PHONY) rather than tracking $(ANDROID_APK) as a real target: the script's own APK output
timestamp isn't a reliable signal for "needs rebuilding" the way a .o's mtime is, since it
depends on source files, the Java sources, AND the manifest all at once.
ANDROID DISABLED (experimental platform; its shell.nix SDK is commented out too):
android: FORCE
	NIXPKGS_ACCEPT_ANDROID_SDK_LICENSE=1 NIXPKGS_ALLOW_UNFREE=1 ./tools/build_android_manual.sh
