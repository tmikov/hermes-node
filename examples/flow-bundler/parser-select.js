// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// Chooses which vendored Hermes parser this example runs against, from
// FLOW_BUNDLER_PARSER: "native" (the default) or "wasm".
//
// This repository vendors the Hermes parser twice -- as a Node-API addon
// (external/hermes-parser-native) and as a WebAssembly module
// (external/hermes-parser-wasm) -- and both are meant to be exercised. They
// should produce identical bundles, and this example is where that gets
// checked against real input: run.sh diffs the bundler's output against
// expected/ either way, so a parser that disagrees fails the same way a
// broken bundler would.
//
// Selection is done by resolution rather than by installation. npm puts the
// native package at node_modules/hermes-parser and the wasm one at
// node_modules/hermes-parser-wasm; "wasm" mode rewrites requests for the
// former into the latter. Two alternatives were rejected:
//
//   - A shim package exporting one or the other. It cannot work, because
//     bundler/utils.js imports deep paths -- hermes-parser/dist/traverse/
//     SimpleTraverser and two more -- and a shim only re-exports an index.
//   - Swapping node_modules/hermes-parser on disk before each run. It works,
//     but leaves the tree in whichever state ran last, so a plain `node
//     bundler/buildBundleCLI.js` afterwards silently uses a parser nobody
//     chose.
//
// The hook has to be installed before anything requires the parser, which is
// why babel-register.js requires this file first: it is itself preloaded
// with `hermes-node -r`, ahead of the bundler and of
// babel-plugin-syntax-hermes-parser, the plugin that actually calls
// HermesParser.parse.

'use strict';

const Module = require('module');

const PARSER = process.env.FLOW_BUNDLER_PARSER || 'native';

if (PARSER !== 'native' && PARSER !== 'wasm') {
  throw new Error(
    `FLOW_BUNDLER_PARSER must be "native" or "wasm", got ${JSON.stringify(PARSER)}`,
  );
}

if (PARSER === 'wasm') {
  const FROM = 'hermes-parser';
  const TO = 'hermes-parser-wasm';
  const original = Module._resolveFilename;

  // Exact match or a subpath. Deliberately not a prefix test on its own:
  // "hermes-parser-wasm" also begins with "hermes-parser" and must be left
  // alone, or the rewrite would recurse. Package names that merely contain
  // it -- babel-plugin-syntax-hermes-parser, prettier-plugin-hermes-parser
  // -- do not begin with it and are unaffected.
  Module._resolveFilename = function (request, ...rest) {
    if (typeof request === 'string') {
      if (request === FROM) {
        request = TO;
      } else if (request.startsWith(FROM + '/')) {
        request = TO + request.slice(FROM.length);
      }
    }
    return original.call(this, request, ...rest);
  };
}

module.exports = {parser: PARSER};
