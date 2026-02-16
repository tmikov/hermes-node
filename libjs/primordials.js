// Copyright (c) Tzvetan Mikov.
// Primordials thin shim (Option B).
//
// Re-exports built-in prototypes and constructors in the flat namespace that
// Node's lib/*.js files expect (e.g. ArrayPrototypePush, ObjectKeys, SafeMap).
// No tamper-resistance -- SafeMap IS Map, nothing is frozen.
//
// This file is evaluated early in bootstrap. It sets globalThis.primordials.
// The module loader injects `primordials` into each internal module wrapper.
'use strict';

(function() {

var primordials = { __proto__: null };

// Aliases for globals that Hermes's strict-mode linter might not know about.
var Promise = globalThis.Promise;
var WeakRef = globalThis.WeakRef;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

var {
  defineProperty: ReflectDefineProperty,
  getOwnPropertyDescriptor: ReflectGetOwnPropertyDescriptor,
  ownKeys: ReflectOwnKeys,
} = Reflect;

var { apply, bind, call } = Function.prototype;
var uncurryThis = bind.bind(call);
primordials.uncurryThis = uncurryThis;

var applyBind = bind.bind(apply);
primordials.applyBind = applyBind;

// Methods for which we also create an `Apply` variant (uses .apply instead
// of .call so callers can pass an array of arguments).
var varargsMethods = [
  'ArrayOf',
  'ArrayPrototypePush',
  'ArrayPrototypeUnshift',
  'MathHypot',
  'MathMax',
  'MathMin',
  'StringFromCharCode',
  'StringFromCodePoint',
  'StringPrototypeConcat',
  'TypedArrayOf',
];

function getNewKey(key) {
  return typeof key === 'symbol'
    ? 'Symbol' + key.description[7].toUpperCase() + key.description.slice(8)
    : key[0].toUpperCase() + key.slice(1);
}

function copyAccessor(dest, prefix, key, desc) {
  ReflectDefineProperty(dest, prefix + 'Get' + key, {
    __proto__: null,
    value: uncurryThis(desc.get),
    enumerable: desc.enumerable,
  });
  if (desc.set !== undefined) {
    ReflectDefineProperty(dest, prefix + 'Set' + key, {
      __proto__: null,
      value: uncurryThis(desc.set),
      enumerable: desc.enumerable,
    });
  }
}

function copyPropsRenamed(src, dest, prefix) {
  for (var i = 0, keys = ReflectOwnKeys(src), len = keys.length; i < len; i++) {
    var key = keys[i];
    var newKey = getNewKey(key);
    var desc = ReflectGetOwnPropertyDescriptor(src, key);
    if ('get' in desc) {
      copyAccessor(dest, prefix, newKey, desc);
    } else {
      var name = prefix + newKey;
      ReflectDefineProperty(dest, name, { __proto__: null, ...desc });
      if (varargsMethods.indexOf(name) !== -1) {
        ReflectDefineProperty(dest, name + 'Apply', {
          __proto__: null,
          value: applyBind(desc.value, src),
        });
      }
    }
  }
}

function copyPropsRenamedBound(src, dest, prefix) {
  for (var i = 0, keys = ReflectOwnKeys(src), len = keys.length; i < len; i++) {
    var key = keys[i];
    var newKey = getNewKey(key);
    var desc = ReflectGetOwnPropertyDescriptor(src, key);
    if ('get' in desc) {
      copyAccessor(dest, prefix, newKey, desc);
    } else {
      var value = desc.value;
      if (typeof value === 'function') {
        desc.value = value.bind(src);
      }
      var name = prefix + newKey;
      ReflectDefineProperty(dest, name, { __proto__: null, ...desc });
      if (varargsMethods.indexOf(name) !== -1) {
        ReflectDefineProperty(dest, name + 'Apply', {
          __proto__: null,
          value: applyBind(value, src),
        });
      }
    }
  }
}

function copyPrototype(src, dest, prefix) {
  for (var i = 0, keys = ReflectOwnKeys(src), len = keys.length; i < len; i++) {
    var key = keys[i];
    var newKey = getNewKey(key);
    var desc = ReflectGetOwnPropertyDescriptor(src, key);
    if ('get' in desc) {
      copyAccessor(dest, prefix, newKey, desc);
    } else {
      var value = desc.value;
      if (typeof value === 'function') {
        desc.value = uncurryThis(value);
      }
      var name = prefix + newKey;
      ReflectDefineProperty(dest, name, { __proto__: null, ...desc });
      if (varargsMethods.indexOf(name) !== -1) {
        ReflectDefineProperty(dest, name + 'Apply', {
          __proto__: null,
          value: applyBind(value),
        });
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

primordials.Proxy = Proxy;
primordials.globalThis = globalThis;

// URI / legacy functions
[
  decodeURI,
  decodeURIComponent,
  encodeURI,
  encodeURIComponent,
].forEach(function(fn) { primordials[fn.name] = fn; });

[
  escape,
  eval,
  unescape,
].forEach(function(fn) { primordials[fn.name] = fn; });

// ---------------------------------------------------------------------------
// Namespace objects (Math, JSON, Reflect, Proxy)
// ---------------------------------------------------------------------------

var namespaceNames = ['JSON', 'Math', 'Proxy', 'Reflect'];
// Atomics is not available in Hermes — skip if undefined.
if (typeof Atomics !== 'undefined') namespaceNames.push('Atomics');

namespaceNames.forEach(function(name) {
  copyPropsRenamed(globalThis[name], primordials, name);
});

// ---------------------------------------------------------------------------
// V8-specific Error APIs polyfill
// ---------------------------------------------------------------------------
// Error.captureStackTrace is a V8-specific API that Hermes doesn't have.
// Node's lib/*.js files use it extensively via primordials.ErrorCaptureStackTrace.
// Our polyfill creates a stack trace by instantiating an Error and copying
// its .stack property to the target object.
if (typeof Error.captureStackTrace !== 'function') {
  Error.captureStackTrace = function captureStackTrace(targetObject, constructorOpt) {
    var stackContainer = new Error();
    Object.defineProperty(targetObject, 'stack', {
      configurable: true,
      get: function() {
        var stack = stackContainer.stack;
        Object.defineProperty(targetObject, 'stack', {
          value: stack,
          configurable: true,
          writable: true,
        });
        return stack;
      },
      set: function(value) {
        Object.defineProperty(targetObject, 'stack', {
          value: value,
          configurable: true,
          writable: true,
        });
      },
    });
  };
}
// Error.stackTraceLimit: Hermes doesn't have this V8 property.
// Some Node modules check/set it. Define it if missing.
if (!('stackTraceLimit' in Error)) {
  Error.stackTraceLimit = 10;
}

// ---------------------------------------------------------------------------
// FinalizationRegistry polyfill (no-op)
// ---------------------------------------------------------------------------
// Hermes does not yet have FinalizationRegistry. Provide a no-op polyfill so
// that SafeFinalizationRegistry is available in primordials. All Node.js uses
// (abort_controller, event_target, process/finalization) are leak-prevention
// cleanup -- no correctness impact if callbacks never fire.
// This polyfill will be removed when Hermes adds native FinalizationRegistry.
if (typeof FinalizationRegistry === 'undefined') {
  globalThis.FinalizationRegistry = function FinalizationRegistry(callback) {};
  globalThis.FinalizationRegistry.prototype.register = function(target, heldValue, unregisterToken) {};
  globalThis.FinalizationRegistry.prototype.unregister = function(unregisterToken) {};
}

// ---------------------------------------------------------------------------
// Intrinsic constructors
// ---------------------------------------------------------------------------

var intrinsicNames = [
  'AggregateError',
  'Array',
  'ArrayBuffer',
  'BigInt',
  'BigInt64Array',
  'BigUint64Array',
  'Boolean',
  'DataView',
  'Date',
  'Error',
  'EvalError',
  'Float32Array',
  'Float64Array',
  'Function',
  'Int16Array',
  'Int32Array',
  'Int8Array',
  'Map',
  'Number',
  'Object',
  'RangeError',
  'ReferenceError',
  'RegExp',
  'Set',
  'String',
  'Symbol',
  'SyntaxError',
  'TypeError',
  'URIError',
  'Uint16Array',
  'Uint32Array',
  'Uint8Array',
  'Uint8ClampedArray',
  'WeakMap',
  'WeakRef',
  'WeakSet',
];
// FinalizationRegistry is not available in all Hermes builds.
if (typeof FinalizationRegistry !== 'undefined')
  intrinsicNames.push('FinalizationRegistry');

intrinsicNames.forEach(function(name) {
  var original = globalThis[name];
  if (typeof original === 'undefined') return; // skip missing
  primordials[name] = original;
  copyPropsRenamed(original, primordials, name);
  copyPrototype(original.prototype, primordials, name + 'Prototype');
});

// Promise needs bound static methods (e.g. Promise.all needs `this === Promise`).
['Promise'].forEach(function(name) {
  var original = globalThis[name];
  primordials[name] = original;
  copyPropsRenamedBound(original, primordials, name);
  copyPrototype(original.prototype, primordials, name + 'Prototype');
});

// ---------------------------------------------------------------------------
// Abstract intrinsics not on globalThis
// ---------------------------------------------------------------------------

// %TypedArray% — the hidden superclass of all typed arrays.
var TypedArray = Reflect.getPrototypeOf(Uint8Array);
primordials.TypedArray = TypedArray;
copyPrototype(TypedArray, primordials, 'TypedArray');
copyPrototype(TypedArray.prototype, primordials, 'TypedArrayPrototype');

// ArrayIterator / StringIterator prototypes.
var ArrayIteratorPrototype =
  Reflect.getPrototypeOf(Array.prototype[Symbol.iterator]());
primordials.ArrayIterator = { prototype: ArrayIteratorPrototype };
copyPrototype({ prototype: ArrayIteratorPrototype }, primordials, 'ArrayIterator');
copyPrototype(ArrayIteratorPrototype, primordials, 'ArrayIteratorPrototype');

var StringIteratorPrototype =
  Reflect.getPrototypeOf(String.prototype[Symbol.iterator]());
primordials.StringIterator = { prototype: StringIteratorPrototype };
copyPrototype({ prototype: StringIteratorPrototype }, primordials, 'StringIterator');
copyPrototype(StringIteratorPrototype, primordials, 'StringIteratorPrototype');

// %IteratorPrototype% — parent of all iterator prototypes.
primordials.IteratorPrototype = Reflect.getPrototypeOf(ArrayIteratorPrototype);

// ---------------------------------------------------------------------------
// Safe* variants — just the originals (no tamper-resistance)
// ---------------------------------------------------------------------------

primordials.SafeMap = Map;
primordials.SafeWeakMap = WeakMap;
primordials.SafeSet = Set;
primordials.SafeWeakSet = WeakSet;
primordials.SafeWeakRef = WeakRef;
if (typeof FinalizationRegistry !== 'undefined')
  primordials.SafeFinalizationRegistry = FinalizationRegistry;

// SafeArrayIterator — thin wrapper, no frozen prototype (Option B).
var ArrayPrototypeSymbolIterator = primordials.ArrayPrototypeSymbolIterator;
var ArrayIteratorPrototypeNext = primordials.ArrayIteratorPrototypeNext;
primordials.SafeArrayIterator = function SafeArrayIterator(iterable) {
  this._iterator = ArrayPrototypeSymbolIterator(iterable);
};
primordials.SafeArrayIterator.prototype = {
  __proto__: null,
  next: function next() {
    return ArrayIteratorPrototypeNext(this._iterator);
  },
  [Symbol.iterator]: function() { return this; },
};

// SafeStringIterator
var StringPrototypeSymbolIterator = primordials.StringPrototypeSymbolIterator;
var StringIteratorPrototypeNext = primordials.StringIteratorPrototypeNext;
primordials.SafeStringIterator = function SafeStringIterator(iterable) {
  this._iterator = StringPrototypeSymbolIterator(iterable);
};
primordials.SafeStringIterator.prototype = {
  __proto__: null,
  next: function next() {
    return StringIteratorPrototypeNext(this._iterator);
  },
  [Symbol.iterator]: function() { return this; },
};

// makeSafe — identity for Option B (no tamper-resistance).
primordials.makeSafe = function makeSafe(unsafe, safe) {
  // Copy prototype methods from unsafe to safe.
  var srcProto = unsafe.prototype;
  var dstProto = safe.prototype;
  ReflectOwnKeys(srcProto).forEach(function(key) {
    if (!ReflectGetOwnPropertyDescriptor(dstProto, key)) {
      ReflectDefineProperty(
        dstProto, key,
        { __proto__: null, ...ReflectGetOwnPropertyDescriptor(srcProto, key) }
      );
    }
  });
  // Copy statics
  ReflectOwnKeys(unsafe).forEach(function(key) {
    if (!ReflectGetOwnPropertyDescriptor(safe, key)) {
      ReflectDefineProperty(
        safe, key,
        { __proto__: null, ...ReflectGetOwnPropertyDescriptor(unsafe, key) }
      );
    }
  });
  return safe;
};

// ---------------------------------------------------------------------------
// SafePromise helpers
// ---------------------------------------------------------------------------

var PromisePrototypeThen = primordials.PromisePrototypeThen;
var PromiseResolve = primordials.PromiseResolve;
var ArrayPrototypeMap = primordials.ArrayPrototypeMap;
var ArrayPrototypePushApply = primordials.ArrayPrototypePushApply;
var ArrayPrototypeSlice = primordials.ArrayPrototypeSlice;

// SafePromisePrototypeFinally
primordials.SafePromisePrototypeFinally = function(thisPromise, onFinally) {
  return new Promise(function(a, b) {
    PromisePrototypeThen(
      PromisePrototypeThen(thisPromise, function(v) {
        if (typeof onFinally === 'function') onFinally();
        return v;
      }, function(e) {
        if (typeof onFinally === 'function') onFinally();
        throw e;
      }),
      a, b
    );
  });
};

// AsyncIteratorPrototype — the prototype of all async generator prototypes.
// In V8, this is derived from the async generator prototype chain:
//   Reflect.getPrototypeOf(Reflect.getPrototypeOf(async function*(){}).prototype)
// Hermes's async generator prototype chain is flat (gen.prototype.__proto__
// is Object.prototype), so the intermediate AsyncIteratorPrototype object
// does not exist. Provide a spec-compliant standalone object.
primordials.AsyncIteratorPrototype = Object.create(Object.prototype, {
  [Symbol.asyncIterator]: {
    value: function() { return this; },
    writable: true,
    configurable: true,
  },
});

function arrayToPromiseIterable(promises, mapFn) {
  return ArrayPrototypeMap(promises, function(promise, i) {
    return new Promise(function(a, b) {
      PromisePrototypeThen(
        mapFn == null ? promise : mapFn(promise, i),
        a, b
      );
    });
  });
}

primordials.SafePromiseAll = function(promises, mapFn) {
  return new Promise(function(a, b) {
    Promise.all(arrayToPromiseIterable(promises, mapFn)).then(a, b);
  });
};

primordials.SafePromiseAllReturnArrayLike = function(promises, mapFn) {
  return new Promise(function(resolve, reject) {
    var length = promises.length;
    var returnVal = Array(length);
    Object.setPrototypeOf(returnVal, null);
    if (length === 0) { resolve(returnVal); return; }
    var pendingPromises = length;
    for (var i = 0; i < length; i++) {
      (function(idx) {
        var promise = mapFn != null ? mapFn(promises[idx], idx) : promises[idx];
        PromisePrototypeThen(PromiseResolve(promise), function(result) {
          returnVal[idx] = result;
          if (--pendingPromises === 0) resolve(returnVal);
        }, reject);
      })(i);
    }
  });
};

primordials.SafePromiseAllReturnVoid = function(promises, mapFn) {
  return new Promise(function(resolve, reject) {
    var pendingPromises = promises.length;
    if (pendingPromises === 0) { resolve(); return; }
    var onFulfilled = function() {
      if (--pendingPromises === 0) resolve();
    };
    for (var i = 0; i < promises.length; i++) {
      var promise = mapFn != null ? mapFn(promises[i], i) : promises[i];
      PromisePrototypeThen(PromiseResolve(promise), onFulfilled, reject);
    }
  });
};

primordials.SafePromiseAllSettled = function(promises, mapFn) {
  return new Promise(function(a, b) {
    Promise.allSettled(arrayToPromiseIterable(promises, mapFn)).then(a, b);
  });
};

primordials.SafePromiseAllSettledReturnVoid = function(promises, mapFn) {
  return new Promise(function(resolve) {
    var pendingPromises = promises.length;
    if (pendingPromises === 0) { resolve(); return; }
    var onSettle = function() {
      if (--pendingPromises === 0) resolve();
    };
    for (var i = 0; i < promises.length; i++) {
      var promise = mapFn != null ? mapFn(promises[i], i) : promises[i];
      PromisePrototypeThen(PromiseResolve(promise), onSettle, onSettle);
    }
  });
};

primordials.SafePromiseAny = function(promises, mapFn) {
  return new Promise(function(a, b) {
    Promise.any(arrayToPromiseIterable(promises, mapFn)).then(a, b);
  });
};

primordials.SafePromiseRace = function(promises, mapFn) {
  return new Promise(function(a, b) {
    Promise.race(arrayToPromiseIterable(promises, mapFn)).then(a, b);
  });
};

// ---------------------------------------------------------------------------
// hardenRegExp — identity for Option B
// ---------------------------------------------------------------------------

primordials.hardenRegExp = function hardenRegExp(pattern) {
  return pattern;
};

// ---------------------------------------------------------------------------
// SafeStringPrototypeSearch
// ---------------------------------------------------------------------------

var RegExpPrototypeExec = primordials.RegExpPrototypeExec;
primordials.SafeStringPrototypeSearch = function(str, regexp) {
  regexp.lastIndex = 0;
  var match = RegExpPrototypeExec(regexp, str);
  return match ? match.index : -1;
};

// ---------------------------------------------------------------------------
// SafeArrayPrototypePushApply — chunked push for large arrays
// ---------------------------------------------------------------------------

primordials.SafeArrayPrototypePushApply = function(arr, items) {
  var end = 0x10000;
  if (end < items.length) {
    var start = 0;
    do {
      ArrayPrototypePushApply(arr, ArrayPrototypeSlice(items, start, start = end));
      end += 0x10000;
    } while (end < items.length);
    items = ArrayPrototypeSlice(items, start);
  }
  return ArrayPrototypePushApply(arr, items);
};

// ---------------------------------------------------------------------------
// Export
// ---------------------------------------------------------------------------

globalThis.primordials = primordials;

})();
