// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Parses the file named on the command line with the Hermes parser -- the
// WebAssembly build this time -- and writes its ESTree AST to stdout.
//
// Deliberately byte-for-byte the same program as
// examples/hermes-parser-ast/ast.js apart from this comment. The two
// examples differ in what `hermes-parser` resolves to and in what they
// bundle to, not in what they ask the parser for, which is what makes their
// output directly comparable.

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
