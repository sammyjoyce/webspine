{
  description = "docs2epub, a rendered documentation site to EPUB CLI";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
    hegel-cpp = {
      url = "github:hegeldev/hegel-cpp/v0.13.0?dir=nix";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs = { self, nixpkgs, flake-utils, hegel-cpp }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        lib = pkgs.lib;
        libhegel = hegel-cpp.packages.${system}.libhegel;
        hegelEnv = {
          HEGEL_CPP_SOURCE = "${hegel-cpp.sourceInfo.outPath}";
          HEGEL_LIBHEGEL_LIBRARY = "${libhegel}/lib/libhegel_c.so";
        };
        fontsConf = pkgs.makeFontsConf { fontDirectories = [ pkgs.dejavu_fonts ]; };
        runtimeTools = [ pkgs.chromium pkgs.epubcheck ];
        buildTools = [ pkgs.cmake pkgs.ninja pkgs.pkg-config pkgs.makeWrapper ];
        libraries = with pkgs; [
          cli11
          curl
          gtest
          libxml2
          libzip
          nlohmann_json
          openssl
          vips
        ];
        app = pkgs.stdenv.mkDerivation (hegelEnv // {
          pname = "docs2epub";
          version = "0.1.0";
          src = lib.fileset.toSource {
            root = ./.;
            fileset = lib.fileset.unions [ ./CMakeLists.txt ./src ./tests ];
          };
          nativeBuildInputs = buildTools;
          buildInputs = libraries;
          nativeCheckInputs = runtimeTools;
          doCheck = true;
          FONTCONFIG_FILE = fontsConf;
          preCheck = ''
            export HOME=$TMPDIR
          '';
          postFixup = ''
            wrapProgram $out/bin/docs2epub \
              --set-default FONTCONFIG_FILE ${fontsConf} \
              --prefix PATH : ${lib.makeBinPath runtimeTools}
          '';
        });
      in {
        packages.default = app;
        apps.default = {
          type = "app";
          program = "${app}/bin/docs2epub";
        };
        devShells.default = pkgs.mkShell (hegelEnv // {
          packages = buildTools ++ libraries ++ runtimeTools ++ [ pkgs.clang-tools ];
          FONTCONFIG_FILE = fontsConf;
        });
        checks.default = app;
      });
}
