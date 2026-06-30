# Local Chromium Workspace Guidelines

## Local Customization Policy

This checkout is a local Chromium workspace with private, business-specific
customizations. Treat local feature changes as intentional product work for this
workspace, not as upstream Chromium CLs by default.

When reviewing or editing code:

- Preserve local business logic unless the user explicitly asks to remove it.
- Evaluate changes for local correctness, build health, security, stability,
  maintainability, and fit with nearby Chromium patterns.
- Do not reject, strip, or redesign a change merely because it would be hard to
  upstream or unlikely to pass Chromium review.
- Mention upstream compatibility only when it affects the user's stated goal or
  when the user explicitly asks for upstream-ready guidance.
- Prefer small, focused fixes that keep local custom behavior working.

## Project Structure & Module Organization

This is the Chromium `src` checkout. Product and platform code lives in top-level
directories such as `chrome/`, `android_webview/`, `ash/`, `ios/`, and
`content/`; shared libraries live in `base/`, `components/`, `services/`, `ui/`,
`net/`, and similar directories. Documentation is under `docs/`, style rules
under `styleguide/`, test infrastructure under `testing/`, build tooling under
`build/` and `tools/`, external code under `third_party/`, and generated build
outputs under `out/`. Prefer adding code inside existing product or component
trees where practical, but local business modules may intentionally diverge from
upstream conventions when that best serves the local product.

## Build, Test, and Development Commands

```sh
# Debug build dir is out/main, release build dir is out/mainr.
gn gen out/mainr
autoninja -C out/mainr chrome
autoninja -C out/mainr unit_tests browser_tests
out/mainr/unit_tests --gtest_filter="SuiteName.TestName"      # macOS/Linux
out/mainr/unit_tests.exe --gtest_filter="SuiteName.TestName"  # Windows
git cl format --js
```

`gn gen` creates Ninja files for a build directory. `autoninja` builds targets;
use GN labels without the leading `//`, for example `chrome/test:unit_tests`.
Examples use forward slashes so they are readable across platforms; Windows
shells may also use backslashes. Run the narrowest relevant test binary before
broad suites.

### Local Build Environment Rules

Before running any compile/build/test command, make sure `depot_tools` is first
on `PATH` for that command. Use the platform-appropriate `depot_tools` location
and wrapper: `autoninja` from POSIX shells, or `autoninja.bat` from Windows
PowerShell/cmd.

On Windows for this local checkout, prepend `D:\dev\depot_tools_for_chromium`
to `PATH` and invoke `autoninja.bat` directly, for example:

```powershell
$env:Path = "D:\dev\depot_tools_for_chromium;" + $env:Path
autoninja.bat -C out/mainr <target>
```

On macOS/Linux, use `autoninja` directly.

This checkout is an external local development environment, not a Google
internal remote-build setup. Do not invoke `siso` directly, do not request
`siso login`, and do not switch to RBE/reclient/remote execution. Do not run
`gn clean`, do not create temporary output directories, and do not regenerate or
modify existing build directories unless the user explicitly asks. Avoid broad
targets such as a full Chromium build unless requested; prefer the narrowest
relevant target. If a build appears to fail because of environment setup, stop
and ask the user before trying alternate tools or changing build directories.

### BUA / Glic Verification Rules

When validating `bua-server2` in Glic, open Glic through the real sidebar UI.
Do not validate by directly opening `chrome://glic` as a normal browser tab
with CDP or `/json/new`; that path is not representative of the user's product
flow. If automation is needed, launch Chromium with the relevant Glic dev flags
and trigger or inspect the sidebar-hosted Glic webview target.

## Coding Style & Naming Conventions

Follow the language guides in `styleguide/`. C++ follows Chromium's Google C++
style variant and should accept the checkout's `clang-format` output. Use spaces,
not tabs. Keep platform-specific code behind the macros in `build/build_config.h`.
Test-only C++ helpers should use conventional suffixes such as `ForTesting`, and
test support targets should be marked `testonly=true`. For local-only APIs,
document the local contract clearly enough that future changes can preserve the
business behavior.

## Testing Guidelines

Add or update tests for behavior changes. Unit test files use `_unittest.cc`;
browser tests use `_browsertest.cc`. Place tests near the code they cover, and
put reusable test support in a local `test/` subdirectory when possible. Build
and run the specific target touched by your change, then expand to `unit_tests`,
`browser_tests`, or platform suites as risk requires.

## Local Patch & Review Guidelines

This workspace is not assumed to be preparing upstream Chromium submissions.
Default review should focus on whether the local feature works, is safe enough
for the intended environment, and is maintainable for this fork.

Use normal Git commits and patch files as requested by the user. Commit subjects
should be short one-line summaries, followed by a body when context is helpful.
Before finalizing a local change, review `git diff`, run formatting when relevant,
and run the narrowest useful build or test target.

Only use Chromium Gerrit workflows such as `git cl upload`, `git cl owners`, CQ,
`Bug:` footers, or upstream ownership guidance when the user explicitly asks to
prepare an upstream-ready CL.
