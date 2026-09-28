# docs2epub

`docs2epub` turns a public documentation site into a reflowable EPUB 3 book. It renders pages in Chromium, caches the extracted content, builds the EPUB without network access, and validates the packaged result with EPUBCheck and multi-width browser checks.

It is a single C++23 binary. It drives headless Chromium over the DevTools protocol on `--remote-debugging-pipe`, parses HTML with libxml2, processes images with libvips, fetches with libcurl, and writes the archive with libzip.

## Run it

Nix supplies the matching Chromium and EPUBCheck versions. Outside Nix, `chromium` and `epubcheck` must be on `PATH`, or set `DOCS2EPUB_CHROMIUM` to a Chromium binary.

```sh
nix run github:sammyjoyce/docs2epub -- https://docs.example.com -o example-docs.epub
```

The command exits with code 0 only when scraping, building, coverage, EPUBCheck, accessibility checks, and reflow checks pass. Add `--json` for a stable machine-readable result. Progress and validator files stay in `.docs2epub/<host>-<path>/`.

## Install

The flake builds on `x86_64-linux` and `aarch64-linux`. The wrapped binary finds its own Chromium, EPUBCheck, and fonts, so it needs no other setup.

Install it into your profile:

```sh
nix profile install github:sammyjoyce/docs2epub
```

Use it from another flake. Pick one of the two options below.

```nix
{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    docs2epub = {
      url = "github:sammyjoyce/docs2epub";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs = { nixpkgs, docs2epub, ... }:
    let
      system = "x86_64-linux";
      # Option 1: the overlay adds pkgs.docs2epub, built against your nixpkgs.
      pkgs = import nixpkgs { inherit system; overlays = [ docs2epub.overlays.default ]; };
    in {
      devShells.${system}.default = pkgs.mkShell {
        packages = [
          pkgs.docs2epub
          # Option 2: the package output, without an overlay.
          # docs2epub.packages.${system}.default
        ];
      };
    };
}
```

For NixOS or Home Manager, add the overlay to `nixpkgs.overlays` and put `pkgs.docs2epub` in `environment.systemPackages` or `home.packages`.

`inputs.nixpkgs.follows` is optional. It reuses your nixpkgs, so you do not download a second copy of Chromium. Packages from the overlay and from `packages.<system>` skip the test suite, because the tests need the hegel-cpp engine. `nix flake check` in this repository runs the tests.

## Rerun one stage

```sh
docs2epub scrape https://docs.example.com --workspace .docs2epub/example
docs2epub build --workspace .docs2epub/example -o example-docs.epub
docs2epub validate example-docs.epub --workspace .docs2epub/example --json
docs2epub inspect report --workspace .docs2epub/example --json
docs2epub doctor --json
```

The scrape workspace stores one JSON record per rendered page and downloads images once. `build` reads only that workspace. You can change the EPUB cleaner or stylesheet and rebuild without scraping the site again.

## Sites outside the generic layout

The generic extractor searches common documentation roots such as `main article`, `article`, and `[role=main]`. It reads navigation from `#sidebar-content`, documentation nav elements, or sidebars. Override either selector when a site uses a different layout.

```sh
docs2epub https://docs.example.com \
  --content-selector '#documentation' \
  --nav-selector '.docs-sidebar'
```

The initial release targets public, single-version documentation sites. It does not automate login flows, merge languages, or crawl beyond the entry path.

## What the builder preserves

- Sitemap pages plus sidebar reading order.
- Expanded details and tab panels as static content.
- Headings, code, callouts, tables, figures, links, and source URLs.
- Local cross-page links with stable EPUB filenames.
- Reader-controlled colors and reflow-safe code and tables.
- EPUB accessibility metadata, document language, image alternatives, table headers, and heading order.

The validator checks the actual EPUB ZIP. It verifies package structure, XHTML parsing, internal links, discovered-page coverage, EPUBCheck output, and horizontal overflow at phone, large-phone, and tablet widths with enlarged text.

## Develop

```sh
nix develop
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
build/docs2epub doctor
```

`nix flake check` runs the same suites in the build sandbox. The tests are unit tests, an end-to-end run against a local fixture site, and [Hegel](https://github.com/hegeldev/hegel-cpp) property tests for URL scoping, canonicalization, and file naming. The flake pins hegel-cpp and its prebuilt engine, so the build needs no network.
