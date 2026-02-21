// Copyright (c) Tzvetan Mikov.
// Test that package.json "main" field is respected.
// Verifies that require('my-package') loads from the path specified in "main",
// not the default index.js.
'use strict';

function assert(cond, msg) {
  if (!cond) throw new Error('Assertion failed: ' + msg);
}
function assertEqual(a, b, msg) {
  if (a !== b) throw new Error('Assertion failed: ' + msg + ' (' + JSON.stringify(a) + ' !== ' + JSON.stringify(b) + ')');
}

// 1. require('my-package') loads lib/entry.js (from "main" field), not index.js.
var pkg = require('my-package');
assertEqual(pkg, 'main-entry', 'package.json "main" field directs to lib/entry.js');

// 2. require.resolve returns the path to the "main" entry point.
var resolvedPath = require.resolve('my-package');
assert(resolvedPath.endsWith('node_modules/my-package/lib/entry.js'),
  'resolved path points to lib/entry.js, got: ' + resolvedPath);

// 3. "main" field without .js extension -- Node tries adding .js automatically.
var noExt = require('no-ext-package');
assertEqual(noExt, 'no-ext-entry', '"main": "src/index" resolves to src/index.js');

// 4. require.resolve for extensionless main also resolves correctly.
var noExtPath = require.resolve('no-ext-package');
assert(noExtPath.endsWith('node_modules/no-ext-package/src/index.js'),
  'extensionless main resolves to src/index.js, got: ' + noExtPath);

console.log('PASS');
