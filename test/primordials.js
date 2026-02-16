// Copyright (c) Tzvetan Mikov.
// RUN: cat %source_dir/libjs/primordials.js %s > %t.js && %hermes -Xasync-generators %t.js
// Test for the primordials thin shim.
'use strict';

var passed = 0;
var failed = 0;

function assert(cond, msg) {
  if (cond) {
    passed++;
  } else {
    failed++;
    print('FAIL: ' + msg);
  }
}

var p = globalThis.primordials;
assert(typeof p === 'object', 'primordials is an object');

// ---- uncurryThis / applyBind ----
assert(typeof p.uncurryThis === 'function', 'uncurryThis');
assert(typeof p.applyBind === 'function', 'applyBind');

// ---- Prototype methods as uncurried functions ----
assert(p.ArrayPrototypePush([1, 2], 3) === 3, 'ArrayPrototypePush returns new length');
var arr = [1, 2];
p.ArrayPrototypePush(arr, 3);
assert(arr.length === 3, 'ArrayPrototypePush mutates');

assert(p.StringPrototypeSlice('hello', 1) === 'ello', 'StringPrototypeSlice');
assert(p.StringPrototypeStartsWith('hello', 'he') === true, 'StringPrototypeStartsWith');
assert(p.StringPrototypeEndsWith('hello', 'lo') === true, 'StringPrototypeEndsWith');

assert(p.ObjectKeys({a: 1, b: 2}).length === 2, 'ObjectKeys');
assert(typeof p.ObjectDefineProperty === 'function', 'ObjectDefineProperty');
assert(typeof p.ObjectGetOwnPropertyDescriptor === 'function', 'ObjectGetOwnPropertyDescriptor');
assert(typeof p.ObjectSetPrototypeOf === 'function', 'ObjectSetPrototypeOf');
assert(typeof p.ObjectFreeze === 'function', 'ObjectFreeze');
assert(typeof p.ObjectCreate === 'function', 'ObjectCreate');

// ---- ArrayIsArray ----
assert(p.ArrayIsArray([]) === true, 'ArrayIsArray([])');
assert(p.ArrayIsArray({}) === false, 'ArrayIsArray({})');

// ---- Number statics ----
assert(p.NumberIsNaN(NaN) === true, 'NumberIsNaN(NaN)');
assert(p.NumberIsNaN(42) === false, 'NumberIsNaN(42)');
assert(p.NumberIsFinite(42) === true, 'NumberIsFinite(42)');
assert(p.NumberIsFinite(Infinity) === false, 'NumberIsFinite(Infinity)');
assert(typeof p.NumberParseInt === 'function', 'NumberParseInt');

// ---- Safe variants are originals ----
assert(p.SafeMap === Map, 'SafeMap === Map');
assert(p.SafeSet === Set, 'SafeSet === Set');
assert(p.SafeWeakMap === WeakMap, 'SafeWeakMap === WeakMap');
assert(p.SafeWeakSet === WeakSet, 'SafeWeakSet === WeakSet');

// ---- Constructors ----
assert(p.Map === Map, 'Map');
assert(p.Set === Set, 'Set');
assert(p.Array === Array, 'Array');
assert(p.String === String, 'String');
assert(p.Object === Object, 'Object');
assert(p.RegExp === RegExp, 'RegExp');
assert(p.Error === Error, 'Error');
assert(p.TypeError === TypeError, 'TypeError');
assert(p.Promise === Promise, 'Promise');
assert(p.Symbol === Symbol, 'Symbol');
assert(p.Date === Date, 'Date');
assert(p.Boolean === Boolean, 'Boolean');
assert(p.Number === Number, 'Number');
assert(p.Function === Function, 'Function');

// ---- Date prototype ----
assert(typeof p.DatePrototypeGetTime === 'function', 'DatePrototypeGetTime');
var d = new Date(0);
assert(p.DatePrototypeGetTime(d) === 0, 'DatePrototypeGetTime(epoch) === 0');

// ---- RegExp prototype ----
assert(typeof p.RegExpPrototypeExec === 'function', 'RegExpPrototypeExec');
assert(typeof p.RegExpPrototypeTest === 'function', 'RegExpPrototypeTest');
assert(p.RegExpPrototypeTest(/abc/, 'xabcy') === true, 'RegExpPrototypeTest');
assert(p.RegExpPrototypeTest(/abc/, 'xyz') === false, 'RegExpPrototypeTest false');

// ---- Promise statics (bound) ----
assert(typeof p.PromiseAll === 'function', 'PromiseAll');
assert(typeof p.PromiseResolve === 'function', 'PromiseResolve');
assert(typeof p.PromiseReject === 'function', 'PromiseReject');

// ---- Promise prototype ----
assert(typeof p.PromisePrototypeThen === 'function', 'PromisePrototypeThen');
assert(typeof p.PromisePrototypeCatch === 'function', 'PromisePrototypeCatch');

// ---- Math statics ----
assert(typeof p.MathMax === 'function', 'MathMax');
assert(typeof p.MathMin === 'function', 'MathMin');
assert(typeof p.MathFloor === 'function', 'MathFloor');
assert(p.MathMax(1, 2, 3) === 3, 'MathMax(1,2,3)');

// ---- JSON ----
assert(typeof p.JSONStringify === 'function', 'JSONStringify');
assert(typeof p.JSONParse === 'function', 'JSONParse');
assert(p.JSONStringify({a: 1}) === '{"a":1}', 'JSONStringify');

// ---- Reflect ----
assert(typeof p.ReflectApply === 'function', 'ReflectApply');
assert(typeof p.ReflectConstruct === 'function', 'ReflectConstruct');
assert(typeof p.ReflectDefineProperty === 'function', 'ReflectDefineProperty');
assert(typeof p.ReflectOwnKeys === 'function', 'ReflectOwnKeys');
assert(typeof p.ReflectGetPrototypeOf === 'function', 'ReflectGetPrototypeOf');
assert(typeof p.ReflectGet === 'function', 'ReflectGet');
assert(typeof p.ReflectSet === 'function', 'ReflectSet');

// ---- Symbol statics ----
assert(typeof p.SymbolIterator === 'symbol', 'SymbolIterator');
assert(p.SymbolIterator === Symbol.iterator, 'SymbolIterator === Symbol.iterator');
assert(typeof p.SymbolFor === 'function', 'SymbolFor');
assert(typeof p.SymbolHasInstance === 'symbol', 'SymbolHasInstance');
assert(typeof p.SymbolMatch === 'symbol', 'SymbolMatch');

// ---- TypedArray ----
assert(typeof p.TypedArray === 'function', 'TypedArray');
assert(typeof p.TypedArrayPrototypeSet === 'function', 'TypedArrayPrototypeSet');

// ---- ArrayBuffer ----
assert(typeof p.ArrayBufferIsView === 'function', 'ArrayBufferIsView');
assert(p.ArrayBufferIsView(new Uint8Array(4)) === true, 'ArrayBufferIsView(Uint8Array)');
assert(p.ArrayBufferIsView({}) === false, 'ArrayBufferIsView({})');

// ---- Map / Set prototype ----
assert(typeof p.MapPrototypeGet === 'function', 'MapPrototypeGet');
assert(typeof p.MapPrototypeSet === 'function', 'MapPrototypeSet');
assert(typeof p.MapPrototypeHas === 'function', 'MapPrototypeHas');
assert(typeof p.MapPrototypeDelete === 'function', 'MapPrototypeDelete');
assert(typeof p.SetPrototypeAdd === 'function', 'SetPrototypeAdd');
assert(typeof p.SetPrototypeHas === 'function', 'SetPrototypeHas');

var m = new Map();
p.MapPrototypeSet(m, 'key', 'val');
assert(p.MapPrototypeGet(m, 'key') === 'val', 'MapPrototypeGet/Set');
assert(p.MapPrototypeHas(m, 'key') === true, 'MapPrototypeHas');

// ---- Error prototype ----
assert(typeof p.ErrorPrototypeToString === 'function', 'ErrorPrototypeToString');

// ---- Getters ----
assert(typeof p.RegExpPrototypeGetFlags === 'function', 'RegExpPrototypeGetFlags');
assert(typeof p.RegExpPrototypeGetSource === 'function', 'RegExpPrototypeGetSource');
assert(typeof p.RegExpPrototypeGetGlobal === 'function', 'RegExpPrototypeGetGlobal');
assert(p.RegExpPrototypeGetFlags(/abc/gi) === 'gi', 'RegExpPrototypeGetFlags(/abc/gi)');

// ---- ArrayPrototypeGetLength ----
assert(typeof p.ArrayPrototypeSlice === 'function', 'ArrayPrototypeSlice');
assert(p.ArrayPrototypeSlice([1,2,3], 1).length === 2, 'ArrayPrototypeSlice');

// ---- ArrayPrototypePush Apply variant ----
assert(typeof p.ArrayPrototypePushApply === 'function', 'ArrayPrototypePushApply');

// ---- SafeArrayIterator ----
assert(typeof p.SafeArrayIterator === 'function', 'SafeArrayIterator');
var it = new p.SafeArrayIterator([10, 20, 30]);
var r1 = it.next();
assert(r1.value === 10 && r1.done === false, 'SafeArrayIterator 1st');
var r2 = it.next();
assert(r2.value === 20 && r2.done === false, 'SafeArrayIterator 2nd');
it.next(); // 30
var r4 = it.next();
assert(r4.done === true, 'SafeArrayIterator done');

// ---- SafeStringIterator ----
assert(typeof p.SafeStringIterator === 'function', 'SafeStringIterator');
var sit = new p.SafeStringIterator('ab');
assert(sit.next().value === 'a', 'SafeStringIterator a');
assert(sit.next().value === 'b', 'SafeStringIterator b');
assert(sit.next().done === true, 'SafeStringIterator done');

// ---- hardenRegExp ----
assert(typeof p.hardenRegExp === 'function', 'hardenRegExp');
var rx = /test/;
assert(p.hardenRegExp(rx) === rx, 'hardenRegExp returns same regex');

// ---- SafeStringPrototypeSearch ----
assert(typeof p.SafeStringPrototypeSearch === 'function', 'SafeStringPrototypeSearch');
assert(p.SafeStringPrototypeSearch('hello world', /world/) === 6, 'SafeStringPrototypeSearch');
assert(p.SafeStringPrototypeSearch('hello', /xyz/) === -1, 'SafeStringPrototypeSearch miss');

// ---- SafePromise helpers ----
assert(typeof p.SafePromiseAll === 'function', 'SafePromiseAll');
assert(typeof p.SafePromiseRace === 'function', 'SafePromiseRace');
assert(typeof p.SafePromiseAny === 'function', 'SafePromiseAny');
assert(typeof p.SafePromiseAllSettled === 'function', 'SafePromiseAllSettled');
assert(typeof p.SafePromisePrototypeFinally === 'function', 'SafePromisePrototypeFinally');
assert(typeof p.SafePromiseAllReturnArrayLike === 'function', 'SafePromiseAllReturnArrayLike');
assert(typeof p.SafePromiseAllReturnVoid === 'function', 'SafePromiseAllReturnVoid');
assert(typeof p.SafePromiseAllSettledReturnVoid === 'function', 'SafePromiseAllSettledReturnVoid');

// ---- makeSafe ----
assert(typeof p.makeSafe === 'function', 'makeSafe');

// ---- URI functions ----
assert(typeof p.decodeURI === 'function', 'decodeURI');
assert(typeof p.encodeURI === 'function', 'encodeURI');
assert(typeof p.decodeURIComponent === 'function', 'decodeURIComponent');
assert(typeof p.encodeURIComponent === 'function', 'encodeURIComponent');
assert(typeof p.escape === 'function', 'escape');
assert(typeof p.unescape === 'function', 'unescape');
assert(typeof p.eval === 'function', 'eval');

// ---- globalThis ----
assert(p.globalThis === globalThis, 'globalThis');
assert(p.Proxy === Proxy, 'Proxy');

// ---- IteratorPrototype ----
assert(typeof p.IteratorPrototype === 'object', 'IteratorPrototype');
assert(typeof p.IteratorPrototype[Symbol.iterator] === 'function', 'IteratorPrototype[Symbol.iterator]');

// ---- AsyncIteratorPrototype ----
assert(typeof p.AsyncIteratorPrototype === 'object', 'AsyncIteratorPrototype');
assert(typeof p.AsyncIteratorPrototype[Symbol.asyncIterator] === 'function',
       'AsyncIteratorPrototype has @@asyncIterator');
// Verify @@asyncIterator returns `this`
assert(p.AsyncIteratorPrototype[Symbol.asyncIterator]() === p.AsyncIteratorPrototype,
       'AsyncIteratorPrototype[@@asyncIterator]() returns this');

// ---- FinalizationRegistry (polyfill or native) ----
assert(typeof p.SafeFinalizationRegistry === 'function', 'SafeFinalizationRegistry exists');
var fr = new p.SafeFinalizationRegistry(function() {});
assert(typeof fr.register === 'function', 'FinalizationRegistry register method exists');
assert(typeof fr.unregister === 'function', 'FinalizationRegistry unregister method exists');
// register/unregister must not throw
fr.register({}, 'held');
fr.unregister({});

// ---- Verify FunctionPrototype methods ----
assert(typeof p.FunctionPrototypeCall === 'function', 'FunctionPrototypeCall');
assert(typeof p.FunctionPrototypeBind === 'function', 'FunctionPrototypeBind');
assert(typeof p.FunctionPrototypeApply === 'function', 'FunctionPrototypeApply');

// ---- Boolean / Number prototype ----
assert(typeof p.BooleanPrototypeValueOf === 'function', 'BooleanPrototypeValueOf');
assert(p.BooleanPrototypeValueOf(true) === true, 'BooleanPrototypeValueOf(true)');

assert(typeof p.NumberPrototypeToFixed === 'function', 'NumberPrototypeToFixed');
assert(p.NumberPrototypeToFixed(3.14159, 2) === '3.14', 'NumberPrototypeToFixed');

// ---- StringPrototype ----
assert(typeof p.StringPrototypeCharCodeAt === 'function', 'StringPrototypeCharCodeAt');
assert(p.StringPrototypeCharCodeAt('A', 0) === 65, 'StringPrototypeCharCodeAt');
assert(typeof p.StringPrototypeIncludes === 'function', 'StringPrototypeIncludes');
assert(typeof p.StringPrototypeReplace === 'function', 'StringPrototypeReplace');
assert(typeof p.StringPrototypeSplit === 'function', 'StringPrototypeSplit');
assert(typeof p.StringPrototypeTrim === 'function', 'StringPrototypeTrim');
assert(typeof p.StringPrototypeToLowerCase === 'function', 'StringPrototypeToLowerCase');
assert(typeof p.StringPrototypeToUpperCase === 'function', 'StringPrototypeToUpperCase');

// ---- String statics ----
assert(typeof p.StringFromCharCode === 'function', 'StringFromCharCode');
assert(p.StringFromCharCode(65) === 'A', 'StringFromCharCode(65)');

// ---- WeakRef ----
assert(typeof p.SafeWeakRef === 'function', 'SafeWeakRef');
assert(p.SafeWeakRef === WeakRef, 'SafeWeakRef === WeakRef');

// ---- BigInt ----
assert(typeof p.BigInt === 'function', 'BigInt');

// ---- ArrayPrototype methods ----
assert(typeof p.ArrayPrototypeForEach === 'function', 'ArrayPrototypeForEach');
assert(typeof p.ArrayPrototypeMap === 'function', 'ArrayPrototypeMap');
assert(typeof p.ArrayPrototypeFilter === 'function', 'ArrayPrototypeFilter');
assert(typeof p.ArrayPrototypeFind === 'function', 'ArrayPrototypeFind');
assert(typeof p.ArrayPrototypeJoin === 'function', 'ArrayPrototypeJoin');
assert(typeof p.ArrayPrototypeIncludes === 'function', 'ArrayPrototypeIncludes');
assert(typeof p.ArrayPrototypeIndexOf === 'function', 'ArrayPrototypeIndexOf');
assert(typeof p.ArrayPrototypeConcat === 'function', 'ArrayPrototypeConcat');
assert(typeof p.ArrayPrototypeSort === 'function', 'ArrayPrototypeSort');
assert(typeof p.ArrayPrototypeEvery === 'function', 'ArrayPrototypeEvery');
assert(typeof p.ArrayPrototypeSome === 'function', 'ArrayPrototypeSome');
assert(typeof p.ArrayPrototypeReduce === 'function', 'ArrayPrototypeReduce');

// V8-specific Error API polyfills
assert(typeof p.ErrorCaptureStackTrace === 'function', 'ErrorCaptureStackTrace');
assert(typeof p.ErrorStackTraceLimit === 'number', 'ErrorStackTraceLimit');
var captureTarget = {};
p.ErrorCaptureStackTrace(captureTarget);
assert(typeof captureTarget.stack === 'string', 'captureStackTrace sets .stack');
// Verify Error.captureStackTrace exists on the global Error
assert(typeof Error.captureStackTrace === 'function', 'Error.captureStackTrace exists');
assert('stackTraceLimit' in Error, 'Error.stackTraceLimit exists');

// Result
print('');
print('Passed: ' + passed);
print('Failed: ' + failed);
if (failed > 0) {
  print('FAIL');
  // Use a non-zero exit to signal failure
  throw new Error(failed + ' tests failed');
} else {
  print('PASS: primordials shim');
}
