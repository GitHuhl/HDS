## Comprehensive nix-shell environment for this project -- one `nix-shell` at the repo root gives
## you everything needed for every build target documented in README.md's "Build & run" section:
##
##   make                  -> ./synth_engine        (native Linux backend, ALSA)
##   make clap             -> ./hds.clap            (Linux CLAP plugin)
##   python3 src/frontend.py                        (PySide6 GUI -- see the auto-venv shellHook below)
##
##   make arm64            -> synth_engine-arm64    (aarch64 Linux, e.g. Raspberry Pi 5)
##
## Scope note: Windows support has been dropped entirely (standalone synth_engine.exe was already
## gone; the Windows CLAP cross-build followed it -- this project will never target Windows again,
## given the real, ongoing cost of maintaining a second audio backend, WASAPI vs. ALSA). The
## ImGui/GLFW touch client (hds_pi_touch) is also gone -- the Pi target is backend-only now
## (synth_engine-arm64, no GUI client for that target). See git history if either is ever revived.
## WavPack recording support is disabled for now too (commented out via HDS_WAVPACK_ENABLED in
## src/backend.c, not deleted) -- pkgs.wavpack/aarch64.wavpack are left in buildInputs below since
## re-enabling it needs nothing from this file to change, only that one flag flipped back.
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
    pkgs.wavpack             # NOT currently linked -- WavPack recording is disabled (see
                              # HDS_WAVPACK_ENABLED in src/backend.c); left here so re-enabling it
                              # needs no shell.nix change, just that one flag flipped back.

    # --- Python frontend (python3 src/frontend.py) --- PySide6/python-osc's exact pinned
    # versions (see requirements.txt) aren't reliably both present/current in every nixpkgs
    # channel, so this shell provides just the interpreter + pip and lets the shellHook below
    # manage a local .venv/ from requirements.txt directly -- the same versions anyone running
    # `pip install -r requirements.txt` outside Nix would get, rather than whatever nixpkgs
    # happens to carry this week.
    pkgs.python3
    pkgs.python3Packages.pip
    pkgs.python3Packages.virtualenv

    # --- Raspberry Pi 5 / aarch64 Linux cross-build (make arm64) ---
    # aarch64.stdenv.cc installs its binaries under the aarch64-unknown-linux-gnu- prefix (gcc,
    # g++, ar, ...) -- no bare `gcc`/`g++` names, so this can sit directly on PATH alongside the
    # native toolchain with zero collision risk. The Makefile's ARM64_CC references this prefixed
    # name directly (see its own comment).
    aarch64.stdenv.cc
    aarch64.alsa-lib
    aarch64.liblo
    aarch64.wavpack   # NOT currently linked (see pkgs.wavpack's own comment above)
    pkgs.patchelf   # rewrites the Nix store RPATH/interpreter the cross linker bakes into arm64 --
                     # those store paths don't exist on the Pi. The Makefile's arm64 rule runs
                     # `patchelf --set-rpath '$ORIGIN/arm64-libs' --set-interpreter
                     # /lib/ld-linux-aarch64.so.1` on the output as a final step (see the
                     # Makefile's own comment for the full story -- a wrong/missing interpreter is
                     # what actually stops the binary from starting at all, not just a missing
                     # RPATH entry).
  ];

  shellHook = ''
    echo "HDS dev shell -- native Linux build, CLAP plugin, Python frontend, and Raspberry Pi 5"
    echo "backend cross-build all ready."
    echo
    echo "  make            -> ./synth_engine"
    echo "  make clap       -> ./hds.clap"
    echo "  make arm64      -> synth_engine-arm64 (aarch64 Linux backend, e.g. Raspberry Pi 5)"
    echo

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
    export ARM64_BUNDLE_LIBS="${aarch64.liblo}/lib/liblo.so.7"

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
