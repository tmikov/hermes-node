// Copyright (c) Tzvetan Mikov.
// Test circular dependencies across node_modules packages.
// Verifies that partial exports are returned (not an infinite loop) and that
// the exports object reference is shared so late additions are visible.
'use strict';

function assert(cond, msg) {
  if (!cond) throw new Error('Assertion failed: ' + msg);
}
function assertEqual(a, b, msg) {
  if (a !== b) throw new Error('Assertion failed: ' + msg + ' (' + JSON.stringify(a) + ' !== ' + JSON.stringify(b) + ')');
}

// 1. require('pkg-a') completes without hanging (no infinite loop)
var pkgA = require('pkg-a');
assertEqual(pkgA.name, 'pkg-a', 'pkg-a loaded successfully');

// 2. pkg-a successfully loaded pkg-b
assertEqual(pkgA.pkgB.name, 'pkg-b', 'pkg-b loaded by pkg-a');

// 3. pkg-b saw pkg-a's early exports (set before require('pkg-b'))
assertEqual(pkgA.pkgB.sawPkgAName, 'pkg-a', 'pkg-b saw pkg-a name');
assertEqual(pkgA.pkgB.sawPkgAEarly, 'a-early-value', 'pkg-b saw pkg-a early export');

// 4. pkg-b did NOT see pkg-a's late exports (set after require('pkg-b'))
assertEqual(pkgA.pkgB.sawPkgALate, undefined, 'pkg-b did not see pkg-a late export at require time');

// 5. But since exports is a shared reference, the late value IS on the object now
assertEqual(pkgA.pkgB.pkgARef.late, 'a-late-value', 'late export visible via shared reference');

// 6. pkg-a's late export is also directly visible
assertEqual(pkgA.late, 'a-late-value', 'pkg-a late export set after circular resolution');

// 7. Module caching works -- second require returns same instance
var pkgA2 = require('pkg-a');
assert(pkgA === pkgA2, 'same pkg-a instance from cache');

// 8. require('pkg-b') returns the same cached instance too
var pkgB = require('pkg-b');
assert(pkgB === pkgA.pkgB, 'same pkg-b instance from cache');

console.log('PASS');
