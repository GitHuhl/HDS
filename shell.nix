## Comprehensive nix-shell environment for this project -- one `nix-shell` at the repo root gives
## you everything needed for every build target documented in README.md's "Build & run" section:
##
##   make                  -> ./synth_engine        (native Linux backend, ALSA)
##   make clap             -> ./hds.clap            (Linux CLAP plugin)
##   make pi-touch         -> ./hds_pi_touch         (ImGui/GLFW touch client)
##   python3 src/frontend.py                        (PySide6 GUI -- see the auto-venv shellHook below)
##
##   nix-shell                            # once, to enter the shell
##   tools/build_windows_deps.sh          # once, to cross-build liblo/wavpack for Windows
##   make windows                         -> synth_engine.exe
##   make clap-windows                    -> hds-windows.clap
##
##   make arm64                           -> synth_engine-arm64  (aarch64 Linux, e.g. Raspberry Pi 5)
##   make pi-touch-arm64                  -> hds_pi_touch-arm64  (same target, touch client)
##
## This used to be scoped to the Windows cross-build alone (see git history) -- broadened to cover
## every target in one place since a project shell.nix that only covers a third of `make`'s targets
## invites exactly the "wait, why doesn't this build" confusion it exists to prevent. The tradeoff,
## stated plainly: this pulls in the full mingw-w64 cross toolchain even for someone who only ever
## runs `make`/`make clap`/`make pi-touch` -- a real but one-time download/build-closure cost, not a
## correctness problem, and simpler than maintaining several separate shell.nix entry points that
## could silently drift out of sync with each other.
## `pkgs` is pinned to a fixed nixpkgs-24.05 release tarball (URL + content hash below) rather
## than the unpinned `<nixpkgs>` this file used to use. Root-caused directly: two `nix-shell`
## entries against unpinned `<nixpkgs>` -- one on this build machine, one on a developer's own
## machine, at two different times -- resolved two genuinely different nixpkgs revisions (whatever
## each machine's own `nix-channel` state happened to be), which cross-built liblo/wavpack/
## alsa-lib/glfw at different upstream VERSIONS with different SONAMEs (confirmed: one such
## mismatch produced a binary whose NEEDED entry was `liblo.so.3` while a separately-built
## `arm64-libs/` bundle only shipped `liblo.so.7` -- two builds of the "same" cross target, from
## two unpinned nix-shell entries, silently incompatible with each other). A fixed nixpkgs
## revision makes `nix-shell` fully reproducible -- the exact same dependency versions (and
## SONAMEs) every time, on every machine -- which is what makes it safe to ever build
## `synth_engine-arm64` and `arm64-libs/` in two SEPARATE `make arm64` runs (e.g. once here, once
## later on the user's own machine) and still have them match. Still: always copy a binary and
## its OWN freshly-built `arm64-libs/` to the Pi together, from the same `make arm64` run, rather
## than mixing an old bundle with a new binary -- this pin closes the cross-machine/cross-time
## drift risk, not the "I copied stale files" risk.
{ pkgs ? import (builtins.fetchTarball {
    url = "https://releases.nixos.org/nixos/24.05/nixos-24.05.7376.b134951a4c9f/nixexprs.tar.xz";
    sha256 = "1f8j7fh0nl4qmqlxn6lis8zf7dnckm6jri4rwmj0qm1qivhr58lv";
  }) {} }:

let
  # pkgsCross.mingwW64's wrapped gcc is what actually provides the x86_64-w64-mingw32-gcc binary
  # name the Makefile's WIN_CC and tools/build_windows_deps.sh both hardcode (nixpkgs' cross
  # stdenv names cross compilers by their target triple, matching the same convention Debian's
  # mingw-w64 package uses) -- so nothing in either of those files needs to change to work here.
  mingw = pkgs.pkgsCross.mingwW64;
  # pkgsCross.aarch64-multiplatform: a real aarch64-unknown-linux-gnu cross toolchain, for
  # cross-building synth_engine/hds_pi_touch for a Raspberry Pi 5 (or any other aarch64 Linux
  # host) from this x86_64 machine -- for the common case where the Pi itself runs plain Raspberry
  # Pi OS, not NixOS, so `nix-shell` isn't available to build directly on the device. Unlike the
  # Windows cross-build above (which needs tools/build_windows_deps.sh to cross-build liblo/
  # wavpack from source, since mingw builds of them aren't in nixpkgs), every dependency this
  # target needs -- alsa-lib, liblo, wavpack, glfw -- already exists as a real nixpkgs package for
  # aarch64-linux: alsa-lib/liblo/wavpack/the cross gcc wrapper itself all come straight from
  # cache.nixos.org (seconds, no local build). `make arm64` (the backend alone, no GLFW) is
  # unaffected either way and builds in seconds.
  aarch64 = pkgs.pkgsCross.aarch64-multiplatform;

  # aarch64's plain `glfw` package unconditionally cross-builds Wayland support (nixpkgs'
  # glfw/3.x.nix has no toggle for this -- it's ON by default on every non-Darwin Unix, per
  # upstream GLFW's own CMakeLists.txt). Two real, confirmed problems with that, not
  # hypothetical ones -- hit directly building `hds_pi_touch-arm64` for real:
  #   1. nixpkgs' cross-splicing of `wayland-scanner` (a NativeBuildInput -- a tool that must run
  #      on THIS build machine, x86_64, to generate C headers from Wayland's XML protocol specs
  #      at build time) is broken for this target: it resolves to an aarch64-CROSS-BUILT
  #      wayland-scanner instead of a native x86_64 one, which the build then tries to *execute*
  #      on this x86_64 machine and fails outright with `cannot execute binary file: Exec format
  #      error` -- a real nixpkgs packaging bug for this specific package/cross-target
  #      combination, not a local misconfiguration.
  #   2. Even before hitting that crash, cross-building Wayland's own dependency chain
  #      (wayland-protocols, libxkbcommon, and what they pull in) triggered a genuine, multi-hour
  #      from-source build (confirmed by directly watching it: openssl, curl, sqlite, then all of
  #      LLVM 17.0.6) -- NOT because of `libGL` (nixpkgs' `libGL` is a lightweight `libglvnd`
  #      dispatch stub, confirmed by reading its own derivation -- not full Mesa at all), but
  #      because of the Wayland deps specifically.
  # Fix: override glfw with `-DGLFW_BUILD_WAYLAND=OFF` (a real, independent upstream CMake option
  # -- confirmed against GLFW 3.4's own src/CMakeLists.txt that every `wayland-scanner`
  # invocation is gated behind `if (GLFW_BUILD_WAYLAND)`, so this fully skips the broken codegen
  # step) and drop wayland/wayland-protocols/libxkbcommon/wayland-scanner from the derivation's
  # own build inputs entirely, so Nix never even tries to build that chain. X11 support
  # (`GLFW_BUILD_X11`, a separate, independent option) is untouched and stays on -- confirmed this
  # rebuilds in under two minutes with zero LLVM/Mesa involvement, and Raspberry Pi OS's Wayland
  # desktop (labwc, the Bookworm-and-later default) already runs X11-only apps transparently via
  # XWayland, so this is a real, low-cost trade, not a functionality loss for the Pi's own touch
  # display.
  aarch64Glfw = aarch64.glfw.overrideAttrs (old: {
    cmakeFlags = (old.cmakeFlags or [ ]) ++ [ "-DGLFW_BUILD_WAYLAND=OFF" ];
    nativeBuildInputs = pkgs.lib.subtractLists [ aarch64.wayland-scanner ] (old.nativeBuildInputs or [ ]);
    buildInputs = pkgs.lib.subtractLists
      [ aarch64.wayland aarch64.wayland-protocols aarch64.libxkbcommon ]
      (old.buildInputs or [ ]);
  });
in
pkgs.mkShell {
  buildInputs = [
    # --- Native Linux build (make / make clap) ---
    # gcc/gnumake are already part of mkShell's default stdenv toolchain -- listed explicitly
    # anyway, matching this file's own existing convention of naming every tool it relies on
    # rather than depending on stdenv's implicit defaults staying the way they are.
    pkgs.gcc
    pkgs.gnumake
    pkgs.pkg-config       # only pi-touch's Makefile target actually invokes pkg-config (for
                           # glfw3's cflags/libs) -- backend.c/clap_plugin.c link -lasound/-llo/
                           # -lwavpack directly via hardcoded LDLIBS, no pkg-config lookup needed
                           # for those, but nix's cc-wrapper still needs the matching dev packages
                           # below on its search path either way.
    pkgs.alsa-lib          # backend.c's audio I/O (ALSA) -- headers + lib in one nixpkgs derivation
    pkgs.liblo              # the OSC control protocol, both backend.c and pi_touch_frontend.cpp
    pkgs.wavpack             # Recording tab's WavPack format option

    # --- Pi/touchscreen client (make pi-touch) -- see src/pi_touch_frontend.cpp's own header
    # comment for what this target actually builds. Dear ImGui itself is vendored into
    # third_party/imgui/ (not a nixpkgs dependency) -- only its own system deps are needed here.
    pkgs.glfw               # nixpkgs attribute is `glfw`, but its pkg-config module is still
                             # named `glfw3` (upstream's own naming, unchanged across distros) --
                             # matches exactly what the Makefile's pi-touch target looks up.
    pkgs.libGL               # -lGL at link time; GLVND dispatches to the real driver at runtime.
    pkgs.wayland             # NOT linked into hds_pi_touch at all (no DT_NEEDED entry) -- GLFW's
                              # own Wayland backend is loaded via dlopen("libwayland-client.so") at
                              # RUNTIME, by design, so ONE glfw build can support multiple backends.
                              # Confirmed directly on a real NixOS/Plasma-Wayland machine: that
                              # dlopen() call fails ("Wayland: Failed to load libwayland-client")
                              # purely because NixOS keeps every package's libraries in an isolated
                              # Nix store path -- there is no traditional "/usr/lib" a bare-filename
                              # dlopen() can just find things in, so the library needs to be
                              # explicitly put on LD_LIBRARY_PATH (see the shellHook below) even
                              # though it's already present on the very same Wayland desktop the
                              # app is running on. Nix-shell is the only supported way to build/run
                              # this project's targets -- this is fixed inside it, not by leaving it.

                              # NOTE, separately: also confirmed directly on the same NixOS machine
                              # -- a nix-shell-built hds_pi_touch can link against nixpkgs' own
                              # isolated libglvnd/Mesa instead of the system's real, properly-
                              # configured GPU driver, so GL context creation can fail outright (GLX
                              # reports zero usable framebuffer configs) even though the same
                              # system's driver stack works fine for every other GL app. Also fixed
                              # via LD_LIBRARY_PATH below (NixOS's own published real-driver
                              # location, /run/opengl-driver/lib) rather than leaving nix-shell.

    # --- Python frontend (python3 src/frontend.py) --- PySide6/python-osc's exact pinned
    # versions (see requirements.txt) aren't reliably both present/current in every nixpkgs
    # channel, so this shell provides just the interpreter + pip and lets the shellHook below
    # manage a local .venv/ from requirements.txt directly -- the same versions anyone running
    # `pip install -r requirements.txt` outside Nix would get, rather than whatever nixpkgs
    # happens to carry this week.
    pkgs.python3
    pkgs.python3Packages.pip
    pkgs.python3Packages.virtualenv

    # --- Windows cross-build (make windows / make clap-windows) ---
    mingw.stdenv.cc      # x86_64-w64-mingw32-gcc / -g++ / -ar / -windres
    pkgs.curl             # fetches the liblo/WavPack release tarballs
    pkgs.gnutar
    pkgs.gzip
    pkgs.bzip2
    # Both liblo (SourceForge) and WavPack (wavpack.com) are fetched as real "make dist" release
    # tarballs that already ship a working ./configure -- no autoconf/automake/libtool/gettext
    # bootstrap needed (an earlier version of this file provided those for bootstrapping WavPack's
    # raw GitHub source snapshot instead; that path turned out to be a real rabbit hole of
    # mismatched-gettext-version autotools errors and was dropped in favor of the real tarball).

    # --- Raspberry Pi 5 / aarch64 Linux cross-build (make arm64 / make pi-touch-arm64) ---
    # aarch64.stdenv.cc installs its binaries under the aarch64-unknown-linux-gnu- prefix (gcc,
    # g++, ar, ...), exactly like mingw.stdenv.cc above uses the x86_64-w64-mingw32- prefix -- no
    # bare `gcc`/`g++` names, so this can sit directly on PATH alongside the native and mingw
    # toolchains with zero collision risk. The Makefile's ARM64_CC/ARM64_CXX reference these
    # prefixed names directly (see its own comment), matching WIN_CC's exact convention.
    aarch64.stdenv.cc
    aarch64.alsa-lib
    aarch64.liblo
    aarch64.wavpack
    aarch64Glfw
    aarch64.libGL
    pkgs.patchelf   # rewrites the Nix store RPATH/interpreter the cross linker bakes into
                     # arm64/pi-touch-arm64 -- those store paths don't exist on the Pi. The
                     # Makefile's arm64/pi-touch-arm64 rules run `patchelf --set-rpath
                     # '$ORIGIN/arm64-libs' --set-interpreter /lib/ld-linux-aarch64.so.1` on the
                     # output as a final step (see the Makefile's own comment for the full story --
                     # a wrong/missing interpreter is what actually stops the binary from starting
                     # at all, not just a missing RPATH entry).
  ];

  shellHook = ''
    echo "HDS dev shell -- native Linux build, CLAP plugin, Pi-touch client, Python frontend,"
    echo "and Windows cross-build toolchain all ready."
    echo
    echo "  make            -> ./synth_engine"
    echo "  make clap       -> ./hds.clap"
    echo "  make pi-touch   -> ./hds_pi_touch"
    echo "  tools/build_windows_deps.sh  (once)  then  make windows / make clap-windows"
    echo "  make arm64 / make pi-touch-arm64  -> synth_engine-arm64 / hds_pi_touch-arm64 (aarch64 Linux, e.g. Pi 5)"
    echo

    # hds_pi_touch (native, this machine's own architecture -- NOT the aarch64 cross-build below)
    # needs two libraries findable by bare-filename dlopen() at RUNTIME, neither of which shows up
    # as a link-time NEEDED dependency: GLFW's own Wayland backend (libwayland-client.so, see
    # pkgs.wayland's own comment above) and, separately, a real GLX/EGL driver vendor library for
    # GL context creation (nixpkgs' own isolated libglvnd/Mesa can fail to find the system's real
    # one). Both are fixed the same way: put the real library directory on LD_LIBRARY_PATH ahead of
    # anything else, every time this shell is entered, so `make pi-touch && ./hds_pi_touch` just
    # works with no manual per-invocation environment juggling ever needed again.
    # `/run/opengl-driver/lib` is NixOS's own published real-driver location (harmless to prepend
    # even when it doesn't exist, e.g. on a non-NixOS host running this same shell.nix -- an absent
    # directory on LD_LIBRARY_PATH is silently skipped by the dynamic loader, not an error).
    export LD_LIBRARY_PATH="/run/opengl-driver/lib:${pkgs.wayland}/lib:$LD_LIBRARY_PATH"

    # Cross-built aarch64 libraries live under content-addressed Nix store paths, not a fixed
    # relative directory like $(WIN_DEPS) -- export their include/lib paths here so the Makefile's
    # ARM64_CFLAGS/ARM64_LDFLAGS (referenced the same way WIN_CFLAGS/WIN_LDFLAGS already are) can
    # find them. ARM64_CC/ARM64_CXX are NOT exported here -- aarch64.stdenv.cc's prefixed binary
    # names (aarch64-unknown-linux-gnu-gcc/-g++) are already on PATH via buildInputs above, exactly
    # like WIN_CC's x86_64-w64-mingw32-gcc, so the Makefile's own `?=` default covers it.
    export ARM64_CFLAGS="-I${aarch64.alsa-lib.dev}/include -I${aarch64.liblo}/include -I${aarch64.wavpack.dev}/include -I${aarch64Glfw}/include"
    export ARM64_LDFLAGS="-L${aarch64.alsa-lib}/lib -L${aarch64.liblo}/lib -L${aarch64.wavpack}/lib -L${aarch64Glfw}/lib -L${aarch64.libGL}/lib"

    # liblo/wavpack are pure protocol/codec libraries -- no external config files, no hardware
    # coupling -- safe (and worth doing) to bundle straight alongside the output binary via a
    # $ORIGIN-relative RPATH, so the Pi needs no `apt-get install` for these two. alsa-lib/libGL/
    # glfw are deliberately NOT bundled, for two different reasons confirmed by direct inspection
    # rather than assumed:
    #   - alsa-lib is NOT just an ABI-compatible .so -- `strings` on this cross-built
    #     libasound.so.2 shows its compiled-in default config directory is THIS BUILD MACHINE's
    #     own /nix/store/...-alsa-lib-.../share/alsa path (where alsa.conf, card definitions, and
    #     dmix/plug setups live). That directory won't exist on the Pi, and even if it did, it
    #     would describe the WRONG hardware -- the Pi's own apt-installed alsa-lib is compiled
    #     with the correct /usr/share/alsa path and is what actually knows how to talk to the
    #     Pi's real sound chip. Bundling this one wouldn't just risk "missing files" the way
    #     liblo/wavpack's absence would -- it risks the audio device silently behaving wrong.
    #   - libGL must dispatch to whatever driver actually matches the Pi's real GPU/kernel DRM
    #     setup, which a generically cross-built Mesa never can -- bundling it risks shipping the
    #     wrong driver, not just an unnecessary one.
    # See the Makefile's own comment and the README's apt-get instructions for what the Pi needs
    # installed for these three instead.
    export ARM64_BUNDLE_LIBS="${aarch64.liblo}/lib/liblo.so.7 ${aarch64.wavpack}/lib/libwavpack.so.1"

    # Auto-provision a local venv for the PySide6 frontend, matching requirements.txt exactly --
    # created once, reused (and only re-synced when requirements.txt actually changes) on every
    # later `nix-shell` entry. Kept out of Nix's own package set deliberately -- see the buildInputs
    # comment above.
    if [ ! -d .venv ]; then
      echo "Creating .venv/ for src/frontend.py's dependencies (PySide6, numpy, python-osc)..."
      python3 -m venv .venv
    fi
    if [ requirements.txt -nt .venv/.requirements-installed ] 2>/dev/null || [ ! -f .venv/.requirements-installed ]; then
      if .venv/bin/pip install --quiet -r requirements.txt; then
        touch .venv/.requirements-installed
      else
        echo "WARNING: pip install failed (see above) -- src/frontend.py's deps are NOT ready." >&2
        echo "         Fix your network/pip config and re-enter this shell to retry." >&2
      fi
    fi
    export PATH="$PWD/.venv/bin:$PATH"
    if [ -f .venv/.requirements-installed ]; then
      echo "  python3 src/frontend.py   (.venv/ activated on PATH -- PySide6 etc. ready)"
    fi
  '';
}
