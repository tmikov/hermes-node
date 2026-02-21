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
  SafeSet,
  StringPrototypeCharCodeAt,
  StringPrototypeIncludes,
  StringPrototypeSlice,
  StringPrototypeStartsWith,
} = primordials;

var { validateString } = require('internal/validators');
var { setOwnProperty } = require('internal/util');
var { BuiltinModule } = require('internal/bootstrap/realm');
var { pathToFileURL, fileURLToPath, URL } = require('internal/url');
var { canParse: URLCanParse } = internalBinding('url');
var path = require('path');

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
  if (StringPrototypeCharCodeAt(content, 0) === 0xFEFF) {
    content = StringPrototypeSlice(content, 1);
  }
  return content;
}

// CJS conditions for package.json "exports" resolution.
var _cjsConditions;
var _cjsConditionsArray;

function initializeCjsConditions() {
  _cjsConditionsArray = ObjectFreeze(['require', 'node']);
  _cjsConditions = new SafeSet(_cjsConditionsArray);
}

function getCjsConditions() {
  if (_cjsConditions === undefined) {
    initializeCjsConditions();
  }
  return _cjsConditions;
}

function getCjsConditionsArray() {
  if (_cjsConditionsArray === undefined) {
    initializeCjsConditions();
  }
  return _cjsConditionsArray;
}

function normalizeReferrerURL(referrerName) {
  if (referrerName === null || referrerName === undefined) {
    return undefined;
  }
  if (typeof referrerName === 'string') {
    if (path.isAbsolute(referrerName)) {
      return pathToFileURL(referrerName).href;
    }
    if (StringPrototypeStartsWith(referrerName, 'file://') ||
        URLCanParse(referrerName)) {
      return referrerName;
    }
    return undefined;
  }
  return undefined;
}

function urlToFilename(url) {
  if (url && StringPrototypeStartsWith(url, 'file://')) {
    try {
      return fileURLToPath(new URL(url));
    } catch (e) {
      // Not a proper file URL, return as-is.
    }
  }
  return url;
}

function getBuiltinModule(id) {
  validateString(id, 'id');
  var normalizedId = BuiltinModule.normalizeRequirableId(id);
  return normalizedId ? _loaderRequire(normalizedId) : undefined;
}

// Capture bootstrap loader require before REPL overwrites globalThis.require.
var _loaderRequire = globalThis.require;

function loadBuiltinModule(id) {
  if (!BuiltinModule.canBeRequiredByUsers(id)) {
    return undefined;
  }
  var mod = BuiltinModule.map.get(id);
  if (!mod) {
    return undefined;
  }
  // Use compileForPublicLoader to load the module via bootstrap require.
  mod.compileForPublicLoader();
  return mod;
}

function toRealPath(requestPath) {
  return require('fs').realpathSync(requestPath);
}

var _hasStartedUserCJSExecution = false;
var _hasStartedUserESMExecution = false;

module.exports = {
  addBuiltinLibsToObject: addBuiltinLibsToObject,
  assertBufferSource: function() {},
  constants: constants,
  enableCompileCache: enableCompileCache,
  flushCompileCache: flushCompileCache,
  getBuiltinModule: getBuiltinModule,
  getCjsConditions: getCjsConditions,
  getCjsConditionsArray: getCjsConditionsArray,
  getCompileCacheDir: getCompileCacheDir,
  initializeCjsConditions: initializeCjsConditions,
  loadBuiltinModule: loadBuiltinModule,
  makeRequireFunction: makeRequireFunction,
  normalizeReferrerURL: normalizeReferrerURL,
  stringify: function(body) { return typeof body === 'string' ? body : String(body); },
  stripBOM: stripBOM,
  toRealPath: toRealPath,
  urlToFilename: urlToFilename,
  hasStartedUserCJSExecution: function() { return _hasStartedUserCJSExecution; },
  setHasStartedUserCJSExecution: function() { _hasStartedUserCJSExecution = true; },
  hasStartedUserESMExecution: function() { return _hasStartedUserESMExecution; },
  setHasStartedUserESMExecution: function() { _hasStartedUserESMExecution = true; },
};
