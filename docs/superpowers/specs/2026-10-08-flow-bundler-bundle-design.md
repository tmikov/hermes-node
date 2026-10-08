# Design: bundling the Flow bundler example

**Status:** Draft for review, 2026-10-08.

`examples/flow-bundler` runs the Hermes benchmark suite's Flow bundler under
hermes-node, loading its Flow-typed ESM sources through `@babel/register`
(`babel-register.js`, preloaded with `-r`). Its README has a section, "Why
there is no build-bundle.sh", arguing that this example cannot be bundled.
This design produces all three artifact kinds from it anyway -- an AOT
container, a `--build-exe` executable and a `build-native` executable -- each
checked against the same six `expected/` bundles the `-r` run is.

**Scope is this example.** No hermes-node change is made or needed. A general
"apply the program's require hook at build time" producer feature was
considered and explicitly set aside; nothing here forecloses it, and the
staging trick below is specific enough to this example that it should not be
mistaken for one.

## Why the README's conclusion no longer holds

The README makes two observations, and both remain true:

1. The entry, `bundler/buildBundleCLI.js`, is Flow-typed ESM, which Hermes's
   compiler cannot parse, and a parse failure in the entry is a hard
   producer error.
2. In a bundle `require.extensions` sits outside the closed world, so a
   packaged `@babel/register` hook is installed and then never consulted.

What the README concluded from them -- that pre-transpiling `bundler/` would
"no longer be this example" -- was too strong. The example demonstrates two
separable things:

- **Babel parsing Flow with a vendored `hermes-parser` as sources load.** That
  is the `-r` run in `run.sh`, which this design leaves untouched.
- **The bundler's own run-time work**: `@babel/core`, `@babel/generator`,
  three transform plugins, `hermes-transform`, `prettier` and `hermes-parser`
  parsing and rewriting the fixture app. That half has no dependency on the
  hook at all, and it is exactly what an artifact exercises.

Transpiling the bundler's eight source files before bundling removes the
hook from the picture without removing the second half from the example.
The README section is rewritten to say this; both observations stay in it,
since they are still the reason the transpile step exists.

## Decisions

- **Three artifacts**: `dist/flow-bundler.hbb`, `dist/flow-bundler` (from
  `--build-exe`), `dist/flow-bundler-native` (from `build-native`).
- **WebAssembly parser only**, baked into every artifact. No artifact needs a
  sidecar file; the only things beside it at run time are its input and its
  output. The native addon stays covered by the existing `-r` run.
- **Configuration is code, so it is baked in.** See "Running an artifact".
- **Verification is a separate script**, `check-artifacts.sh`, not new arms
  in `run.sh`. See "Verification".

## Build pipeline (`build-bundle.sh`)

`build-bundle.sh [out-dir] [build-dir]`, defaulting to `./dist` and
`cmake-build-release`, matching the other examples' `build-bundle.sh`.
Everything it writes lands under `out-dir`; `dist/` joins
`examples/flow-bundler/.gitignore`.

### 1. Stage

`<out>/stage/` is deleted and rebuilt on every run:

- `stage/node_modules/` is a real directory holding **one symlink per entry**
  of the example's `node_modules/` (a scope directory such as `@babel` is
  linked whole), except `hermes-parser`, which is a symlink to
  `hermes-parser-wasm`.
- `stage/bundler/` receives the transpiled sources (step 2).
- `stage/build.config.js` is a copy of `build.config.js`, byte for byte.
- `stage/fixture/src` is a symlink to `fixture/src`, so the unbundled
  recording run in step 3 finds its input where `build.config.js` looks.

**Why staging, rather than `parser-select.js`.** `parser-select.js` chooses
the parser by patching `Module._resolveFilename` at run time. A bundle never
gets there: the bundle loader's `Module._load` answers a bundled
`require('hermes-parser')` from the edge table the producer recorded, and
consults `_resolveFilename` only for requires it did not build itself
(`libjs/bundle-loader.js`). So the redirect has to happen at discovery time.
Staging does it there with no hermes-node change, for two measured reasons:

- **The producer never resolves symlinks** (`lib/bundle/bundle_resolve.cpp`,
  its documented no-realpath policy), so a file reached through
  `stage/node_modules/hermes-parser` keeps that spelled identity and the
  bundle root stays `stage/`.
- **There is exactly one `hermes-parser` in the installed tree**, at the top
  level -- every package that declares it (`hermes-transform`,
  `hermes-eslint`, `babel-plugin-syntax-hermes-parser`), and the bundler's
  own `utils.js`, resolves to that one
  directory. The single top-level redirect therefore covers the whole graph,
  and the native package never enters the container. `build-bundle.sh`
  asserts this (no `node_modules/*/node_modules/hermes-parser`) rather than
  assuming it, since an `npm install` that nested a copy would silently put
  the native parser back into some corner of the graph.

**The redirect works for the producer only, and that asymmetry is a trap.**
hermes-node's run-time disk loader resolves symlinks to real paths, as Node
does, so a package loaded through the farm runs from the example's real
`node_modules/` and resolves `hermes-parser` beside its real path -- the
native package. Measured: running the staged tree unbundled, with nothing
else, records **zero** Wasm entries and still produces six matching outputs,
because it quietly used the addon. Step 3 therefore preloads
`parser-select.js` as well.

The README rejected swapping `node_modules/hermes-parser` on disk because it
leaves the real tree in whichever state ran last. Staging never touches the
real tree, so that objection does not apply.

### 2. Transpile

A new `transpile.js`, run **under hermes-node**, walks `bundler/` and writes
each `.js` file through `@babel/core`'s `transformFileSync` with
`babel.config.js`'s `presets` and `plugins`, `configFile: false`,
`babelrc: false` and `root` set to the example directory -- the same options
`babel-register.js` hands `@babel/register`. It requires `./parser-select`
first with `FLOW_BUNDLER_PARSER=wasm`, so the transpile parses with the
same parser the artifacts carry and never looks for the addon.

Measured: 8 files, 0.85 s.

Approach B -- running the installed `@babel/register` hook itself and
capturing what it hands `Module.prototype._compile` -- was considered. It
gives parity with the `-r` run by construction, but depends on `pirates`'
internals, and `@babel/register` is `transformSync` with these options, so
calling Babel directly yields the same output without the trick.

### 3. Record the Wasm module

Run the staged tree **unbundled**, from disk, with `--record-wasm` and the
Wasm parser selected at run time:

```
FLOW_BUNDLER_PARSER=wasm hermes-node -r ./parser-select.js \
  --record-wasm=<out>/stage/parser.wasmrec \
  stage/bundler/buildBundleCLI.js -c stage/build.config.js
```

`-r ./parser-select.js` is not optional; see the trap at the end of step 1.
The script then **asserts the record holds exactly one entry** (by
`--dump-wasm`), because the failure without the preload is silent: correct
output, an empty record, and artifacts that compile the parser at every
launch.

A Wasm cache entry is keyed by a digest over its bytes alone, so a recording
taken from the unbundled run names the same entry the artifacts will look
up -- measured, the same digest (`sha256:14f4b4a0...`, 2,050,672 bytes) as
a recording taken from a bundled run. This doubles as a check that the
staging works before anything is built from it, and saves the producer pass
that recording from a first, unbaked container would cost (15 s measured).
The run writes `stage/out/`, which is discarded.

### 4. Build the artifacts

From `stage/`, with one shared set of `--include` flags:

```
--include=../build.config.js
--include=@babel/plugin-transform-flow-strip-types
--include=@babel/plugin-transform-class-properties
--include=@babel/plugin-transform-classes
```

`--include` resolves from the entry's directory, `stage/bundler/`, hence
`../build.config.js`. The three plugins are reached only through
`build.config.js`'s `plugin()` helper, a computed `require.resolve`, which the
scanner cannot follow.

- `--build-bundle=<out>/flow-bundler.hbb --bake-wasm=stage/parser.wasmrec`
- `--build-exe=<out>/flow-bundler --kit=<build>/kit <out>/flow-bundler.hbb`,
  when the kit exists
- `build-native stage/bundler/buildBundleCLI.js -o <out>/flow-bundler-native
  --kit=<build>/kit --bake-wasm=stage/parser.wasmrec` plus the includes, when
  the kit carries `shermes`

The latter two are skipped with a message, as `examples/tetris/build-bundle.sh`
does, when their prerequisites are absent.

**The producer's warnings are expected, and the script does not assert
their absence** (contrast tetris, whose graph produces none). Measured, they
are:

- eight unresolvable optional probes -- `metro-react-native-babel-preset`
  (a branch of `buildBundleCLI.js` this config does not take),
  `prettier/plugins/*` (Prettier 3 paths; this example pins 2.8.8),
  `@babel/preset-typescript`, `v8`, `supports-color`;
- 17 computed requires in 8 files and 4 `require`-as-value escapes;
- `@babel/core/lib/config/files/import.cjs`, packaged as a throwing stub
  (`import()` inside a `.cjs`; the same file the native-compilation design's
  Task 17 was about).

The artifacts run correctly with all of them in place. The script prints
the producer's output so they stay visible; it does not filter them.

Measured (Release, Linux x86-64): the container is 1,331 modules, 19,824,504
bytes baked (17,773,792 unbaked) and takes 15 s to build; the `--build-exe`
executable is 34,911,504 bytes and links in 0.4 s. `build-native` compiles
1,243 modules natively, plus the `import.cjs` stub, into a 122,726,688-byte executable in 3 min 34 s wall at `--jobs=12` on a
16-core machine, 1,280 s of CPU; GNU `time -v` reported 1.66 GB maximum
resident set size, which is the largest single process in the build tree,
not the sum of concurrent jobs. `build-bundle.sh` passes no `--jobs`, so the
producer's own default applies.

## Running an artifact

An artifact reads its input from `<artifact dir>/fixture/src` and writes
`<artifact dir>/out/`, because those paths come from `build.config.js`'s
`__dirname`, and a bundled module's `__dirname` is its identity re-rooted at
the artifact's directory. `build-bundle.sh` therefore links
`<out>/fixture/src` to the example's fixture, so the artifacts run where
they were built.

The config file is named with `-c` exactly as in the `-r` run, but **the
path names a file that need not exist**: `-c <artifact dir>/build.config.js`
is answered from the container, because `buildBundleCLI.js` `require`s it,
and a closed world does not load code from disk. That is the honest
consequence of a closed world, not a workaround to hide: a different config
means a different artifact. A config passed that is not the packaged one
throws `MODULE_NOT_FOUND` naming `--include`, which is the message to see.

Arguments: the `.hbb` needs `--` before them (`hermes-node
--bundle=flow-bundler.hbb -- -c ...`), since `-c` would otherwise be parsed as
a hermes-node flag. The executables take them directly.

## Verification (`check-artifacts.sh`)

`check-artifacts.sh [build-dir]` runs `build-bundle.sh` into a temporary
directory, then for each artifact that was built:

1. copies the artifact **alone** into a fresh empty directory, plus a copy of
   `fixture/src` -- no `node_modules`, no `bundler/`, no staging tree, no
   `build.config.js` on disk;
2. runs it with `HERMES_NODE_DISABLE_COMPILE_CACHE=1` and
   `HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE`;
3. requires all six `out/*.js` to `cmp` equal to `expected/`;
4. requires exactly one `wasm container hit` and no `wasm store` in the
   trace, which is what proves the Wasm module was baked rather than
   compiled. Asserted from tracing, never from timing.

Before any of that, it checks the `.hbb` structurally with `--dump`: the
module table must list `node_modules/hermes-parser/dist/HermesParserWASM.js`
and must not list `HermesParserAddon.js`, and there must be no `NATIVES`
section. Measured, that holds. It is what pins the staging redirect
directly, rather than inferring it from the Wasm hit -- the hit says the
Wasm parser ran, not that the native one is absent.

Measured, for all three: six of six match and one `wasm container hit`;
1.3 s for the `.hbb` and the `--build-exe` executable, 1.2 s for the native
one (single runs, not a benchmark). The unbaked container took 6.0 s, with
the trace showing `wasm container miss` then `wasm store` instead.

**Why a separate script rather than arms in `run.sh`.** `run.sh` is what
`check-hermes-node-examples` runs (`examples/run-examples.sh`), and tetris and
ditz2 both put their artifact arms there. This example's `build-native`
does not fit that budget: 3.5 minutes of wall clock and over 20 minutes of
CPU, against the 15 s `run.sh` costs today. `run.sh` stays exactly as it is,
so the examples suite keeps its current cost.

## README

"Why there is no build-bundle.sh" is replaced by a section explaining:

- the two observations above, unchanged in substance, as the reason a
  transpile step exists;
- why transpiling does not make this a different example;
- the staging directory, and why `parser-select.js` cannot do this job
  inside a bundle;
- the baked configuration and the `--` argument separator;
- the expected producer warnings;
- the measured sizes and timings.

The "Layout" list gains `build-bundle.sh`, `transpile.js` and
`check-artifacts.sh`.

## Not in scope

- A general producer feature for require hooks (see Scope).
- Packaging the native parser addon into any artifact.
- Adding these artifacts to `check-hermes-node-examples`.
- Teaching the producer to follow `build.config.js`'s computed
  `require.resolve`, or anything else that would make the `--include` flags
  unnecessary.
