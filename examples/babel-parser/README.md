# babel-parser

Runs Babel under hermes-node, and doubles as the worked example of what a
static bundler can and cannot see.

```sh
npm install
../../cmake-build-release/bin/hermes-node parse.js
../../cmake-build-release/bin/hermes-node transform.js
./run.sh                      # everything below, checked
```

| file | what it is |
| --- | --- |
| `parse.js` | `@babel/parser` only: parse a snippet, print the AST shape |
| `transform.js` | `@babel/core` + `@babel/preset-env`, preset named by string |
| `transform-static.js` | the same, with the preset `require`d instead |
| `rollup.config.mjs` | optional: bundle `transform-static.js` with rollup |

## Why there are two transform scripts

`transform.js` is the idiomatic form:

```js
babel.transformSync(code, { presets: ['@babel/preset-env'] });
```

Babel resolves that name at run time, from configuration. No static
bundler can follow it -- not `hermes-node --build-bundle`, not rollup.
The bundle builds with no complaint and runs fine while `node_modules` is
still on disk, because the missing module is answered by the disk
fallback. Delete the tree and it dies:

```
Error: Cannot find module '@babel/preset-env'
```

`transform-static.js` differs by two lines -- it requires the preset and
passes the object -- and that is enough to make the dependency ordinary
and the bundle self-contained:

```sh
hermes-node --build-bundle=app.hbb transform-static.js
mv node_modules /tmp/ && hermes-node --bundle=app.hbb   # still PASS
```

This is the general shape of the problem, not a Babel quirk: if you want
a dynamic dependency in the bundle, make it a static one. `run.sh` checks
both bundles with the tree hidden, which is the only way to tell a
self-contained container from one that is quietly still reading the disk.

## Build-time warnings

`--build-bundle` reports what it could not follow. On this example:

```
warning: not packaging '@babel/preset-typescript' ... (cannot be resolved, ...)
warning: 5 computed require() calls in 2 files: not packaged, ...
warning: require used as a value in 1 place in 1 file: ...
warning: cannot compile .../import.cjs (SyntaxError: ...); packaged as a
         module that throws when required
```

None of them stop the build, and none of them stop this program: they are
optional-dependency probes, configuration-driven loads, and a three-line
module that exists only to call dynamic `import()`. `--verbose` lists the
positions. `run.sh` silences them, since it is checking behavior.

## Optional: rollup

Rollup is a `devDependency`, so a plain `npm install` gets it and
`npm install --omit=dev` does not. `run.sh` reports SKIP when it is
absent.

```sh
npm run bundle:rollup                                  # -> rollup-out.cjs
../../cmake-build-release/bin/hermes-node rollup-out.cjs
```

The config carries three workarounds, and two of them are needed for
plain Node as well:

- **`ignore`** for `@babel/preset-typescript`, `supports-color` and `v8`.
  Each is required inside a `try`/`catch` or a lazy function. Rollup
  hoists an unresolved require to the top of the bundle, which turns a
  guarded probe into an unconditional load -- the first build of this
  crashed under Node before `main` ran. `ignore` leaves the `require`
  where it was written.
- **a `load()` hook** replacing `@babel/core`'s `import.cjs`, three lines
  whose only job is to call dynamic `import()`. Hermes cannot compile
  that, and since rollup inlines everything into one file, leaving it in
  makes the whole bundle uncompilable instead of one module.
  `--build-bundle` does this substitution itself, per module, and says so.

Stacking the two bundlers is the smallest and fastest result, because
rollup collapses the module graph and the container then holds one
bytecode blob instead of 270:

| | size | run |
| --- | --- | --- |
| `transform.js` from disk, warm compile cache | -- | 0.19 s |
| `node rollup-out.cjs` | 4.1 MB | 0.22 s |
| `hermes-node rollup-out.cjs` | 4.1 MB | 0.09 s |
| `--bundle` of `transform-static.js` | 4.1 MB | 0.10 s |
| `--bundle` of `rollup-out.cjs` | 2.9 MB | 0.07 s |

Measured on one Linux box with a release build; treat them as ratios.
