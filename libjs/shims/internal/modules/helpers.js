// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Shim for internal/modules/helpers.js
//
// Provides makeRequireFunction and addBuiltinLibsToObject for the REPL.
// Other exports are stubs or re-exported from the modules binding.

'use strict';

var {
  ArrayPrototypeForEach,
  ObjectDefineProperty,
  ObjectFreeze,
  ObjectPrototypeHasOwnProperty,
  StringPrototypeIncludes,
} = primordials;

var { validateString } = require('internal/validators');
var { setOwnProperty } = require('internal/util');

var $Module = null;
function lazyModule() {
  if ($Module === null) {
    $Module = require('internal/modules/cjs/loader').Module;
  }
  return $Module;
}

/**
 * Create the module-scoped `require` function to pass into CommonJS modules.
 * @param {object} mod - The module to create the `require` function for.
 * @returns {function(string): unknown}
 */
function makeRequireFunction(mod) {
  var Module = lazyModule();

  function require(path) {
    return mod.require(path);
  }

  function resolve(request, options) {
    validateString(request, 'request');
    return Module._resolveFilename(request, mod, false, options);
  }

  require.resolve = resolve;

  function paths(request) {
    validateString(request, 'request');
    return Module._resolveLookupPaths(request, mod);
  }

  resolve.paths = paths;

  setOwnProperty(require, 'main', process.mainModule);

  require.extensions = Module._extensions || {};
  require.cache = Module._cache || {};

  return require;
}

/**
 * Add built-in modules to a global or REPL scope object as lazy-loaded
 * non-enumerable properties.
 * @param {object} object - The object to add the built-in modules to.
 * @param {string} dummyModuleName - Label for the dummy module.
 */
function addBuiltinLibsToObject(object, dummyModuleName) {
  var Module = lazyModule();
  var builtinModules = Module.builtinModules;
  if (!builtinModules) return;

  var dummyModule = new Module(dummyModuleName);

  ArrayPrototypeForEach(builtinModules, function(name) {
    // Skip underscored, slash-containing, or already-defined names.
    if (name[0] === '_' ||
        StringPrototypeIncludes(name, '/') ||
        ObjectPrototypeHasOwnProperty(object, name)) {
      return;
    }

    var setReal = function(val) {
      delete object[name];
      object[name] = val;
    };

    try {
      ObjectDefineProperty(object, name, {
        __proto__: null,
        get: function() {
          var lib = dummyModule.require(name);
          try {
            ObjectDefineProperty(object, name, {
              __proto__: null,
              get: function() { return lib; },
              set: setReal,
              configurable: true,
              enumerable: false,
            });
          } catch (e) {
            // If the property is no longer configurable, ignore the error.
          }
          return lib;
        },
        set: setReal,
        configurable: true,
        enumerable: false,
      });
    } catch (e) {
      // Ignore property definition failures.
    }
  });
}

// Re-export compile cache functions from the modules binding as stubs.
var {
  compileCacheStatus: _compileCacheStatus,
  flushCompileCache,
} = internalBinding('modules');

var compileCacheStatus = { __proto__: null };
for (var i = 0; i < _compileCacheStatus.length; ++i) {
  compileCacheStatus[_compileCacheStatus[i]] = i;
}
ObjectFreeze(compileCacheStatus);
var constants = { __proto__: null, compileCacheStatus: compileCacheStatus };
ObjectFreeze(constants);

function enableCompileCache() {
  return { status: 0 };
}

function getCompileCacheDir() {
  return undefined;
}

function stripBOM(content) {
  if (content.charCodeAt(0) === 0xFEFF) {
    content = content.slice(1);
  }
  return content;
}

function loadBuiltinModule() {
  return undefined;
}

function toRealPath(requestPath) {
  return require('fs').realpathSync(requestPath);
}

module.exports = {
  addBuiltinLibsToObject: addBuiltinLibsToObject,
  constants: constants,
  enableCompileCache: enableCompileCache,
  flushCompileCache: flushCompileCache,
  getCompileCacheDir: getCompileCacheDir,
  loadBuiltinModule: loadBuiltinModule,
  makeRequireFunction: makeRequireFunction,
  stripBOM: stripBOM,
  toRealPath: toRealPath,
  hasStartedUserCJSExecution: function() { return false; },
  setHasStartedUserCJSExecution: function() {},
  hasStartedUserESMExecution: function() { return false; },
  setHasStartedUserESMExecution: function() {},
  getCjsConditions: function() { return new Set(['require', 'node']); },
  getCjsConditionsArray: function() { return ['require', 'node']; },
  initializeCjsConditions: function() {},
  normalizeReferrerURL: function() { return undefined; },
  stringify: function(body) { return typeof body === 'string' ? body : String(body); },
  assertBufferSource: function() {},
  getBuiltinModule: function() { return undefined; },
  urlToFilename: function(url) { return url; },
};
