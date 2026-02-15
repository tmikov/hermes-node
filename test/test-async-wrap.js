// Test for the async_wrap binding stub.
'use strict';

var assert = function(cond, msg) {
  if (!cond) throw new Error('Assertion failed: ' + (msg || ''));
};

var aw = internalBinding('async_wrap');

// --- Providers object ---
assert(typeof aw.Providers === 'object', 'Providers is an object');
assert(aw.Providers.NONE === 0, 'NONE is 0');
assert(aw.Providers.PROMISE !== undefined, 'PROMISE exists');
assert(aw.Providers.TCPWRAP !== undefined, 'TCPWRAP exists');
assert(aw.Providers.ZLIB !== undefined, 'ZLIB exists');
assert(typeof aw.Providers.FSREQCALLBACK === 'number', 'FSREQCALLBACK is a number');

// --- Stub functions ---
assert(typeof aw.setupHooks === 'function', 'setupHooks is a function');
assert(typeof aw.setCallbackTrampoline === 'function', 'setCallbackTrampoline is a function');
assert(typeof aw.pushAsyncContext === 'function', 'pushAsyncContext is a function');
assert(typeof aw.popAsyncContext === 'function', 'popAsyncContext is a function');
assert(typeof aw.executionAsyncResource === 'function', 'executionAsyncResource is a function');
assert(typeof aw.clearAsyncIdStack === 'function', 'clearAsyncIdStack is a function');
assert(typeof aw.queueDestroyAsyncId === 'function', 'queueDestroyAsyncId is a function');
assert(typeof aw.setPromiseHooks === 'function', 'setPromiseHooks is a function');
assert(typeof aw.getPromiseHooks === 'function', 'getPromiseHooks is a function');
assert(typeof aw.registerDestroyHook === 'function', 'registerDestroyHook is a function');

// No-ops don't throw
aw.setupHooks({init: function(){}, before: function(){}, after: function(){}, destroy: function(){}, promise_resolve: function(){}});
aw.setCallbackTrampoline(function(){});
aw.pushAsyncContext(1, 0, {});
aw.popAsyncContext(1);
aw.clearAsyncIdStack();
aw.queueDestroyAsyncId(42);
aw.setPromiseHooks(function(){}, function(){}, function(){}, function(){});
aw.registerDestroyHook({}, 42);

// --- Shared arrays ---
// async_hook_fields: Uint32Array
var ahf = aw.async_hook_fields;
assert(ahf instanceof Uint32Array, 'async_hook_fields is Uint32Array');
assert(ahf.length >= 9, 'async_hook_fields has at least kFieldsCount elements');
assert(ahf[0] === 0, 'async_hook_fields initialized to 0');

// async_id_fields: Float64Array
var aif = aw.async_id_fields;
assert(aif instanceof Float64Array, 'async_id_fields is Float64Array');
assert(aif.length >= 4, 'async_id_fields has at least kUidFieldsCount elements');
assert(aif[0] === 0, 'async_id_fields initialized to 0');

// execution_async_resources: Array
var ear = aw.execution_async_resources;
assert(Array.isArray(ear), 'execution_async_resources is an array');
assert(ear.length === 0, 'execution_async_resources starts empty');

// async_ids_stack: Float64Array
var ais = aw.async_ids_stack;
assert(ais instanceof Float64Array, 'async_ids_stack is Float64Array');

// --- constants object ---
var c = aw.constants;
assert(typeof c === 'object', 'constants is an object');
assert(c.kInit === 0, 'kInit is 0');
assert(c.kBefore === 1, 'kBefore is 1');
assert(c.kAfter === 2, 'kAfter is 2');
assert(c.kDestroy === 3, 'kDestroy is 3');
assert(c.kPromiseResolve === 4, 'kPromiseResolve is 4');
assert(c.kTotals === 5, 'kTotals is 5');
assert(c.kCheck === 6, 'kCheck is 6');
assert(c.kStackLength === 7, 'kStackLength is 7');
assert(c.kUsesExecutionAsyncResource === 8, 'kUsesExecutionAsyncResource is 8');
assert(c.kExecutionAsyncId === 0, 'kExecutionAsyncId is 0');
assert(c.kTriggerAsyncId === 1, 'kTriggerAsyncId is 1');
assert(c.kAsyncIdCounter === 2, 'kAsyncIdCounter is 2');
assert(c.kDefaultTriggerAsyncId === 3, 'kDefaultTriggerAsyncId is 3');

// --- Shared arrays are writable ---
ahf[0] = 42;
assert(ahf[0] === 42, 'async_hook_fields is writable');
ahf[0] = 0;

aif[2] = 100; // kAsyncIdCounter
assert(aif[2] === 100, 'async_id_fields is writable');
aif[2] = 0;

console.log('PASS');
