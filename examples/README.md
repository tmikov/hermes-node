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
