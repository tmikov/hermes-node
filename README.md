# hermes-node-compat

Node.js built-in module compatibility layer for the [Hermes](https://github.com/facebook/hermes) JavaScript engine.

Reuses Node's own `lib/*.js` files and ports Node's C++ native bindings to work with Hermes via Node-API. The result is a runtime that can execute JavaScript code depending on Node.js APIs (`fs`, `path`, `events`, `buffer`, `stream`, `process`, etc.) on Hermes.

## Status

Early development. See `history/initial/progress.md` for current progress.

