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
  //   compileAndRun(source, sourceUrl, enableTS) -> value  (native: compile+run)
  //   primordials         -> object  (the primordials object)
  //   internalBinding     -> function (the internalBinding function)
  return function setup(loadBytecodeModule, readFileSync, compileAndRun, primordials, internalBinding) {
    // Module cache: moduleId -> { exports, loaded }
    var cache = Object.create(null);

    // Load vendored package data (generated at build time from vendored/*/).
    var vendoredDataFn = loadBytecodeModule('vendored-packages');
    var vendoredData = vendoredDataFn ? vendoredDataFn() : { packages: {}, moduleIds: [] };
    var vendoredPackages = vendoredData.packages;      // e.g. { 'ws': 'vendored/ws/index' }
    var vendoredIdSet = new Set(vendoredData.moduleIds); // Set of all vendored embedded IDs

    // True once a bundle is running. A bundle is a closed world: the
    // container is the only source of module code, and this loader's disk
    // fallback below is a second one -- it reads an arbitrary path and
    // compiles it. That fallback exists to serve __loadUserScript, which
    // bundle mode never uses, so a bundle can close it outright.
    //
    // It matters because this loader IS globalThis.require. An ordinary
    // require() inside a bundled module does not reach it (the CommonJS
    // wrapper's own `require` parameter shadows the global), but
    // `(0, eval)('require')`, `global.require` and
    // `new Function('return require')()` all do, and each of those would
    // otherwise compile and run any file named by a computed specifier --
    // exactly the containment property a bundle is supposed to have.
    //
    // Deleting the global instead would not work: libjs/shims/domain.js
    // calls globalThis.require('events') at its own load time, so
    // require('domain') inside a bundle needs it to still be there. The
    // flag is what goes away, not the function.
    var diskLoadingClosed = false;

    // The module wrapper template. Node uses:
    // (function(exports, require, module, __filename, __dirname) { ... })
    // We add primordials and internalBinding as closure variables rather than
    // wrapper parameters, matching how Node's internal loader works -- modules
    // access them as free variables from the compilation scope.

    function loadModule(id, filepath) {
      // Try embedded bytecode first.
      var compiledFn = loadBytecodeModule(id);
      if (!compiledFn) {
        // Not embedded, and in a closed world there is nowhere else to
        // look: the container is the only source of module code, and this
        // is the path that would otherwise read and compile an arbitrary
        // file. See diskLoadingClosed above.
        if (diskLoadingClosed) {
          var closed = new Error(
            "Cannot find module '" + id + "'\n" +
            '  A bundle is running, so the container is the only source of' +
            ' module code.\n' +
            '  This fallback does not read the filesystem.');
          closed.code = 'MODULE_NOT_FOUND';
          throw closed;
        }
        // Disk fallback for user scripts.
        var source = readFileSync(filepath);

        // Strip shebang line — Hermes doesn't handle #! like V8.
        if (source.length >= 2 && source[0] === '#' && source[1] === '!') {
          var nl = source.indexOf('\n');
          source = nl >= 0 ? source.slice(nl) : '';
        }

        // Wrap in the Node module function wrapper.
        var wrapped =
          '(function(exports, require, module, __filename, __dirname) {' +
          source +
          '\n});\n//# sourceURL=' + filepath + '\n';

        // Compile via native hermes_run_script (persistent bytecode).
        var isTS = filepath.length > 3 &&
            filepath.substring(filepath.length - 3) === '.ts';
        compiledFn = compileAndRun(wrapped, filepath, isTS);
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
      var isVendored = fromDir.substring(0, 9) === 'vendored/';

      function require(name) {
        // Relative path requires are resolved relative to the requiring file.
        if (name.charAt(0) === '.') {
          // Vendored modules resolve against embedded IDs (no disk probing).
          var resolvedPath = isVendored
            ? resolveVendoredRelative(fromDir, name)
            : resolveRelative(fromDir, name);
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
          return isVendored
            ? resolveVendoredRelative(fromDir, name)
            : resolveRelative(fromDir, name);
        }
        return name;
      };
      return require;
    }

    // Does this path name a readable file? Answers false without asking the
    // filesystem once a bundle is running: resolution is module code
    // loading's other half, and a closed world does not consult the disk
    // for either. Every candidate then misses, resolveRelative() returns
    // its default, and loadModule() reports the closed-world error.
    function probeFile(p) {
      if (diskLoadingClosed) return false;
      try {
        readFileSync(p);
        return true;
      } catch (e) {
        return false;
      }
    }

    // Resolve a relative require to a file path, trying CJS conventions:
    // 1. exact path, 2. path + .js, 3. path + .ts,
    // 4. path/index.js, 5. path/index.ts
    function resolveRelative(fromDir, name) {
      var base = resolvePath(fromDir, name);
      // If already ends in .js or .ts, use as-is.
      if (base.length > 3) {
        var ext = base.substring(base.length - 3);
        if (ext === '.js' || ext === '.ts') {
          return base;
        }
      }
      var withJs = base + '.js';
      if (probeFile(withJs)) return withJs;
      var withTs = base + '.ts';
      if (probeFile(withTs)) return withTs;
      // A directory with an index file.
      var indexJs = base + '/index.js';
      if (probeFile(indexJs)) return indexJs;
      var indexTs = base + '/index.ts';
      if (probeFile(indexTs)) return indexTs;
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

    // Resolve a relative require within a vendored package against embedded IDs.
    // No disk probing -- purely checks vendoredIdSet.
    function resolveVendoredRelative(fromDir, name) {
      var base = resolvePath(fromDir, name);
      if (vendoredIdSet.has(base)) return base;
      var indexId = base + '/index';
      if (vendoredIdSet.has(indexId)) return indexId;
      return base; // will error at loadModule time
    }

    // The main require implementation.
    function requireModule(name) {
      // Strip 'node:' prefix so require('node:fs') === require('fs').
      var bareName = name;
      if (name.substring(0, 5) === 'node:')
        bareName = name.slice(5);

      // Check cache first.
      if (cache[bareName]) {
        return cache[bareName].exports;
      }

      // Check vendored packages (e.g. 'ws' -> 'vendored/ws/index').
      var vendoredId = vendoredPackages[bareName];
      if (vendoredId) {
        var mod = loadModule(vendoredId, vendoredId);
        // Also cache under the bare package name.
        cache[bareName] = mod;
        return mod.exports;
      }

      // Load and execute the module. For internal modules the ID is used
      // as both the module ID and filepath (bytecode is looked up by ID,
      // filepath only matters for user scripts).
      var mod = loadModule(bareName, bareName);
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

      // Closes this loader's disk fallback for the rest of the process.
      // Called by libjs/bundle-loader.js's installBundleLoader(), before
      // the bundle's entry module runs.
      //
      // One-way on purpose: there is no matching re-open, so the fact that
      // user code can reach this global buys it nothing -- calling it again
      // is a no-op and deleting it cannot undo a close that already
      // happened. globalThis.require has to stay reachable (domain.js
      // requires 'events' through it, lazily), so what the bundle takes
      // away is the disk path inside it, not the function itself.
      //
      // Non-writable, non-configurable: an ordinary assignment would let a
      // -r/--require preload -- which runs before installBundleLoader()
      // ever calls this function -- replace it with a no-op before the
      // close happens, reopening disk loading with no error at all. Deleting
      // the global was already harmless (loadModule() would just throw
      // "globalThis.__closeDiskModuleLoading is not a function" from
      // installBundleLoader()'s call below), but replacing it failed open,
      // silently. defineProperty makes both failure modes closed: a preload
      // assigning to the property in sloppy mode is now a silent no-op (the
      // assignment is dropped, the original closer survives), the same
      // assignment in strict mode throws TypeError, and `delete` returns
      // false and leaves the property in place. Either way the function
      // installBundleLoader() calls is still the real one.
      Object.defineProperty(globalThis, '__closeDiskModuleLoading', {
        value: function() {
          diskLoadingClosed = true;
        },
        writable: false,
        configurable: false,
        enumerable: true,
      });

      // Expose loadUserScript for the C++ bootstrap to run user scripts
      // as CJS modules with full Node.js module resolution (node_modules/,
      // package.json, exports, etc.).
      globalThis.__loadUserScript = function(filepath) {
        var CJSModule = requireModule('internal/modules/cjs/loader').Module;
        var p = requireModule('path');
        CJSModule._load(p.resolve(filepath), null, true);
      };

      // Prepare the scope that -e / --eval code runs in, before the C++
      // bootstrap evaluates it as a plain script.
      //
      // The point is `require`. By default eval code sees globalThis.require,
      // this bootstrap loader, which resolves embedded module ids and
      // otherwise just reads the path off disk and compiles it as JavaScript
      // source. Requiring an addon that way handed the compiler a shared
      // library ("unrecognized Unicode character \u7f", the first byte of the
      // ELF magic); requiring a .json file handed it JSON. Replacing it with a
      // require built for a module named "[eval]" in the current directory
      // gives Node's behavior: dispatch by extension (.node via
      // process.dlopen, .json via JSON.parse) and node_modules resolution
      // rooted at the cwd. Internal module ids still resolve, because
      // __initCJS wraps Module._load with a fallback to this loader.
      //
      // The code is still evaluated as a script rather than wrapped in a
      // module function, because Node's -e runs at global scope: `var x = 1`
      // is expected to leave globalThis.x set, and wrapping it would not.
      globalThis.__setupEvalContext = function() {
        var CJSModule = requireModule('internal/modules/cjs/loader').Module;
        var p = requireModule('path');
        var cwd = process.cwd();
        var absolute = p.join(cwd, '[eval]');

        // Matching Node exactly: module.id is the bare "[eval]" while
        // module.filename is absolute, and the __filename / __dirname the
        // code sees are "[eval]" and "." rather than real paths. Resolution
        // still happens from the cwd, via module.paths and the absolute
        // filename handed to createRequire.
        var mod = new CJSModule('[eval]', null);
        mod.filename = absolute;
        mod.paths = CJSModule._nodeModulePaths(cwd);

        globalThis.require = CJSModule.createRequire(absolute);
        globalThis.module = mod;
        globalThis.exports = mod.exports;
        globalThis.__filename = '[eval]';
        globalThis.__dirname = '.';
      };

      // Initialize Node's CJS loader so Module.builtinModules and other
      // static properties are available before the REPL or user code runs.
      // Also installs fallback resolution and TypeScript support.
      globalThis.__initCJS = function() {
        var cjs = requireModule('internal/modules/cjs/loader');
        if (cjs.initializeCJS) {
          cjs.initializeCJS();
        }

        var CJSModule = cjs.Module;

        // Wrap Module._load to fall back to the bootstrap loader for
        // embedded modules that aren't public builtins (e.g. internal/*
        // modules used in tests and during bootstrap).
        var origLoad = CJSModule._load;
        CJSModule._load = function(request, parent, isMain) {
          try {
            return origLoad(request, parent, isMain);
          } catch (e) {
            if (e.code === 'MODULE_NOT_FOUND') {
              // Check if the bootstrap loader can resolve this module.
              if (cache[request]) return cache[request].exports;
              if (loadBytecodeModule(request)) {
                return requireModule(request);
              }
              // Check vendored packages (bare name fallback when not in node_modules).
              var vendoredId = vendoredPackages[request];
              if (vendoredId) {
                return requireModule(request);
              }
            }
            throw e;
          }
        };

        // Register .ts extension handler. Our compileAndRun native
        // function supports TypeScript via Hermes's enable_ts flag.
        var helpers = requireModule('internal/modules/helpers');
        CJSModule._extensions['.ts'] = function(module, filename) {
          var fs = requireModule('fs');
          var p = requireModule('path');
          var content = fs.readFileSync(filename, 'utf8');
          var wrapped = CJSModule.wrap(content);
          var compiledFn = compileAndRun(wrapped, filename, true);
          var dirname = p.dirname(filename);
          var req = helpers.makeRequireFunction(module);
          compiledFn(module.exports, req, module, filename, dirname);
        };
      };
    }

    // Return the require function.
    return requireModule;
  };
})()
