// Copyright (c) Tzvetan Mikov.
// Test the internal/options shim.
'use strict';

// Helper
function assert(cond, msg) {
  if (!cond) throw new Error('Assertion failed: ' + msg);
}
function assertEqual(a, b, msg) {
  if (a !== b) throw new Error('Assertion failed: ' + msg + ' (' + a + ' !== ' + b + ')');
}

var options = require('internal/options');

// --- getOptionValue ---
assertEqual(typeof options.getOptionValue, 'function', 'getOptionValue is function');

// Boolean flags
assertEqual(options.getOptionValue('--pending-deprecation'), false, '--pending-deprecation');
assertEqual(options.getOptionValue('--no-deprecation'), false, '--no-deprecation');
assertEqual(options.getOptionValue('--abort-on-uncaught-exception'), false, '--abort-on-uncaught-exception');
assertEqual(options.getOptionValue('--warnings'), true, '--warnings');
assertEqual(options.getOptionValue('--expose-internals'), false, '--expose-internals');
assertEqual(options.getOptionValue('--frozen-intrinsics'), false, '--frozen-intrinsics');
assertEqual(options.getOptionValue('--experimental-require-module'), true, '--experimental-require-module');
assertEqual(options.getOptionValue('--experimental-detect-module'), true, '--experimental-detect-module');
assertEqual(options.getOptionValue('--strip-types'), false, '--strip-types');
assertEqual(options.getOptionValue('--inspect-brk'), false, '--inspect-brk');
assertEqual(options.getOptionValue('--no-experimental-sqlite'), true, '--no-experimental-sqlite');

// String flags
assertEqual(options.getOptionValue('--unhandled-rejections'), 'throw-or-warn', '--unhandled-rejections');
assertEqual(options.getOptionValue('--input-type'), '', '--input-type');
assertEqual(options.getOptionValue('--diagnostic-dir'), '', '--diagnostic-dir');

// Number flags
assertEqual(options.getOptionValue('--max-http-header-size'), 16384, '--max-http-header-size');
assertEqual(options.getOptionValue('--network-family-autoselection-attempt-timeout'), 250, '--network-family-autoselection-attempt-timeout');

// Array flags
var conditions = options.getOptionValue('--conditions');
assert(Array.isArray(conditions), '--conditions is array');
assertEqual(conditions.length, 0, '--conditions is empty array');

var reqArr = options.getOptionValue('--require');
assert(Array.isArray(reqArr), '--require is array');
assertEqual(reqArr.length, 0, '--require is empty array');

var importArr = options.getOptionValue('--import');
assert(Array.isArray(importArr), '--import is array');
assertEqual(importArr.length, 0, '--import is empty array');

// Unknown option returns undefined
assertEqual(options.getOptionValue('--nonexistent'), undefined, 'unknown option');

// --- refreshOptions ---
assertEqual(typeof options.refreshOptions, 'function', 'refreshOptions is function');
options.refreshOptions(); // Should not throw

// --- getEmbedderOptions ---
assertEqual(typeof options.getEmbedderOptions, 'function', 'getEmbedderOptions is function');
var embedder = options.getEmbedderOptions();
assertEqual(typeof embedder, 'object', 'embedder options is object');
assertEqual(embedder.noBrowserGlobals, false, 'noBrowserGlobals');
assertEqual(embedder.hasEmbedderPreload, false, 'hasEmbedderPreload');
assertEqual(embedder.noGlobalSearchPaths, false, 'noGlobalSearchPaths');

// --- getCLIOptionsInfo ---
assertEqual(typeof options.getCLIOptionsInfo, 'function', 'getCLIOptionsInfo is function');
var info = options.getCLIOptionsInfo();
assertEqual(typeof info, 'object', 'getCLIOptionsInfo returns object');
assert(info.options instanceof Map, 'info.options is Map');
assert(info.aliases instanceof Map, 'info.aliases is Map');

// --- getOptionsAsFlagsFromBinding ---
assertEqual(typeof options.getOptionsAsFlagsFromBinding, 'function', 'getOptionsAsFlagsFromBinding is function');
assertEqual(typeof options.getOptionsAsFlagsFromBinding(), 'string', 'getOptionsAsFlagsFromBinding returns string');

// --- getAllowUnauthorized ---
assertEqual(typeof options.getAllowUnauthorized, 'function', 'getAllowUnauthorized is function');
assertEqual(options.getAllowUnauthorized(), false, 'getAllowUnauthorized returns false');

// --- generateConfigJsonSchema ---
assertEqual(typeof options.generateConfigJsonSchema, 'function', 'generateConfigJsonSchema is function');
var schema = options.generateConfigJsonSchema();
assertEqual(typeof schema, 'object', 'schema is object');
assertEqual(schema.type, 'object', 'schema type is object');

console.log('PASS');
