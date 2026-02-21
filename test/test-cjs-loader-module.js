// Copyright (c) Tzvetan Mikov.
// Test Node's CJS loader Module class loaded in hermes-node.
//
// RUN: %hermes-node %s | %FileCheck %s

'use strict';

var { Module } = require('internal/modules/cjs/loader');

// Module.builtinModules is a frozen array containing known modules.
console.log(Array.isArray(Module.builtinModules));
// CHECK: true
console.log(Module.builtinModules.includes('fs'));
// CHECK: true
console.log(Module.builtinModules.includes('events'));
// CHECK: true
console.log(Module.builtinModules.includes('vm'));
// CHECK: true
console.log(Module.builtinModules.includes('domain'));
// CHECK: true

// Module._extensions is an object with extension keys.
console.log(typeof Module._extensions);
// CHECK: object
console.log('.js' in Module._extensions);
// CHECK: true

// Module._cache is an object.
console.log(typeof Module._cache);
// CHECK: object

// Module.globalPaths is an array.
console.log(Array.isArray(Module.globalPaths));
// CHECK: true

// Module._nodeModulePaths returns node_modules directories.
var paths = Module._nodeModulePaths('/home/user/project');
console.log(Array.isArray(paths));
// CHECK: true
console.log(paths[0]);
// CHECK: /home/user/project/node_modules
console.log(paths[1]);
// CHECK: /home/user/node_modules
console.log(paths[2]);
// CHECK: /home/node_modules
console.log(paths[3]);
// CHECK: /node_modules

// Root path.
var rootPaths = Module._nodeModulePaths('/');
console.log(rootPaths.length);
// CHECK: 1
console.log(rootPaths[0]);
// CHECK: /node_modules

// Module._resolveLookupPaths for a built-in returns null.
console.log(Module._resolveLookupPaths('fs', null));
// CHECK: null

// Module._resolveLookupPaths for relative with no parent returns ['.'].
var relPaths = Module._resolveLookupPaths('./foo', null);
console.log(JSON.stringify(relPaths));
// CHECK: ["."]

// Module._resolveFilename for built-in returns the module name.
console.log(Module._resolveFilename('fs', null, false));
// CHECK: fs

// Constructor creates a module object.
var mod = new Module('<repl>');
console.log(mod.id);
// CHECK: <repl>
console.log(mod.loaded);
// CHECK: false
console.log(typeof mod.exports);
// CHECK: object
// Note: the real CJS loader doesn't set mod.paths in the constructor.
// It's set during Module.prototype.load(). For a fresh module, it's undefined.
console.log(mod.paths);
// CHECK: undefined

// Module.require works (loads a built-in).
console.log(typeof mod.require('path'));
// CHECK: object

// helpers.js makeRequireFunction works with Module.
var { makeRequireFunction, addBuiltinLibsToObject } = require('internal/modules/helpers');
console.log(typeof makeRequireFunction);
// CHECK: function
console.log(typeof addBuiltinLibsToObject);
// CHECK: function

// Create a require function from a module.
var testMod = new Module('/tmp/test.js');
testMod.filename = '/tmp/test.js';
var req = makeRequireFunction(testMod);
console.log(typeof req);
// CHECK: function
console.log(typeof req.resolve);
// CHECK: function

// addBuiltinLibsToObject adds lazy getters.
var obj = {};
addBuiltinLibsToObject(obj, 'test');
// Access a built-in via the lazy getter.
console.log(typeof obj.path);
// CHECK: object
console.log(typeof obj.path.join);
// CHECK: function

console.log('PASS');
// CHECK: PASS
