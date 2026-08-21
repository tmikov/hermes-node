# hermes-parser-native

## What this is

This is the Hermes native parser addon: a Node-API (N-API) replacement for
the WebAssembly build of the `hermes-parser` npm package. Upstream Hermes
publishes `hermes-parser` as a WebAssembly module compiled from the same
parser/AST/sema C++ code that ships in the Hermes engine itself. `hermes-node`
has no WebAssembly support, so that package cannot run here.

The native addon exposes the identical parse result (the same ESTree JSON,
verified byte-for-byte by `unittests/HermesParserNative` and by the
`examples/flow-bundler` and `examples/hermes-parser-ast` end-to-end
examples) through a `.node` addon instead
of a `.wasm` module. It lives on a Hermes fork branch that the `hermes/`
submodule -- pinned to a specific upstream commit for the rest of this
project -- cannot simultaneously point at, so it is vendored here as a
source copy rather than referenced as a second submodule.

## Provenance

- **Source repository:** `https://github.com/tmikov/hermes.git`
- **Branch:** `parser-native`
- **Commit:** `8e8a9a6ce8192d22cd94c6b0362926d37b59709d`
  (`git describe --tags --match "v*"` reports `v0.1.1-9392-g8e8a9a6ce`, i.e.
  9392 commits after tag `v0.1.1`)

Two directories were copied verbatim from that commit:

| Source (in the fork)                            | Copied to             |
| ------------------------------------------------ | ---------------------- |
| `tools/hermes-parser-native/`                     | `napi/` (10 files, `__tests__/` dropped) |
| `tools/hermes-parser/js/hermes-parser-native/`    | `package/` (129 files: 34 under `src/`, 92 under `dist/`, plus `package.json`, `README.md`, `LICENSE`) |

## How to re-sync

To pick up upstream changes from the `parser-native` branch:

1. Recopy the two directories listed above from the new commit, replacing
   `napi/` and `package/` here (re-apply the divergences below, notably
   `package/dist/` staying committed and `__tests__`/`__test_utils__`/
   `__benchmarks__`/`yarn.lock` staying excluded).
2. Regenerate `package/dist/` from the refreshed `package/src/`:
   ```sh
   cmake --build cmake-build-asan --target hermes-parser-native-dist
   ```
   This target is opt-in (not part of `ALL`) because it needs `npm` and
   Babel; a normal build of this repository needs no JavaScript toolchain.
3. Rerun the tests: `HermesParserNativeTest` and
   `HermesParserKindHashSyncTest` (via `check-hermes-node`, which builds and
   runs both as part of the unit test suite) and, if the change
   could affect parse output, `examples/flow-bundler`
   (`check-hermes-node-examples` in a Release build).
4. Update the commit SHA in this file and commit the result.

## After bumping the `hermes/` submodule

The addon derives a hash of the node-kind table from the submodule's
`include/hermes/AST/ESTree.def` at compile time (`napi/KindHash.h`) and
stamps it into every serialized parse result. The JavaScript side carries
its own copy of that number in `package/src/HermesParserKindHash.js` (and in
the transpiled `package/dist/HermesParserKindHash.js`), generated
independently by `scripts/genKindHash.js`, and refuses to deserialize a
result whose stamp disagrees. So a submodule bump that adds, removes,
reorders or reshapes an ESTree node desyncs the vendored package from the
addon, and the package stops parsing anything with a "node-kind table
mismatch" error.

`unittests/HermesParserKindHashSyncTest.cpp` catches that offline: it
recomputes the hash from the submodule's definitions and requires the
committed JavaScript to contain that exact number. It runs as part of
`check-hermes-node`, needs no network and no JavaScript toolchain. When it
fails after a submodule bump, rerun

```sh
cmake --build cmake-build-asan --target hermes-parser-native-dist
```

which regenerates `package/src/HermesParserKindHash.js` first and then all
of `package/dist/` from `package/src/`, and commit both.

## How regeneration stays reproducible

`scripts/regen-dist.sh` runs `npm ci`, not `npm install`, against a
committed `package-lock.json` whose `overrides` block pins roughly 105
transitive `@babel/*` packages. This is what makes regeneration
reproducible byte-for-byte: leave the overrides block and the lockfile
alone. A future maintainer who "cleans up" the overrides block will get a
different `dist/` the next time someone regenerates it, with no error to
flag the drift.

## Divergences from upstream

- `package/dist/` is committed here; upstream gitignores it and generates it
  at publish time.
- `__tests__/`, `__test_utils__/`, `__benchmarks__/` and `yarn.lock` were not
  copied.
- The wrapper `CMakeLists.txt` at the top of this directory replaces
  `napi/CMakeLists.txt`. The upstream file hardcodes
  `${CMAKE_SOURCE_DIR}/include/hermes/napi`, which resolves correctly in the
  Hermes tree it came from but not here; it is kept in place for provenance
  but is not used by the build.
- `scripts/regen-dist.sh`, plus its own `package.json`, `package-lock.json`
  and `babel.config.js`, replace upstream's `build-native.sh`, which assumes
  the Hermes JS workspace (a Yarn monorepo this repository does not have).
- `scripts/genKindHash.js` has one edited output path.
- The unit test target is `HermesParserNativeTest`, singular, not the
  fork's `HermesParserNativeTests`: `unittests/lit.cfg` discovers GoogleTest
  binaries by the singular `Test` suffix, so the plural name would never be
  collected.

## Limitations

- The addon builds for the host platform only; `linux-x64` is the only
  platform this project actually exercises.
- `package/dist/` is generated code, not source. Edit `package/src/` and
  regenerate with the opt-in `hermes-parser-native-dist` target (see "How to
  re-sync" above); do not hand-edit `dist/`.
- A consumer that depends on this package through a `file:` path -- both
  `examples/flow-bundler` and `examples/hermes-parser-ast` do, pointing at
  `external/hermes-parser-native/package` --
  must work around npm 11's default of `install-links=false`. That default
  symlinks a `file:` dependency into `node_modules` instead of copying it,
  and Node then resolves that package's own requires relative to the
  symlink's real target -- outside the consumer's directory -- where the
  vendored package's own dependency (`hermes-estree`) is not found even if
  the consumer declares it. Setting `install-links=true` makes npm copy the
  package instead, which fixes resolution regardless of what else is
  declared. Each example's `.npmrc` sets `install-links=true`, and each
  `package.json` also declares `hermes-estree` directly as a belt-and-braces
  measure.

## The Sema landmine

With the current `hermes/` submodule pin, `CheckImplicitReturn.cpp:248`
asserts on any `try { } catch { } finally { }` in parsed input. The split
that would prevent it is gated on `compile_` at `SemanticResolver.cpp:794`,
and the parser path here runs with `compile=false`. Assertions-enabled
builds (e.g. `cmake-build-asan`) abort on such input; Release builds are
unaffected because `resolveASTForParser` runs only the resolver and never
reads the result the assertion would guard.

Nothing under `examples/flow-bundler` or `examples/hermes-parser-ast`
triggers this (`hermes-parser-ast/sample.js` deliberately parses a class
private field and an optional chain, no `try`/`finally`). The fix lives on the
`sema-implicit-return-fixes` branch, intended for upstream; it has not been
pulled into `parser-native` or into this vendored copy.

## When to delete this directory

Once `hermes-node` supports WebAssembly, this vendored copy is no longer
needed. **Two examples depend on it, and they have different fates.**

`examples/flow-bundler` stays, but it does **not** survive untouched: it
names the addon and the `file:` dependency in four places (steps 3 to 6).

`examples/hermes-parser-ast` does **not** stay as it is. Its subject *is* a
native `.node` addon -- it exists to exercise the AOT bundle format's
native-addon support end to end, with an `--include` naming the addon, an
assertion that a sidecar was produced, and a `--verify-natives` check. The
published `hermes-parser` is a WebAssembly module, so repointing the
dependency leaves the example with nothing native to package and three
checks that can no longer pass. Delete it, or repoint it at some other real
`.node` addon (step 8). The lit coverage of bundled natives does not depend
on this decision: `test/bundle-natives.js`,
`test/bundle-natives-errors.js` and `test/bundle-verify-natives.js` use the
`hello_addon` fixture the top-level `CMakeLists.txt` builds, not this
package.

Do the CMake edits before the `git rm`, so that no reconfigure in the middle
of the sequence dies with `add_subdirectory given source ... which is not an
existing directory`. Better still, treat steps 1 to 6 and step 8 as one
change and build nothing until the end -- step 7 is that build.

1. Remove the CMake references, in this order and before deleting anything:
   - `add_subdirectory(external/hermes-parser-native)` in the top-level
     `CMakeLists.txt`;
   - `add_subdirectory(HermesParserNative)` in `unittests/CMakeLists.txt`,
     along with the `HermesParserKindHashSyncTest` block just above it
     (`add_node_compat_unittest`, its `target_include_directories` and its
     `target_compile_definitions`);
   - the `hermes-parser-napi` entry in the `DEPENDS` of the
     `check-hermes-node-examples` target in the top-level `CMakeLists.txt`.
2. `git rm -r external/hermes-parser-native unittests/HermesParserNative` and
   `git rm unittests/HermesParserKindHashSyncTest.cpp`. This takes the
   `POST_BUILD` step that stages the addon into `package/prebuilds/` and the
   `hermes-parser-native-dist` target with it, since both live in
   `external/hermes-parser-native/CMakeLists.txt`.

   Steps 3 to 7 below are `examples/flow-bundler`; step 8 is
   `examples/hermes-parser-ast`.

3. In `examples/flow-bundler/run.sh`, delete the whole block between the
   `# --- BEGIN vendored native parser addon` and
   `# --- END vendored native parser addon` marker lines, markers included.
   It resolves the built `.node` file, hard-fails if it is missing with a
   message naming the now-deleted `hermes-parser-napi` target, and exports
   `HERMES_PARSER_NATIVE_ADDON`. Leaving it in place makes
   `check-hermes-node-examples` fail rather than skip: `run-examples.sh` runs
   under `set -e`.
4. In `examples/flow-bundler/package.json`, repoint the `hermes-parser`
   dependency from `file:../../external/hermes-parser-native/package` at a
   published version on npm.
5. In `examples/flow-bundler/.npmrc`, delete the `install-links=true` line
   and the comment block above it. Its entire justification is the `file:`
   dependency. Keep `legacy-peer-deps=true`, which is about the
   `prettier@2.8.8` pin and is unrelated.
6. Regenerate `examples/flow-bundler/package-lock.json`. It carries the
   `file:` path twice -- once in the root `packages[""]` dependency map, and
   once as a `node_modules/hermes-parser` entry whose `"name"` is
   `hermes-parser-native` and whose `"resolved"` is the `file:` path.
   Repointing `package.json` alone leaves the lockfile describing the deleted
   directory. Regenerate it with a plain `npm install` in
   `examples/flow-bundler/`, which re-resolves the changed dependency and
   leaves every other pin in the lockfile alone.

   **Check the lockfile diff before committing it.** The example's expected
   bundles are byte-exact and depend on the pinned `@babel/generator` and
   `@babel/helpers` versions, not only on the parser; a re-resolution that
   moves them makes four of the six bundles mismatch for reasons that have
   nothing to do with parsing. The diff should touch only the
   `hermes-parser` and `hermes-estree` entries. See "A note on
   reproducibility" in `examples/flow-bundler/README.md`; this step is the
   one sanctioned exception to that file's "keep `package-lock.json`
   committed and unmodified".
7. Re-verify the example end to end before committing:
   ```sh
   cmake --build cmake-build-release --target hermes-node
   (cd examples/flow-bundler && rm -rf node_modules && npm ci)
   ./examples/flow-bundler/run.sh cmake-build-release
   ```
   It must still print `PASS: 6 bundles match expected/`.
8. Deal with `examples/hermes-parser-ast`. It has the same four touch
   points as flow-bundler -- a `# --- BEGIN vendored native parser addon` /
   `# --- END` marker block in its `run.sh`, the `file:` dependency in its
   `package.json`, `install-links=true` in its `.npmrc`, and the `file:`
   path twice in its `package-lock.json` -- but they cannot be fixed the
   same way, for the reason given at the top of this section.

   Note also that its marker block is **not self-contained**: it derives
   `PLATFORM_ARCH` from `hermes-node` itself and copies the built addon into
   `node_modules/hermes-parser/prebuilds/$PLATFORM_ARCH/`, and
   `PLATFORM_ARCH` is used further down by the `--include=` argument. So
   deleting the block alone leaves an undefined variable, and the three
   native-specific checks below it (the `--include`, the
   `out/hermes-parser.node` sidecar assertion, and `--verify-natives`) have
   nothing to assert against either way.

   Either `git rm -r examples/hermes-parser-ast` and drop its paragraph
   from `examples/README.md`, or repoint it at a real `.node` addon: keep
   `ast.js` and `sample.js` if the replacement parses, otherwise the
   example is really a new one and only the run.sh shape is worth reusing.
9. Update the prose that still describes a vendored addon:
   - `CLAUDE.md`, the bullet about `external/hermes-parser-native/`, and --
     if step 8 deleted the example -- the reference to
     `examples/hermes-parser-ast` in the AOT Bundles natives bullet;
   - `examples/README.md`, the `flow-bundler/` paragraph, and the
     `hermes-parser-ast/` paragraph if step 8 deleted it;
   - `examples/flow-bundler/README.md`, which mentions the vendored addon in
     its opening paragraph, in the `babel.config.js`/`package.json`/`.npmrc`
     layout bullets, and in the `hermes-parser-napi` build command under
     "Running it";
   - `examples/hermes-parser-ast/README.md`, if the example survives step 8:
     it names `external/hermes-parser-native/package/dist/HermesParserAddon.js`
     as the loader whose computed requires motivate `--include`;
   - `history/plans/2026-08-12-flow-bundler-example-design.md`, its plan, and
     `history/plans/2026-08-21-bundle-natives-{design,plan}.md` are
     historical records; leave them.
