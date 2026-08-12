# hermes-parser-native

## What this is

This is the Hermes native parser addon: a Node-API (N-API) replacement for
the WebAssembly build of the `hermes-parser` npm package. Upstream Hermes
publishes `hermes-parser` as a WebAssembly module compiled from the same
parser/AST/sema C++ code that ships in the Hermes engine itself. `hermes-node`
has no WebAssembly support, so that package cannot run here.

The native addon exposes the identical parse result (the same ESTree JSON,
verified byte-for-byte by `unittests/HermesParserNative` and by the
`examples/flow-bundler` end-to-end example) through a `.node` addon instead
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
- A consumer that depends on this package through a `file:` path (as
  `examples/flow-bundler` does, pointing at `external/hermes-parser-native/package`)
  must work around npm 11's default of `install-links=false`. That default
  symlinks a `file:` dependency into `node_modules` instead of copying it,
  and Node then resolves that package's own requires relative to the
  symlink's real target -- outside the consumer's directory -- where the
  vendored package's own dependency (`hermes-estree`) is not found even if
  the consumer declares it. Setting `install-links=true` makes npm copy the
  package instead, which fixes resolution regardless of what else is
  declared. `examples/flow-bundler/.npmrc` sets `install-links=true`; its
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

Nothing under `examples/flow-bundler` triggers this. The fix lives on the
`sema-implicit-return-fixes` branch, intended for upstream; it has not been
pulled into `parser-native` or into this vendored copy.

## When to delete this directory

Once `hermes-node` supports WebAssembly, this vendored copy is no longer
needed:

1. `git rm -r external/hermes-parser-native unittests/HermesParserNative`.
   This removes the `POST_BUILD` step that copies the built addon into
   `package/prebuilds/` and the `hermes-parser-native-dist` target along
   with it, since both live in `external/hermes-parser-native/CMakeLists.txt`.
2. Repoint `examples/flow-bundler`'s `hermes-parser` dependency at the
   published `hermes-parser` package on npm instead of the `file:` path.
3. Remove the two now-dangling `add_subdirectory` lines that referenced the
   deleted directories: `add_subdirectory(external/hermes-parser-native)` in
   the top-level `CMakeLists.txt`, and `add_subdirectory(HermesParserNative)`
   in `unittests/CMakeLists.txt`. Also drop the `hermes-parser-napi`
   dependency of the `check-hermes-node-examples` target.
