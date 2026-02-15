// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Internal module loader for hermes-node-compat.
//
// This file is evaluated by the C++ ModuleLoader::init() and must return
// a setup function. The setup function receives native helpers and returns
// a require() function that loads Node's lib/*.js modules.
//
// The returned require() follows Node CJS conventions:
//   - Module cache (each module loaded once)
//   - Circular dependency support (module.exports set before execution)
//   - Module wrapper: (function(exports, require, module, __filename, __dirname) { ... })

(function() {
  'use strict';

  // The setup function receives:
  //   readFileSync(path) -> string  (native: reads file from disk)
  //   libJsPath          -> string  (path to libjs/ directory, trailing /)
  //   libJsNodePath      -> string  (path to libjs-node/ directory, trailing /)
  //   primordials         -> object  (the primordials object)
  //   internalBinding     -> function (the internalBinding function)
  return function setup(readFileSync, libJsPath, libJsNodePath, primordials, internalBinding) {
    // Module cache: moduleId -> { exports, loaded }
    var cache = Object.create(null);

    // Resolve a module name to a filesystem path and a module ID.
    // Returns { id: string, filepath: string } or null if not found.
    function resolve(name) {
      // Check for shim overrides first: libjs/shims/<name>.js
      // e.g. 'internal/options' -> libjs/shims/internal/options.js
      var shimPath = libJsPath + 'shims/' + name + '.js';
      try {
        readFileSync(shimPath);
        return { id: name, filepath: shimPath };
      } catch (e) {
        // No shim, fall through.
      }

      // Standard resolution: libjs-node/<name>.js
      var nodePath = libJsNodePath + name + '.js';
      return { id: name, filepath: nodePath };
    }

    // The module wrapper template. Node uses:
    // (function(exports, require, module, __filename, __dirname) { ... })
    // We add primordials and internalBinding as closure variables rather than
    // wrapper parameters, matching how Node's internal loader works — modules
    // access them as free variables from the compilation scope.

    function loadModule(id, filepath) {
      // Read the module source.
      var source = readFileSync(filepath);

      // Wrap in the Node module function wrapper.
      var wrapped =
        '(function(exports, require, module, __filename, __dirname) {\n' +
        source +
        '\n});\n//# sourceURL=' + filepath + '\n';

      // Compile the wrapper. This gives us a function.
      // We use an indirect eval via (0, eval) to get global scope eval in
      // strict mode contexts.
      var compiledFn = (0, eval)(wrapped);

      // Create the module object.
      var mod = {
        id: id,
        exports: {},
        loaded: false,
        filename: filepath,
      };

      // Cache BEFORE execution to handle circular dependencies.
      // If module B requires module A while A is still executing,
      // B will get A's partially-populated exports object.
      cache[id] = mod;

      // Compute __dirname from __filename.
      var lastSlash = filepath.lastIndexOf('/');
      var dirname = lastSlash >= 0 ? filepath.substring(0, lastSlash) : '.';

      // Execute the module.
      compiledFn(mod.exports, makeRequire(filepath), mod, filepath, dirname);

      mod.loaded = true;
      return mod;
    }

    // Create a require function bound to the requesting module's filepath
    // (for relative path resolution in the future).
    function makeRequire(fromFilepath) {
      function require(name) {
        return requireModule(name);
      }
      require.resolve = function(name) {
        var resolved = resolve(name);
        return resolved.filepath;
      };
      return require;
    }

    // The main require implementation.
    function requireModule(name) {
      // Check cache first.
      if (cache[name]) {
        return cache[name].exports;
      }

      // Resolve the module.
      var resolved = resolve(name);
      if (!resolved) {
        throw new Error("Cannot find module '" + name + "'");
      }

      // Also check cache by resolved ID (handles aliasing).
      if (cache[resolved.id]) {
        return cache[resolved.id].exports;
      }

      // Load and execute the module.
      var mod = loadModule(resolved.id, resolved.filepath);
      return mod.exports;
    }

    // Set up the global primordials object so that modules can access it.
    // Node's internal modules expect `primordials` to be available in their
    // compilation scope. Since we wrap modules with a simple function wrapper
    // (not a module-scope injection), we put primordials on globalThis.
    // The modules destructure from it: const { X, Y } = primordials;
    if (typeof globalThis !== 'undefined') {
      globalThis.primordials = primordials;
      globalThis.internalBinding = internalBinding;
    }

    // Return the require function.
    return requireModule;
  };
})()
