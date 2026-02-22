// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %s | %FileCheck %s
// CHECK: PASS
// Test the public 'module' builtin and module_wrap binding constants.
'use strict';

function assert(cond, msg) {
  if (!cond) throw new Error('Assertion failed: ' + msg);
}
function assertEqual(a, b, msg) {
  if (a !== b) throw new Error('Assertion failed: ' + msg + ' (' + a + ' !== ' + b + ')');
}

// --- require('module') returns the Module constructor ---
var Module = require('module');
assertEqual(typeof Module, 'function', "require('module') is a function");
assertEqual(Module.name, 'Module', 'Module.name is Module');

// --- Standard Module static properties ---
assert(Array.isArray(Module.builtinModules), 'builtinModules is array');
assert(Module.builtinModules.length > 0, 'builtinModules is non-empty');
assert(Module.builtinModules.indexOf('fs') !== -1, 'builtinModules includes fs');
assert(Module.builtinModules.indexOf('module') !== -1, 'builtinModules includes module');

// --- Module.createRequire ---
assertEqual(typeof Module.createRequire, 'function', 'createRequire is function');
var req = Module.createRequire(__filename);
assertEqual(typeof req, 'function', 'createRequire returns a function');
var path = req('path');
assertEqual(typeof path.join, 'function', 'createRequire require works');

// --- Module.isBuiltin ---
assertEqual(typeof Module.isBuiltin, 'function', 'isBuiltin is function');
assertEqual(Module.isBuiltin('fs'), true, 'fs is builtin');
assertEqual(Module.isBuiltin('module'), true, 'module is builtin');
assertEqual(Module.isBuiltin('nonexistent'), false, 'nonexistent is not builtin');

// --- Module._resolveFilename ---
assertEqual(typeof Module._resolveFilename, 'function', '_resolveFilename is function');
// Resolving a builtin returns its name.
assertEqual(Module._resolveFilename('fs'), 'fs', '_resolveFilename resolves builtin');
// Resolving this file returns its absolute path.
assertEqual(Module._resolveFilename(__filename), __filename, '_resolveFilename resolves absolute path');

// --- Module._nodeModulePaths ---
assertEqual(typeof Module._nodeModulePaths, 'function', '_nodeModulePaths is function');
var paths = Module._nodeModulePaths('/home/user/project/src');
assert(Array.isArray(paths), '_nodeModulePaths returns array');
assert(paths.length > 0, '_nodeModulePaths returns non-empty array');
// Should walk up the directory tree appending node_modules.
assertEqual(paths[0], '/home/user/project/src/node_modules',
  '_nodeModulePaths first entry is immediate node_modules');
assert(paths.indexOf('/home/user/project/node_modules') !== -1,
  '_nodeModulePaths includes parent node_modules');
assert(paths.indexOf('/node_modules') !== -1,
  '_nodeModulePaths includes root node_modules');

// --- module_wrap binding has required constants ---
var mw = internalBinding('module_wrap');
assertEqual(mw.kUninstantiated, 0, 'kUninstantiated is 0');
assertEqual(mw.kInstantiating, 1, 'kInstantiating is 1');
assertEqual(mw.kInstantiated, 2, 'kInstantiated is 2');
assertEqual(mw.kEvaluating, 3, 'kEvaluating is 3');
assertEqual(mw.kEvaluated, 4, 'kEvaluated is 4');
assertEqual(mw.kErrored, 5, 'kErrored is 5');
assertEqual(mw.kSourcePhase, 1, 'kSourcePhase is 1');
assertEqual(mw.kEvaluationPhase, 2, 'kEvaluationPhase is 2');

// --- module_wrap function stubs exist ---
assertEqual(typeof mw.ModuleWrap, 'function', 'ModuleWrap is function');
assertEqual(typeof mw.throwIfPromiseRejected, 'function', 'throwIfPromiseRejected is function');
assertEqual(typeof mw.createRequiredModuleFacade, 'function', 'createRequiredModuleFacade is function');
assertEqual(typeof mw.setImportModuleDynamicallyCallback, 'function', 'setImportModuleDynamicallyCallback is function');
assertEqual(typeof mw.setInitializeImportMetaObjectCallback, 'function', 'setInitializeImportMetaObjectCallback is function');

// --- throwIfPromiseRejected is a no-op ---
assertEqual(mw.throwIfPromiseRejected(), undefined, 'throwIfPromiseRejected returns undefined');

console.log('PASS');
