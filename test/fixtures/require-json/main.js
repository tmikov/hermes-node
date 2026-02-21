// Copyright (c) Tzvetan Mikov.
// Test that require() of .json files works correctly.
'use strict';

function assert(cond, msg) {
  if (!cond) throw new Error('Assertion failed: ' + msg);
}
function assertEqual(a, b, msg) {
  if (a !== b) throw new Error('Assertion failed: ' + msg + ' (' + JSON.stringify(a) + ' !== ' + JSON.stringify(b) + ')');
}

var path = require('path');

// 1. require('./data.json') loads and parses JSON
var data = require('./data.json');
assertEqual(typeof data, 'object', 'JSON require returns an object');
assertEqual(data.key, 'value', 'string field');
assertEqual(data.number, 42, 'number field');

// 2. Nested objects and arrays are preserved
assert(Array.isArray(data.nested.array), 'nested array is an Array');
assertEqual(data.nested.array.length, 3, 'nested array length');
assertEqual(data.nested.array[0], 1, 'nested array[0]');
assertEqual(data.nested.bool, true, 'nested bool');
assertEqual(data.empty, null, 'null field');

// 3. Second require returns the same cached object
var data2 = require('./data.json');
assert(data === data2, 'JSON modules are cached (same reference)');

// 4. require.resolve works for .json files
var resolved = require.resolve('./data.json');
assert(resolved.endsWith('data.json'), 'require.resolve returns path ending with data.json');
assert(path.isAbsolute(resolved), 'require.resolve returns absolute path');

// 5. JSON from node_modules works too
var pkgData = require('json-pkg');
assertEqual(pkgData.name, 'json-pkg', 'JSON package name');
assertEqual(pkgData.config.setting, 'on', 'JSON package config');

// 6. require of package whose main points to a .json file
var jsonMain = require('json-main-pkg');
assertEqual(jsonMain.answer, 42, 'package with JSON main entry');

console.log('PASS');
