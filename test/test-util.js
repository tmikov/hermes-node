// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %s | %FileCheck %s
// CHECK: PASS
// Test the util binding.
'use strict';

var u = internalBinding('util');

// Helper
function assert(cond, msg) {
  if (!cond) throw new Error('Assertion failed: ' + msg);
}

// --- sleep ---
// Just verify it doesn't throw.
u.sleep(1);

// --- guessHandleType ---
// stdout (fd 1) should be one of the known types.
var handleCode = u.guessHandleType(1);
assert(typeof handleCode === 'number', 'guessHandleType returns number');
assert(handleCode >= 0 && handleCode <= 5, 'guessHandleType in valid range');

// --- getOwnNonIndexProperties ---
var obj = { a: 1, b: 2 };
obj[0] = 'zero';
obj[42] = 'fortytwo';
var props = u.getOwnNonIndexProperties(obj, 0); // ALL_PROPERTIES
assert(Array.isArray(props), 'getOwnNonIndexProperties returns array');
// Should contain 'a' and 'b' but not '0' or '42'.
var hasA = false, hasB = false, has0 = false, has42 = false;
for (var i = 0; i < props.length; i++) {
  if (props[i] === 'a') hasA = true;
  if (props[i] === 'b') hasB = true;
  if (props[i] === '0') has0 = true;
  if (props[i] === '42') has42 = true;
}
assert(hasA, 'getOwnNonIndexProperties includes "a"');
assert(hasB, 'getOwnNonIndexProperties includes "b"');
assert(!has0, 'getOwnNonIndexProperties excludes "0"');
assert(!has42, 'getOwnNonIndexProperties excludes "42"');

// Test with ONLY_ENUMERABLE filter (value 2).
var obj2 = {};
Object.defineProperty(obj2, 'visible', { value: 1, enumerable: true, configurable: true });
Object.defineProperty(obj2, 'hidden', { value: 2, enumerable: false, configurable: true });
var enumProps = u.getOwnNonIndexProperties(obj2, 2); // ONLY_ENUMERABLE
var hasVisible = false, hasHidden = false;
for (var i = 0; i < enumProps.length; i++) {
  if (enumProps[i] === 'visible') hasVisible = true;
  if (enumProps[i] === 'hidden') hasHidden = true;
}
assert(hasVisible, 'ONLY_ENUMERABLE includes enumerable');
assert(!hasHidden, 'ONLY_ENUMERABLE excludes non-enumerable');

// --- privateSymbols ---
assert(typeof u.privateSymbols === 'object', 'privateSymbols is object');
assert(typeof u.privateSymbols.arrow_message_private_symbol === 'symbol',
  'arrow_message_private_symbol is symbol');
assert(typeof u.privateSymbols.decorated_private_symbol === 'symbol',
  'decorated_private_symbol is symbol');
assert(typeof u.privateSymbols.exit_info_private_symbol === 'symbol',
  'exit_info_private_symbol is symbol');
assert(typeof u.privateSymbols.host_defined_option_symbol === 'symbol',
  'host_defined_option_symbol is symbol');
assert(typeof u.privateSymbols.transfer_mode_private_symbol === 'symbol',
  'transfer_mode_private_symbol is symbol');
assert(typeof u.privateSymbols.entry_point_module_private_symbol === 'symbol',
  'entry_point_module_private_symbol is symbol');
assert(typeof u.privateSymbols.entry_point_promise_private_symbol === 'symbol',
  'entry_point_promise_private_symbol is symbol');
assert(typeof u.privateSymbols.module_source_private_symbol === 'symbol',
  'module_source_private_symbol is symbol');
assert(typeof u.privateSymbols.module_export_private_symbol === 'symbol',
  'module_export_private_symbol is symbol');
assert(typeof u.privateSymbols.module_first_parent_private_symbol === 'symbol',
  'module_first_parent_private_symbol is symbol');
assert(typeof u.privateSymbols.module_last_parent_private_symbol === 'symbol',
  'module_last_parent_private_symbol is symbol');

// Test that private symbols work as property keys.
var testObj = {};
testObj[u.privateSymbols.arrow_message_private_symbol] = 'hello';
assert(testObj[u.privateSymbols.arrow_message_private_symbol] === 'hello',
  'private symbol works as property key');

// --- constants ---
assert(typeof u.constants === 'object', 'constants is object');
assert(u.constants.kPending === 0, 'kPending === 0');
assert(u.constants.kFulfilled === 1, 'kFulfilled === 1');
assert(u.constants.kRejected === 2, 'kRejected === 2');
assert(u.constants.kExiting === 0, 'kExiting === 0');
assert(u.constants.kExitCode === 1, 'kExitCode === 1');
assert(u.constants.kHasExitCode === 2, 'kHasExitCode === 2');
assert(u.constants.ALL_PROPERTIES === 0, 'ALL_PROPERTIES === 0');
assert(u.constants.ONLY_WRITABLE === 1, 'ONLY_WRITABLE === 1');
assert(u.constants.ONLY_ENUMERABLE === 2, 'ONLY_ENUMERABLE === 2');
assert(u.constants.ONLY_CONFIGURABLE === 4, 'ONLY_CONFIGURABLE === 4');
assert(u.constants.SKIP_STRINGS === 8, 'SKIP_STRINGS === 8');
assert(u.constants.SKIP_SYMBOLS === 16, 'SKIP_SYMBOLS === 16');
assert(u.constants.kDisallowCloneAndTransfer === 0, 'kDisallowCloneAndTransfer === 0');
assert(u.constants.kTransferable === 1, 'kTransferable === 1');
assert(u.constants.kCloneable === 2, 'kCloneable === 2');

// --- shouldAbortOnUncaughtToggle ---
assert(u.shouldAbortOnUncaughtToggle instanceof Uint32Array,
  'shouldAbortOnUncaughtToggle is Uint32Array');
assert(u.shouldAbortOnUncaughtToggle.length === 1,
  'shouldAbortOnUncaughtToggle length is 1');
assert(u.shouldAbortOnUncaughtToggle[0] === 0,
  'shouldAbortOnUncaughtToggle initial value is 0');

// --- Stubs don't crash ---
u.getPromiseDetails(Promise.resolve(42));
u.getProxyDetails({});
u.previewEntries([]);

// getPromiseDetails returns undefined for non-promise.
assert(u.getPromiseDetails(42) === undefined, 'getPromiseDetails(42) === undefined');
// getPromiseDetails returns undefined for promise (stub).
assert(u.getPromiseDetails(Promise.resolve(1)) === undefined,
  'getPromiseDetails stub returns undefined');

// --- getConstructorName ---
function MyClass() {}
var inst = new MyClass();
var ctorName = u.getConstructorName(inst);
assert(ctorName === 'MyClass', 'getConstructorName returns "MyClass", got: ' + ctorName);

var plainObj = {};
var plainCtorName = u.getConstructorName(plainObj);
assert(plainCtorName === 'Object', 'getConstructorName({}) returns "Object", got: ' + plainCtorName);

// --- arrayBufferViewHasBuffer ---
var view = new Uint8Array(4);
assert(u.arrayBufferViewHasBuffer(view) === true, 'arrayBufferViewHasBuffer returns true');

// --- isInsideNodeModules ---
// Stub: returns default value (second arg).
assert(u.isInsideNodeModules(100, true) === true, 'isInsideNodeModules returns default true');
assert(u.isInsideNodeModules(100, false) === false, 'isInsideNodeModules returns default false');

// --- getCallSites ---
var sites = u.getCallSites(10);
assert(Array.isArray(sites), 'getCallSites returns array');

// --- parseEnv ---
var parsed = u.parseEnv('KEY=value');
assert(typeof parsed === 'object', 'parseEnv returns object');

console.log('PASS');
