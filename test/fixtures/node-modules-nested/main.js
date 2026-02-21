// Copyright (c) Tzvetan Mikov.
// Test nested node_modules resolution.
// Verifies that packages with their own node_modules/ are resolved correctly,
// and that hoisted (top-level) packages are found from nested packages.
'use strict';

function assert(cond, msg) {
  if (!cond) throw new Error('Assertion failed: ' + msg);
}
function assertEqual(a, b, msg) {
  if (a !== b) throw new Error('Assertion failed: ' + msg + ' (' + JSON.stringify(a) + ' !== ' + JSON.stringify(b) + ')');
}

// 1. require('pkg-a') resolves from top-level node_modules/
var pkgA = require('pkg-a');
assertEqual(pkgA.name, 'pkg-a', 'pkg-a loaded');

// 2. pkg-a loaded its nested pkg-b from pkg-a/node_modules/pkg-b/
assertEqual(pkgA.pkgB.name, 'pkg-b', 'nested pkg-b loaded by pkg-a');
assertEqual(pkgA.pkgB.level, 'nested', 'pkg-b is from nested node_modules');

// 3. Nested pkg-b found top-level pkg-c by walking up the directory tree
assertEqual(pkgA.pkgB.pkgC.name, 'pkg-c', 'pkg-c loaded by nested pkg-b');
assertEqual(pkgA.pkgB.pkgC.level, 'top', 'pkg-c is from top-level node_modules');

// 4. pkg-a loaded hoisted pkg-b-hoisted from top-level node_modules/
assertEqual(pkgA.pkgBHoisted.name, 'pkg-b-hoisted', 'hoisted pkg-b-hoisted loaded');
assertEqual(pkgA.pkgBHoisted.level, 'top', 'pkg-b-hoisted is from top-level');

// 5. Direct require of nested package's dependency finds the nested version
//    (pkg-b is in pkg-a/node_modules/, not top-level, so require from here
//     should NOT find it)
var threw = false;
try {
  require('pkg-b');
} catch (e) {
  threw = true;
  assert(e.code === 'MODULE_NOT_FOUND', 'pkg-b not found at top level: ' + e.code);
}
assert(threw, 'require(pkg-b) from top level should throw MODULE_NOT_FOUND');

// 6. require.resolve for nested packages works
var pkgAPath = require.resolve('pkg-a');
assert(pkgAPath.endsWith('node_modules/pkg-a/index.js'), 'pkg-a resolves to correct path: ' + pkgAPath);

// 7. Caching works across nesting levels -- same top-level pkg-c instance
var pkgC = require('pkg-c');
assert(pkgC === pkgA.pkgB.pkgC, 'same pkg-c instance from cache');

console.log('PASS');
