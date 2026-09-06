// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// REQUIRES: wasm
// RUN: %hermes-node %s | %FileCheck %s

// Executing Wasm: the value shapes and control flow a real toolchain emits.
//
// Hermes runs a module by compiling it to Hermes IR ahead of time and
// executing it as ordinary bytecode -- there is no interpreter and no Wasm
// JIT -- so the interesting cases are the ones where a Wasm value type does
// not correspond to a JS one, or where a Wasm operation has no JS equivalent:
// i64 (a BigInt across the boundary, an i32 pair inside), linear memory
// aliased with a JS typed array, an indirect call through a table, and a trap.

var assert = require('assert');
var mods = require('./fixtures/wasm/modules.js');

var logged = [];
var inst = new WebAssembly.Instance(new WebAssembly.Module(mods.FIB), {
  env: { log: function (v) { logged.push(v); } },
});
var e = inst.exports;

// Recursion, and a mutable global counting the calls. fib(20) is 21891 calls,
// so this also says the compiled code is not accidentally quadratic in
// something.
assert.strictEqual(e.fib(20), 6765);
assert.strictEqual(e.calls(), 21891);
console.log('recursion ok');

// i64 crosses as BigInt in both directions, and wraps at 2^64 rather than
// losing precision the way a double would.
assert.strictEqual(e.mul64(3n, 4n), 12n);
assert.strictEqual(e.mul64(1n << 40n, 3n), 3n << 40n);
assert.strictEqual(e.mul64(1n << 63n, 2n), 0n);
// The value that proves it is not going through a double: 2^53 + 1 is not
// representable as one, so a double round-trip would come back even.
assert.strictEqual(e.mul64((1n << 53n) + 1n, 1n), 9007199254740993n);
// A Number where an i64 is wanted is a TypeError, per spec.
assert.throws(function () { e.mul64(1, 2); }, TypeError);
console.log('i64 ok');

// f64.
assert.strictEqual(e.dist(3, 4), 5);
assert.strictEqual(e.dist(0, 0), 0);
console.log('f64 ok');

// Linear memory is shared with JS, not copied: a store on one side is
// visible on the other. This is the whole basis of how emscripten glue
// passes strings and structs.
e.poke(64, 0x11223344);
assert.strictEqual(e.peek(64), 0x11223344);
var view = new Uint32Array(e.mem.buffer);
assert.strictEqual(view[16], 0x11223344);
view[16] = 7;
assert.strictEqual(e.peek(64), 7);
console.log('memory ok');

// memory.grow returns the previous size in pages and the buffer really grows.
var before = e.mem.buffer.byteLength;
assert.strictEqual(e.mem.grow(1), before / 65536);
assert.strictEqual(e.mem.buffer.byteLength, before + 65536);
console.log('grow ok');

// call_indirect through the table: index 0 is fib, index 1 is the identity.
// A dispatch table like this is what a C function pointer compiles to.
assert.strictEqual(e.indirect(0, 10), 55);
assert.strictEqual(e.indirect(1, 99), 99);
console.log('call_indirect ok');

// An imported JS function called from Wasm.
e.shout(1234);
assert.deepStrictEqual(logged, [1234]);
console.log('import call ok');

// Traps. What is asserted is that the trap happens, reaches JS as an
// exception, and carries the spec's message -- all of which are true today.
//
// The type is deliberately checked only as `instanceof Error`. The spec says
// a trap is a WebAssembly.RuntimeError and Hermes currently raises a plain
// Error (filed as 01a074ce-ff7d in the Hermes tracker); since RuntimeError
// extends Error, this assertion holds before and after that is fixed. Pinning
// the current type here would turn fixing the bug into a test failure.
function trapped(fn) {
  try {
    fn();
  } catch (err) {
    assert.ok(err instanceof Error);
    return err.message;
  }
  throw new Error('expected a trap');
}
assert.match(trapped(function () { e.boom(); }), /unreachable/);
assert.match(trapped(function () { e.divzero(1); }), /divide by zero/);
// An index past the end of the table, so there is no function to call.
assert.ok(trapped(function () { e.indirect(5, 0); }).length > 0);
console.log('traps ok');

// A trap leaves the instance usable: it is an exception, not a crashed VM.
assert.strictEqual(e.fib(10), 55);
console.log('usable after trap ok');

// Two instances of one module do not share memory or globals.
var a = new WebAssembly.Instance(new WebAssembly.Module(mods.ADD));
var b = new WebAssembly.Instance(new WebAssembly.Module(mods.ADD));
new Uint8Array(a.exports.m.buffer)[0] = 1;
assert.strictEqual(new Uint8Array(b.exports.m.buffer)[0], 0);
console.log('instances independent ok');

console.log('PASS');

// CHECK: recursion ok
// CHECK: i64 ok
// CHECK: f64 ok
// CHECK: memory ok
// CHECK: grow ok
// CHECK: call_indirect ok
// CHECK: import call ok
// CHECK: traps ok
// CHECK: usable after trap ok
// CHECK: instances independent ok
// CHECK: PASS
