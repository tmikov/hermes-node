// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// REQUIRES: wasm
// RUN: %hermes-node %s | %FileCheck %s

// The WebAssembly JS API surface. Nothing here executes Wasm code -- that is
// test-wasm-exec.js. This is the half a package touches while loading: does
// the global exist, does a module compile, what do the reflection calls
// report, and are the three error types the ones the spec names.
//
// hermes-node gets all of it from the engine: Hermes creates the WebAssembly
// namespace in GlobalObject.cpp, gated on HERMES_ENABLE_WASM, which the
// top-level CMakeLists.txt turns on. There is no binding of ours underneath
// it, which is exactly why it is worth a test -- nothing else in this repo
// would notice the build option going away.

var assert = require('assert');
var mods = require('./fixtures/wasm/modules.js');

// The global. Node programs feature-detect on this exact expression, so it
// is the first thing to get right.
assert.strictEqual(typeof WebAssembly, 'object');
assert.strictEqual(Object.prototype.toString.call(WebAssembly), '[object WebAssembly]');
console.log('global ok');

// The constructors a loader reaches for.
['Module', 'Instance', 'Memory', 'Table', 'Global'].forEach(function (name) {
  assert.strictEqual(typeof WebAssembly[name], 'function', name);
});
['compile', 'instantiate', 'validate'].forEach(function (name) {
  assert.strictEqual(typeof WebAssembly[name], 'function', name);
});
console.log('constructors ok');

// validate() answers without compiling, and answers both ways.
assert.strictEqual(WebAssembly.validate(mods.ADD), true);
assert.strictEqual(WebAssembly.validate(mods.GARBAGE), false);
console.log('validate ok');

// Compiling, and the reflection a bindings generator uses to decide what to
// pass to instantiate().
var mod = new WebAssembly.Module(mods.ADD);
assert.ok(mod instanceof WebAssembly.Module);
assert.deepStrictEqual(WebAssembly.Module.exports(mod), [
  { name: 'm', kind: 'memory' },
  { name: 'add', kind: 'function' },
]);
assert.deepStrictEqual(WebAssembly.Module.imports(mod), []);
assert.deepStrictEqual(WebAssembly.Module.imports(new WebAssembly.Module(mods.FIB)), [
  { module: 'env', name: 'log', kind: 'function' },
]);
console.log('reflection ok');

// A module accepts its bytes from any of the shapes fs and http hand back.
assert.ok(new WebAssembly.Module(mods.ADD.buffer) instanceof WebAssembly.Module);
assert.ok(new WebAssembly.Module(Buffer.from(mods.ADD)) instanceof WebAssembly.Module);
console.log('byte sources ok');

// Synchronous instantiation.
var inst = new WebAssembly.Instance(mod);
assert.ok(inst instanceof WebAssembly.Instance);
assert.strictEqual(typeof inst.exports.add, 'function');
assert.ok(inst.exports.m instanceof WebAssembly.Memory);
console.log('instance ok');

// The three standalone constructors, which emscripten glue creates directly
// to hand a module its memory rather than letting the module define one.
var mem = new WebAssembly.Memory({ initial: 2, maximum: 4 });
assert.ok(mem.buffer instanceof ArrayBuffer);
assert.strictEqual(mem.buffer.byteLength, 2 * 65536);

var glob = new WebAssembly.Global({ value: 'i32', mutable: true }, 7);
assert.strictEqual(glob.value, 7);
glob.value = 9;
assert.strictEqual(glob.value, 9);

var tbl = new WebAssembly.Table({ element: 'anyfunc', initial: 3 });
assert.strictEqual(tbl.length, 3);
assert.strictEqual(tbl.get(0), null);
console.log('memory/global/table ok');

// The error hierarchy. Each is a distinct constructor and each is an Error,
// which is what a catch block that only means to log will assume.
['CompileError', 'LinkError', 'RuntimeError'].forEach(function (name) {
  assert.strictEqual(typeof WebAssembly[name], 'function', name);
  var e = new WebAssembly[name]('x');
  assert.ok(e instanceof WebAssembly[name]);
  assert.ok(e instanceof Error);
});
assert.ok(!(new WebAssembly.RuntimeError('x') instanceof WebAssembly.LinkError));
console.log('error types ok');

// Bad bytes are a CompileError, not a TypeError and not a crash.
assert.throws(function () { new WebAssembly.Module(mods.GARBAGE); }, WebAssembly.CompileError);

// A missing import is a LinkError, which is how an optional-import probe
// tells "you did not give me the function" from "your module is broken".
assert.throws(function () { new WebAssembly.Instance(new WebAssembly.Module(mods.FIB), {}); },
              WebAssembly.LinkError);
console.log('compile/link errors ok');

// The async API. Node code reaches for these far more often than the
// constructors, because that is what the MDN examples use, and they need a
// working microtask queue underneath.
WebAssembly.compile(mods.ADD)
  .then(function (m) {
    assert.ok(m instanceof WebAssembly.Module);
    return WebAssembly.instantiate(mods.ADD);
  })
  .then(function (res) {
    // instantiate(bytes) resolves to {module, instance}; instantiate(module)
    // resolves to the instance alone. Both spellings appear in the wild.
    assert.ok(res.module instanceof WebAssembly.Module);
    assert.ok(res.instance instanceof WebAssembly.Instance);
    assert.strictEqual(res.instance.exports.add(2, 3), 5);
    return WebAssembly.instantiate(new WebAssembly.Module(mods.ADD));
  })
  .then(function (i) {
    assert.ok(i instanceof WebAssembly.Instance);
    assert.strictEqual(i.exports.add(20, 22), 42);
    console.log('async ok');
    // A rejected compile rejects rather than throwing synchronously.
    return WebAssembly.compile(mods.GARBAGE).then(
      function () { throw new Error('compile(garbage) resolved'); },
      function (e) { assert.ok(e instanceof WebAssembly.CompileError); });
  })
  .then(function () {
    console.log('PASS');
  })
  .catch(function (e) {
    console.log('FAILED: ' + (e && e.stack));
    process.exitCode = 1;
  });

// CHECK: global ok
// CHECK: constructors ok
// CHECK: validate ok
// CHECK: reflection ok
// CHECK: byte sources ok
// CHECK: instance ok
// CHECK: memory/global/table ok
// CHECK: error types ok
// CHECK: compile/link errors ok
// CHECK: async ok
// CHECK: PASS
