## Comprehensive nix-shell environment for this project -- one `nix-shell` at the repo root gives
## you everything needed for every build target documented in README.md's "Build & run" section:
##
##   make                  -> ./synth_engine        (native Linux backend, ALSA)
##   make clap             -> ./hds.clap            (Linux CLAP plugin)
##   make hds-fx           -> ./hds_fx.clap         (standalone multi-FX CLAP plugin -- needs Cairo +
##                                                    X11, see pkgs.cairo/pkgs.xorg.libX11 below)
##   make arm64            -> synth_engine-arm64    (aarch64 Linux, e.g. Raspberry Pi 5)
##
##   make clap-windows     -> hds-windows.clap      (Windows CLAP plugin, cross-built from Linux --
##   make hds-fx-windows   -> hds_fx-windows.clap    see the Makefile's own "Windows CLAP plugins"
##                                                    section; run tools/build_windows_deps.sh once
##                                                    first to cross-build Cairo/pixman/liblo into
##                                                    third_party/win64-deps/)
##
##   make android          -> android/app/build/manual/hds-backend-test.apk (a real signed APK,
##                                                    built by hand with aapt2/javac/d8/apksigner
##                                                    against the androidSdk binding below -- see
##                                                    android/README.md for the full story and
##                                                    NIXPKGS_ACCEPT_ANDROID_SDK_LICENSE/
##                                                    NIXPKGS_ALLOW_UNFREE below for why both must
##                                                    be exported before even entering this shell)
##
## Scope note: the STANDALONE Windows build (synth_engine.exe, a second full audio/MIDI platform
## backend -- WASAPI alongside ALSA) is still dropped for good and never coming back -- that's a
## real, ongoing maintenance cost this project won't take on. The Windows CLAP plugins above are a
## DIFFERENT thing entirely, resurrected in a later pass: clap_plugin.c/hds_fx_plugin.c never touch
## audio/MIDI device code at all (a CLAP host owns that, supplying buffers/note-events instead --
## see clap_plugin.c's own header comment), so cross-building them needed no WASAPI/WinMM code and
## never did even the first time a Windows CLAP build existed, before this project's native
## Xlib/Cairo GUI existed to need porting too (see src/hds_clap_gui_win32.c/hds_fx_gui_win32.c's own
## header comments for that GUI-porting story). The ImGui/GLFW touch client (hds_pi_touch) is gone
## for good too -- the Pi target is backend-only now (synth_engine-arm64, no GUI client for that
## target). See git history if that one's ever revived.
## WavPack recording support is enabled (HDS_WAVPACK_ENABLED in src/hds_standalone.c) --
## pkgs.wavpack/aarch64.wavpack in buildInputs below is what actually provides it; nothing else in
## this file needed to change when it was turned back on.
## `pkgs` is pinned to a fixed nixpkgs-24.05 release tarball (URL + content hash below) rather
## than the unpinned `<nixpkgs>` this file used to use. Root-caused directly: two `nix-shell`
## entries against unpinned `<nixpkgs>` -- one on this build machine, one on a developer's own
## machine, at two different times -- resolved two genuinely different nixpkgs revisions (whatever
## each machine's own `nix-channel` state happened to be), which cross-built liblo/alsa-lib at
## different upstream VERSIONS with different SONAMEs (confirmed: one such mismatch produced a
## binary whose NEEDED entry was `liblo.so.3` while a separately-built `arm64-libs/` bundle only
## shipped `liblo.so.7` -- two builds of the "same" cross target from two unpinned nix-shell
## entries, silently incompatible with each other). A fixed nixpkgs revision makes `nix-shell`
## fully reproducible -- the exact same dependency versions (and SONAMEs) every time, on every
## machine -- which is what makes it safe to ever build `synth_engine-arm64` and `arm64-libs/` in
## two SEPARATE `make arm64` runs (e.g. once here, once later on the user's own machine) and still
## have them match. Still: always copy a binary and its OWN freshly-built `arm64-libs/` to the Pi
## together, from the same `make arm64` run, rather than mixing an old bundle with a new binary --
## this pin closes the cross-machine/cross-time drift risk, not the "I copied stale files" risk.
{ pkgs ? import (builtins.fetchTarball {
    url = "https://releases.nixos.org/nixos/24.05/nixos-24.05.7376.b134951a4c9f/nixexprs.tar.xz";
    sha256 = "1f8j7fh0nl4qmqlxn6lis8zf7dnckm6jri4rwmj0qm1qivhr58lv";
  }) {} }:

let
  # pkgsCross.aarch64-multiplatform: a real aarch64-unknown-linux-gnu cross toolchain, for
  # cross-building synth_engine (the backend engine only -- no GUI client for this target) for a
  # Raspberry Pi 5 (or any other aarch64 Linux host) from this x86_64 machine -- for the common
  # case where the Pi itself runs plain Raspberry Pi OS, not NixOS, so `nix-shell` isn't available
  # to build directly on the device. Every dependency this target needs -- alsa-lib, liblo --
  # already exists as a real nixpkgs package for aarch64-linux, straight from cache.nixos.org
  # (seconds, no local build).
  aarch64 = pkgs.pkgsCross.aarch64-multiplatform;

  # pkgsCross.mingwW64: a real x86_64-w64-mingw32 cross toolchain, for `make clap-windows`/
  # `make hds-fx-windows`. This port's own actual cross-build/link/runtime verification (a real
  # Wine-run GUI click-test, not just "it compiles") used a plain apt-get mingw-w64 install instead
  # (documented in tools/build_windows_deps.sh's own header comment) -- the gap that leaves in THIS
  # file specifically was real, not hypothetical: a first real nix-shell build hit `pthread.h: No
  # such file or directory` compiling backend.c, because (confirmed directly against nixpkgs' own
  # pkgs/os-specific/windows/mingw-w64/{pthreads,default}.nix source, not guessed) nixpkgs splits
  # winpthreads into its own separate derivation, `windows.mingw_w64_pthreads`, that
  # `pkgsCross.mingwW64.stdenv.cc` never searches on its own -- unlike apt's own
  # gcc-mingw-w64-x86-64-posix package, which bundles a real pthread.h/libpthread.a straight into
  # its own cross sysroot with nothing extra needed. (`windows.pthreads` -- note, singular, no
  # `mingw_w64_` prefix -- is a DIFFERENT, unrelated legacy package, the old pthreads-win32/
  # pthread-w32 project; it does not match mingw-w64's own ABI and must not be used here.) Fixed
  # below via WIN_EXTRA_CFLAGS/WIN_PTHREAD_LDLIBS (see the Makefile's own comment on why those two
  # exist as an overridable seam rather than a hardcoded Nix store path baked into the Makefile
  # itself) -- still not independently re-verified end-to-end from inside this port's own dev
  # environment (no Nix available here to actually run `nix-shell` and try it), so if the exact
  # library name below (`-lwinpthread`, matching upstream mingw-w64/winpthreads' own standard
  # output name) turns out not to match what this derivation actually produces, that's the one
  # line to fix.
  mingwPthreads = pkgs.pkgsCross.mingwW64.windows.mingw_w64_pthreads;
  # nixpkgs' own mingw cross gcc/g++ (mingw.stdenv.cc below) unconditionally bakes a `-lmcfgthread`
  # reference into its own link spec regardless of whether the program even uses threads (confirmed
  # directly: visible as `-plugin-opt=-pass-through=-lmcfgthread` in its own collect2 invocation) --
  # a real, live link failure ("cannot find -lmcfgthread") once pthread.h/cairo.h/wine were all fixed
  # and the build got as far as the actual final link step. This is nixpkgs' own "mcf" gcc threading
  # model, a completely different thing from mingwPthreads/winpthreads above (that one is backend.c's
  # own explicit pthread_create/pthread_join calls; this one is the compiler's own runtime, needed
  # even for a program with zero threading code) -- confirmed against nixpkgs' own
  # pkgs/os-specific/windows/{default,mcfgthreads/default}.nix (windows.mcfgthreads, "out"+"dev"
  # outputs). apt's own posix-threaded gcc never needs this at all, since it doesn't use the mcf
  # threading model in the first place.
  mingwMcfgthread = pkgs.pkgsCross.mingwW64.windows.mcfgthreads;
  mingw = pkgs.pkgsCross.mingwW64;

  # android/nix/android-sdk.nix is the single source of truth for this (also used standalone by
  # tools/build_android_manual.sh, when run outside this shell -- see that script's own comment on
  # why it still falls back to calling nix-build itself in that case). Passing THIS file's own
  # `pkgs` through via `{ inherit pkgs; }` instead of letting that file re-import its own pinned
  # nixpkgs tarball a second time -- same nixpkgs revision either way (identical pin, see that
  # file's own header comment), just one fetch/eval instead of two.
  #
  # NIXPKGS_ACCEPT_ANDROID_SDK_LICENSE=1 and NIXPKGS_ALLOW_UNFREE=1 are NOT set here -- they're
  # read from the environment at `pkgs` eval time (both are nixpkgs' own standard mechanism, not
  # something this file invents), which means they must already be exported in the shell BEFORE
  # running `nix-shell` itself, same as nixpkgs expects for any unfree/licensed package. The
  # Makefile's own `android` target sets both for you; entering this shell directly (not via
  # `make android`) needs them exported by hand first.
  androidSdk = import ./android/nix/android-sdk.nix { inherit pkgs; };
  androidNdkVersion = "26.1.10909125";
  androidBuildToolsVersion = "34.0.0";
in
pkgs.mkShell {
  buildInputs = [
    # --- Native Linux build (make / make clap) ---
    # gcc/gnumake are already part of mkShell's default stdenv toolchain -- listed explicitly
    # anyway, matching this file's own existing convention of naming every tool it relies on
    # rather than depending on stdenv's implicit defaults staying the way they are.
    pkgs.gcc
    pkgs.gnumake
    pkgs.alsa-lib          # backend.c's audio I/O (ALSA) -- headers + lib in one nixpkgs derivation
    pkgs.liblo              # the OSC control protocol (backend.c's embedded control surface)
    pkgs.wavpack             # WavPack recording (Recording tab) -- see HDS_WAVPACK_ENABLED in
                              # src/hds_standalone.c and -lwavpack in the Makefile's own LDLIBS.
    pkgs.pipewire            # backend.c's OPTIONAL native PipeWire backend (--backend pipewire,
                              # alongside the always-present ALSA backend above -- see
                              # HDS_PIPEWIRE_ENABLED in src/backend.c). The Makefile auto-detects
                              # this via `pkg-config --exists libpipewire-0.3` at build time, so
                              # just having it on PATH here is what turns the feature on -- nothing
                              # else in this file needs to change for that.
    pkgs.pkg-config          # the Makefile's own PipeWire auto-detection needs `pkg-config` itself
                              # on PATH -- not previously required here since every other dependency
                              # (alsa-lib, liblo) is linked via hardcoded -l flags instead. Also what
                              # `make hds-fx` uses to find Cairo/X11 below (see HDS_FX_CFLAGS/
                              # HDS_FX_LDLIBS in the Makefile).

    # --- HDS FX's own real embedded GUI (make hds-fx) --- src/hds_fx_gui_draw.c (Cairo drawing)
    # + src/hds_fx_gui_x11.c (the X11 window/XEmbed/event-pump shim, see that file's own header
    # comment). Found via `pkg-config --cflags/--libs cairo x11` in the Makefile, same
    # auto-detection posture as the PipeWire block above -- just needs both on PATH here.
    pkgs.cairo
    pkgs.xorg.libX11

    # --- Raspberry Pi 5 / aarch64 Linux cross-build (make arm64) ---
    # aarch64.stdenv.cc installs its binaries under the aarch64-unknown-linux-gnu- prefix (gcc,
    # g++, ar, ...) -- no bare `gcc`/`g++` names, so this can sit directly on PATH alongside the
    # native toolchain with zero collision risk. The Makefile's ARM64_CC references this prefixed
    # name directly (see its own comment).
    aarch64.stdenv.cc
    aarch64.alsa-lib
    aarch64.liblo
    aarch64.wavpack   # NOT currently linked for this arm64 (Raspberry Pi) target specifically --
                      # ARM64_LDFLAGS in the Makefile has no -lwavpack, and arm64-libs/ has no
                      # libwavpack.so bundled for the Pi's own runtime yet. The native Linux build
                      # (pkgs.wavpack above) IS linked -- see its own comment. Left here so wiring
                      # up the Pi target later needs no shell.nix change, just the Makefile/bundling
                      # work (mirroring how liblo is already cross-built/bundled for it).
    pkgs.patchelf   # rewrites the Nix store RPATH/interpreter the cross linker bakes into arm64 --
                     # those store paths don't exist on the Pi. The Makefile's arm64 rule runs
                     # `patchelf --set-rpath '$ORIGIN/arm64-libs' --set-interpreter
                     # /lib/ld-linux-aarch64.so.1` on the output as a final step (see the
                     # Makefile's own comment for the full story -- a wrong/missing interpreter is
                     # what actually stops the binary from starting at all, not just a missing
                     # RPATH entry).

    # --- Windows CLAP plugins (make clap-windows / make hds-fx-windows) ---
    # mingw.stdenv.cc puts x86_64-w64-mingw32-gcc/-g++/-ar/-windres on PATH (the exact names both
    # the Makefile's WIN_CC/WIN_CXX defaults and tools/build_windows_deps.sh's own cross-file
    # expect). No mingw-w64-cross-built Cairo package exists anywhere (checked; upstream ships none
    # either) -- meson/ninja/the autotools chain below are what
    # tools/build_windows_deps.sh uses to cross-build Cairo's own real dependency (pixman) and
    # liblo from source into third_party/win64-deps/ (run that script once per checkout; see its
    # own header comment for exactly what it builds and why). zlib is the one real dependency that
    # doesn't need this -- Ubuntu's own libz-mingw-w64-dev package covers it directly outside Nix;
    # nixpkgs' own mingwW64 cross zlib would work identically if this shell is the only environment
    # ever used, but the deps script's own apt-based path was what this port's own real
    # cross-build/link/Wine-runtime verification actually used, so it's documented as the primary
    # route rather than assumed interchangeable without re-checking.
    mingw.stdenv.cc
    # mingwPthreads/mingwMcfgthread are DELIBERATELY NOT listed here -- a real, live regression
    # confirmed directly: simply listing a plain library/header package (as opposed to
    # mingw.stdenv.cc, a cross-compiler wrapper package) in buildInputs makes nixpkgs' own generic
    # setup hooks auto-inject its /include into NIX_CFLAGS_COMPILE for EVERY compiler invocation in
    # this whole shell, not just the mingw cross target -- including plain native `make clap`/`make`,
    # which then picked up mingwPthreads' own Windows-targeted pthread.h (which itself
    # `#include`s <process.h>, a Windows-only header) instead of glibc's real one, breaking the
    # native Linux build entirely ("fatal error: process.h: No such file or directory" compiling
    # backend.c with plain `gcc`, no mingw cross compiler involved at all). Both packages are still
    # fully available via the `${mingwPthreads}`/`${mingwMcfgthread.out}` interpolations in the
    # shellHook below regardless of buildInputs membership (a Nix string interpolation forces/
    # resolves a derivation on its own, independent of whether it's ALSO listed here) -- WIN_EXTRA_
    # CFLAGS/WIN_PTHREAD_LDLIBS/WIN_EXTRA_LDLIBS already pass their exact include/lib paths
    # explicitly, so the mingw cross-build needs nothing else from them also being visible
    # ambiently.
    pkgs.meson
    pkgs.ninja
    pkgs.autoconf
    pkgs.automake
    pkgs.libtool
    pkgs.gettext   # autopoint (liblo's own ./configure needs regenerating -- see
                     # tools/build_windows_deps.sh's own liblo step) lives in this package
    pkgs.wine64     # optional, for actually running/click-testing a built .clap under Wine +a
                     # virtual X display locally, the same way this port's own verification did --
                     # never required just to CROSS-COMPILE the plugin.
  ];

  shellHook = ''
    echo "HDS dev shell -- native Linux build, CLAP plugin, and Raspberry Pi 5"
    echo "backend cross-build all ready."
    echo
    echo "  make            -> ./synth_engine   (pkg-config auto-detects PipeWire -- run with"
    echo "                                       --backend pipewire once built to use it)"
    echo "  make clap       -> ./hds.clap"
    echo "  make hds-fx     -> ./hds_fx.clap    (standalone multi-FX CLAP plugin, real embedded GUI)"
    echo "  make arm64      -> synth_engine-arm64 (aarch64 Linux backend, e.g. Raspberry Pi 5)"
    echo
    echo "  Windows CLAP plugins (cross-built from Linux): run tools/build_windows_deps.sh ONCE"
    echo "  per checkout first (cross-builds Cairo/pixman/liblo into third_party/win64-deps/), then:"
    echo "    make clap-windows    -> hds-windows.clap"
    echo "    make hds-fx-windows  -> hds_fx-windows.clap"
    echo
    echo "  make android    -> android/app/build/manual/hds-backend-test.apk (see android/README.md)"
    echo

    # pkgsCross.mingwW64.stdenv.cc alone has no pthread.h/libwinpthread on its search path (see the
    # mingwPthreads binding's own comment above for why -- confirmed against nixpkgs' own source) --
    # unlike the apt-based posix-threading gcc-mingw-w64 package, which needs no equivalent override
    # at all. Library name (-lwinpthread) matches upstream mingw-w64/winpthreads' own standard output
    # name. This whole nix-shell path (this line through WIN_EXTRA_RUNTIME_DLLS below) has now been
    # independently verified end-to-end against a REAL nix-shell entry, byte-for-byte the same
    # nixpkgs-24.05 pin this file itself uses (not guessed, not just read from nixpkgs source) --
    # `make clap-windows`/`make hds-fx-windows` both succeed and the resulting DLL's own import table
    # was checked directly with `objdump -p`.
    export WIN_EXTRA_CFLAGS="-I${mingwPthreads}/include"
    export WIN_PTHREAD_LDLIBS="-L${mingwPthreads}/lib -lwinpthread"
    # nixpkgs' own mcfgthread derivation ships no true static .a at all -- only libmcfgthread.dll.a
    # (import lib) + the real libmcfgthread-N.dll (confirmed directly: `find` on the real store path
    # lists exactly a .la, a .dll.a, and a bin/*.dll, nothing else) -- and this is not a packaging
    # gap to work around: upstream mcfgthread's own Makefile.am gives it a custom linker entry point
    # (--entry,__MCF_dll_startup, -nostdlib) that fully replaces normal DLL/CRT startup, meaning it
    # is BY DESIGN only ever loadable as its own standalone DLL -- statically merging its objects into
    # another DLL would never invoke that startup logic at all, silently breaking its own init/cleanup
    # (a real correctness bug, not just a link nuisance). No -Wl,-Bdynamic/-Bstatic bracket needed
    # here at all: the Makefile's own WIN_LDFLAGS no longer uses a blanket -static (see its own
    # comment for exactly why that made mcfgthread unresolvable no matter how it was bracketed, once
    # gcc's own automatically-appended default-library tail is taken into account) -- ld's own default
    # (dynamic) mode already finds mcfgthread's import library correctly on its own, exactly what it
    # needs anyway. WIN_EXTRA_RUNTIME_DLLS ships the resulting real runtime DLL dependency alongside
    # the built .clap (a glob rather than a hardcoded filename, since the "-N" version suffix on
    # libmcfgthread-N.dll is this package's own upstream soname-style versioning, not something to
    # pin here). Verified directly: the built DLL's own import table lists libmcfgthread-1.dll plus
    # only the same unavoidable Windows system DLLs any Windows program needs regardless of how it's
    # linked (KERNEL32/USER32/GDI32/msvcrt/WS2_32/IPHLPAPI) -- no libstdc++-6.dll, no
    # libwinpthread-1.dll; this really is the one remaining, irreducible DLL dependency.
    export WIN_EXTRA_LDLIBS="-L${mingwMcfgthread.out}/lib -lmcfgthread"
    export WIN_EXTRA_RUNTIME_DLLS="${mingwMcfgthread.out}/bin/*.dll"

    # tools/build_windows_deps.sh cross-builds pixman/cairo/liblo for the mingw target from source
    # (no such mingw-w64 packages exist anywhere -- see that script's own header comment), and needs
    # real upstream source tarballs to do it. Its original apt-get-source-based fetch route does not
    # work at all on this (or any NixOS/nix-shell) host -- there is no apt-get here. These three
    # point that script straight at the exact tarballs nixpkgs' own ordinary (non-cross) pixman/
    # cairo/liblo packages already fetch via fetchurl -- an already-hash-pinned, already-correct
    # upstream URL/version nixpkgs itself verified, so this needs no URL-guessing of its own and
    # tracks whatever pixman/cairo/liblo version this nixpkgs revision already carries.
    export HDS_WIN_PIXMAN_SRC="${pkgs.pixman.src}"
    export HDS_WIN_CAIRO_SRC="${pkgs.cairo.src}"
    export HDS_WIN_LIBLO_SRC="${pkgs.liblo.src}"

    # Cross-built aarch64 libraries live under content-addressed Nix store paths -- export their
    # include/lib paths here so the Makefile's ARM64_CFLAGS/ARM64_LDFLAGS can find them. ARM64_CC
    # is NOT exported here -- aarch64.stdenv.cc's prefixed binary name
    # (aarch64-unknown-linux-gnu-gcc) is already on PATH via buildInputs above, so the Makefile's
    # own `?=` default covers it. No wavpack include/lib path here -- WavPack recording is
    # disabled (see HDS_WAVPACK_ENABLED in src/backend.c), so nothing in this build actually
    # references libwavpack right now.
    export ARM64_CFLAGS="-I${aarch64.alsa-lib.dev}/include -I${aarch64.liblo}/include"
    export ARM64_LDFLAGS="-L${aarch64.alsa-lib}/lib -L${aarch64.liblo}/lib"

    # liblo is a pure protocol library -- no external config files, no hardware coupling -- safe
    # (and worth doing) to bundle straight alongside the output binary via a $ORIGIN-relative
    # RPATH, so the Pi needs no `apt-get install` for it (WavPack was bundled here too before it
    # was disabled -- see src/backend.c's HDS_WAVPACK_ENABLED). alsa-lib is deliberately NOT
    # bundled -- it's not just an ABI-compatible .so: `strings` on this cross-built libasound.so.2
    # shows its compiled-in default config directory is THIS BUILD MACHINE's own
    # /nix/store/...-alsa-lib-.../share/alsa path (where alsa.conf, card definitions, and
    # dmix/plug setups live). That directory won't exist on the Pi, and even if it did, it would
    # describe the WRONG hardware -- the Pi's own apt-installed alsa-lib is compiled with the
    # correct /usr/share/alsa path and is what actually knows how to talk to the Pi's real sound
    # chip. Bundling this one wouldn't just risk "missing files" the way liblo's absence would --
    # it risks the audio device silently behaving wrong. See the Makefile's own comment and the
    # README's apt-get instructions for what the Pi needs installed for alsa-lib instead.
    #
    # libstdc++/libgcc_s: needed now that the NAM FX slot's C++ objects are cross-compiled and
    # linked into synth_engine-arm64 too (see the Makefile's own ARM64_CXX comment). Bundled for
    # the exact same reason as the glibc-version note in the Makefile's own arm64 comment block --
    # this cross toolchain's libstdc++ needs to be no OLDER than what the binary's own
    # GLIBCXX_x.x.x-versioned symbol references need, and there's no reason to gamble on whatever
    # libstdc++6 happens to already be on a given Pi's own Raspberry Pi OS image when bundling the
    # matching one is this cheap. `stdenv.cc.cc` is nixpkgs' own name for the unwrapped compiler
    # package underneath the aarch64-unknown-linux-gnu-g++ wrapper on PATH above; `.lib` is its
    # output containing these two runtime shared libraries -- the standard nixpkgs convention for
    # this, though not independently re-verified against this specific pinned nixpkgs-24.05
    # revision the way the liblo/alsa-lib paths above were (no Nix available to re-check from
    # inside this port's own dev environment -- worth confirming with `ls` on the actual path
    # `nix-shell` resolves this to, the first time this target is built for real).
    export ARM64_BUNDLE_LIBS="${aarch64.liblo}/lib/liblo.so.7 ${aarch64.stdenv.cc.cc.lib}/lib/libstdc++.so.6 ${aarch64.stdenv.cc.cc.lib}/lib/libgcc_s.so.1"

    # Android (make android / tools/build_android_manual.sh): unlike every other buildInputs
    # entry above, androidSdk's own layout (composeAndroidPackages' output under
    # libexec/android-sdk/...) has no top-level bin/ for nixpkgs' generic setup hooks to find on
    # their own -- aapt2/d8/apksigner/zipalign live under build-tools/<ver>/, the NDK clang under
    # ndk/<ver>/toolchains/llvm/prebuilt/linux-x86_64/bin/ -- so both are added to PATH here
    # explicitly, same posture as ARM64_CFLAGS/WIN_EXTRA_CFLAGS above (an explicit path export,
    # not reliance on buildInputs' automatic PATH injection). ANDROID_SDK_ROOT/ANDROID_NDK_ROOT
    # are the same env var names tools/build_android_manual.sh checks for before falling back to
    # running nix-build itself -- see that script's own comment.
    export ANDROID_SDK_ROOT="${androidSdk}/libexec/android-sdk"
    export ANDROID_NDK_ROOT="$ANDROID_SDK_ROOT/ndk/${androidNdkVersion}"
    export PATH="$ANDROID_SDK_ROOT/build-tools/${androidBuildToolsVersion}:$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH"
  '';
}
