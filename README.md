# docs2epub

`docs2epub` turns a public documentation site into a reflowable EPUB 3 book. It renders pages in Chromium, caches the extracted content, builds the EPUB without network access, and validates the packaged result with EPUBCheck and multi-width browser checks.

It is a single C++23 binary. It drives headless Chromium over the DevTools protocol on `--remote-debugging-pipe`, parses HTML with libxml2, processes images with libvips, fetches with libcurl, and writes the archive with libzip.

## Run it

Nix supplies the matching Chromium and EPUBCheck versions. Outside Nix, `chromium` and `epubcheck` must be on `PATH`, or set `DOCS2EPUB_CHROMIUM` to a Chromium binary.

```sh
nix run . -- https://docs.example.com -o example-docs.epub
```

The command exits with code 0 only when scraping, building, coverage, EPUBCheck, accessibility checks, and reflow checks pass. Add `--json` for a stable machine-readable result. Progress and validator files stay in `.docs2epub/<host>-<path>/`.

## Rerun one stage

```sh
nix run . -- scrape https://docs.example.com --workspace .docs2epub/example
nix run . -- build --workspace .docs2epub/example -o example-docs.epub
nix run . -- validate example-docs.epub --workspace .docs2epub/example --json
nix run . -- inspect report --workspace .docs2epub/example --json
nix run . -- doctor --json
```

The scrape workspace stores one JSON record per rendered page and downloads images once. `build` reads only that workspace. You can change the EPUB cleaner or stylesheet and rebuild without scraping the site again.

## Sites outside the generic layout

The generic extractor searches common documentation roots such as `main article`, `article`, and `[role=main]`. It reads navigation from `#sidebar-content`, documentation nav elements, or sidebars. Override either selector when a site uses a different layout.

```sh
nix run . -- https://docs.example.com \
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
