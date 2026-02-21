// Copyright (c) Tzvetan Mikov.
// Test basic node_modules resolution.
// Verifies that require('my-package') finds and loads from node_modules/.
'use strict';

function assert(cond, msg) {
  if (!cond) throw new Error('Assertion failed: ' + msg);
}
function assertEqual(a, b, msg) {
  if (a !== b) throw new Error('Assertion failed: ' + msg + ' (' + JSON.stringify(a) + ' !== ' + JSON.stringify(b) + ')');
}

// 1. require('my-package') resolves from node_modules/
var pkg = require('my-package');
assertEqual(typeof pkg, 'object', 'package is an object');
assertEqual(pkg.hello, 'world', 'package.hello is world');
assertEqual(pkg.version, '1.0.0', 'package.version is 1.0.0');

// 2. Second require returns the cached instance.
var pkg2 = require('my-package');
assert(pkg === pkg2, 'require caches node_modules packages');

// 3. module.filename and __dirname are set correctly for this script.
var path = require('path');
assert(__filename.endsWith('main.js'), '__filename ends with main.js');
assertEqual(__dirname, path.dirname(__filename), '__dirname matches');

// 4. The package's module is cached by its resolved filename.
var Module = require('internal/modules/cjs/loader').Module;
var resolvedPath = require.resolve('my-package');
assert(resolvedPath.endsWith('node_modules/my-package/index.js'), 'resolved path ends with node_modules/my-package/index.js');
assert(Module._cache[resolvedPath] !== undefined, 'package is in Module._cache');

console.log('PASS');
