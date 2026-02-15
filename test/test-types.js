// Copyright (c) Tzvetan Mikov.
// Test the types binding.
'use strict';

var t = internalBinding('types');

// Helper
function assert(cond, msg) {
  if (!cond) throw new Error('Assertion failed: ' + msg);
}

// --- isArrayBuffer ---
assert(t.isArrayBuffer(new ArrayBuffer(8)) === true, 'isArrayBuffer(ArrayBuffer)');
assert(t.isArrayBuffer({}) === false, 'isArrayBuffer({})');
assert(t.isArrayBuffer(42) === false, 'isArrayBuffer(42)');
assert(t.isArrayBuffer(null) === false, 'isArrayBuffer(null)');
assert(t.isArrayBuffer(undefined) === false, 'isArrayBuffer(undefined)');

// --- isTypedArray ---
assert(t.isTypedArray(new Uint8Array(4)) === true, 'isTypedArray(Uint8Array)');
assert(t.isTypedArray(new Float64Array(2)) === true, 'isTypedArray(Float64Array)');
assert(t.isTypedArray(new Int32Array(1)) === true, 'isTypedArray(Int32Array)');
assert(t.isTypedArray(new ArrayBuffer(8)) === false, 'isTypedArray(ArrayBuffer)');
assert(t.isTypedArray({}) === false, 'isTypedArray({})');

// --- isDataView ---
assert(t.isDataView(new DataView(new ArrayBuffer(8))) === true, 'isDataView(DataView)');
assert(t.isDataView(new Uint8Array(4)) === false, 'isDataView(Uint8Array)');
assert(t.isDataView({}) === false, 'isDataView({})');

// --- isDate ---
assert(t.isDate(new Date()) === true, 'isDate(Date)');
assert(t.isDate({}) === false, 'isDate({})');
assert(t.isDate('2024-01-01') === false, 'isDate(string)');

// --- isPromise ---
assert(t.isPromise(Promise.resolve()) === true, 'isPromise(Promise)');
assert(t.isPromise({then: function(){}}) === false, 'isPromise(thenable)');
assert(t.isPromise({}) === false, 'isPromise({})');

// --- isRegExp ---
assert(t.isRegExp(/abc/) === true, 'isRegExp(/abc/)');
assert(t.isRegExp(new RegExp('abc')) === true, 'isRegExp(new RegExp)');
assert(t.isRegExp({}) === false, 'isRegExp({})');

// --- isMap ---
assert(t.isMap(new Map()) === true, 'isMap(Map)');
assert(t.isMap({}) === false, 'isMap({})');

// --- isSet ---
assert(t.isSet(new Set()) === true, 'isSet(Set)');
assert(t.isSet({}) === false, 'isSet({})');

// --- isWeakMap ---
assert(t.isWeakMap(new WeakMap()) === true, 'isWeakMap(WeakMap)');
assert(t.isWeakMap({}) === false, 'isWeakMap({})');

// --- isWeakSet ---
assert(t.isWeakSet(new WeakSet()) === true, 'isWeakSet(WeakSet)');
assert(t.isWeakSet({}) === false, 'isWeakSet({})');

// --- isNativeError ---
assert(t.isNativeError(new Error()) === true, 'isNativeError(Error)');
assert(t.isNativeError(new TypeError()) === true, 'isNativeError(TypeError)');
assert(t.isNativeError(new RangeError()) === true, 'isNativeError(RangeError)');
assert(t.isNativeError({}) === false, 'isNativeError({})');
assert(t.isNativeError('error') === false, 'isNativeError(string)');

// --- isExternal ---
// Externals can't be created from JS; just test that non-externals return false.
assert(t.isExternal({}) === false, 'isExternal({})');
assert(t.isExternal(42) === false, 'isExternal(42)');

// --- isAnyArrayBuffer ---
assert(t.isAnyArrayBuffer(new ArrayBuffer(8)) === true, 'isAnyArrayBuffer(ArrayBuffer)');
assert(t.isAnyArrayBuffer({}) === false, 'isAnyArrayBuffer({})');

// --- isArrayBufferView ---
assert(t.isArrayBufferView(new Uint8Array(4)) === true, 'isArrayBufferView(Uint8Array)');
assert(t.isArrayBufferView(new DataView(new ArrayBuffer(8))) === true, 'isArrayBufferView(DataView)');
assert(t.isArrayBufferView(new ArrayBuffer(8)) === false, 'isArrayBufferView(ArrayBuffer)');
assert(t.isArrayBufferView({}) === false, 'isArrayBufferView({})');

// --- isNumberObject ---
assert(t.isNumberObject(Object(42)) === true, 'isNumberObject(Object(42))');
assert(t.isNumberObject(42) === false, 'isNumberObject(42)');
assert(t.isNumberObject({}) === false, 'isNumberObject({})');

// --- isStringObject ---
assert(t.isStringObject(Object('hello')) === true, 'isStringObject(Object("hello"))');
assert(t.isStringObject('hello') === false, 'isStringObject("hello")');

// --- isBooleanObject ---
assert(t.isBooleanObject(Object(true)) === true, 'isBooleanObject(Object(true))');
assert(t.isBooleanObject(true) === false, 'isBooleanObject(true)');

// --- isSymbolObject ---
assert(t.isSymbolObject(Object(Symbol('x'))) === true, 'isSymbolObject(Object(Symbol))');
assert(t.isSymbolObject(Symbol('x')) === false, 'isSymbolObject(Symbol)');

// --- isBigIntObject ---
assert(t.isBigIntObject(Object(BigInt(42))) === true, 'isBigIntObject(Object(BigInt))');
assert(t.isBigIntObject(BigInt(42)) === false, 'isBigIntObject(BigInt)');

// --- isBoxedPrimitive ---
assert(t.isBoxedPrimitive(Object(42)) === true, 'isBoxedPrimitive(Object(42))');
assert(t.isBoxedPrimitive(Object('a')) === true, 'isBoxedPrimitive(Object("a"))');
assert(t.isBoxedPrimitive(Object(true)) === true, 'isBoxedPrimitive(Object(true))');
assert(t.isBoxedPrimitive(Object(Symbol())) === true, 'isBoxedPrimitive(Object(Symbol))');
assert(t.isBoxedPrimitive(Object(BigInt(1))) === true, 'isBoxedPrimitive(Object(BigInt))');
assert(t.isBoxedPrimitive(42) === false, 'isBoxedPrimitive(42)');
assert(t.isBoxedPrimitive({}) === false, 'isBoxedPrimitive({})');

// --- isGeneratorFunction ---
// Hermes does support generator functions.
var genFn = (function*() { yield 1; });
// genFn is actually a generator OBJECT, not a generator function.
// The function itself is a generator function:
var genFnCtor = function*() { yield 1; };
assert(t.isGeneratorFunction(genFnCtor) === true, 'isGeneratorFunction(function*)');
assert(t.isGeneratorFunction(function(){}) === false, 'isGeneratorFunction(function)');
assert(t.isGeneratorFunction({}) === false, 'isGeneratorFunction({})');

// --- isGeneratorObject ---
var gen = genFnCtor();
assert(t.isGeneratorObject(gen) === true, 'isGeneratorObject(generator)');
assert(t.isGeneratorObject({}) === false, 'isGeneratorObject({})');
assert(t.isGeneratorObject(genFnCtor) === false, 'isGeneratorObject(genFn)');

// --- isMapIterator ---
var mapIter = new Map([[1,2]]).entries();
assert(t.isMapIterator(mapIter) === true, 'isMapIterator(map.entries())');
assert(t.isMapIterator({}) === false, 'isMapIterator({})');

// --- isSetIterator ---
var setIter = new Set([1,2]).values();
assert(t.isSetIterator(setIter) === true, 'isSetIterator(set.values())');
assert(t.isSetIterator({}) === false, 'isSetIterator({})');

// --- isProxy (stub, always false) ---
assert(t.isProxy({}) === false, 'isProxy({})');

// --- isModuleNamespaceObject (stub, always false) ---
assert(t.isModuleNamespaceObject({}) === false, 'isModuleNamespaceObject({})');

// --- isArgumentsObject ---
assert(t.isArgumentsObject({}) === false, 'isArgumentsObject({})');
// Test with an actual arguments object (non-strict).
var argObj = (function() { return arguments; })(1, 2, 3);
assert(t.isArgumentsObject(argObj) === true, 'isArgumentsObject(arguments)');

// --- isUint8Array ---
assert(t.isUint8Array(new Uint8Array(4)) === true, 'isUint8Array(Uint8Array)');
assert(t.isUint8Array(new Uint16Array(4)) === false, 'isUint8Array(Uint16Array)');
assert(t.isUint8Array(new Int8Array(4)) === false, 'isUint8Array(Int8Array)');
assert(t.isUint8Array({}) === false, 'isUint8Array({})');

// --- isAsyncFunction ---
// Hermes supports async functions.
var asyncFn = async function() { return 42; };
assert(t.isAsyncFunction(asyncFn) === true, 'isAsyncFunction(async fn)');
assert(t.isAsyncFunction(function(){}) === false, 'isAsyncFunction(function)');
assert(t.isAsyncFunction({}) === false, 'isAsyncFunction({})');

// --- Functions exist check ---
assert(typeof t.isArrayBuffer === 'function', 'isArrayBuffer is a function');
assert(typeof t.isArrayBufferView === 'function', 'isArrayBufferView is a function');
assert(typeof t.isAsyncFunction === 'function', 'isAsyncFunction is a function');
assert(typeof t.isDataView === 'function', 'isDataView is a function');
assert(typeof t.isDate === 'function', 'isDate is a function');
assert(typeof t.isExternal === 'function', 'isExternal is a function');
assert(typeof t.isMap === 'function', 'isMap is a function');
assert(typeof t.isMapIterator === 'function', 'isMapIterator is a function');
assert(typeof t.isModuleNamespaceObject === 'function', 'isModuleNamespaceObject is a function');
assert(typeof t.isNativeError === 'function', 'isNativeError is a function');
assert(typeof t.isPromise === 'function', 'isPromise is a function');
assert(typeof t.isRegExp === 'function', 'isRegExp is a function');
assert(typeof t.isSet === 'function', 'isSet is a function');
assert(typeof t.isSetIterator === 'function', 'isSetIterator is a function');
assert(typeof t.isTypedArray === 'function', 'isTypedArray is a function');
assert(typeof t.isWeakMap === 'function', 'isWeakMap is a function');
assert(typeof t.isWeakSet === 'function', 'isWeakSet is a function');
assert(typeof t.isGeneratorFunction === 'function', 'isGeneratorFunction is a function');
assert(typeof t.isGeneratorObject === 'function', 'isGeneratorObject is a function');
assert(typeof t.isArgumentsObject === 'function', 'isArgumentsObject is a function');
assert(typeof t.isNumberObject === 'function', 'isNumberObject is a function');
assert(typeof t.isStringObject === 'function', 'isStringObject is a function');
assert(typeof t.isBooleanObject === 'function', 'isBooleanObject is a function');
assert(typeof t.isBigIntObject === 'function', 'isBigIntObject is a function');
assert(typeof t.isSymbolObject === 'function', 'isSymbolObject is a function');
assert(typeof t.isBoxedPrimitive === 'function', 'isBoxedPrimitive is a function');
assert(typeof t.isProxy === 'function', 'isProxy is a function');
assert(typeof t.isAnyArrayBuffer === 'function', 'isAnyArrayBuffer is a function');
assert(typeof t.isUint8Array === 'function', 'isUint8Array is a function');

console.log('PASS');
