## Nix-shell environment for cross-building the Windows targets (`make windows`, `make
## clap-windows`) on NixOS/Nix, where there's no apt/dnf/pacman to `apt-get install mingw-w64`
## from. Run `nix-shell` at the repo root, then inside it:
##
##   tools/build_windows_deps.sh   # once, to cross-build liblo/wavpack into third_party/win64-deps/
##   make windows                  # -> synth_engine.exe
##   make clap-windows             # -> hds-windows.clap
##
## This shell is scoped to the Windows cross-build only -- the native Linux build (`make`,
## `make clap`) and the Python frontend need their own separate ALSA/liblo/wavpack/PySide6
## dependencies, which this file doesn't attempt to provide (use your normal NixOS system
## packages / a separate python nix-shell for those, same as any other Linux dev environment).
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
    mingw.stdenv.cc      # x86_64-w64-mingw32-gcc / -g++ / -ar / -windres
    pkgs.gnumake
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
    echo "HDS Windows cross-build shell -- mingw-w64 toolchain ready."
    echo "Run: tools/build_windows_deps.sh   (once)  then  make windows / make clap-windows"
  '';
}
