// Copyright (c) Tzvetan Mikov.
// Test require.resolve() and require.resolve.paths().
// Verifies that require.resolve returns absolute paths and require.resolve.paths
// returns the correct search directories.
'use strict';

var path = require('path');

function assert(cond, msg) {
  if (!cond) throw new Error('Assertion failed: ' + msg);
}
function assertEqual(a, b, msg) {
  if (a !== b) throw new Error('Assertion failed: ' + msg + ' (' + JSON.stringify(a) + ' !== ' + JSON.stringify(b) + ')');
}

// 1. require.resolve for a node_modules package returns the absolute path
//    to the resolved entry point (respecting package.json "main").
var resolvedPkg = require.resolve('resolve-pkg');
assert(path.isAbsolute(resolvedPkg), 'resolved path is absolute: ' + resolvedPkg);
assert(resolvedPkg.endsWith('/node_modules/resolve-pkg/lib/entry.js'),
  'resolved path points to lib/entry.js via "main" field: ' + resolvedPkg);

// 2. require.resolve for a relative path returns the absolute path.
var resolvedLocal = require.resolve('./local-module');
assert(path.isAbsolute(resolvedLocal), 'local resolved path is absolute: ' + resolvedLocal);
assert(resolvedLocal.endsWith('/local-module.js'),
  'local resolved path ends with local-module.js: ' + resolvedLocal);

// 3. require.resolve for a relative .js file (with extension) works.
var resolvedLocalExplicit = require.resolve('./local-module.js');
assertEqual(resolvedLocal, resolvedLocalExplicit,
  'resolving with and without .js extension gives same result');

// 4. require.resolve matches what require() actually loads.
var pkg = require('resolve-pkg');
assertEqual(pkg.name, 'resolve-pkg', 'package loaded correctly');
var resolvedAgain = require.resolve('resolve-pkg');
assertEqual(resolvedPkg, resolvedAgain, 'require.resolve is consistent');

// 5. require.resolve.paths for a non-builtin returns an array of search paths.
var searchPaths = require.resolve.paths('resolve-pkg');
assert(Array.isArray(searchPaths), 'paths returns an array');
assert(searchPaths.length > 0, 'paths array is non-empty');
// Should include the node_modules directory relative to this file.
var hasLocalNodeModules = searchPaths.some(function(p) {
  return p.endsWith('/require-resolve/node_modules');
});
assert(hasLocalNodeModules, 'paths includes local node_modules dir: ' + JSON.stringify(searchPaths));

// 6. require.resolve.paths for a builtin module returns null.
var builtinPaths = require.resolve.paths('fs');
assertEqual(builtinPaths, null, 'paths for builtin "fs" returns null');

// 7. require.resolve for a builtin module returns the module name.
var resolvedBuiltin = require.resolve('fs');
assertEqual(resolvedBuiltin, 'fs', 'require.resolve("fs") returns "fs"');

// 8. require.resolve for a non-existent module throws MODULE_NOT_FOUND.
var threw = false;
try {
  require.resolve('nonexistent-package-xyz');
} catch (e) {
  threw = true;
  assert(e.code === 'MODULE_NOT_FOUND',
    'expected MODULE_NOT_FOUND, got: ' + e.code);
}
assert(threw, 'require.resolve for non-existent package should throw');

// 9. require.resolve with { paths: [...] } option restricts search.
var customPaths = require.resolve('resolve-pkg', {
  paths: [path.join(__dirname, 'node_modules')]
});
assertEqual(customPaths, resolvedPkg,
  'require.resolve with custom paths finds the same package');

// 10. require.resolve with { paths: [] } for a non-builtin throws.
threw = false;
try {
  require.resolve('resolve-pkg', { paths: [] });
} catch (e) {
  threw = true;
  assert(e.code === 'MODULE_NOT_FOUND',
    'expected MODULE_NOT_FOUND with empty paths, got: ' + e.code);
}
assert(threw, 'require.resolve with empty paths should throw for non-builtin');

console.log('PASS');
