# Contributing to Aurora

Thanks for helping. Aurora is a small project, so a few conventions keep it
reviewable.

## Before you start

- **Bugs:** open an issue with the *Bug report* template. Include the Aurora
  version (Settings > About), the OS, and the startup log if the bug is about
  the engine (run with `AURORA_LOG_LEVEL=debug`; on Windows add `--shell`).
- **Features:** open a *Feature request* first for anything larger than a
  small fix, so we can agree on the approach before you write code.
- **Security issues:** do **not** open a public issue — see
  [SECURITY.md](SECURITY.md).

## Building and testing

The core library and its unit tests build with only a C++20 compiler and
CMake (SQLite is vendored):

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DAURORA_BUILD_TESTS=ON
cmake --build build
./build/bin/aurora_tests
```

The full browser needs Qt 6.5+ and a CEF binary distribution
(`./scripts/fetch_cef.sh`); see the README and [docs/build.md](docs/build.md).
CI builds Windows, macOS and Linux on every push to `main`; keep it green.

## Code style

- C++20, the existing formatting (4-space indent, ~90 columns), and the
  existing warning set: CI builds with `-DAURORA_WERROR=ON`.
- **Keep CEF and Qt apart.** `aurora_core` depends on neither; anything that
  can be pure logic (policies, parsers, URL handling) goes there and gets a
  doctest case in `tests/`. Qt TUs include `cef/CefRuntime.hpp`, never CEF
  headers.
- **Comment the why.** The codebase documents decisions, platform quirks and
  the bug a line fixes next to the code. New code is expected to do the same.
- Never weaken the security model for convenience (sandbox, certificate
  checks, permission defaults) — read [docs/security.md](docs/security.md).

## Commits and pull requests

- Small, focused commits with an imperative subject line ("Add reader mode",
  not "added stuff"), and a body that explains why when it is not obvious.
- One logical change per pull request; update the README / docs when you
  change user-visible behaviour or shortcuts.
- Add or update unit tests for logic changes. UI-only changes should say how
  they were checked manually.
- By contributing you agree that your contribution is licensed under the MIT
  license of this repository.
