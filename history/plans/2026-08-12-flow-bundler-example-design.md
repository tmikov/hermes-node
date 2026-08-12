# Design: Flow Bundler Example and Vendored Native Parser

**Status:** Design approved 2026-08-12. Implementation plan to follow.

## Goal

Add an example that runs the Hermes Flow bundler — a real Node build tool —
under `hermes-node`, end to end, with a mechanical check that its output is
correct. Doing so requires vendoring the Hermes native parser addon into this
repository.

## Context

The bundler parses JavaScript through Babel, and Babel is configured to
delegate parsing to `hermes-parser`. The package published on npm under that
name is a WebAssembly blob, and `hermes-node` has no WebAssembly:

```
$ hermes-node -e 'console.log(typeof WebAssembly)'
undefined
```

So the example cannot use the published package at all. It needs
`hermes-parser-native`, a Node-API addon that replaces the wasm blob with a
compiled shared library. That addon exists on the `parser-native` branch of
`tmikov/hermes` and is not published to npm.

The `hermes/` submodule tracks `n-api` and must keep pointing at a branch that
can plausibly be upstreamed, so it cannot be moved to `parser-native`. A second
submodule was considered and rejected: it doubles a 221 MB checkout plus 122 MB
of git objects, and leaves two Hermes trees at different commits in one
repository.

This whole arrangement is **temporary**. When `hermes-node` gains WebAssembly
support, the example switches to the published `hermes-parser` and the vendored
copy is deleted.

## Scope

In scope:

- A vendored copy of the native parser addon (C++ and npm package) under
  `external/hermes-parser-native/`.
- CMake wiring to build the addon as part of the normal build.
- An opt-in target to regenerate the package's `dist/` from its `src/`.
- A self-contained `examples/flow-bundler/` that bundles a fixture and diffs
  the result against committed expected output.
- An opt-in `check-hermes-node-examples` target.

Out of scope:

- Publishing `hermes-parser-native` to npm.
- Porting the fork's JavaScript test suites (differential tests against the
  wasm parser, `DistFreshness`, `ForkDrift`). Those need `node`, `jest` and a
  network install, and they compare against packages that live in the Hermes
  fork rather than here.
- Any change to the `hermes/` submodule pin.

## Design

### 1. Layout

```
external/hermes-parser-native/
  README.md          provenance: source repo, branch, commit SHA; what was
                     copied; how to re-sync; known limitations; when to delete
  CMakeLists.txt     wrapper target, following the external/llhttp pattern
  napi/              verbatim from hermes/tools/hermes-parser-native/
                     (12 files, 100 KB)
  package/           verbatim from
                     hermes/tools/hermes-parser/js/hermes-parser-native/
                       src/    Flow + ESM sources, kept for regeneration
                       dist/   committed CommonJS — what actually runs
                       package.json, README.md, LICENSE
  scripts/           build-native.sh, genKindHash.js, distManifest.js
```

The addon's own C++ unit tests go where this repository keeps unit tests rather
than inside `external/`:

```
unittests/HermesParserNative/
  CMakeLists.txt     add_node_compat_unittest(HermesParserNativeTest ...)
  5 test sources     verbatim from hermes/unittests/HermesParserNative/
```

They compile two of the addon's translation units directly
(`HermesParserJSSerializer.cpp`, `HermesParserDiagHandler.cpp`) and include
headers from `external/hermes-parser-native/napi`, so the copied `CMakeLists.txt`
needs its paths adjusted. The target is renamed from the fork's
`HermesParserNativeTests` to **`HermesParserNativeTest`**: Lit's GoogleTest
discovery in `unittests/lit.cfg` matches the suffix `Test`, and the plural form
would silently never be collected.

Each half is a verbatim copy, so re-syncing from the fork is two directory
copies. Removing the whole arrangement is `git rm -r` of two directories.

`external/` is the right home: it is this repository's convention for vendored
third-party source with a wrapper `CMakeLists.txt` and `README.md` in the outer
directory. `vendored/` is not — that holds JavaScript packages compiled into the
`hermes-node` binary through the embedded `vendored-packages` map.

**Deliberate divergence from the fork:** `dist/` is gitignored upstream and
committed here, so that building this repository never requires a JavaScript
toolchain. The vendored `.gitignore` is adjusted accordingly and the README
records why.

### 2. C++ build

The wrapper `CMakeLists.txt` declares the target itself rather than reusing the
fork's, matching how `external/llhttp` handles vendored llhttp. This avoids
patching the inner copy: the fork's version hardcodes
`${CMAKE_SOURCE_DIR}/include/hermes/napi`, which resolves to the Hermes root
there and to this repository's root here. The wrapper uses
`${PROJECT_SOURCE_DIR}/hermes/include/hermes/napi`, the idiom already used by
`lib/bindings`, `lib/runtime`, `lib/module-loader` and others.

The target is a `MODULE` library with `PREFIX ""`, `OUTPUT_NAME hermes-parser`
and `SUFFIX .node`, linking `hermesAST`, `hermesParser`, `hermesSema` and
`LLVHSupport` — all four already built for `hermes-node`, so the incremental
cost is three translation units and a link. The `napi_*` symbols are left
undefined and resolve from the host process at `dlopen` time, which is what lets
one binary work in both Node and `hermes-node`.

It builds as part of the normal build.

A `POST_BUILD` step copies the result to
`package/prebuilds/<platform>-<arch>/hermes-parser.node`, which is gitignored.
That is the *last* of the three locations the package's own loader checks
(`HERMES_PARSER_NATIVE_ADDON`, then a hardcoded fork-only development path that
never resolves here, then `prebuilds/`), and the only one that works in this
repository without help from the caller, so the vendored package behaves like a
published one for any ad-hoc consumer. The example is not such a consumer:
`run.sh` sets `HERMES_PARSER_NATIVE_ADDON` unconditionally so that it tests the
build directory it was given rather than whatever was staged last. (Corrected
after the final review; the original text said `prebuilds/` was checked first
and that the staging is what lets the example need no environment variable.
Neither was true.)

### 3. Regenerating `dist/`

A `hermes-parser-native-dist` target, deliberately **not** part of `ALL`, runs
the vendored `build-native.sh` adapted to this layout. It needs `yarn` or `npm`
and Babel, and fails with a clear message when they are missing. Nothing else in
the build depends on it.

`dist/build-manifest.json` stays committed. It records a content hash per source
file, so a `dist/` that has drifted from `src/` is detectable by content rather
than by timestamp.

### 4. The example

`examples/flow-bundler/` is fully self-contained. It does not reference the
`hermes/` submodule, so a submodule bump cannot break it and the bundler's
`benchmarks/` directory — which is not an API and carries no stability promise —
is not a dependency.

```
examples/flow-bundler/
  package.json        the bundler's dependencies, plus
                      "hermes-parser":
                        "file:../../external/hermes-parser-native/package"
  package-lock.json
  bundler/            verbatim from flow-bundler/src/     8 files, 116 KB, MIT
  fixture/src/        verbatim from MiniReact/no-objects/src/
                                                          21 files, 200 KB, MIT
  expected/           the 6 committed .js bundles         276 KB
  babel.config.js     copied (MIT); plugin paths pointed at this node_modules
  babel-register.js   written fresh (see Licensing below)
  build.config.js     written fresh (see Licensing below)
  run.sh              bundle, then diff against expected/
  README.md
```

Roughly 600 KB and about 40 files.

**Why `file:` works.** npm installs a `file:` dependency under the *key*, not
under the package's own name, so `require('hermes-parser')` resolves even though
the vendored package is named `hermes-parser-native`. This matters because
`babel-plugin-syntax-hermes-parser` requires it by name internally. Verified
directly; no rename and no yarn `resolutions` hack is needed.

**Why the configs are rewritten rather than copied.** The bundler's
`babel.config.js` resolves every preset and plugin through
`path.resolve(__dirname, 'node_modules')`, and `build.config.js` uses
`require.resolve(..., {paths: [FLOW_BUNDLER_ROOT]})`. Both point at the
directory they live in. Once the bundler is copied under `bundler/` with the
example's `node_modules` one level up, those paths have to change. The example's
`babel-register.js` also has to set `configFile`, `root` and `only` explicitly,
since Babel otherwise resolves configuration relative to each compiled file.

**Why the expected output can be copied verbatim.** The generated bundles embed
no paths. The only path-like string in `out/simple.js` is
`//# sourceMappingURL=simple.js.map`, a bare filename. Relocating the sources
therefore does not change the bundle bytes, and the committed bundles are valid
expectations without regeneration.

**Source maps are produced but not checked.** The six `.map` files total 460 KB
and their `sources` arrays are paths relative to the output directory. They add
little verification value for their size.

**Licensing.** Two files in the natural copy set are marked "Confidential and
proprietary": `flow-bundler/babel-register.js` and
`MiniReact/no-objects/build.config.js`. Neither is copied. Both are files the
example must author fresh anyway for the path reasons above. Everything actually
copied is MIT: the 21 fixture sources and the bundler sources carry the standard
MIT header, as does the generated output derived from them. `babel.config.js` is
the one exception to the *header*, not to the licence: upstream gives it a
copyright line and `@format` with no licence paragraph, and it is copied with
that header verbatim rather than "completed" to the standard one. (Corrected
after the final review, which found that the copy had gained an MIT clause the
original does not have.)

**Accepted cost.** This forks the bundler; it will not pick up upstream fixes.
The README records the source commit. Given the arrangement is temporary, this
is preferred over coupling the example to the submodule's `benchmarks/`
directory.

### 5. Test target

`check-hermes-node-examples`, **not** part of `check-hermes-node`. It runs the
example when `examples/flow-bundler/node_modules` exists and skips with a clear
message otherwise. The default suite stays offline and finishes in about 20
seconds; the example needs a network install of Babel, prettier and yargs.

### 6. Documented limitations

In `external/hermes-parser-native/README.md`:

- The addon builds for the host platform only. `linux-x64` is the only platform
  ever exercised.
- `dist/` is committed generated code; regenerate with the opt-in target after
  changing `src/`.
- With the current submodule pin, `CheckImplicitReturn.cpp:248` asserts on any
  `try { } catch { } finally { }` in parsed input, because the split that would
  prevent it is gated on `compile_` at `SemanticResolver.cpp:794` and the parser
  path runs with `compile=false`. An assertions-enabled build therefore aborts on
  such input. Release builds are unaffected: `resolveASTForParser` runs only the
  resolver, so the wrong value is computed and never read. The example's own
  inputs contain no `try/catch/finally` and no Flow `match`, verified by
  inspection, so it works in every configuration today. The fix exists on the
  `sema-implicit-return-fixes` branch and is intended for upstream; when it
  lands, this note goes away.

### 7. Removal path

When `hermes-node` supports WebAssembly, the outline is: drop the CMake
references first, then `git rm` the two directories, then repoint the example's
dependency from the `file:` path to `hermes-parser` from npm.

The example survives, but **not unchanged**: it names the addon in `run.sh`, in
`package.json`, in `package-lock.json` (twice) and in `.npmrc`. (Corrected after
the final review, which found the original "the example itself survives
unchanged" false and the step order able to strand a mid-sequence CMake
reconfigure.)

The operative, step-by-step recipe is "When to delete this directory" in
`external/hermes-parser-native/README.md`. It is the one that is maintained;
this section is a historical sketch.

## Testing

- The addon compiles as part of the normal build on every configuration already
  built, including Debug+ASAN.
- `examples/flow-bundler/run.sh` bundles the fixture and diffs six `.js` files
  against `expected/`, exiting non-zero on any mismatch.
- `check-hermes-node-examples` runs that script when the example has been
  installed, and skips otherwise.
- The vendored addon's own C++ unit tests (5 sources) are copied to
  `unittests/HermesParserNative/` and registered with the existing
  GTest-under-Lit suite, so they run under `check-hermes-node` with no network
  and no JavaScript toolchain.
- `unittests/HermesParserKindHashSyncTest.cpp` — added after the final review —
  recomputes the ESTree node-kind hash from the `hermes/` submodule and requires
  the committed `package/{src,dist}/HermesParserKindHash.js` to contain it, so a
  submodule bump that desyncs the vendored package fails offline instead of only
  in the opt-in example.

## Verification of the claims above

Each factual claim in this document was checked against the tree rather than
assumed:

- `typeof WebAssembly` is `undefined` under `hermes-node`.
- `hermesAST`, `hermesParser`, `hermesSema` and `LLVHSupport` already exist as
  targets in `cmake-build-asan`.
- The npm `file:` key-aliasing behaviour was tested with a scratch package.
- `out/simple.js` contains no embedded source paths.
- The two proprietary headers were located by grep across the copy set.
- The assert predicate, its `compile_` gate, and the absence of any
  `try/catch/finally` or Flow `match` in the example's inputs were each read or
  grepped directly.
