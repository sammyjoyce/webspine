{
  description = "webspine, a rendered documentation site to EPUB CLI";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    hegel-cpp = {
      url = "github:hegeldev/hegel-cpp/v0.13.0?dir=nix";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs =
    { self, nixpkgs, hegel-cpp }:
    let
      # Chromium in nixpkgs and the pipe-based DevTools transport are Linux-only.
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      # Builds against the consumer's nixpkgs; the test suite needs hegel-cpp and stays in checks.
      overlays.default = final: _prev: {
        webspine = final.callPackage ./nix/package.nix { };
      };

      packages = forAllSystems (pkgs: {
        default = self.packages.${pkgs.stdenv.hostPlatform.system}.webspine;
        webspine = pkgs.callPackage ./nix/package.nix { };
      });

      apps = forAllSystems (pkgs: {
        default = {
          type = "app";
          program = nixpkgs.lib.getExe self.packages.${pkgs.stdenv.hostPlatform.system}.default;
          meta.description = "Turn a documentation site into a validated EPUB";
        };
      });

      checks = forAllSystems (pkgs: {
        default = pkgs.callPackage ./nix/package.nix {
          hegelSource = hegel-cpp.sourceInfo.outPath;
          libhegel = hegel-cpp.packages.${pkgs.stdenv.hostPlatform.system}.libhegel;
        };
      });

      devShells = forAllSystems (
        pkgs:
        let
          tested = self.checks.${pkgs.stdenv.hostPlatform.system}.default;
        in
        {
          default = pkgs.mkShell {
            inputsFrom = [ tested ];
            packages = tested.passthru.runtimeTools ++ [ pkgs.clang-tools ];
            inherit (tested) HEGEL_CPP_SOURCE HEGEL_LIBHEGEL_LIBRARY FONTCONFIG_FILE;
          };
        }
      );
    };
}
