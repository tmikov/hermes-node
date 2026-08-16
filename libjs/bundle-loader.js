// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// Wraps Node's CJS Module._load so a bundled (importer, specifier) pair is
// served from the container without touching the filesystem. Anything that
// misses falls through to the original _load, which resolves and compiles
// from disk exactly as it does without a bundle.

(function() {
  'use strict';

  // Builtin precedence is decided here, before the edge table is consulted,
  // so a bundle can never shadow fs, path, or any other embedded builtin.
  // normalizeRequirableId() is the same predicate Node's own Module._load
  // uses (BuiltinModule.canBeRequiredWithoutScheme for a bare name, the full
  // requirable set for a 'node:' one), and for bare names it is exactly the
  // list the producer skipped when it walked the graph -- so a specifier
  // that is a builtin here never had an edge to find in the first place.
  //
  // Do NOT "simplify" this to Module.isBuiltin. That is
  // BuiltinModule.isBuiltin, which answers for allRequirableIds -- builtins
  // PLUS the vendored packages ('ws'). The producer's isBuiltinSpecifier
  // (lib/bundle/bundle_resolve.cpp) mirrors builtinIds only, so it really
  // does bundle a bare require('ws') resolved out of node_modules. Answering
  // "builtin" here for that specifier would hand it to the vendored copy and
  // silently ignore the version the bundle shipped.
  var BuiltinModule =
    globalThis.require('internal/bootstrap/realm').BuiltinModule;

  // The same factory Node's own loader uses to build the `require` it hands
  // a module (Module.prototype._compile calls it). Reusing it is what keeps
  // require.cache, require.extensions, require.resolve.paths and
  // require.main identical between a module served from the container and
  // the same file compiled from disk after an edge-table miss -- identical
  // source must not mean different semantics depending on whether the
  // producer happened to package it.
  var makeRequireFunction =
    globalThis.require('internal/modules/helpers').makeRequireFunction;

  return function installBundleLoader(Module, bundle, path) {
    var root = bundle.root();

    // The `require` a bundled module sees. Everything except resolve() is
    // Node's own, built by makeRequireFunction off this module record, so
    // require.cache, require.extensions, require.resolve.paths and
    // require.main are the real ones rather than stand-ins.
    //
    // resolve() is the one thing a bundle has to answer differently.
    // Module._resolveFilename walks the filesystem, and a bundled program's
    // tree may not exist any more, so the edge table is consulted first: a
    // hit answers path.join(root, identity), which is exactly the
    // __filename that module sees when it loads. Anything with no edge -- a
    // builtin, a computed specifier, a .node addon -- falls through to the
    // real resolver, which is also where a builtin gets its bare-name
    // answer. The builtin check comes first for the same reason it does in
    // the Module._load wrapper below: whatever _load would hand to the
    // original loader, resolve() must resolve the same way.
    //
    // Returning the specifier text unchanged (what this used to do) is
    // worse than an error: `path.dirname(require.resolve('pkg'))` is a
    // common way to find a package root, and it silently yielded '.'.
    //
    // An explicit `options.paths` is the caller replacing the search path
    // outright, which is a different question from the one the edge table
    // answers: the edge records where THIS importer's specifier resolved at
    // build time, not where it resolves from some other directory. Node
    // honours the option, so the edge table is skipped whenever one is
    // present -- including a malformed one, which Module._resolveFilename
    // then rejects with the error Node gives.
    function makeRequire(mod, identity) {
      var req = makeRequireFunction(mod);
      var baseResolve = req.resolve;
      function resolve(request, options) {
        var hasPaths = options !== undefined && options !== null &&
          options.paths !== undefined;
        if (!hasPaths &&
            BuiltinModule.normalizeRequirableId(request) === undefined) {
          var target = bundle.lookup(identity, request);
          if (target !== undefined) return path.join(root, target);
        }
        return baseResolve(request, options);
      }
      resolve.paths = baseResolve.paths;
      req.resolve = resolve;
      return req;
    }

    // Node's updateChildren(), which runs on every Module._load including
    // the cache-hit path: a module required by several importers appears in
    // each importer's children. Returning early on a hit without this makes
    // module.children under-report.
    function updateChildren(parentMod, child, scan) {
      if (!parentMod) return;
      var children = parentMod.children;
      if (!children) return;
      if (scan && children.indexOf(child) !== -1) return;
      children.push(child);
    }

    // The other half of Node's _load finally block (see loader.js's
    // `if (threw)`): a module that threw while loading is spliced out of its
    // importer's children as well as out of the cache, so a failed require()
    // leaves no trace at all. Without this, the usual shape around an
    // optional dependency -- require() in a try/catch, retried -- grows
    // children without bound, every entry a loaded:false record with empty
    // exports.
    function removeChild(parentMod, child) {
      if (!parentMod) return;
      var children = parentMod.children;
      if (!Array.isArray(children)) return;
      var index = children.indexOf(child);
      if (index !== -1) children.splice(index, 1);
    }

    // The one place a bundled module is instantiated. The entry point goes
    // through it too (see the returned runEntry), so there is no second copy
    // of this logic for the main module to drift away from.
    //
    // The record this builds stands in for a real Module wherever Node's own
    // loader can see it -- it is handed back to Module._load as `parent` on
    // every fallback, and published into Module._cache -- so it carries the
    // fields that loader reads off a parent, not just the five the module
    // wrapper needs. Each one below is there because something in
    // libjs-node/internal/modules/cjs/loader.js reads it.
    //
    // It is a plain object, though, not a `new Module(...)`: it duck-types
    // the fields, and nothing in the loader's paths through here needs the
    // prototype. Since Module._cache is the loader's only cache, these
    // records are what a third-party `require.cache` walker sees, so the
    // difference is observable -- `require.cache[f] instanceof Module` is
    // false and `module.constructor.name` is 'Object', where Node and a
    // disk load both say Module. Recorded as a known limitation in
    // history/plans/progress-aot-bundle.md rather than fixed: constructing
    // real Module instances would run Node's constructor, which does its own
    // updateChildren() and paths setup, and the fields would have to be
    // overwritten straight afterwards.
    function loadIdentity(target, parentMod, isMain) {
      var filename = path.join(root, target);
      // Module._cache is the ONLY cache. There is no second, identity-keyed
      // one, for two reasons.
      //
      // It would be redundant: identity -> path.join(root, identity) is
      // injective (identities are distinct normalized paths relative to one
      // root), so the two would key the same set of modules.
      //
      // And it would be wrong. `delete require.cache[require.resolve(x)]`
      // followed by a fresh require() is the standard reload idiom, and it
      // reaches only Module._cache. A private cache still holding the record
      // would hand the caller the stale module and report success, where the
      // same code on disk re-executes it. A cache the loader keeps to itself
      // is a cache the program cannot invalidate.
      //
      // Sharing this one also means a module reached through the disk
      // fallback and a module reached through the edge table are the same
      // record: the fallback resolves to exactly this filename, so a
      // singleton stays a singleton across the boundary.
      // `!== undefined`, not a truthiness test, because that is the test
      // Node's _load makes and a program can put anything in require.cache.
      // `require.cache[f] = null` followed by require(f) throws a TypeError
      // under Node and from disk; a truthiness test would quietly
      // re-instantiate from the container instead.
      var cached = Module._cache[filename];
      if (cached !== undefined) {
        updateChildren(parentMod, cached, true);
        return cached.exports;
      }

      var dirname = path.dirname(filename);
      var mod = {
        // Node names the main module '.' and every other module by its
        // filename (see Module._load's isMain branch).
        id: isMain ? '.' : filename,
        exports: {},
        loaded: false,
        filename: filename,
        // Module._load keys its relativeResolveCache on
        // `${parent.path}\x00${request}`. Without `path` every bundled
        // module keys as "undefined", so two of them in different
        // directories falling back on the same specifier text -- say two
        // packages each requiring './build/Release/binding.node' -- would
        // share one entry and the second would be handed the first's module.
        path: dirname,
        // Module._resolveLookupPaths tests parent?.paths?.length; without
        // this a bare specifier that falls back to disk never searches the
        // importer's node_modules chain, only the global paths.
        paths: Module._nodeModulePaths(dirname),
        // Node's main module has parent === null, and the legacy
        // `if (!module.parent)` entry idiom keys on exactly that: leaving it
        // undefined everywhere would fire that guard in every bundled
        // module. updateChildren() pushes into children on the fallback
        // path, so it has to be an array.
        parent: parentMod || null,
        children: [],
        __bundleIdentity: target,
      };
      // Node's Module.prototype.require, with this record as the parent. It
      // exists for two reasons: `module.require(...)` is public API that a
      // bundled module can call, and makeRequireFunction's `require` is a
      // one-line forwarder to it. Going through the real prototype method
      // rather than calling Module._load directly keeps its argument
      // validation and requireDepth bookkeeping; it reads Module._load
      // dynamically (through wrapModuleLoad), so the bundle wrapper
      // installed below still sees the call.
      mod.require = function(id) {
        return Module.prototype.require.call(mod, id);
      };
      // Fresh record, so no scan: it cannot already be in children.
      updateChildren(parentMod, mod, false);
      // Cached BEFORE the module body runs, which is what makes circular
      // requires terminate -- the same reason libjs/loader.js:78 does it,
      // and the same moment Node's own _load publishes into Module._cache.
      Module._cache[filename] = mod;
      // Published before the body runs too, for the same kind of reason: the
      // entry's own require.main is read from inside its body (via
      // process.mainModule, which is where makeRequireFunction reads it),
      // and the entry is instantiated before anything it pulls in. Node
      // makes the main module reachable the same two ways --
      // process.mainModule and require.main -- and
      // `if (require.main === module)` is the standard way a CLI guards its
      // entry point, so a bundled program whose whole body sits inside that
      // guard would do nothing at all without this.
      if (isMain) process.mainModule = mod;

      // A module that throws while loading must not stay in the cache, or a
      // later require() of it returns the empty exports of a module that
      // never ran, silently, instead of throwing again. Nor may it stay in
      // the importer's children, or a retried require() piles up one dead
      // record per attempt. Node undoes both in one place (Module._load's
      // `let threw = true` / finally, which deletes the cache entry and
      // splices the module out of `parent.children`), and so does this.
      // Only on a throw: removing unconditionally would break the cycle
      // handling the early publish above exists for.
      var threw = true;
      try {
        var payload = bundle.load(target);
        if (typeof payload === 'string') {
          mod.exports = JSON.parse(payload);
        } else {
          payload(mod.exports, makeRequire(mod, target), mod, filename,
            dirname);
        }
        threw = false;
      } finally {
        if (threw) {
          delete Module._cache[filename];
          removeChild(parentMod, mod);
        }
      }
      mod.loaded = true;
      return mod.exports;
    }

    var originalLoad = Module._load;
    Module._load = function(request, parent, isMain) {
      // No bundled importer means no row in the edge table to look up: the
      // edges are keyed by (importer, specifier), and only a bundled module
      // can be an importer. Builtins are handed to the original loader for
      // the same reason plus one more -- they must win over the bundle.
      // Written as a conditional rather than `parent && ...` so that a null
      // parent yields undefined rather than null: null would slip past the
      // guard below and reach the miss branch, which would log a bogus
      // "[bundle] miss: X from null" under HERMES_NODE_DEBUG_NATIVE.
      var importer = parent ? parent.__bundleIdentity : undefined;
      if (importer === undefined ||
          BuiltinModule.normalizeRequirableId(request) !== undefined) {
        return originalLoad.call(this, request, parent, isMain);
      }

      var target = bundle.lookup(importer, request);
      if (target === undefined) {
        // A miss inside a valid bundle is not an error: a computed require()
        // is invisible to static discovery, and a .node addon is deliberately
        // left out. Both fall back to disk. The log line is the only way to
        // tell a fallback from a bundled load, so it is worth a flag.
        if (process.env.HERMES_NODE_DEBUG_NATIVE &&
            process.env.HERMES_NODE_DEBUG_NATIVE.indexOf('BUNDLE') >= 0) {
          console.error('[bundle] miss: ' + request + ' from ' + importer);
        }
        return originalLoad.call(this, request, parent, isMain);
      }

      return loadIdentity(target, parent, false);
    };

    // Runs the bundle's entry module, through the same path as every other
    // bundled module. It is the main module: the bundle is the program, and
    // like Node's main module it has no parent.
    return function runEntry() {
      return loadIdentity(bundle.entry(), null, true);
    };
  };
})()
