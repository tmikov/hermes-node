# flow-bundler

Runs the Hermes benchmark suite's Flow bundler end to end under hermes-node:
hermes-node loads the bundler through a `-r` require hook, and Babel parses
the bundler's own sources and the fixture app with `hermes-parser`. `run.sh`
then checks the bundler's output against committed expected output.

`hermes-parser` here is vendored, and vendored twice -- as a Node-API addon
(`external/hermes-parser-native`) and as a WebAssembly module
(`external/hermes-parser-wasm`). `run.sh` runs the bundler once with each and
requires both to produce the same six bundles. That is the check worth
having: the two are meant to be interchangeable, this is the largest real
codebase in the tree to try it on, and the expected output was generated
upstream by `node` running the WebAssembly parser -- so a pass says the two
vendored parsers agree with each other *and* with the published one.

The wasm build could not run here at all until hermes-node gained
WebAssembly, which is why the native addon was vendored first and why this
example ran only against it for a while.

```sh
./run.sh                              # both parsers
FLOW_BUNDLER_PARSER=wasm ./run.sh     # just one
FLOW_BUNDLER_VERBOSE=1 ./run.sh       # have babel-register name its choice
```

## Layout

- `bundler/` -- the Flow bundler itself (`BuildBundle.js`, `ModuleGraph.js`,
  etc.), copied verbatim from
  `hermes/benchmarks/build-helpers/flow-bundler/src/` at submodule commit
  `9aaccbe5e9a975dfbd0f59e51c253d902d9f3a65` (`v0.2.1-9087-g9aaccbe5e`).
- `fixture/src/` -- the MiniReact "no-objects" benchmark app the bundler
  bundles, copied verbatim from `hermes/benchmarks/MiniReact/no-objects/src/`
  at the same commit.
- `expected/` -- the six bundles (`simple.js`, `simple-stripped.js`,
  `simple-lowered.js`, `music.js`, `music-stripped.js`, `music-lowered.js`)
  that `hermes/benchmarks/MiniReact/no-objects/out/` ships, generated
  upstream by `node` running the WebAssembly `hermes-parser`. The bundles
  embed no paths, so relocating the sources into this directory does not
  change them.
- `babel.config.js` -- copied verbatim, license header included, with one
  explanatory comment added above the `NODE_MODULES` line; nothing else
  changed, since that path was already correct by construction. Note that
  upstream's header for this file is shorter than the MIT header the rest of
  the copied sources carry: copyright line and `@format`, no license
  paragraph. It is reproduced as-is rather than "completed".
- `babel-register.js`, `build.config.js` -- written fresh rather than
  copied. Both resolve paths relative to their own directory in the Hermes
  tree, which no longer applies here, and both carry a
  "Confidential and proprietary" header in the upstream tree.
- `package.json` / `package-lock.json` -- the bundler's own dependency list,
  trimmed of `flow-bin` (an unused, large binary) and `hermes-eslint`
  (unused here), plus the three `@babel/plugin-transform-*` packages
  `build.config.js` resolves by name. `hermes-parser` points at
  `external/hermes-parser-native/package` via a `file:` dependency instead
  of the published package, and `hermes-parser-wasm` at
  `external/hermes-parser-wasm/package` beside it; `parser-select.js`
  chooses between them at run time. The lockfile pins the `@babel/*` packages to the
  same versions the upstream benchmark used; without it, `npm install`
  resolves newer `@babel/generator`/`@babel/helpers` releases whose output
  formatting has since changed, and four of the six bundles stop matching
  `expected/` for reasons unrelated to parsing (see "A note on
  reproducibility" below).
- `parser-select.js` -- reads `FLOW_BUNDLER_PARSER` and, for `wasm`,
  patches `Module._resolveFilename` so `hermes-parser` and its subpaths
  resolve to `hermes-parser-wasm`. `babel-register.js` requires it first,
  before anything can pull the parser in. The file itself records why the
  two obvious alternatives (a re-exporting shim package; swapping
  `node_modules/hermes-parser` on disk) do not work here.
- `.npmrc` -- sets `legacy-peer-deps=true` (this dependency set mixes
  `prettier@2.8.8` with packages that peer-depend on `prettier@^3`, exactly
  as the upstream benchmark's own `package.json` does) and
  `install-links=true` (npm's default of symlinking `file:` dependencies
  means requires from inside `hermes-parser` resolve via the symlink's real
  path, outside this directory, where its own `hermes-estree` dependency is
  not found; copying it instead makes that resolve correctly).
- `build-bundle.sh`, `transpile.js`, `check-artifacts.sh` -- build the
  example into an AOT container and two executables, and verify them; see
  "Bundling it". Output lands in `dist/`.
- `prettier-stub/` -- stands in for `prettier` inside those artifacts only;
  see "Why prettier is stubbed".

## Running it

```sh
cmake --build cmake-build-release --target hermes-node hermes-parser-napi
(cd examples/flow-bundler && npm install --no-audit --no-fund)
./examples/flow-bundler/run.sh cmake-build-release
```

`npm install` is required once before the first run. `run.sh` builds the two
fixture apps (`simple` and `music`) into `out/`, in three variants each
(unmodified, Flow-stripped, and stripped-and-lowered to ES5 classes), and
compares all six against `expected/`. It prints `PASS: 6 bundles match
expected/` and exits 0 on success.

Run under `cmake-build-release`, not an ASan build: the bundler run takes
several seconds under a release build but exceeds ten minutes under ASan.

## Bundling it

```sh
cmake --build cmake-build-release --target hermes-node hermes-node-kit
./examples/flow-bundler/build-bundle.sh            # into ./dist
./examples/flow-bundler/check-artifacts.sh         # build, then verify
```

`build-bundle.sh` produces four artifacts, each carrying the WebAssembly
`hermes-parser` and `build.config.js`. They differ in how the parser's Wasm
module reaches the run without being compiled there: the first two and
`flow-bundler-native-bcwasm` carry its compiled bytecode baked in, while
`flow-bundler-native` compiles the module itself to native code at build
time, like the JavaScript around it. The two native executables are
otherwise identical, so the last two rows are the cost and the gain of
native WebAssembly alone:

| artifact | from | size | build | run | peak RSS |
| --- | --- | --- | --- | --- | --- |
| `dist/flow-bundler.hbb` | `--build-bundle` (1,317 modules) | 11.2 MB | 3.2 s | 1.26 s | 98 MB |
| `dist/flow-bundler` | `--build-exe` | 26.3 MB | 0.4 s link | 1.26 s | 98 MB |
| `dist/flow-bundler-native` | `build-native` (1,229 modules + the Wasm module) | 85.8 MB | 226 s | 1.08 s | 133 MB |
| `dist/flow-bundler-native-bcwasm` | `build-native --bytecode-wasm` | 69.8 MB | 53 s | 1.16 s | 125 MB |

So native WebAssembly costs 15,955,600 bytes (85,769,976 against
69,814,376) and 173 s of build (226.1 s against 52.7 s, almost all of it
one `cc` process on the C generated from the 768 KB module), and saves
about 0.08 s, 7%, of a 1.16 s run. Measured on Linux x86-64 (Xeon
E-2288G), Release, 16 cores, `build-native` at its default `--jobs`; all of
`build-bundle.sh` takes 4 min 55 s wall. Run times and peak RSS are the
middle of five runs of the whole fixture build with the compile cache
disabled. Unbaked, the container takes about 6 s: the difference is
compiling the parser's Wasm module at launch. The executables are skipped,
with a note, when the kit (or, for `build-native`, `shermes` in it) is
missing.

`check-artifacts.sh` runs each artifact **alone** -- copied into an empty
directory with only `fixture/src` beside it, no `node_modules`, no
`bundler/`, no `build.config.js`, compile cache disabled -- and requires the
six bundles in `expected/` and, in the compile-cache trace, exactly one
hit from the tier that should serve the parser -- `wasm native hit` for
`flow-bundler-native`, `wasm container hit` for the other three -- with no
hit from the other tier, no `wasm store` and no fallback warning. It also checks with `--dump` that the
container holds `HermesParserWASM.js` and no native addon. It is not part
of `run.sh`, and so not of `check-hermes-node-examples`, because
`build-native` alone takes minutes here.

### Running an artifact

```sh
cd examples/flow-bundler/dist
hermes-node --bundle=flow-bundler.hbb -- -c build.config.js
./flow-bundler -c build.config.js
./flow-bundler-native -c build.config.js
./flow-bundler-native-bcwasm -c build.config.js
```

The input is read from `<artifact dir>/fixture/src` and the output written
to `<artifact dir>/out/`: `build.config.js` computes both from its own
`__dirname`, and a bundled module's `__dirname` is its packaged path
re-rooted at the artifact's directory. `build-bundle.sh` links
`dist/fixture/src` for that reason; an artifact copied elsewhere needs a
`fixture/src` beside it. The bundler also writes a source map per bundle.

**The `-c` path names a file that need not exist.** `buildBundleCLI.js`
`require`s the config, a bundle loads code only from itself, and so the
packaged `build.config.js` answers. A different config means a different
artifact; naming one the artifact does not hold fails with
`Not in the bundle. Add it with: --include=...`, which is the message to
expect. The `.hbb` also needs `--` before the program's arguments, or
hermes-node parses `-c` as a flag of its own; the executables take them
directly.

### Why a transpile step

This example could not be bundled as it stands, for two independent
reasons, and both still hold.

The entry, `bundler/buildBundleCLI.js`, is Flow-typed ESM. `--build-bundle`
compiles every module with Hermes's own compiler, and a parse failure in
the entry -- the one file the program is certain to load -- is a hard error:

```
error: failed to parse entry .../bundler/buildBundleCLI.js: 11:12: 'from' expected
```

And the hook cannot help inside a bundle. `babel-register.js` compiles the
sources as they load by hooking `require.extensions`, and in a bundle
`Module._load` is answered from the container, with `require.extensions`
outside that boundary by design. A preload that wraps
`require.extensions['.js']` and prints on every call prints under
`hermes-node -r pre.js main.js` and prints nothing under `--preload` plus
`--bundle`, while the program still runs from the container.

So `build-bundle.sh` runs the hook's work ahead of time instead:
`transpile.js` calls `@babel/core` over `bundler/` with the options
`babel-register.js` passes `@babel/register`, writing CommonJS into a
staging directory. That does not turn this into a different example. It
demonstrates two things, and only the first involves the hook: Babel
parsing Flow with a vendored `hermes-parser` as sources load, which is
`run.sh` and is unchanged; and the bundler's own run-time work --
`@babel/core`, `@babel/generator`, three transform plugins,
`hermes-transform`, `prettier` and `hermes-parser` parsing and rewriting the
fixture -- which is exactly what the artifacts run.

### Why a staging directory

`parser-select.js` chooses the parser by patching
`Module._resolveFilename` at run time, and a bundle never gets there: the
bundle loader answers a bundled `require('hermes-parser')` from the edge
table the producer recorded. So the choice has to be made at build time.
`dist/stage/node_modules` is a directory of symlinks into the real
`node_modules`, except that `hermes-parser` points at `hermes-parser-wasm`.
The producer never resolves symlinks, so it packages the WebAssembly parser
under the name every importer uses, and the native package never enters the
graph. That covers every importer only because there is exactly one
`hermes-parser` in the installed tree; `build-bundle.sh` refuses to run if
npm ever nests another. The real `node_modules` is never touched, which is
what made swapping it on disk unacceptable (see `parser-select.js`). The
same farm replaces `prettier` with `prettier-stub/`; see the next section.

**The redirect fools the producer and nothing else.** hermes-node's disk
loader *does* resolve symlinks, as Node does, so running the staged tree
unbundled finds the native parser beside each package's real path, produces
six correct bundles, and compiles no Wasm at all. That matters for one
step: the run that records the Wasm module for baking runs from disk, so it
preloads `parser-select.js` too, and `build-bundle.sh` checks the record
holds exactly one module. Without both, the failure is silent: correct
output, an empty record, and artifacts that compile the parser at every
launch.

### Why prettier is stubbed

Nothing in the bundler formats code, yet prettier was half of every
artifact. `hermes-transform`'s `transform.js` loads its `print.js`
unconditionally, and `print.js` requires `prettier` at the top -- the only
importer in the graph -- and uses it only inside `print()`, as
`prettier.format`. The bundler never calls `print()`, and could not here:
its first act is `require('prettier/plugins/estree')`, a Prettier 3 path,
while this example pins 2.8.8. Prettier 2's `index.js` then names every
language parser it ships in a literal `require()`, so the producer packaged
all of them -- Flow, TypeScript (with its whole compiler), Babel, Markdown,
HTML, YAML, CSS, Glimmer and more.

Measured with `HERMES_NODE_DEBUG_NATIVE=BUNDLE`, which logs every bundled
`require`: of 1,331 packaged modules, 198 were never loaded, 7.0 MB of the
container's 17.4 MB of bytecode, and 7.0 MB of that was prettier. The rest
of prettier -- `index.js`, `doc.js`, `third-party.js`, about 1.56 MB -- was
loaded at startup and never used.

`prettier-stub/` is a package named `prettier` whose `format` throws,
linked into the staging `node_modules`. `check-artifacts.sh` checks the
container lists no `prettier/parser-*` file. Before and after, same
machine, same three-run method:

| | before | after |
| --- | --- | --- |
| `.hbb` | 19.7 MB, built in 15 s | 11.2 MB, 3.2 s |
| `--build-exe` | 34.8 MB | 26.3 MB |
| `build-native` (Wasm baked as bytecode, today's `--bytecode-wasm`) | 122.7 MB (96.3 MB `.text`), 216 s compile | 69.8 MB (50.5 MB `.text`), 53 s |
| run (`.hbb` / exe / native) | 1.32 / 1.30 / 1.21 s | 1.29 / 1.27 / 1.20 s |

The native compile time fell by three quarters where the size fell by
less than half; why has not been measured. Stubbing is safe only while
nothing calls `print()`. A call fails today on its Prettier 3 `require`
before reaching prettier at all; with that path fixed, it would reach the
stub's `format`, which throws naming `prettier-stub`.

### Expected producer warnings

The producer warns, and unlike `examples/tetris` that is expected here; the
scripts print the warnings rather than asserting there are none. There are
eight unresolvable optional probes (`metro-react-native-babel-preset`, a
`buildBundleCLI.js` branch this config does not take; three
`prettier/plugins/*` paths inside `print()`, which are Prettier 3's;
`@babel/preset-typescript` twice; `v8`; `supports-color`), 13 computed
requires in 6 files and 3 `require`-as-value escapes, and
`@babel/core/lib/config/files/import.cjs`, packaged as a module that throws
if loaded (`import()` inside a `.cjs`). The four `--include` flags in
`build-bundle.sh` are the computed requires that matter: `build.config.js`
itself, and the three plugins it reaches through a `require.resolve` the
scanner cannot follow.

## A note on reproducibility

The whole point of this example is that its output is byte-identical to
bundles generated upstream by the published, WebAssembly-based
`hermes-parser` -- and, since both vendored parsers are run, identical
between them too. That guarantee depends on more than the parser: `@babel/generator` also changed
how it prints a comment immediately before a call argument at some point
after 7.23.5, and `@babel/helpers` (a transitive dependency of
`@babel/core`, not a direct one) switched to pre-minified helper bodies for
`@babel/plugin-transform-classes` at some point after 7.23.2. Both are
pulled in by caret ranges (`^7.23.x`) in `package.json`, so an `npm install`
without the committed `package-lock.json` -- e.g. after deleting it -- will
resolve newer releases and four of the six bundles will stop matching
`expected/` for reasons that have nothing to do with `hermes-parser`. Keep
`package-lock.json` committed and unmodified.

The sanctioned reasons to touch it are adding or retiring a vendored parser,
which changes the `file:` dependencies the lockfile records. Adding
`hermes-parser-wasm` was one, and its whole diff was the two entries that
dependency introduces -- nothing else re-resolved, which is the property to
check after any such change. The recipe for retiring the native addon --
including what the resulting lockfile diff is allowed to contain -- is
"When to delete this directory" in
`external/hermes-parser-native/README.md`.
