// Copyright (c) Tzvetan Mikov.
// Test that package.json "exports" field is respected.
// Verifies conditional exports, subpath exports, and wildcard exports.
'use strict';

function assert(cond, msg) {
  if (!cond) throw new Error('Assertion failed: ' + msg);
}
function assertEqual(a, b, msg) {
  if (a !== b) throw new Error('Assertion failed: ' + msg + ' (' + JSON.stringify(a) + ' !== ' + JSON.stringify(b) + ')');
}

// 1. Conditional exports: require() uses the "require" condition, not "import".
var pkg = require('my-package');
assertEqual(pkg, 'cjs-entry', '"exports" "." with "require" condition loads cjs.js');

// 2. require.resolve also respects "exports" field.
var resolvedPath = require.resolve('my-package');
assert(resolvedPath.endsWith('/cjs.js'),
  'resolved path points to cjs.js, got: ' + resolvedPath);

// 3. Subpath export: require('my-package/utils') resolves via exports["./utils"].
var utils = require('my-package/utils');
assertEqual(utils, 'utils', 'subpath export "./utils" loads utils.js');

// 4. Wildcard/pattern export: require('my-package/lib/helper') resolves via exports["./lib/*"].
var helper = require('my-package/lib/helper');
assertEqual(helper, 'helper', 'wildcard export "./lib/*" loads lib/helper.js');

// 5. Simple string exports: package with "exports": "./main.js".
var simple = require('simple-exports');
assertEqual(simple, 'simple-exports-entry', 'string "exports" loads main.js');

// 6. require.resolve for simple string exports.
var simplePath = require.resolve('simple-exports');
assert(simplePath.endsWith('/main.js'),
  'simple exports resolved path points to main.js, got: ' + simplePath);

// 7. Accessing non-exported subpath should throw ERR_PACKAGE_PATH_NOT_EXPORTED.
var threw = false;
try {
  require('my-package/secret');
} catch (e) {
  threw = true;
  assert(e.code === 'ERR_PACKAGE_PATH_NOT_EXPORTED',
    'expected ERR_PACKAGE_PATH_NOT_EXPORTED, got: ' + e.code);
}
assert(threw, 'require of non-exported subpath should throw');

console.log('PASS');
