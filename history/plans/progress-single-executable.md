# Implementation Progress

Tracks progress on `history/plans/2026-08-23-single-executable-plan.md`
(implementation plan) and its companion design doc
`history/plans/2026-08-23-single-executable-design.md`. The spike the design
rests on (`2026-08-23-macos-partial-link-spike.md` and its `-results.md`) sits
beside them; it was produced on a Mac, on branch `work-mac`.

## Status

| Step | Description | Status |
|------|-------------|--------|
| Task 1 | Cut the kit (`utils/make-kit.py`, `hermes-node-kit`) | done |
| Task 2 | Embedded-bundle run path (`openEmbeddedBundle`, `bundle_main.cpp`) | done |
| Task 3 | Kit manifest reader (`lib/build-exe/kit_manifest.cpp`) | done |
| Task 4 | The `--build-exe` producer (`lib/build-exe/build_exe.cpp`) | done |
| Task 5 | Flag surface (`--build-exe`, `--kit`, conflict rows) | done |
| Task 6 | Tests and the `linker-available` gate | done |
| Task 7 | Documentation | done |

All seven tasks are complete, plus a whole-branch review and its fix round.
Every task was reviewed individually as well.

## What the whole-branch review caught that no task review could

**The macOS universal release build would have failed its test gate,
deterministically.** `.github/workflows/release.yml` configures the macOS
release with `-DCMAKE_OSX_ARCHITECTURES="x86_64;arm64"` and then runs
`check-hermes-node`, which depends on `hermes-node-kit`. So the release cuts
a *universal* kit -- and the payload object was being assembled without the
manifest's driver flags, making it host-arch only, so the universal link
could not resolve `hermesNodeBundleStart`/`hermesNodeBundleEnd` for the
second slice. No macOS release could have been cut.

It came from two decisions that were each correct alone: the ruling that the
check target depend on the kit (so the suite cannot run against a stale one),
and the producer's judgment not to forward driver flags to the assemble step
(true for every kit that existed at the time). Neither task's review could
see the pair; only a pass over the whole branch, including the CI and release
workflows, could. That is the argument for the final review existing.

The fix (`4534d38`) forwards the manifest's **entire** `driverflag` list to
the assemble step rather than a target-selecting subset, because a
hand-maintained subset drifts and drifts at someone else's link -- the same
reasoning that made `kit.manifest` a recording of the real link line rather
than a construction from one. It is also the cheaper failure mode: a
forwarded flag that should not be there is a loud driver error at *this*
build, whereas a missing one is an undefined symbol at a customer's. Verified
against both real manifests, including the ASAN one with `-fsanitize=address`.
`utils/make-kit.py` gained `multi_arch_warning()` as a backstop -- a warning,
deliberately, so it cannot itself block a release.

**One consequence worth keeping in view:** the lowercase `.s` extension of the
generated payload file became load-bearing at that moment. clang runs the C
preprocessor over `.S` and not over `.s`, and the assemble step now forwards
`-D` flags over a file that holds an arbitrary user-supplied path inside a
quoted `.incbin`. `CLAUDE.md` had said `.S`, so the documentation was pointing
at the single edit that would break it (`e834733`).

## What shipped

`hermes-node --build-exe=<out> <bundle.hbb>` links a standalone executable
from an AOT container. It is a fifth verb dispatched by `runToolVerb()`
before `runHermesNode`, so no runtime, event loop or `napi_env` exists while
it runs: it reads an already-compiled container, generates one assembly file
and runs the toolchain.

- **The kit** is cut by `utils/make-kit.py`, driven as a `RULE_LAUNCH_LINK`
  on the `hermes-node-kit` probe target, so it is derived from CMake's real
  link line rather than from a hand-written list. Four files:
  `libhermes-node-kit.a` (the merged closure), `libhermesNapi.a` (kept
  separate, because it is genuinely `-force_load`ed),
  `hermes-node-bundle-main.o`, and `kit.manifest`
  (`version`/`cc`/`driverflag`/`linkarg`, with `{kit}` substituted).
- **The producer** (`hermesNodeBuildExe`, `lib/build-exe/`) validates the
  container with `BundleReader::open()` and this binary's own
  `bundleGenerationTag()`, checks the kit's version against
  `HERMES_NODE_VERSION_STRING`, emits a `.incbin` payload object, assembles
  it and links. VM-free by construction, which is what lets `BuildExeTest`
  run with no runtime.
- **The run path** is `openEmbeddedBundle()` plus `runEmbeddedBundle()`: the
  same validation chain as a container on disk, with the root taken from the
  executable's own realpath'd directory instead of the container's.
  `rootDirectoryFor()` is the single copy both cases call.
- **The app entry** is `tools/hermes-node/bundle_main.cpp`: no flag parsing,
  no fuse, `process.argv` as `[exe, exe, ...userArgs]`.

## Measurements

Linux x86_64, `cmake-build-asan` unless noted.

- Kit-relinked `hermes-node` vs CMake's own binary: 12,835,776 against
  12,831,944 (+3,832, +0.030%), 145 `napi_` and 3717 total dynamic exports on
  both, sorted export name lists diff empty. (Losing `--gc-sections` would
  cost +1.1 MB, so the delta is padding, not lost folding.)
- A produced app: 12,682,552 bytes against `hermes-node`'s 12,836,584 --
  154,032 bytes smaller, because it carries neither the CLI parser nor the
  tool verbs. The two link configurations really are distinct.
- Payload symbol at 0x6630f0 (16-aligned); `end - start` exactly the
  container's size. `GNU_STACK` `RW`, and confirmed load-bearing by
  re-assembling without the note: `RWE` plus the linker's own deprecation
  warning.
- Kit size: ~40 MB Release, ~755 MB ASAN. Re-cut measured at 4.8 s.
- Suite: 174 pass / 0 fail before, 179 / 0 after, in both `cmake-build-asan`
  and `cmake-build-release`. 283 unit tests. With no kit: 2 pass /
  3 UNSUPPORTED / 0 fail, so the `linker-available` gate turns off cleanly.

macOS arm64 numbers (partial link, folding, dead-stripping, ad-hoc signature)
are the spike's and are quoted in the design doc; nothing in these seven
tasks ran on macOS.

## Defects the implementation found in its own plan

Recorded because each is a trap that would otherwise be rediscovered:

- **The `.note.GNU-stack` directive.** The plan's assembler stub omitted it,
  which marks every produced Linux executable's stack executable. Caught in
  Task 2, carried into Task 4, and now emitted by `payloadAssembly()`'s ELF
  branch.
- **The payload's base alignment.** `BundleReader`'s offset checks are modulo
  the offset, which is only sufficient when the base is aligned -- true for
  free under `mmap`, not true for a linked section. `openEmbeddedBundle()`
  now demands it, so a generator mistake is a message rather than undefined
  behaviour.
- **`#ifdef __APPLE__` around the Mach-O branch made it unbuilt, not merely
  unexercised**, in the test binary as well as the product: a typo there
  would have passed every test on every host we run. Replaced by an
  `ObjectFormat` parameter, and the Mach-O text is asserted with `EXPECT_EQ`
  on the complete generated string.
- **A POST_BUILD copy of the entry object into the kit does not work.** The
  kit target's link rule is the stamp, which does not depend on
  `bundle_main.cpp`, so editing it left a stale `.o` in the kit -- caught by
  a wrong test result, not by inspection. Replaced with an OUTPUT-tracked
  rule.
- **The kit's outputs are invisible to the build system**, so `rm -rf kit`
  once left a build that reported nothing to do and a kit that never came
  back. Fixed by aiming the probe link's `-o` at a stamp inside the kit
  directory.
- **`.incbin` paths are not opaque strings.** GAS resolves `\t` inside one,
  so a path containing a backslash can assemble successfully against a file
  the literal path does not name -- reproduced, then rejected outright along
  with `"`, CR and LF.

## Concerns

- **CI now pays what a developer pays.** `.github/workflows/ci.yml` runs
  `check-hermes-node` in an ASAN-debug and a Release configuration on Linux
  and macOS; each run now cuts a kit (755 MB ASAN, 39 MB Release) and links
  up to four ~185 MB executables. Each producing test deletes its output on
  its last RUN line, so the artifacts do not accumulate, but the kit stays.
  A failing gated run no longer uploads them (commit `04b1f73`).
- **Nothing on this branch ran on macOS.** The design's macOS claims --
  ad-hoc linker-signing, `MH_SUBSECTIONS_VIA_SYMBOLS`, the merged-archive
  link -- are the spike's measurements, taken on a Mac before any of this
  code existed. `utils/make-kit.py`'s `libtool -static` branch is newer than
  the spike and has never executed anywhere. The first macOS CI run is the
  experiment.
- **The default kit location does not hold in this repo's build tree.**
  `resolveKitDir()` looks beside the running binary, which is right for a
  release layout and wrong for `bin/hermes-node` next to `kit/`. Developers
  pass `--kit=<build dir>/kit`.
- **`test/build-exe-natives.js` asserts the missing-sidecar message, not
  `err.code`.** A regression that changed only `code = 'MODULE_NOT_FOUND'` --
  the value an optional-dependency probe branches on -- would slip past. The
  pre-existing container-shaped sibling `test/bundle-natives-errors.js` has
  the identical gap.
- **`process.exitCode` is not honoured by this runtime in any mode.** Found
  by the Task 6 review and verified independently: `process.exitCode = 3`
  exits 0 under `hermes-node` and 3 under node v24.13.1, and the `'exit'`
  event receives 0, because `runHermesNode()` computes a local exit code and
  never reads the property. Pre-existing and mode-independent -- a plain
  script, `--bundle` and a produced executable behave alike -- so it is not a
  defect of this work. Recorded in `CLAUDE.md` under "Bootstrap Sequence"
  rather than pinned by a test; the `--build-exe` tests use
  `process.exit(3)` and an uncaught throw instead.

## Deliberately not done

Named here so they are not read as omissions; the design's "What does not
work" section carries the reasoning.

- Windows. Untestable for us, and nothing in the design is Windows-hostile.
- Cross-compilation. The one capability injection has that linking does not.
- Universal macOS binaries. The mechanism works on a toy case; nobody has cut
  a two-slice kit, and `ld -r`, `libtool` and `lipo` all emit a valid-looking
  empty slice for a missing architecture and exit 0.
- A self-contained Linux executable. It still needs ICU 74, `libstdc++.so.6`
  and `libgcc_s.so.1`; `HERMES_USE_STATIC_ICU=ON` is its own round.
- **Shipping a kit in a release.** `.github/workflows/release.yml` stages the
  binary alone, and there is no `install()` rule for the kit directory, so
  `--build-exe` on a released `hermes-node` fails with the missing-manifest
  error however the binary was obtained. Everything the feature needs to be
  usable outside a build tree exists; what is missing is the decision to add
  ~40 MB (Release) of kit to every release artifact, which is a product call
  and not one to make inside this round.
- Teaching the read-only verbs to open a produced executable.
- Accepting a `.js` entry directly. Strictly additive later, dispatched on
  the magic bytes.
