# Contributing

Bug reports and pull requests are welcome.

## Report a bug

Search the [existing issues](https://github.com/sammyjoyce/webspine/issues) first. A new issue needs:

- The command you ran and the documentation URL, if the site is public.
- The output of `webspine doctor --json`.
- The expected and the actual result. For a failed run, attach `report.json` from the workspace.

## Build and test

The Nix dev shell supplies the compiler, libraries, Chromium, EPUBCheck, and the hegel-cpp engine.

```sh
nix develop
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

`nix flake check` runs the same suites in the build sandbox. CI runs `nix flake check` on every pull request.

## Send a pull request

- Keep one change per pull request.
- Add a test that fails without your change. Use a property test in `tests/property_test.cpp` for URL, path, or naming rules. Use a fixture page in `tests/fixtures/site/` for extraction or EPUB output.
- Match the style of the surrounding code.
- Run `nix flake check` before you push.

By contributing, you agree that your contribution is licensed under the [MIT License](LICENSE).
