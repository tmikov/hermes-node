# babel-parser

Runs Babel under hermes-node, and doubles as the worked example of what a
static bundler can and cannot see.

```sh
npm install
../../cmake-build-release/bin/hermes-node parse.js
../../cmake-build-release/bin/hermes-node transform.js
../../cmake-build-release/bin/hermes-node ast.js parse.js
./run.sh                      # everything below, checked
./build-bundle.sh             # the four AOT bundles, kept in ./dist
```

| file | what it is |
| --- | --- |
| `parse.js` | `@babel/parser` only: parse a snippet, print the AST shape |
| `transform.js` | `@babel/core` + `@babel/preset-env`, preset named by string; bundled with `--include` |
| `transform-static.js` | the same, with the preset `require`d instead |
| `ast.js` | `@babel/parser` on a file named by argument; prints the full AST as JSON -- see "Modules vs. data" below |
| `rollup.config.mjs` | optional: bundle `transform-static.js` with rollup |
| `build-bundle.sh` | builds the four AOT bundles; `run.sh` calls it too |

## Why there are two transform scripts

`transform.js` is the idiomatic form:

```js
babel.transformSync(code, { presets: ['@babel/preset-env'] });
```

Babel resolves that name at run time, from configuration. No static
bundler can follow it -- not `hermes-node --build-bundle`, not rollup. The
bundle builds with no complaint and is missing the preset. A bundle is a
closed world, so running it says exactly that:

```
Error: Cannot find module '@babel/preset-env'
  required by node_modules/@babel/core/lib/config/files/plugins.js
  Not in the bundle. Add it with:
    --include=@babel/preset-env
```

There are two ways to fix it, and this directory shows both.

**Name it at build time.** `--include` packages a root the walk cannot
find, and then walks it exactly like the entry. The source is untouched:

```sh
hermes-node --build-bundle=transform.hbb --include=@babel/preset-env transform.js
mv node_modules /tmp/ && hermes-node --bundle=transform.hbb   # PASS
```

**Or make the dependency static.** `transform-static.js` differs from
`transform.js` by two lines -- it requires the preset and passes the
object -- which turns it into an ordinary edge the walk follows on its
own:

```sh
hermes-node --build-bundle=app.hbb transform-static.js
mv node_modules /tmp/ && hermes-node --bundle=app.hbb   # PASS
```

Prefer `--include` when the source is not yours to edit, which is the
usual case for a dependency several packages down. This is the general
shape of the problem, not a Babel quirk. `run.sh` checks all three
bundles with the tree hidden, which is the only way to tell a
self-contained container from one with a hole in it.

## Build-time warnings

`--build-bundle` reports what it could not follow. On this example:

```
warning: not packaging '@babel/preset-typescript/package.json' ... (cannot be resolved, ...)
warning: not packaging '@babel/preset-typescript' ... (cannot be resolved, ...)
warning: not packaging 'v8' ... (cannot be resolved, ...)
warning: not packaging 'supports-color' ... (cannot be resolved, ...)
warning: 9 computed require()/require.resolve() calls in 4 files, not packaged:
  ... (one line per call site, up to 10; --verbose lists the rest)
warning: require used as a value in 1 place in 1 file, not packaged:
  ...
warning: cannot compile .../import.cjs (SyntaxError: ...); packaged as a
         module that throws when required
```

None of them stop the build, and none of them stop this program: they are
optional-dependency probes, configuration-driven loads, and a three-line
module that exists only to call dynamic `import()`. The computed-call count
covers `require.resolve()` alongside `require()` -- a literal
`require.resolve()` is a discovery edge exactly like a literal `require()`,
and a computed one is the same kind of gap. What they do is predict where a
closed world can fail -- a warning here is a candidate for `--include`, and
the run-time error names the specifier when one of them turns out to
matter. `--verbose` lists the positions. `run.sh` silences them, since it
is checking behavior.

## Modules vs. data

"Closed world" describes `require()`: a bundled program cannot load a
*module* the container doesn't have. It says nothing about the files a
program reads as data with `fs`, and `ast.js` is here to show that in
practice, not just state it.

`ast.js` parses whatever file its argument names and prints the AST. A
positional argument reaches a bundled program the same way it reaches an
unbundled one -- `hermes-node --bundle=ast.hbb foo.js` hands `foo.js` to the
bundled program, not to `hermes-node` -- and the program opens it with an
ordinary `fs.readFileSync` at run time:

```sh
hermes-node --build-bundle=ast.hbb ast.js
mv node_modules /tmp/ && hermes-node --bundle=ast.hbb parse.js   # still works
```

`@babel/parser` comes from the container; `parse.js` -- the file being
parsed -- comes from disk, right next to the container, with `node_modules`
moved out of the way. That split is the point: a bundle makes *code*
self-contained so you can ship one file, while the program keeps reading
and writing whatever data files it always did.

## Optional: rollup

Rollup is a `devDependency`, so a plain `npm install` gets it and
`npm install --omit=dev` does not. `run.sh` reports SKIP when it is
absent.

```sh
npm run bundle:rollup                                  # -> rollup-out.cjs
../../cmake-build-release/bin/hermes-node rollup-out.cjs
```

The config carries three workarounds -- `ignore` handles two of them, the
`load()` hook a third -- and only one is needed for plain Node as well:

- **`ignore`** for `@babel/preset-typescript`, `supports-color` and `v8`.
  The first two are optional dependencies genuinely missing from
  `node_modules`, each probed inside a `try`/`catch`; unignored, rollup
  hoists the unresolved `require` to the top of the bundle, turning the
  guarded probe into an unconditional load -- the first build of this
  crashed under Node before `main` ran, so this part is needed for plain
  Node too. `v8` is different: it's a real Node builtin, so hoisting it is
  harmless under Node, but hermes-node has none, so an unguarded
  `require('v8')` fails immediately with `Cannot find module 'v8'` --
  Hermes-only. `ignore` leaves all three `require`s where they were
  written.
- **a `load()` hook** replacing `@babel/core`'s `import.cjs`, three lines
  whose only job is to call dynamic `import()`. Hermes cannot compile
  that, and since rollup inlines everything into one file, leaving it in
  makes the whole bundle uncompilable instead of one module.
  `--build-bundle` does this substitution itself, per module, and says so.
  Also Hermes-only -- plain Node runs the original file fine.

Stacking the two bundlers is the smallest and fastest result, because
rollup collapses the module graph and the container then holds one
bytecode blob instead of 574:

| | size | run |
| --- | --- | --- |
| `transform.js` from disk, warm compile cache | -- | 0.21 s |
| `node rollup-out.cjs` | 4.1 MB | 0.23 s |
| `hermes-node rollup-out.cjs` | 4.1 MB | 0.13 s |
| `--bundle` of `transform.js` (`--include`) | 4.2 MB | 0.11 s |
| `--bundle` of `transform-static.js` | 4.2 MB | 0.11 s |
| `--bundle` of `rollup-out.cjs` | 2.9 MB | 0.07 s |

The two `transform` containers hold 574 modules each; the rollup one holds
1. The `--include` and `transform-static.js` containers land at 4,217,208
and 4,217,264 bytes -- 56 bytes apart, same module count. Naming the preset
at build time and requiring it in the source produce essentially the same
artifact, which is the point of "Why there are two transform scripts"
above.

Measured on one Linux box with a release build; treat them as ratios.
