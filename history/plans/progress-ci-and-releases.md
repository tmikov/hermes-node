# Implementation Progress

Tracks progress on `history/plans/2026-05-14-ci-and-releases-design.md`
(design doc) and the corresponding implementation plan executed on
2026-05-14.

## Status

| Step | Description | Depends On | Status | Brief Note (optional) |
|------|-------------|------------|--------|-----------------------|
| Task 1 | Version derivation infrastructure (cmake/version.cmake, version.h.in, wiring) | -- | done | Generates `cmake-build-*/generated/hermes/node-compat/version.h` from git tag / -D / fallback. |
| Task 2 | --version / -v CLI flag in hermes-node | 1 | done | Returns before runtime init. Verified locally. |
| Task 3 | utils/format.sh --check mode | -- | done | clang-format --dry-run --Werror over FORMAT_DIRS. Exit 1 on diffs. |
| Task 4 | THIRD_PARTY_LICENSES.md | -- | done | Covers Hermes + 9 external/ deps + vendored/ws + libjs-node. ASCII only. |
| Task 5 | .github/workflows/ci.yml (format-check + 2x2 matrix) | 1, 2, 3 | done-local | Written and YAML-validated. Live CI validation pending PR push. |
| Task 6 | .github/workflows/release.yml (linux-x64, macos-universal, publish) | 1, 2, 4 | done-local | Written and YAML-validated. Live release validation pending throwaway tag push. |
| Task 7 | Progress file + MEMORY.md bookkeeping | -- | done | This file + MEMORY.md update. |

## Context Notes

### Task 1: Version derivation

- `cmake/version.cmake` priority: `-DHERMES_NODE_VERSION` > `git describe
  --tags --always --dirty --match v*` (stripping leading `v`) >
  `0.0.0-dev` fallback.
- Header template `include/hermes/node-compat/version.h.in` defines
  `HERMES_NODE_VERSION_STRING`.
- Generated file lives at `${CMAKE_BINARY_DIR}/generated/hermes/node-compat/version.h`;
  `tools/hermes-node/CMakeLists.txt` adds the generated dir to that
  target's include path (only consumer right now).
- First local build produced `60d05ea-dirty` since the repo has no
  `v*` tags yet -- `git describe --always` falls back to the abbreviated
  SHA. That's expected.

### Task 2: --version / -v flag

- Handler placed BEFORE the `--help` check so it short-circuits
  without runtime initialization (consistent with `--help`).
- `-v` was previously unused; no conflict.
- `--node-version` (existing flag taking a value) is unaffected -- it's
  matched only by exact string compare.

### Task 3: format.sh --check

- New `--check` short-circuits early (after the script `cd`s to repo
  root) by running `clang-format --dry-run --Werror -style=file` over
  all `.h .c .cpp` files under `FORMAT_DIRS` (lib, tools, include,
  unittests). Exits 1 on any diff.
- Verified by deliberately appending mis-indented code to a file:
  exit 1; restoring the file: exit 0.

### Task 4: THIRD_PARTY_LICENSES.md

- Plan's Task 4 enumeration omitted Hermes itself even though it's
  statically linked into `hermes-node`. The design doc lists it
  explicitly -- I added it.
- For deps whose vendored copy ships its own LICENSE file
  (libuv/cares/llhttp/ada/simdutf/zlib/ws/libjs-node/Hermes), the
  document points to the in-tree path. Where the vendored subtree
  does NOT ship a LICENSE (brotli, zstd, picohash), the applicable
  license text is inlined.
- We elect the BSD-3-Clause portion of zstd's BSD/GPLv2 dual license.

### Task 5: ci.yml

- One `format-check` job + `build-and-test` matrix (`ubuntu-latest`/
  `macos-latest` x `asan-debug-o1`/`release`) -> 5 total jobs.
- `fail-fast: false` so individual matrix cells don't mask each other.
- ccache wired via `CMAKE_*_COMPILER_LAUNCHER=ccache`; capped at 3 GB.
  Cache key includes OS + config + SHA, with restore-keys falling
  back to OS+config prefix.
- macOS uses Apple Clang from Xcode CLT (no compiler explicitly set).
- Linux pins `clang-18`.
- `clang-format-18` installed via apt with `update-alternatives` so
  `clang-format` resolves to the right version for `format.sh`.
- `concurrency` cancels in-progress runs when a new push lands on the
  same PR.

### Task 6: release.yml

- Three jobs: `build-linux`, `build-macos-universal`, `publish` (needs
  both).
- Tag-triggered (`v*`). Pre-release detected by presence of `-` in
  tag name (e.g. `v0.1.0-rc.1`).
- macOS uses `-DCMAKE_OSX_ARCHITECTURES="x86_64;arm64"` for a
  universal binary; verified with `lipo -info` post-build (fails the
  job if either slice is missing).
- Version sanity check runs the freshly built binary's `--version`
  and asserts it matches the tag (stripped of `v`).
- macOS binary gets ad-hoc `codesign` to avoid Gatekeeper first-run
  pain on developer machines.
- `publish` job is idempotent: if a release for the tag already
  exists (re-run scenario), it uses `gh release upload --clobber`;
  otherwise it creates a fresh release with `--generate-notes`.
- `permissions: contents: write` granted at workflow level so the
  default `GITHUB_TOKEN` can create the release.
- ccache cache scope for release is keyed separately
  (`ccache-release-*`) from CI cache.

### Manual validation steps (deferred)

The two GitHub Actions workflows can't be validated end-to-end from a
local shell -- they need to actually run on GitHub.

1. **CI validation:** open a draft PR introducing all the Task 1-5
   changes. Iterate until all 5 jobs are green. Watch the
   `ccache stats` step for hit ratio. Expected initial run: cold
   cache (0% hits). Subsequent pushes on the same PR should see
   high hit ratios on touched-file-only changes.

2. **Release validation:** push a throwaway tag `v0.0.0-test1`:
   - Confirm both build jobs succeed.
   - Confirm `--version` reports `0.0.0-test1`.
   - Confirm GitHub Release is created with both tarballs +
     SHA256SUMS, marked as pre-release.
   - `lipo -info hermes-node` on macOS tarball shows both archs.
   - `tar xzf` on each tarball yields a runnable binary.
   - After verification: delete the test tag and release.

3. **First real release:** tag `v0.1.0` once both workflows are
   known-good.

### Out of scope (deferred from design doc + plan)

- Linux arm64 builds.
- Apple Developer ID signing + notarization (we use ad-hoc signing).
- Branch-protection required-status-checks rule on master.
- macos-13 (Intel) smoke-test job for the x86_64 universal slice.
- README "Installing from GitHub Releases" section -- to be added
  after the first real release exists to link to.
- Docker image / Homebrew tap.
