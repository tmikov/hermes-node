// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %s | %FileCheck %s
// CHECK: PASS
// Test the internal/modules/helpers shim.
'use strict';

var helpers = require('internal/modules/helpers');

// Helper
function assert(cond, msg) {
  if (!cond) throw new Error('Assertion failed: ' + msg);
}
function assertEqual(a, b, msg) {
  if (a !== b) throw new Error('Assertion failed: ' + msg + ' (' + a + ' !== ' + b + ')');
}

// --- Verify exports exist ---
assertEqual(typeof helpers.makeRequireFunction, 'function', 'makeRequireFunction is function');
assertEqual(typeof helpers.addBuiltinLibsToObject, 'function', 'addBuiltinLibsToObject is function');
assertEqual(typeof helpers.stripBOM, 'function', 'stripBOM is function');
assertEqual(typeof helpers.enableCompileCache, 'function', 'enableCompileCache is function');
assertEqual(typeof helpers.flushCompileCache, 'function', 'flushCompileCache is function');
assertEqual(typeof helpers.getCompileCacheDir, 'function', 'getCompileCacheDir is function');
assertEqual(typeof helpers.loadBuiltinModule, 'function', 'loadBuiltinModule is function');
assertEqual(typeof helpers.toRealPath, 'function', 'toRealPath is function');
assertEqual(typeof helpers.getCjsConditions, 'function', 'getCjsConditions is function');
assertEqual(typeof helpers.getCjsConditionsArray, 'function', 'getCjsConditionsArray is function');
assertEqual(typeof helpers.initializeCjsConditions, 'function', 'initializeCjsConditions is function');
assertEqual(typeof helpers.normalizeReferrerURL, 'function', 'normalizeReferrerURL is function');
assertEqual(typeof helpers.stringify, 'function', 'stringify is function');
assertEqual(typeof helpers.assertBufferSource, 'function', 'assertBufferSource is function');
assertEqual(typeof helpers.getBuiltinModule, 'function', 'getBuiltinModule is function');
assertEqual(typeof helpers.urlToFilename, 'function', 'urlToFilename is function');
assertEqual(typeof helpers.hasStartedUserCJSExecution, 'function', 'hasStartedUserCJSExecution is function');
assertEqual(typeof helpers.setHasStartedUserCJSExecution, 'function', 'setHasStartedUserCJSExecution is function');
assertEqual(typeof helpers.hasStartedUserESMExecution, 'function', 'hasStartedUserESMExecution is function');
assertEqual(typeof helpers.setHasStartedUserESMExecution, 'function', 'setHasStartedUserESMExecution is function');

// --- constants ---
assertEqual(typeof helpers.constants, 'object', 'constants is object');
assertEqual(typeof helpers.constants.compileCacheStatus, 'object', 'compileCacheStatus is object');
assertEqual(helpers.constants.compileCacheStatus.FAILED, 0, 'compileCacheStatus.FAILED is 0');
assertEqual(helpers.constants.compileCacheStatus.ENABLED, 1, 'compileCacheStatus.ENABLED is 1');

// --- stripBOM ---
assertEqual(helpers.stripBOM('\uFEFFhello'), 'hello', 'stripBOM removes BOM');
assertEqual(helpers.stripBOM('hello'), 'hello', 'stripBOM preserves non-BOM');

// --- stub behavior ---
assertEqual(helpers.enableCompileCache().status, 0, 'enableCompileCache returns status 0');
assertEqual(helpers.getCompileCacheDir(), undefined, 'getCompileCacheDir returns undefined');
assertEqual(helpers.loadBuiltinModule('nonexistent'), undefined, 'loadBuiltinModule nonexistent returns undefined');
assertEqual(helpers.hasStartedUserCJSExecution(), false, 'hasStartedUserCJSExecution returns false');
assertEqual(helpers.hasStartedUserESMExecution(), false, 'hasStartedUserESMExecution returns false');
assertEqual(helpers.getBuiltinModule('nonexistent'), undefined, 'getBuiltinModule nonexistent returns undefined');

// loadBuiltinModule with a real module returns an object with exports.
var fsMod = helpers.loadBuiltinModule('fs');
assert(fsMod !== undefined, 'loadBuiltinModule fs returns non-undefined');
assertEqual(typeof fsMod.exports, 'object', 'loadBuiltinModule fs has exports');
assertEqual(typeof fsMod.exports.readFileSync, 'function', 'loadBuiltinModule fs.exports.readFileSync is function');

// getBuiltinModule with a real module returns the module exports.
var pathMod = helpers.getBuiltinModule('path');
assert(pathMod !== undefined, 'getBuiltinModule path returns non-undefined');
assertEqual(typeof pathMod.join, 'function', 'getBuiltinModule path.join is function');

// --- stringify ---
assertEqual(helpers.stringify('test'), 'test', 'stringify string passthrough');

console.log('PASS');
