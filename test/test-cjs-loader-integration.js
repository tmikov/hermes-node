// Copyright (c) Tzvetan Mikov.
// RUN: %hermes-node %s | %FileCheck %s
// CHECK: PASS
// Test that Node's CJS loader is wired into __loadUserScript.
// Verifies:
//   - Built-in module loading (path, fs, etc.)
//   - Module constructor and cache
//   - module.filename and __dirname are set correctly
//   - require.resolve works
//   - require.main is set for the entry script
//   - JSON require works (module._extensions['.json'])
'use strict';

function assert(cond, msg) {
  if (!cond) throw new Error('Assertion failed: ' + msg);
}
function assertEqual(a, b, msg) {
  if (a !== b) throw new Error('Assertion failed: ' + msg + ' (' + JSON.stringify(a) + ' !== ' + JSON.stringify(b) + ')');
}

// 1. Built-in modules work through Node's CJS loader.
var path = require('path');
assertEqual(path.join('a', 'b'), 'a/b', 'path.join works');

var fs = require('fs');
assertEqual(typeof fs.readFileSync, 'function', 'fs.readFileSync is function');

// 2. module object is properly set up.
assertEqual(typeof module, 'object', 'module is an object');
assertEqual(typeof module.exports, 'object', 'module.exports is an object');
assert(module.filename.length > 0, 'module.filename is set');
assert(module.filename.endsWith('test-cjs-loader-integration.js'), 'module.filename ends with script name');
assertEqual(module.id, '.', 'module.id is . for main module');

// 3. __filename and __dirname are set.
assert(__filename.endsWith('test-cjs-loader-integration.js'), '__filename ends with script name');
assertEqual(__dirname, path.dirname(__filename), '__dirname matches path.dirname(__filename)');

// 4. require.resolve returns absolute path for built-in modules.
assertEqual(require.resolve('path'), 'path', 'require.resolve for builtin returns name');

// 5. require.main is this module.
assertEqual(require.main, module, 'require.main is this module');

// 6. process.mainModule is this module.
assertEqual(process.mainModule, module, 'process.mainModule is this module');

// 7. Module._cache contains this module.
var Module = require('internal/modules/cjs/loader').Module;
assert(Module._cache[__filename] !== undefined, 'Module._cache has entry for this file');

// 8. Module.builtinModules is a frozen array with expected modules.
assert(Array.isArray(Module.builtinModules), 'Module.builtinModules is array');
assert(Module.builtinModules.includes('fs'), 'builtinModules includes fs');
assert(Module.builtinModules.includes('path'), 'builtinModules includes path');
assert(Module.builtinModules.includes('repl'), 'builtinModules includes repl');

// 9. Second require of same module returns cached version.
var path2 = require('path');
assert(path === path2, 'require caches modules');

// 10. JSON require works.
var pkg = require('./fixtures/test-package.json');
assertEqual(pkg.name, 'my-test-package', 'JSON require parses name field');
assertEqual(pkg.main, 'lib/index.js', 'JSON require parses main field');

console.log('PASS');
