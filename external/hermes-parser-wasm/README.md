# hermes-parser-wasm

## What this is

This is the WebAssembly build of the `hermes-parser` npm package: the Hermes
JavaScript/Flow parser compiled to wasm with emscripten, plus the JavaScript
that drives it. It is the counterpart to the sibling
`external/hermes-parser-native/`, which wraps the same parser as a Node-API
`.node` addon instead.

Both are vendored on purpose. They are not competing answers to "how should
this project parse JavaScript" -- they are two different pieces of runtime
surface for `hermes-node` to exercise. The native addon exercises
`process.dlopen`, the Node-API surface and `.node` resolution; this one
exercises `WebAssembly`, the emscripten glue's use of typed arrays and
`Function`, and a 900 KB module compiled at run time. Neither replaces the
other, and a change that breaks either is a regression.

`hermes-node` gained WebAssembly support when `HERMES_ENABLE_WASM` was turned
on by default; before that this package could not run here at all, which is
why the native addon was vendored first.

## Provenance

- **Source repository:** `https://github.com/tmikov/hermes.git`
- **Branch:** `hermes-parser-wasm-fixes`
- **Commit:** `9570d1246dee28cad33dc73145875825b41479d4`
  (`git describe --tags --match "v*"` reports `v0.2.1-9089-g9570d1246`)
- **Package version:** 0.37.0, the same version the sibling native package
  is built from.

Copied verbatim from that commit:

| Source (in the fork)                                   | Copied to      |
| ------------------------------------------------------ | -------------- |
| `tools/hermes-parser/js/hermes-parser/src/`              | `package/src/` |
| `tools/hermes-parser/js/hermes-parser/package.json`, `README.md`, `LICENSE` | `package/` |
| `tools/hermes-parser/js/scripts/genWasmParser.js`        | `scripts/` (one edited path) |

`package/dist/` is generated from `package/src/` plus an emscripten build of
the parser; see below.

### Why this branch, rather than npm

Two changes on it are not in the published `hermes-parser@0.37.0`, and one of
them is not in this repository's `hermes/` submodule either:

- `9570d1246` "Resolve source positions without sorting them" changes **C++
  inside the wasm binary** (`tools/hermes-parser/HermesParserJSSerializer.cpp`
  and a new `SourcePositionMap.h`). npm's prebuilt blob predates it, and the
  `hermes/` submodule has no `SourcePositionMap.h` at all -- it is a different
  lineage. So the wasm cannot be taken from npm and cannot be built from the
  submodule.
- `edcc55f62` "Allow the WASM parser to be built outside the internal build"
  makes an open-source build possible at all, and fixes `HermesParser.js` for
  emscripten 4.0.12 and later, where the `-sMODULARIZE` factory returns a
  Promise even under `-sWASM_ASYNC_COMPILATION=0`. Without it every parse
  fails with `cwrap` undefined. This is not optional for us: the committed
  blob is built with emscripten 6.0.6, which has that behaviour.

## How to re-sync

The wasm is built out of tree, from a Hermes checkout, because it needs
emscripten and nothing in this repository does:

```sh
source /path/to/emsdk/emsdk_env.sh
emcmake cmake -B build-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-wasm --target hermes-parser-wasm
```

Then, from this directory:

```sh
./scripts/regen-dist.sh /path/to/hermes/build-wasm/bin/hermes-parser-wasm.js
```

which reinstalls the pinned Babel toolchain, rebuilds `package/dist/` from
`package/src/`, drops the emscripten output in as
`dist/HermesParserWASM.js`, and writes `dist/build-manifest.json` last.

Recopy `package/src/` from the branch first if the JavaScript changed, update
the commit SHA above, and commit the result.

The committed `dist/` was built with **emscripten 6.0.6**
(`ce75e06884093bcefb86a6b8fd56a5d62a4cc245`), the version commit `edcc55f62`
records as verified.

## How regeneration stays reproducible

`scripts/regen-dist.sh` runs `npm ci`, not `npm install`, against a committed
`package-lock.json` whose `overrides` block pins roughly 105 transitive
`@babel/*` packages -- the same block the sibling native package uses, for the
same reason. Leave it alone; a future maintainer who "cleans it up" will get a
different `dist/` from the next regeneration, with no error to flag the drift.

That reproducibility is checkable rather than asserted. Regenerating from this
branch's `src/` produces `dist/` files that are **byte-for-byte identical to
the published `hermes-parser@0.37.0`** for 83 of the 86 files npm ships. The
three that differ are `HermesParser.js` and its two copies
(`HermesParser.js.flow`, `dist/src/HermesParser.js`), and the difference is
exactly commit `edcc55f62`'s emscripten fix. If a future regeneration diverges
anywhere else, the toolchain has drifted.

## Divergences from upstream

- `package/dist/` is committed here; upstream gitignores it and generates it
  at publish time. That is what keeps a build of this repository free of any
  JavaScript toolchain and of emsdk.
- `package/src/` is committed here; upstream publishes only `dist/`.
- `__tests__/`, `__test_utils__/` and `yarn.lock` were not copied.
- `scripts/regen-dist.sh`, with its own `package.json`, `package-lock.json`
  and `babel.config.js`, replaces upstream's `build.sh`, which drives a Yarn
  monorepo this repository does not have and rebuilds six packages when we
  want one.
- `scripts/genWasmParser.js` has one edited output path: upstream's copy sits
  in `tools/hermes-parser/js/scripts` and writes to a sibling package
  directory, where here `scripts/` and `package/` are siblings.
- `scripts/babel.config.js` deliberately **omits** the
  `babel-plugin-syntax-hermes-parser` override that the native package's copy
  carries. Upstream disables that override for its `BOOTSTRAP_PACKAGES`, and
  `hermes-parser` is one of them -- the plugin depends on this very package,
  so it cannot build it. The consequence is not cosmetic: with the override
  on, Babel parses through hermes-parser and drops comments that
  `@babel/parser` keeps, which removed every `$FlowExpectedError` annotation
  and every MIT license header and made 52 of 56 files differ from npm's
  published `dist/`. `regen-dist.sh` runs `flow-remove-types` before Babel for
  the same upstream reason.

## Limitations

- The package's own name in `package.json` is `hermes-parser`, matching
  upstream. A consumer that wants both parsers installed at once must alias
  at least one of them, since the native package is separately named
  `hermes-parser-native`.
- A consumer depending on this through a `file:` path must set
  `install-links=true` in `.npmrc`, exactly as the native package's consumers
  do: npm 11 otherwise symlinks the dependency and resolves its own
  `hermes-estree` requirement outside the consumer's directory. See the
  sibling README for the full explanation.
- `package/dist/` is generated code. Edit `package/src/` and regenerate; do
  not hand-edit `dist/`.
- **Nothing guards the pairing of `HermesParserWASM.js` with
  `HermesParserNodeDeserializers.js`.** The native addon stamps a hash of the
  node-kind table into every parse result and refuses a mismatch; this package
  has no equivalent, because upstream ships both halves from one build and
  never had the problem. Here they must come from the same Hermes commit, and
  a mismatched pair would deserialize into the wrong node kinds silently. That
  the whole `dist/` is regenerated by one script from one checkout is what
  currently keeps them in step.

## Verified

Parsing `libjs-node/vm.js` (12,970 bytes) produces a 3015-node ESTree AST
serializing to 485,044 bytes, with SHA-256
`efafa8997076be4c28c0649b62784139e7e7a8e73e58daaf1a681b82ac50ef58` under all
of: node + npm's `hermes-parser`, node + this package, `hermes-node` + this
package, and `hermes-node` + the native addon. The two vendored parsers and
the published one agree byte for byte.
