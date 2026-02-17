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
  //   loadBytecodeModule(id) -> function|undefined  (native: load pre-compiled bytecode)
  //   readFileSync(path) -> string  (native: reads file from disk, for user scripts)
  //   primordials         -> object  (the primordials object)
  //   internalBinding     -> function (the internalBinding function)
  return function setup(loadBytecodeModule, readFileSync, primordials, internalBinding) {
    // Module cache: moduleId -> { exports, loaded }
    var cache = Object.create(null);

    // The module wrapper template. Node uses:
    // (function(exports, require, module, __filename, __dirname) { ... })
    // We add primordials and internalBinding as closure variables rather than
    // wrapper parameters, matching how Node's internal loader works -- modules
    // access them as free variables from the compilation scope.

    function loadModule(id, filepath) {
      // Try embedded bytecode first.
      var compiledFn = loadBytecodeModule(id);
      if (!compiledFn) {
        // Disk fallback for user scripts.
        var source = readFileSync(filepath);

        // Wrap in the Node module function wrapper.
        var wrapped =
          '(function(exports, require, module, __filename, __dirname) {' +
          source +
          '\n});\n//# sourceURL=' + filepath + '\n';

        // Compile the wrapper. This gives us a function.
        // We use an indirect eval via (0, eval) to get global scope eval in
        // strict mode contexts.
        compiledFn = (0, eval)(wrapped);
      }

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
    // so that relative requires (./foo, ../bar) resolve correctly.
    function makeRequire(fromFilepath) {
      var lastSlash = fromFilepath.lastIndexOf('/');
      var fromDir = lastSlash >= 0 ? fromFilepath.substring(0, lastSlash) : '.';

      function require(name) {
        // Relative path requires are resolved relative to the requiring file.
        if (name.charAt(0) === '.') {
          var resolvedPath = resolveRelative(fromDir, name);
          // Check cache by resolved path.
          if (cache[resolvedPath]) {
            return cache[resolvedPath].exports;
          }
          var mod = loadModule(resolvedPath, resolvedPath);
          return mod.exports;
        }
        return requireModule(name);
      }
      require.resolve = function(name) {
        if (name.charAt(0) === '.') {
          return resolveRelative(fromDir, name);
        }
        return name;
      };
      return require;
    }

    // Resolve a relative require to a file path, trying CJS conventions:
    // 1. exact path, 2. path + .js, 3. path/index.js
    function resolveRelative(fromDir, name) {
      var base = resolvePath(fromDir, name);
      // If already ends in .js, use as-is.
      if (base.length > 3 && base.substring(base.length - 3) === '.js') {
        return base;
      }
      // Try base.js first.
      var withJs = base + '.js';
      try {
        readFileSync(withJs);
        return withJs;
      } catch (e) {
        // fall through
      }
      // Try base/index.js (directory with index).
      var indexJs = base + '/index.js';
      try {
        readFileSync(indexJs);
        return indexJs;
      } catch (e) {
        // fall through
      }
      // Default to .js extension (will fail at load time with a clear error).
      return withJs;
    }

    // Simple path resolution: resolve a relative path against a base directory.
    function resolvePath(base, relative) {
      var parts = base.split('/');
      var relParts = relative.split('/');
      for (var i = 0; i < relParts.length; i++) {
        if (relParts[i] === '..') {
          parts.pop();
        } else if (relParts[i] !== '.' && relParts[i] !== '') {
          parts.push(relParts[i]);
        }
      }
      return parts.join('/');
    }

    // The main require implementation.
    function requireModule(name) {
      // Check cache first.
      if (cache[name]) {
        return cache[name].exports;
      }

      // Load and execute the module. For internal modules the ID is used
      // as both the module ID and filepath (bytecode is looked up by ID,
      // filepath only matters for user scripts).
      var mod = loadModule(name, name);
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
      globalThis.require = requireModule;

      // Expose loadUserScript for the C++ bootstrap to run user scripts
      // as CJS modules (with path-aware require for relative imports).
      globalThis.__loadUserScript = function(filepath) {
        loadModule(filepath, filepath);
      };
    }

    // Return the require function.
    return requireModule;
  };
})()
