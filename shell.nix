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
{ pkgs ? import <nixpkgs> {} }:

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
  # aarch64-linux, confirmed directly against this same nixpkgs channel: alsa-lib/liblo/wavpack/
  # the cross gcc wrapper itself all come straight from cache.nixos.org (seconds, no local build);
  # glfw's own X11+Wayland+Mesa dependency chain is a real, legitimate nixpkgs cross derivation but
  # is NOT fully cached for this target as of this nixpkgs channel, so a first `nix-shell` entry
  # that touches `make pi-touch-arm64` will need to compile a genuine chunk of that chain (openssl,
  # curl, wayland, wayland-protocols, libxkbcommon, the X11 libs, mesa) from source -- a one-time,
  # if slow, cost, not a sign anything is misconfigured. `make arm64` (the backend alone, no GLFW)
  # is unaffected and builds in seconds once alsa-lib/liblo/wavpack are in the Nix store.
  aarch64 = pkgs.pkgsCross.aarch64-multiplatform;
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
                              # NOTE: on a non-NixOS host (this project's usual dev machine target
                              # per README), OpenGL apps built inside nix-shell sometimes can't find
                              # the system's real GPU driver through nixpkgs' own libGL stub -- if
                              # `./hds_pi_touch` fails to create a GL context from inside this shell
                              # but runs fine built outside it, wrap it with `nixGL` (a small,
                              # well-known community tool for exactly this non-NixOS-OpenGL
                              # mismatch) rather than fighting nixpkgs' Mesa packaging directly.

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
    aarch64.glfw
    aarch64.libGL
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

    # Cross-built aarch64 libraries live under content-addressed Nix store paths, not a fixed
    # relative directory like $(WIN_DEPS) -- export their include/lib paths here so the Makefile's
    # ARM64_CFLAGS/ARM64_LDFLAGS (referenced the same way WIN_CFLAGS/WIN_LDFLAGS already are) can
    # find them. ARM64_CC/ARM64_CXX are NOT exported here -- aarch64.stdenv.cc's prefixed binary
    # names (aarch64-unknown-linux-gnu-gcc/-g++) are already on PATH via buildInputs above, exactly
    # like WIN_CC's x86_64-w64-mingw32-gcc, so the Makefile's own `?=` default covers it.
    export ARM64_CFLAGS="-I${aarch64.alsa-lib.dev}/include -I${aarch64.liblo}/include -I${aarch64.wavpack.dev}/include -I${aarch64.glfw}/include"
    export ARM64_LDFLAGS="-L${aarch64.alsa-lib}/lib -L${aarch64.liblo}/lib -L${aarch64.wavpack}/lib -L${aarch64.glfw}/lib -L${aarch64.libGL}/lib"

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
