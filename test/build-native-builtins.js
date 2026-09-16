// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// The built-in modules are natively compiled by default, and the artifact
// says so in the one way that cannot be faked: it carries no Hermes bytecode
// beyond the single blob Hermes itself runs at runtime creation.
//
// REQUIRES: linker-available, shermes-available

'use strict';
var path = require('path');
var assert = require('assert');
assert.strictEqual(path.join('a', 'b'), 'a/b');
console.log('PASS');

// A DEFECT, asserted rather than skipped so that fixing it later fails here
// and is noticed (dz 01a0a417-48e6). An error raised through
// internal/errors' hideStackFrames() -- every ERR_* validation error, and
// every AssertionError from strictEqual/deepStrictEqual -- loses its whole
// stack when the built-ins are native, where the --bytecode-builtins
// artifact below names the failing line. Both artifacts run this, so the
// two prefixes are what state the difference: neither line alone would
// distinguish "native loses the stack" from "nothing here has a stack".
var frames = -1;
try {
  assert.strictEqual(1, 2);
} catch (e) {
  frames = String(e.stack).split('\n').filter(function (line) {
    return line.trim().slice(0, 3) === 'at ';
  }).length;
}
console.log('ASSERT-FRAMES', frames === 0 ? 'none' : 'some');

// Built twice from this same file: once with the default (native built-ins)
// and once declining them. No --bake-wasm anywhere -- a baked Wasm entry is
// Hermes bytecode inside the container and would break the exact count below.
// RUN: rm -rf %t.nb && mkdir -p %t.nb
// RUN: %hermes-node build-native %s -o %t.nb/native --kit=%kit_dir
// RUN: %hermes-node build-native %s -o %t.nb/bytecode --kit=%kit_dir --bytecode-builtins

// Both must run, and run identically. A binary that links the wrong registry
// still works -- that is exactly why the counts below exist -- so this check
// alone would prove nothing about which one was linked.
// RUN: %t.nb/native | %FileCheck %s --check-prefixes=CHECK,NATIVE
// RUN: %t.nb/bytecode | %FileCheck %s --check-prefixes=CHECK,BYTECODE

// One bytecode blob per architecture slice in the default artifact: Hermes's
// own ExtensionsBytecode, which loadAndInstallExtensions() runs at every
// runtime creation and which nothing in this repo can reach. The 187 embedded
// modules and Hermes's InternalJavaScript are both native, so neither
// contributes. An EXACT number, not a bound: a threshold would let an
// accidentally-bytecoded InternalJavaScript unit pass.
// RUN: python3 %S/fixtures/native/count-magic.py %t.nb/native 1

// And --bytecode-builtins must carry the built-ins it asked for. A floor
// rather than an exact number: the point of this line is only that the flag
// did something, and a floor cannot be broken by an unrelated blob appearing.
// RUN: python3 %S/fixtures/native/count-magic.py %t.nb/bytecode 180 9999

// CHECK: PASS
// NATIVE: ASSERT-FRAMES none
// BYTECODE: ASSERT-FRAMES some
