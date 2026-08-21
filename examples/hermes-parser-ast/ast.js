// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Parses the file named on the command line with the Hermes parser -- a
// native addon -- and writes its ESTree AST to stdout.
//
// The pairing with examples/babel-parser/ast.js is the point: the same job
// with a pure-JavaScript parser bundles to one file, and this one bundles
// to a file plus a shared object.

'use strict';

const fs = require('fs');
const { parse } = require('hermes-parser');

const file = process.argv[2];
if (!file) {
  console.error('usage: ast.js <file.js>');
  process.exit(1);
}

const source = fs.readFileSync(file, 'utf8');
const ast = parse(source, { babel: false });
console.log(JSON.stringify(ast, null, 2));
