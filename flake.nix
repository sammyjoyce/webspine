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
    {
      self,
      nixpkgs,
      hegel-cpp,
    }:
    let
      # hegel-cpp publishes no prebuilt engine for x86_64-darwin, and nixpkgs has no Chromium there.
      systems = [
        "x86_64-linux"
        "aarch64-linux"
        "aarch64-darwin"
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
          # Darwin's strict Nix sandbox blocks loopback sockets and the macOS
          # services Chromium needs. CI runs e2e outside the build sandbox.
          buildE2eTests = !pkgs.stdenv.hostPlatform.isDarwin;
          # hegel-cpp builds libhegel with stdenvNoCC, whose fixDarwinDylibNames hook
          # needs install_name_tool from cctools but gets no toolchain on PATH.
          libhegel = hegel-cpp.packages.${pkgs.stdenv.hostPlatform.system}.libhegel.overrideAttrs (old: {
            nativeBuildInputs = old.nativeBuildInputs ++ nixpkgs.lib.optional pkgs.stdenv.hostPlatform.isDarwin pkgs.cctools;
          });
        };
        format =
          pkgs.runCommand "webspine-format-check"
            {
              src = nixpkgs.lib.fileset.toSource {
                root = ./.;
                fileset = nixpkgs.lib.fileset.unions [
                  ./.clang-format
                  (nixpkgs.lib.fileset.fileFilter (file: file.hasExt "cpp" || file.hasExt "hpp") ./.)
                ];
              };
              nativeBuildInputs = [ pkgs.clang-tools ];
            }
            ''
              cd $src
              find . \( -name '*.cpp' -o -name '*.hpp' \) -print0 | xargs -0 clang-format --dry-run --Werror
              touch $out
            '';
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
            env = tested.passthru.runtimeEnv // {
              inherit (tested) HEGEL_CPP_SOURCE HEGEL_LIBHEGEL_LIBRARY;
            };
          };
        }
      );
    };
}
