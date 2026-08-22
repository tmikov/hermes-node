# Examples

Run from the project root:

```sh
cmake-build-asan/bin/hermes-node examples/hello-fs.js
```

Four of the examples can also produce an AOT bundle you can keep, with a
`build-bundle.sh` beside their `package.json`:

```sh
cd examples/yargs-cli && ./build-bundle.sh          # writes ./dist
cd examples/yargs-cli && ./build-bundle.sh /tmp/out # or anywhere
```

It takes an optional output directory (default `./dist`) and an optional
build directory (default `cmake-build-release`), builds the bundle with
whatever flags that example needs, and prints what it wrote and what has to
travel with it. Where a `run.sh` exists it calls the same script with a
temporary directory, so the artifact you ship and the artifact the check
exercises are built the same way. `flow-bundler` has no `build-bundle.sh`;
see below.

## hello-fs.js

Creates a temp directory, writes and reads a JSON file, lists the
directory, prints file stats, and cleans up. Exercises `fs`, `path`,
and `console.log` with object inspection. All synchronous.

## hello-fs-async-cb.js

Same thing using async `fs` callbacks and the event loop.

## hello-fs-promises.js

Same thing using `fs/promises` and async/await.

## bufferutil-addon/

Loads `bufferutil`, a real npm package with a prebuilt native NAPI addon
(`.node` shared library). Demonstrates WebSocket frame masking/unmasking.
Requires `npm install` in the directory first.

`./build-bundle.sh` writes an AOT bundle to `./dist` and runs it with no
source tree. `bufferutil` is loaded by `node-gyp-build`, which reads the
filesystem before it requires, so the addon is both packaged (`--include`)
and copied to its identity path under the output directory. Without that
copy the package falls back to its pure-JavaScript implementation, and
`mask.js`'s own unguarded "is this really the native addon?" check then
throws -- so the visible symptom is a crash, not a slower success. The
script's own output explains the duplicate.

## flow-bundler/

Runs the Hermes benchmark suite's Flow bundler end to end under
hermes-node, parsing with the vendored native `hermes-parser` addon, and
checks its output against committed expected bundles. Requires `npm
install` in the directory first; see `flow-bundler/README.md`.

This is the one example with no `build-bundle.sh`: it cannot be bundled.
Its entry is Flow-typed ESM, which the producer refuses, and its sources
are transpiled as they load by `@babel/register`, whose
`require.extensions` hook does not fire inside a bundle. See "Why there is
no build-bundle.sh" in `flow-bundler/README.md`.

## babel-parser/

Parses and transforms JavaScript with Babel. Also the worked example of
what a static bundler can and cannot discover: `transform.js` names its
preset by string, which nothing can follow, and `transform-static.js`
requires it instead, which makes the AOT bundle self-contained. Optionally
bundles with rollup as well. Requires `npm install` in the directory
first; see `babel-parser/README.md`.

`./build-bundle.sh` writes all four bundles to `./dist`; `run.sh` calls the
same script with a temporary directory, so the flags are stated once.

## hermes-parser-ast/

Parses a file with the native Hermes parser addon and prints its ESTree
AST, from disk and then from an AOT bundle -- the native-addon sibling of
`babel-parser/ast.js`. `hermes-parser`'s own loader reaches its `.node`
through three computed `require()` calls, which the scanner cannot follow,
so the addon is named with `--include`; the resulting bundle is a file
plus a sidecar shared object rather than one file. Requires `npm install`
in the directory first; see `hermes-parser-ast/README.md`.

`./build-bundle.sh` writes the bundle and its sidecar to `./dist`; `run.sh`
calls the same script with a temporary directory.

## yargs-cli/

A small CLI built on `yargs`, with a real 16-package transitive dependency
tree. The tree is discovered entirely by literal `require()`, so
`./build-bundle.sh` needs no `--include` and writes one self-contained
`dist/greet.hbb`; the script runs it before reporting success. There is no
`run.sh` -- `test/bundle-yargs.js` covers this example in the lit suite,
gated on the tree being installed. Requires `npm install` in the directory
first.
