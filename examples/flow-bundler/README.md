# flow-bundler

Runs the Hermes benchmark suite's Flow bundler end to end under hermes-node:
hermes-node loads the bundler through a `-r` require hook, Babel parses the
bundler's own sources and the fixture app with `hermes-parser`, and
`hermes-parser` here is the vendored native addon (`external/hermes-parser-native`)
rather than the published `hermes-parser` npm package, which ships a
WebAssembly build that hermes-node cannot run. `run.sh` then checks the
bundler's output against committed expected output, so a pass is evidence
that the native addon parses this real-world codebase the same way the
WebAssembly parser does.

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
  of the published package. The lockfile pins the `@babel/*` packages to the
  same versions the upstream benchmark used; without it, `npm install`
  resolves newer `@babel/generator`/`@babel/helpers` releases whose output
  formatting has since changed, and four of the six bundles stop matching
  `expected/` for reasons unrelated to parsing (see "A note on
  reproducibility" below).
- `.npmrc` -- sets `legacy-peer-deps=true` (this dependency set mixes
  `prettier@2.8.8` with packages that peer-depend on `prettier@^3`, exactly
  as the upstream benchmark's own `package.json` does) and
  `install-links=true` (npm's default of symlinking `file:` dependencies
  means requires from inside `hermes-parser` resolve via the symlink's real
  path, outside this directory, where its own `hermes-estree` dependency is
  not found; copying it instead makes that resolve correctly).

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

## Why there is no build-bundle.sh

The other bundleable examples ship a `build-bundle.sh` that produces an AOT
bundle you can keep. This one does not, because this example cannot be
bundled -- not "has not been", cannot.

There are two independent reasons, and the second is the one that matters.

The first is visible immediately. `--build-bundle` compiles every module it
packages with Hermes's own compiler, and `bundler/buildBundleCLI.js` is
Flow-typed ESM:

```
$ hermes-node --build-bundle=dist/app.hbb --preload=../babel-register.js \
    bundler/buildBundleCLI.js
error: failed to parse entry .../bundler/buildBundleCLI.js: 11:12: 'from' expected
```

The entry is the one file the program is certain to load, so the producer
treats a parse failure there as a hard error rather than packaging a module
that throws.

The second reason survives every way around the first. The bundler's
sources are *meant* to be untranspiled: `babel-register.js` installs
`@babel/register`, which compiles them on the way in by hooking
`require.extensions`. A bundle refuses `-r` at run time and takes
`--preload` at build time instead, so the hook can be packaged and run --
but it will have nothing to do. In a bundle, `Module._load` is intercepted
and answered from the container, and `require.extensions` sits outside that
boundary by design, so the hook never gets a turn.

That is observable in three lines. A preload that wraps
`require.extensions['.js']` and prints on every call prints twice under
`hermes-node -r pre.js main.js`, and prints nothing at all under
`--preload=./pre.js` plus `--bundle`, while the program still runs
correctly from the container.

Routing around the entry-parse error therefore only moves the failure to
run time. Bundling a CommonJS wrapper that `require`s the Flow entry builds
successfully -- the producer packages the unparseable module as one that
throws when required, which is what it does for any file the run may never
reach -- and then:

```
$ hermes-node --bundle=dist/app.hbb -- -c build.config.js
SyntaxError: .../bundler/buildBundleCLI.js: 11:12: 'from' expected
```

Pre-transpiling `bundler/` to CommonJS on disk and bundling that would
produce a working container, but it would no longer be this example: what
this one demonstrates is Babel parsing Flow with the native `hermes-parser`
addon as the sources load.

## A note on reproducibility

The whole point of this example is that its output is byte-identical to
bundles generated by the published, WebAssembly-based `hermes-parser`. That
guarantee depends on more than the parser: `@babel/generator` also changed
how it prints a comment immediately before a call argument at some point
after 7.23.5, and `@babel/helpers` (a transitive dependency of
`@babel/core`, not a direct one) switched to pre-minified helper bodies for
`@babel/plugin-transform-classes` at some point after 7.23.2. Both are
pulled in by caret ranges (`^7.23.x`) in `package.json`, so an `npm install`
without the committed `package-lock.json` -- e.g. after deleting it -- will
resolve newer releases and four of the six bundles will stop matching
`expected/` for reasons that have nothing to do with `hermes-parser`. Keep
`package-lock.json` committed and unmodified.

There is exactly one sanctioned reason to touch it: retiring the vendored
`hermes-parser-native` package, which requires repointing the `file:`
dependency the lockfile records twice. The recipe for that -- including what
the resulting lockfile diff is allowed to contain -- is "When to delete this
directory" in `external/hermes-parser-native/README.md`.
