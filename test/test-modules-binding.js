// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %s | %FileCheck %s
// CHECK: PASS
// Test the modules binding.
'use strict';

var mod = internalBinding('modules');

// Helper
function assert(cond, msg) {
  if (!cond) throw new Error('Assertion failed: ' + msg);
}
function assertEqual(a, b, msg) {
  if (a !== b) throw new Error('Assertion failed: ' + msg + ' (' + a + ' !== ' + b + ')');
}

// --- Functions ---
assertEqual(typeof mod.enableCompileCache, 'function', 'enableCompileCache is function');
assertEqual(typeof mod.getCompileCacheDir, 'function', 'getCompileCacheDir is function');
assertEqual(typeof mod.flushCompileCache, 'function', 'flushCompileCache is function');
assertEqual(typeof mod.readPackageJSON, 'function', 'readPackageJSON is function');
assertEqual(typeof mod.getPackageScopeConfig, 'function', 'getPackageScopeConfig is function');
assertEqual(typeof mod.getPackageType, 'function', 'getPackageType is function');
assertEqual(typeof mod.getNearestParentPackageJSONType, 'function', 'getNearestParentPackageJSONType is function');
assertEqual(typeof mod.setLazyPathHelpers, 'function', 'setLazyPathHelpers is function');

// --- compileCacheStatus ---
assert(Array.isArray(mod.compileCacheStatus), 'compileCacheStatus is array');
assertEqual(mod.compileCacheStatus.length, 4, 'compileCacheStatus has 4 entries');
assertEqual(mod.compileCacheStatus[0], 'FAILED', 'compileCacheStatus[0] is FAILED');
assertEqual(mod.compileCacheStatus[1], 'ENABLED', 'compileCacheStatus[1] is ENABLED');
assertEqual(mod.compileCacheStatus[2], 'ALREADY_ENABLED', 'compileCacheStatus[2] is ALREADY_ENABLED');
assertEqual(mod.compileCacheStatus[3], 'DISABLED', 'compileCacheStatus[3] is DISABLED');

// --- enableCompileCache returns {status, directory} ---
var result = mod.enableCompileCache();
assertEqual(typeof result, 'object', 'enableCompileCache returns object');
assertEqual(result.status, 0, 'enableCompileCache status is 0 (FAILED)');
assertEqual(result.directory, '', 'enableCompileCache directory is empty');

// --- getCompileCacheDir returns string ---
assertEqual(typeof mod.getCompileCacheDir(), 'string', 'getCompileCacheDir returns string');

// --- flushCompileCache is no-op (returns undefined) ---
assertEqual(mod.flushCompileCache(), undefined, 'flushCompileCache returns undefined');

// --- readPackageJSON returns undefined (stub) ---
assertEqual(mod.readPackageJSON('/nonexistent'), undefined, 'readPackageJSON returns undefined');

// --- getPackageScopeConfig returns undefined (stub) ---
assertEqual(mod.getPackageScopeConfig('file:///test'), undefined, 'getPackageScopeConfig returns undefined');

// --- getPackageType returns undefined (stub) ---
assertEqual(mod.getPackageType('file:///test'), undefined, 'getPackageType returns undefined');

console.log('PASS');
