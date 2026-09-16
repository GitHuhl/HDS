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
  ];

  shellHook = ''
    echo "HDS dev shell -- native Linux build, CLAP plugin, Pi-touch client, Python frontend,"
    echo "and Windows cross-build toolchain all ready."
    echo
    echo "  make            -> ./synth_engine"
    echo "  make clap       -> ./hds.clap"
    echo "  make pi-touch   -> ./hds_pi_touch"
    echo "  tools/build_windows_deps.sh  (once)  then  make windows / make clap-windows"
    echo

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
