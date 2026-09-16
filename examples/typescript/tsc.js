// The whole program: TypeScript's own compiler CLI, unmodified.
//
// This requires `typescript/lib/_tsc.js` rather than `typescript/lib/tsc.js`.
// The latter is a 267-byte shim whose only job is to call
// `module.enableCompileCache()` before loading the real compiler -- which
// does nothing here, since hermes-node's compile cache is on by default and
// is deliberately not observable from JavaScript (`enableCompileCache()`
// reports FAILED). Pointing at `_tsc.js` keeps the bundled module graph
// honest: one module, no shim.
require('typescript/lib/_tsc.js');
