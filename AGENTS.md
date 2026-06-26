# Repository Guidelines

## Project Structure & Module Organization

This is the Chromium `src` checkout. Product and platform code lives in top-level
directories such as `chrome/`, `android_webview/`, `ash/`, `ios/`, and
`content/`; shared libraries live in `base/`, `components/`, `services/`, `ui/`,
`net/`, and similar directories. Documentation is under `docs/`, style rules
under `styleguide/`, test infrastructure under `testing/`, build tooling under
`build/` and `tools/`, external code under `third_party/`, and generated build
outputs under `out/`. Prefer adding code inside existing product or component
trees; new top-level directories are reserved for products.

## Build, Test, and Development Commands

Run commands from this directory with `depot_tools` on `D:\dev\depot_tools_for_chromium\`.

```sh
# debug build dir is out\main, release build dir is out\mainr
gn gen out\mainr
autoninja -C out\mainr chrome
autoninja -C out\mainr unit_tests browser_tests
out\mainr\unit_tests.exe --gtest_filter="SuiteName.TestName"
git cl format --js
git cl upload
```

`gn gen` creates Ninja files for a build directory. `autoninja` builds targets;
use GN labels without the leading `//`, for example `chrome/test:unit_tests`.
Run the narrowest relevant test binary before broad suites.

### Local Build Environment Rules

Before running any compile/build/test command, prepend
`D:\dev\depot_tools_for_chromium` to `PATH` for that command. Use
`autoninja.bat` directly for builds, for example:

```powershell
$env:Path = "D:\dev\depot_tools_for_chromium;" + $env:Path
autoninja.bat -C out\mainr <target>
```

This checkout is an external local development environment, not a Google
internal remote-build setup. Do not invoke `siso` directly, do not request
`siso login`, and do not switch to RBE/reclient/remote execution. Do not run
`gn clean`, do not create temporary output directories, and do not regenerate or
modify existing build directories unless the user explicitly asks. Avoid broad
targets such as a full Chromium build unless requested; prefer the narrowest
relevant target. If a build appears to fail because of environment setup, stop
and ask the user before trying alternate tools or changing build directories.

## Coding Style & Naming Conventions

Follow the language guides in `styleguide/`. C++ follows Chromium's Google C++
style variant and should accept the checkout's `clang-format` output. Use spaces,
not tabs. Keep platform-specific code behind the macros in `build/build_config.h`.
Test-only C++ helpers should use conventional suffixes such as `ForTesting`, and
test support targets should be marked `testonly=true`.

## Testing Guidelines

Add or update tests for behavior changes. Unit test files use `_unittest.cc`;
browser tests use `_browsertest.cc`. Place tests near the code they cover, and
put reusable test support in a local `test/` subdirectory when possible. Build
and run the specific target touched by your change, then expand to `unit_tests`,
`browser_tests`, or platform suites as risk requires.

## Commit & Review Guidelines

Chromium uses Gerrit CLs rather than GitHub pull requests. Commit subjects are
short one-line summaries, followed by a blank line and a wrapped body explaining
why the change is needed. Include `Bug:` and `Test:` footers when applicable.
Before upload, review `git diff` or `git upstream-diff`, run formatting and
relevant tests, then use `git cl upload`. Use `git cl owners` to find required
OWNERS reviewers, and run CQ dry runs for significant changes.
