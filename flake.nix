{
  description = "docs2epub, a rendered documentation site to EPUB CLI";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        python = pkgs.python3;
        app = python.pkgs.buildPythonApplication {
          pname = "docs2epub";
          version = "0.1.0";
          pyproject = true;
          src = ./.;
          build-system = [ python.pkgs.setuptools ];
          dependencies = with python.pkgs; [
            beautifulsoup4
            cairosvg
            lxml
            pillow
            playwright
          ];
          nativeBuildInputs = [ pkgs.makeWrapper ];
          nativeCheckInputs = [ python.pkgs.pytestCheckHook pkgs.epubcheck ];
          PLAYWRIGHT_BROWSERS_PATH = pkgs.playwright-driver.browsers;
          PLAYWRIGHT_SKIP_VALIDATE_HOST_REQUIREMENTS = "true";
          postFixup = ''
            wrapProgram $out/bin/docs2epub \
              --set PLAYWRIGHT_BROWSERS_PATH ${pkgs.playwright-driver.browsers} \
              --set PLAYWRIGHT_SKIP_VALIDATE_HOST_REQUIREMENTS true \
              --prefix PATH : ${pkgs.lib.makeBinPath [ pkgs.epubcheck ]}
          '';
        };
      in {
        packages.default = app;
        apps.default = {
          type = "app";
          program = "${app}/bin/docs2epub";
        };
        devShells.default = pkgs.mkShell {
          packages = [
            (python.withPackages (ps: with ps; [
              beautifulsoup4
              cairosvg
              lxml
              pillow
              playwright
              pytest
            ]))
            pkgs.epubcheck
            pkgs.ruff
          ];
          PLAYWRIGHT_BROWSERS_PATH = pkgs.playwright-driver.browsers;
          PLAYWRIGHT_SKIP_VALIDATE_HOST_REQUIREMENTS = "true";
          shellHook = ''
            export PYTHONPATH="$PWD''${PYTHONPATH:+:$PYTHONPATH}"
          '';
        };
        checks.default = app;
      });
}
