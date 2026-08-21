# Examples

Run from the project root:

```sh
cmake-build-asan/bin/hermes-node examples/hello-fs.js
```

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

## flow-bundler/

Runs the Hermes benchmark suite's Flow bundler end to end under
hermes-node, parsing with the vendored native `hermes-parser` addon, and
checks its output against committed expected bundles. Requires `npm
install` in the directory first; see `flow-bundler/README.md`.

## babel-parser/

Parses and transforms JavaScript with Babel. Also the worked example of
what a static bundler can and cannot discover: `transform.js` names its
preset by string, which nothing can follow, and `transform-static.js`
requires it instead, which makes the AOT bundle self-contained. Optionally
bundles with rollup as well. Requires `npm install` in the directory
first; see `babel-parser/README.md`.

## hermes-parser-ast/

Parses a file with the native Hermes parser addon and prints its ESTree
AST, from disk and then from an AOT bundle -- the native-addon sibling of
`babel-parser/ast.js`. `hermes-parser`'s own loader reaches its `.node`
through three computed `require()` calls, which the scanner cannot follow,
so the addon is named with `--include`; the resulting bundle is a file
plus a sidecar shared object rather than one file. Requires `npm install`
in the directory first; see `hermes-parser-ast/README.md`.
