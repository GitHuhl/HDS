{
  # `nix run github:GitHuhl/HDS` (or `nix run .` in a checkout) builds and starts the HDS
  # standalone synth (synth_engine) from this revision. Pass arguments after `--`, e.g.
  #   nix run github:GitHuhl/HDS -- --gui
  # Development builds still use `nix-shell` + `make` (see docs/building.md); this flake only
  # packages the native Linux standalone.
  description = "HDS standalone synth";

  # The exact nixpkgs tarball shell.nix pins, so `nix run` and `nix-shell` build against the same
  # library versions. Keep the two URLs in step when bumping either.
  inputs.nixpkgs = {
    url = "https://releases.nixos.org/nixos/24.05/nixos-24.05.7376.b134951a4c9f/nixexprs.tar.xz";
    flake = false;
  };

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      lib = import "${nixpkgs}/lib";
      forAll = f: lib.genAttrs systems (system: f (import nixpkgs { inherit system; }));
    in {
      packages = forAll (pkgs: rec {
        hds = pkgs.stdenv.mkDerivation {
          pname = "hds";
          version = self.shortRev or self.dirtyShortRev or "dev";
          src = self;

          nativeBuildInputs = [ pkgs.pkg-config pkgs.makeWrapper ];
          buildInputs = [
            pkgs.alsa-lib pkgs.liblo pkgs.wavpack pkgs.pipewire
            pkgs.cairo pkgs.xorg.libX11
          ];

          enableParallelBuilding = true;
          buildFlags = [ "synth_engine" ];
          # The Makefile strips with -s; leave stripping to it.
          dontStrip = true;

          installPhase = ''
            runHook preInstall
            install -Dm755 synth_engine $out/bin/synth_engine
            # The GUI draws text with Cairo's "sans-serif" through fontconfig. Outside NixOS a
            # Nix-built fontconfig may find no fonts at all, so point it at DejaVu unless the
            # user already set FONTCONFIG_FILE.
            makeWrapper $out/bin/synth_engine $out/bin/hds \
              --set-default FONTCONFIG_FILE ${pkgs.makeFontsConf { fontDirectories = [ pkgs.dejavu_fonts ]; }}
            runHook postInstall
          '';

          meta = {
            description = "HDS standalone synth engine (ALSA/PipeWire, optional --gui)";
            mainProgram = "hds";
            platforms = systems;
          };
        };
        default = hds;
      });

      apps = forAll (pkgs: rec {
        hds = { type = "app"; program = "${self.packages.${pkgs.system}.hds}/bin/hds"; };
        default = hds;
      });
    };
}
