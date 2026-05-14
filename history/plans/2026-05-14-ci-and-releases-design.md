# Design: CI and Binary Releases

**Status:** Design approved 2026-05-14. Implementation plan to follow.

## Goal

Stand up GitHub Actions CI that validates pull requests on Linux x86_64
and macOS arm64, and a tagged-release workflow that publishes
self-contained `hermes-node` binaries (Linux x86_64 + macOS universal)
to GitHub Releases.

## Context

- `hermes-node` is currently usable but has no CI and no published
  binaries. Development happens locally; correctness is verified by
  running `cmake --build cmake-build-asan --target check-hermes-node`.
- The binary is **self-contained**: all JS modules (libjs, libjs-node,
  shims, vendored packages) are compiled to Hermes bytecode at build
  time by `lib/embedded-modules/` and statically linked. The release
  artifact is therefore a single executable file plus license metadata.
- The Hermes submodule lives on a public fork's `n-api` branch
  (`tmikov/hermes`). No auth needed; standard
  `git submodule update --init --recursive` works.
- Build requirements: CMake >= 3.21, Ninja, Clang (never gcc per
  `CLAUDE.md`), Python 3 (for the embedded-modules pipeline).

## Scope

In scope:
- PR validation workflow (`ci.yml`)
- Tagged binary release workflow (`release.yml`)
- Version derivation from git tag + `hermes-node --version` flag
- ccache-based compile caching
- Format check enforcement
- Ad-hoc macOS code signing

Out of scope (deliberately deferred):
- Linux arm64 builds
- Full Apple Developer ID signing + notarization
- Required-status-checks branch protection rule (GitHub settings, not
  a workflow change)
- Sharing ccache between CI and release workflows
- macos-13 (Intel) smoke-test job for the x86_64 slice
- Docker image release

## High-Level Architecture

Two GitHub Actions workflow files, each with one purpose:

1. `.github/workflows/ci.yml` — PR validation. Triggered on
   `pull_request` to `master` and `workflow_dispatch`. Runs format
   check + a 2x2 build/test matrix.
2. `.github/workflows/release.yml` — Binary release. Triggered on
   push of a `v*` tag. Builds release binaries on Linux + macOS,
   sanity-tests them, packages tarballs, publishes a GitHub Release
   with auto-generated notes.

The two workflows share no state. They use compatible cache key
conventions but live in separate cache scopes.

## CI Workflow Details

**Triggers:**

```yaml
on:
  pull_request:
    branches: [master]
  workflow_dispatch:

concurrency:
  group: ${{ github.workflow }}-${{ github.ref }}
  cancel-in-progress: true
```

**Job 1: `format-check`** (ubuntu-latest):
- Install `clang-format-18` (matching the Linux Clang we pin).
- Run a check-only mode of `utils/format.sh`. If `format.sh` has no
  check mode today, the implementation plan adds one (small change:
  run formatter dry, then `git diff --exit-code`).
- Fast (< 1 minute); independent of the build matrix.

**Job 2: `build-and-test`** (matrix):

```yaml
strategy:
  fail-fast: false
  matrix:
    os: [ubuntu-latest, macos-latest]
    config: [asan-debug-o1, release]
```

Four total jobs. `fail-fast: false` so a failure on one cell doesn't
hide failures on others.

Per-job steps:

1. **Checkout with submodules**

   ```yaml
   - uses: actions/checkout@v4
     with:
       submodules: recursive
   ```

2. **Install toolchain**

   Linux:
   ```bash
   sudo apt-get update
   sudo apt-get install -y ninja-build cmake python3 ccache \
                           clang-18 clang-format-18 lld-18
   ```

   macOS:
   ```bash
   brew install ninja ccache
   ```
   (Apple Clang and Python 3 are preinstalled via Xcode CLT.)

3. **Restore ccache**

   ```yaml
   - uses: actions/cache@v4
     with:
       path: ~/.cache/ccache  # macOS: ~/Library/Caches/ccache
       key: ccache-${{ matrix.os }}-${{ matrix.config }}-${{ github.sha }}
       restore-keys: |
         ccache-${{ matrix.os }}-${{ matrix.config }}-
   ```

   ccache size cap: 3 GB per key (set via `ccache -M 3G`). Stays well
   under GitHub's 10 GB per-repo cache quota even across the 4 keys.

4. **Configure CMake**

   Common to both configs:
   ```
   -G Ninja
   -DCMAKE_C_COMPILER=clang-18         # Linux only; macOS uses Apple Clang
   -DCMAKE_CXX_COMPILER=clang++-18     # Linux only
   -DCMAKE_C_COMPILER_LAUNCHER=ccache
   -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
   ```

   ASAN-debug-O1 config adds:
   ```
   -DCMAKE_BUILD_TYPE=Debug
   -DHERMES_ENABLE_ADDRESS_SANITIZER=ON
   -DHERMESVM_SANITIZE_HANDLES=ON
   -DCMAKE_C_FLAGS_DEBUG_INIT="-g -O1"
   -DCMAKE_CXX_FLAGS_DEBUG_INIT="-g -O1"
   ```

   Release config adds:
   ```
   -DCMAKE_BUILD_TYPE=Release
   ```

5. **Build + test**

   ```bash
   cmake --build build --target check-hermes-node
   ```

   `check-hermes-node` builds the binary, runs gtest unittests, and
   runs the lit JS test suite. The implementation plan verifies this
   target does not pull in Hermes's own `check-hermes`; if it does, a
   narrower target is introduced.

6. **Upload logs on failure**

   ```yaml
   - if: failure()
     uses: actions/upload-artifact@v4
     with:
       name: test-logs-${{ matrix.os }}-${{ matrix.config }}
       path: |
         build/test/
         build/unittests/
   ```

## Release Workflow Details

**Trigger:**

```yaml
on:
  push:
    tags: ['v*']

concurrency:
  group: release-${{ github.ref }}
  cancel-in-progress: false
```

`cancel-in-progress: false` so a second tag push doesn't abort an
in-flight release.

**Job 1: `build-linux`** (ubuntu-latest, x86_64):

1. Checkout with submodules.
2. Install toolchain (same as CI Linux).
3. Restore ccache (separate key scope; release builds rarely benefit
   from CI's cache anyway).
4. Extract version: `VERSION=${GITHUB_REF_NAME#v}` (strip leading `v`).
5. Configure:
   ```
   -DCMAKE_BUILD_TYPE=Release
   -DHERMES_NODE_VERSION=${VERSION}
   ...standard Clang/ccache flags...
   ```
6. Build + sanity test: `cmake --build build --target check-hermes-node`.
7. **Version sanity check**:
   ```bash
   reported=$(./build/bin/hermes-node --version | awk '{print $2}')
   if [ "$reported" != "$VERSION" ]; then
     echo "Version mismatch: binary reports $reported, tag is $VERSION"
     exit 1
   fi
   ```
8. Strip: `strip build/bin/hermes-node`.
9. Stage: create `hermes-node-${VERSION}-linux-x64/` with:
   - `hermes-node` (the binary)
   - `LICENSE`
   - `README.md`
   - `THIRD_PARTY_LICENSES.md`
10. Tarball: `tar czf hermes-node-${VERSION}-linux-x64.tar.gz <staging-dir>/`.
11. Record SHA256.
12. Upload as workflow artifact.

**Job 2: `build-macos-universal`** (macos-latest, arm64 runner):

Same shape as `build-linux` with these differences:
- Toolchain: `brew install ninja ccache`. Apple Clang from Xcode CLT.
- Configure adds: `-DCMAKE_OSX_ARCHITECTURES=x86_64;arm64`. Single CMake
  build emits a universal binary (Clang/CMake compile each TU twice
  and merge into a fat Mach-O).
- After build, before staging:
  - `lipo -info build/bin/hermes-node` (asserts both arch slices
    present; fails the job if not).
  - `strip -u -r build/bin/hermes-node` (preserves universal symbol
    table; `-u` keeps undefined symbols, `-r` removes only
    non-required symbols).
  - `codesign --sign - --force --options runtime --timestamp=none \
       build/bin/hermes-node` (ad-hoc signature).
- Sanity tests run only on the arm64 slice (the runner's native arch).
  The x86_64 slice is built but not exercised. Accepted risk for
  v0.x; flagged in the "Risks" section.
- Tarball name: `hermes-node-${VERSION}-macos-universal.tar.gz`.

**Job 3: `publish`** (ubuntu-latest, `needs: [build-linux, build-macos-universal]`):

1. Download both artifacts.
2. Build a `SHA256SUMS` file listing both tarballs.
3. Detect pre-release: if `${{ github.ref_name }}` contains `-`
   (e.g. `v0.1.0-rc.1`), set `--prerelease`.
4. Create the GitHub Release:
   ```bash
   gh release create "$GITHUB_REF_NAME" \
     --title "$GITHUB_REF_NAME" \
     --generate-notes \
     ${PRERELEASE_FLAG:-} \
     hermes-node-*.tar.gz SHA256SUMS
   ```
   `--generate-notes` produces release notes from merged PRs and
   commits since the previous tag.

## Versioning

**Single source of truth: the git tag.** All version strings derive
from it.

**CMake** (`cmake/version.cmake`, included from top-level `CMakeLists.txt`):

```cmake
# Priority:
# 1. -DHERMES_NODE_VERSION=X.Y.Z passed by release workflow.
# 2. `git describe --tags --always --dirty --match v*` for local builds.
# 3. Fallback "0.0.0-dev".

if(NOT DEFINED HERMES_NODE_VERSION)
  execute_process(
    COMMAND git describe --tags --always --dirty --match "v*"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE _git_desc
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _git_rc
  )
  if(_git_rc EQUAL 0 AND _git_desc)
    string(REGEX REPLACE "^v" "" HERMES_NODE_VERSION "${_git_desc}")
  else()
    set(HERMES_NODE_VERSION "0.0.0-dev")
  endif()
endif()

message(STATUS "hermes-node version: ${HERMES_NODE_VERSION}")

configure_file(
  ${CMAKE_SOURCE_DIR}/include/hermes/node-compat/version.h.in
  ${CMAKE_BINARY_DIR}/generated/hermes/node-compat/version.h
  @ONLY
)
```

The template `version.h.in`:

```c
#pragma once
#define HERMES_NODE_VERSION_STRING "@HERMES_NODE_VERSION@"
```

The build directory containing `generated/` is added to the include
path of the `hermes-node` executable target.

**Example version values:**

| Build context                       | Resulting version  |
| ----------------------------------- | ------------------ |
| Clean checkout on tag `v0.1.0`      | `0.1.0`            |
| One commit past `v0.1.0`            | `0.1.0-1-g<sha>`   |
| With local uncommitted edits        | `0.1.0-1-g<sha>-dirty` |
| Release CI build (tag-derived)      | `0.1.0`            |
| Tarball with no git metadata        | `0.0.0-dev`        |

**CLI flag** (added to `tools/hermes-node/hermes-node.cpp`):

```cpp
#include <hermes/node-compat/version.h>

if (std::strcmp(argv[i], "--version") == 0 ||
    std::strcmp(argv[i], "-v") == 0) {
  std::printf("hermes-node %s\n", HERMES_NODE_VERSION_STRING);
  return 0;
}
```

Conflict-free: `-v` is currently unused; `--node-version` (existing)
takes a value and is unaffected. Usage text in `hermes-node.cpp`
gets a new line documenting the flag.

## Caching Strategy

ccache via `actions/cache`:
- Compiler-level caching. Hashes preprocessed source + flags.
- Survives Hermes submodule SHA bumps: only sources whose preprocessed
  output actually changed miss the cache. A Hermes header tweak
  doesn't invalidate the world.
- Cache key: `ccache-${OS}-${CONFIG}-${SHA}` with `restore-keys`
  falling back to `ccache-${OS}-${CONFIG}-` for partial reuse on PR
  push.
- Wired into CMake via `CMAKE_C_COMPILER_LAUNCHER=ccache` and
  `CMAKE_CXX_COMPILER_LAUNCHER=ccache`.

Considered and rejected:
- Caching the whole `build/` directory: mtime fragility, full
  invalidation on any SHA bump, embedded absolute paths in
  `CMakeCache.txt`.
- Prebuilt Hermes artifact in a separate workflow: would require
  Hermes to install cleanly via `find_package`; it currently uses
  `add_subdirectory` and exposes CMake macros that depend on its
  source tree.
- sccache with cloud backend: more powerful, but requires a cloud
  bucket and credentials. Overkill for current scale.

## Repo Additions

**New files:**

| Path                                                     | Purpose                                                  |
| -------------------------------------------------------- | -------------------------------------------------------- |
| `.github/workflows/ci.yml`                               | PR validation workflow                                   |
| `.github/workflows/release.yml`                          | Tagged binary release workflow                           |
| `cmake/version.cmake`                                    | Version derivation (tag / git-describe / fallback)       |
| `include/hermes/node-compat/version.h.in`                | Template for generated `version.h`                       |
| `THIRD_PARTY_LICENSES.md`                                | Bundled MIT/BSD notices for vendored deps                |

**Modified files:**

| Path                                       | Change                                                                              |
| ------------------------------------------ | ----------------------------------------------------------------------------------- |
| `CMakeLists.txt`                           | `include(cmake/version.cmake)`; add generated include dir to `hermes-node` target   |
| `tools/hermes-node/hermes-node.cpp`        | Add `--version` / `-v` handling and usage line                                      |
| `utils/format.sh`                          | Add `--check` mode (or sibling script) for CI                                       |
| `README.md`                                | Add "Installing from GitHub Releases" section once first release ships              |

**`THIRD_PARTY_LICENSES.md` contents:**

A single Markdown file collecting the license text from each vendored
dependency that gets compiled into (or whose code is embedded in) the
shipped binary. Sources to enumerate during the plan:
- Hermes (MIT) — `hermes/LICENSE`
- libuv (MIT) — `external/libuv/libuv/LICENSE`
- c-ares (MIT) — `external/cares/cares/LICENSE.md`
- llhttp (MIT) — `external/llhttp/llhttp/LICENSE-MIT`
- simdutf, Ada — from each upstream's LICENSE
- Node.js (MIT) — covers all JS embedded from `libjs-node/`
- Each package under `vendored/` — license per package's `package.json`
  / LICENSE file. Plan enumerates the current set and adds them.

The release packaging step copies this file alongside the binary. The
file lives in the repo root so local users can find it too.

## Testing Strategy for This Work

The CI/release workflows themselves need validation:

1. **CI workflow:** Open a draft PR that introduces `ci.yml`. The PR's
   own checks prove the workflow works. Iterate on the same PR until
   green on all 4 build cells + format check.

2. **Release workflow:** Push a throwaway tag like `v0.0.0-test1` (a
   pre-release per the `-` heuristic) to verify the full pipeline
   end-to-end. Confirm:
   - Both build jobs succeed.
   - The binary's `--version` output matches the tag.
   - The GitHub Release is created with both tarballs + `SHA256SUMS`.
   - `tar xzf` on each tarball yields a runnable `hermes-node`.
   - On macOS, `lipo -info` confirms both arch slices are present.
     Running the x86_64 slice from the arm64 runner requires Rosetta,
     which is not preinstalled on `macos-latest`. The plan documents
     this as a known gap; ad-hoc verification of the x86_64 slice
     happens on an Intel Mac if available, otherwise we rely on
     successful compilation as the only signal.

   After verification, delete the test tag and the test release.

3. **First real release:** Tag `v0.1.0` only after both workflows are
   known-good.

## Risks and Open Questions

- **macOS x86_64 slice is built but not tested in the release
  workflow.** The arm64 runner cannot execute x86_64 natively without
  Rosetta; tests run only on arm64. Arch-specific bugs in the x86_64
  slice could ship undetected. Mitigation: the universal binary's
  x86_64 slice is compiled from the same source the Linux CI
  validates, which catches most arch-independent issues. A separate
  `macos-13` (Intel) smoke-test job can be added later if needed.
- **`check-hermes-node` target scope.** Plan must verify this target
  runs only our suite, not Hermes's `check-hermes`. If it pulls in
  Hermes tests, a narrower target is needed.
- **`utils/format.sh` check mode.** The script today runs in "fix"
  mode. The plan must either add a `--check` flag or wrap the
  existing script with a `git diff --exit-code` postcheck.
- **ccache cache hit rate on macOS universal builds.** Compiling each
  TU twice (x86_64 + arm64) means each arch has its own cache entry.
  Effective cache size doubles. Should still fit under the 3 GB cap
  but worth observing.
- **Release workflow re-runs.** If `gh release create` fails after
  artifacts upload (e.g. transient API hiccup), a manual retry may
  conflict with the partial release. The plan should make `publish`
  idempotent (use `gh release upload --clobber` if the release
  already exists for the tag).

## Out of Scope (Listed for Future)

- Linux arm64 build (`ubuntu-24.04-arm` runner is free; explicitly
  deferred).
- Full Apple Developer ID signing + notarization. Required once
  non-developer users start downloading; until then, ad-hoc signing
  + the macOS first-run approval flow is acceptable.
- Required-status-checks branch protection rule on `master`.
- Sharing ccache between CI and release scopes.
- macos-13 (Intel) smoke test job.
- Docker image release.
- Homebrew tap / formula.
- Auto-generated changelog file in the repo (separate from the
  GitHub Release notes).

## Sequencing for the Implementation Plan

The implementation plan (next document) should sequence the work as:

1. Version derivation (`cmake/version.cmake`, `version.h.in`,
   `CMakeLists.txt` wiring, `--version` flag, usage text).
   Validate locally before touching CI.
2. `format.sh --check` mode.
3. `THIRD_PARTY_LICENSES.md`.
4. `ci.yml` (format check + 4-cell build/test matrix). Iterate via
   a draft PR until green.
5. `release.yml`. Validate via a throwaway `v0.0.0-test1` tag, then
   clean up the test artifacts.
6. README "Installing" section, deferred until after the first real
   release exists to link to.
