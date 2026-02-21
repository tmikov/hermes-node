// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Shim for internal/modules/typescript.js
//
// The real module needs internal/deps/amaro (TypeScript parser, WASM-based)
// and compile cache bindings (getCompileCacheEntry, etc.) we don't have.
// Since we don't support TypeScript type stripping, we throw on any attempt
// to strip types (matching Node's behavior when TypeScript support is disabled).

'use strict';

function stripTypeScriptModuleTypes(source, filename) {
  throw new Error('TypeScript type stripping is not supported');
}

function stripTypeScriptTypes(code, options) {
  throw new Error('TypeScript type stripping is not supported');
}

module.exports = {
  stripTypeScriptModuleTypes,
  stripTypeScriptTypes,
};
