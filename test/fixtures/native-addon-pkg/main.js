// Copyright (c) Tzvetan Mikov.
// Test that require('hello-addon') resolves through node_modules/ to a
// native .node addon, the way real npm packages with native bindings work.
'use strict';

function assert(cond, msg) {
  if (!cond) throw new Error('Assertion failed: ' + msg);
}
function assertEqual(a, b, msg) {
  if (a !== b) throw new Error('Assertion failed: ' + msg +
      ' (' + JSON.stringify(a) + ' !== ' + JSON.stringify(b) + ')');
}

// 1. require('hello-addon') resolves from node_modules/.
var addon = require('hello-addon');
assertEqual(typeof addon.hello, 'function', 'addon.hello is a function');
assertEqual(addon.hello(), 'world', 'addon.hello() returns world');
assertEqual(typeof addon.add, 'function', 'addon.add is a function');
assertEqual(addon.add(2, 3), 5, 'addon.add(2,3) returns 5');

// 2. Module caching works for native addons.
var addon2 = require('hello-addon');
assert(addon === addon2, 'require caches native addon');

// 3. require.resolve returns the .node file path.
var resolved = require.resolve('hello-addon');
assert(resolved.endsWith('hello_addon.node'),
    'resolved path ends with hello_addon.node, got: ' + resolved);

console.log('PASS');
