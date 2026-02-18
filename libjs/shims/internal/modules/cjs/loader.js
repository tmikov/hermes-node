// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Shim for internal/modules/cjs/loader.js
//
// Provides a minimal Module class for the REPL and internal/modules/helpers.js.
// The REPL needs:
//   - Module.builtinModules (array of built-in module names)
//   - Module._nodeModulePaths(dir) (node_modules search paths)
//   - Module._resolveLookupPaths(request, parent) (lookup paths for resolution)
//   - Module._resolveFilename(request, parent, isMain, options) (resolve to filename)
//   - Module._extensions (object keyed by file extensions)
//   - Module._cache (module cache object)
//   - Module.globalPaths (array of global module paths)
//   - new Module(id, parent) with .id, .filename, .paths, .require(), .exports

'use strict';

var {
  ArrayPrototypePush,
  ObjectFreeze,
  ObjectKeys,
  StringPrototypeCharCodeAt,
  StringPrototypeSlice,
} = primordials;

var path = require('path');
var { BuiltinModule } = require('internal/bootstrap/realm');

function Module(id, parent) {
  this.id = id || '';
  this.path = path.dirname(id);
  this.exports = {};
  this.filename = null;
  this.loaded = false;
  this.children = [];
  this.paths = Module._nodeModulePaths(this.path);
}

Module.prototype.require = function(id) {
  return globalThis.require(id);
};

// Static properties.
Module._cache = { __proto__: null };
Module._pathCache = { __proto__: null };
Module._extensions = { __proto__: null, '.js': null, '.json': null, '.node': null };
Module.globalPaths = [];

// Built-in modules list, derived from BuiltinModule.
Module.builtinModules = ObjectFreeze(BuiltinModule.getAllBuiltinModuleIds());

/**
 * Get the paths to the node_modules folders for a given path (POSIX only).
 * @param {string} from Directory path
 * @returns {string[]}
 */
Module._nodeModulePaths = function(from) {
  from = path.resolve(from);
  if (from === '/') {
    return ['/node_modules'];
  }

  var CHAR_FORWARD_SLASH = 47; // '/'
  var paths = [];
  for (var i = from.length - 1, last = from.length; i >= 0; --i) {
    var code = StringPrototypeCharCodeAt(from, i);
    if (code === CHAR_FORWARD_SLASH) {
      // Skip if the segment is 'node_modules' itself.
      var segment = StringPrototypeSlice(from, i + 1, last);
      if (segment !== 'node_modules') {
        ArrayPrototypePush(
          paths,
          StringPrototypeSlice(from, 0, last) + '/node_modules',
        );
      }
      last = i;
    }
  }

  ArrayPrototypePush(paths, '/node_modules');
  return paths;
};

/**
 * Get the paths for module resolution.
 * @param {string} request
 * @param {Module|null} parent
 * @returns {null|string[]}
 */
Module._resolveLookupPaths = function(request, parent) {
  // Built-in modules don't need path lookup.
  if (BuiltinModule.normalizeRequirableId(request)) {
    return null;
  }

  // Non-relative request: use parent's paths + global paths.
  if (request.length > 0 && request[0] !== '.' ||
      (request.length > 1 &&
       request[1] !== '.' &&
       request[1] !== '/')) {
    var paths = [];
    if (parent && parent.paths && parent.paths.length) {
      for (var i = 0; i < parent.paths.length; i++) {
        ArrayPrototypePush(paths, parent.paths[i]);
      }
    }
    for (var j = 0; j < Module.globalPaths.length; j++) {
      ArrayPrototypePush(paths, Module.globalPaths[j]);
    }
    return paths.length > 0 ? paths : null;
  }

  // Relative request with no parent filename (e.g. REPL).
  if (!parent || !parent.id || !parent.filename) {
    return ['.'];
  }

  return [path.dirname(parent.filename)];
};

/**
 * Resolve a module name to a filename.
 * This is a simplified version for our environment.
 * @param {string} request
 * @param {Module|null} parent
 * @param {boolean} isMain
 * @param {object} options
 * @returns {string}
 */
Module._resolveFilename = function(request, parent, isMain, options) {
  // Built-in modules resolve to themselves.
  if (BuiltinModule.normalizeRequirableId(request)) {
    return request;
  }
  // For non-builtin, return the request as-is (our loader handles resolution).
  return request;
};

module.exports = { Module };
