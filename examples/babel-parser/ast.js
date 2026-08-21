// Parse a JavaScript file named on the command line and print its AST as
// pretty-printed JSON.
//
// Unlike the other scripts here, this one takes an argument. That argument
// is what it demonstrates: a positional argument reaches a *bundled*
// program too -- `hermes-node --bundle=ast.hbb foo.js` hands foo.js to the
// bundled program, not to hermes-node -- and the program reads it off the
// filesystem at run time with an ordinary `fs.readFileSync`. A bundle is a
// closed world for require()/require.resolve(): every module comes from
// the container, never the disk. It is closed for modules, not for data --
// this script's own input keeps coming from wherever the caller points it,
// bundle or no bundle.
//
// Usage:
//   hermes-node ast.js <file.js>
//   hermes-node --build-bundle=ast.hbb ast.js
//   hermes-node --bundle=ast.hbb <file.js>

'use strict';

var fs = require('fs');
var parser = require('@babel/parser');

var file = process.argv[2];
if (!file) {
  console.error('usage: ast.js <file.js>');
  process.exit(1);
}

var code;
try {
  code = fs.readFileSync(file, 'utf8');
} catch (e) {
  console.error('usage: ast.js <file.js> (could not read ' + file + ')');
  process.exit(1);
}

var ast = parser.parse(code, { sourceType: 'unambiguous' });

console.log(JSON.stringify(ast, null, 2));
