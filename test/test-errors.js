// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %s | %FileCheck %s
// CHECK: PASS
// Test the errors binding.
'use strict';

var e = internalBinding('errors');

// Helper
function assert(cond, msg) {
  if (!cond) throw new Error('Assertion failed: ' + msg);
}
function assertEqual(a, b, msg) {
  if (a !== b) throw new Error('Assertion failed: ' + msg + ' (' + a + ' !== ' + b + ')');
}

// --- triggerUncaughtException ---
assertEqual(typeof e.triggerUncaughtException, 'function', 'triggerUncaughtException is function');

// --- noSideEffectsToString ---
assertEqual(typeof e.noSideEffectsToString, 'function', 'noSideEffectsToString is function');
assertEqual(e.noSideEffectsToString('hello'), 'hello', 'noSideEffectsToString string');
assertEqual(e.noSideEffectsToString(42), '42', 'noSideEffectsToString number');
assertEqual(e.noSideEffectsToString(null), 'null', 'noSideEffectsToString null');
assertEqual(e.noSideEffectsToString(undefined), 'undefined', 'noSideEffectsToString undefined');
assertEqual(e.noSideEffectsToString(true), 'true', 'noSideEffectsToString boolean');

// --- Stub functions exist ---
assertEqual(typeof e.setPrepareStackTraceCallback, 'function', 'setPrepareStackTraceCallback is function');
assertEqual(typeof e.setGetSourceMapErrorSource, 'function', 'setGetSourceMapErrorSource is function');
assertEqual(typeof e.setSourceMapsEnabled, 'function', 'setSourceMapsEnabled is function');
assertEqual(typeof e.setMaybeCacheGeneratedSourceMap, 'function', 'setMaybeCacheGeneratedSourceMap is function');
assertEqual(typeof e.setEnhanceStackForFatalException, 'function', 'setEnhanceStackForFatalException is function');
assertEqual(typeof e.getErrorSourcePositions, 'function', 'getErrorSourcePositions is function');

// Stubs should not throw when called.
e.setPrepareStackTraceCallback(function() {});
e.setGetSourceMapErrorSource(function() {});
e.setSourceMapsEnabled(true);
e.setMaybeCacheGeneratedSourceMap(function() {});
e.setEnhanceStackForFatalException(function() {}, function() {});
assertEqual(e.getErrorSourcePositions(new Error('test')), undefined, 'getErrorSourcePositions returns undefined');

// --- exitCodes ---
assert(typeof e.exitCodes === 'object', 'exitCodes is object');
assertEqual(e.exitCodes.kNoFailure, 0, 'kNoFailure');
assertEqual(e.exitCodes.kGenericUserError, 1, 'kGenericUserError');
assertEqual(e.exitCodes.kInternalJSParseError, 3, 'kInternalJSParseError');
assertEqual(e.exitCodes.kInternalJSEvaluationFailure, 4, 'kInternalJSEvaluationFailure');
assertEqual(e.exitCodes.kV8FatalError, 5, 'kV8FatalError');
assertEqual(e.exitCodes.kInvalidFatalExceptionMonkeyPatching, 6, 'kInvalidFatalExceptionMonkeyPatching');
assertEqual(e.exitCodes.kExceptionInFatalExceptionHandler, 7, 'kExceptionInFatalExceptionHandler');
assertEqual(e.exitCodes.kInvalidCommandLineArgument, 9, 'kInvalidCommandLineArgument');
assertEqual(e.exitCodes.kBootstrapFailure, 10, 'kBootstrapFailure');
assertEqual(e.exitCodes.kInvalidCommandLineArgument2, 12, 'kInvalidCommandLineArgument2');
assertEqual(e.exitCodes.kUnsettledTopLevelAwait, 13, 'kUnsettledTopLevelAwait');
assertEqual(e.exitCodes.kStartupSnapshotFailure, 14, 'kStartupSnapshotFailure');
assertEqual(e.exitCodes.kAbort, 134, 'kAbort');

// Don't call triggerUncaughtException -- it exits the process.

console.log('PASS');
