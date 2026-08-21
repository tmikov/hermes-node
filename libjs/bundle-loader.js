// Copyright (c) Tzvetan Mikov.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
//
// Wraps Node's CJS Module._load so a bundled (importer, specifier) pair is
// served from the container without touching the filesystem. A pair with no
// row in the edge table -- a computed require(), typically -- gets a second
// chance from the container's resolver.
//
// A bundle is a closed world for require()/require.resolve(): those two are
// the ONLY sources of module code reached through them (code that
// deliberately drops to Module.prototype.load, require.extensions, or reads
// and evals a file itself is outside that boundary, same as unbundled).
// What neither can place is an error naming the importer and the remedy
// (--include), not a filesystem lookup. That is the point of shipping a
// bundle -- "self-contained" is unverifiable while a container that quietly
// still needs node_modules behaves exactly like one that does not -- and it
// is also a containment property: a computed specifier must not be able to
// name any file on the machine and have the loader compile and run it.
//
// What is still handed to the original loader is what the binary itself
// carries rather than what the disk does, and the two get there by
// different routes:
//
//   - Builtins ('fs', 'node:path', ...) are intercepted at the top of the
//     wrapper by normalizeRequirableId() and forwarded VERBATIM, request
//     text unchanged. Module._resolveFilename returns them on its first
//     line, so a bare builtin name reaches nothing on disk.
//   - The vendored packages ('ws') the runtime serves when no node_modules
//     copy was packaged are the only requests embeddedRequest() rewrites.
//     A bare 'ws' is NOT a builtin to Module._resolveFilename, so
//     forwarding it unchanged really would reopen the filesystem; the
//     'node:ws' spelling is what stops that. See embeddedRequest().
//
// Two other doors into the filesystem are shut here rather than being
// reasoned away. globalThis.require -- this loader's bootstrap sibling in
// libjs/loader.js -- reads and compiles an arbitrary path for an id it has
// no embedded bytecode for, and a bundled module can still reach it via
// `(0, eval)('require')` or `global.require` even though its own `require`
// parameter shadows it; installBundleLoader() calls
// __closeDiskModuleLoading() below to take that path away. And a `require`
// built by Module.createRequire() carries no __bundleIdentity, so
// identityOf() derives one from its filename when that filename lies under
// the bundle root, rather than leaving it to fall out of the closed world.

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
  // the same file compiled from disk without a bundle -- identical source
  // must not mean different semantics depending on whether the program was
  // bundled.
  var makeRequireFunction =
    globalThis.require('internal/modules/helpers').makeRequireFunction;

  return function installBundleLoader(Module, bundle, path, fs) {
    var root = bundle.root();
    var entryIdentity = bundle.entry();

    // identity -> sidecar filename, for the container's native addons.
    // Built once, here, rather than asked per require(): a native call on
    // the hot path of every require would cost every module in the bundle
    // to describe the one or two that are addons -- the same reasoning
    // that took the HERMES_NODE_DEBUG_NATIVE read off that path.
    //
    // A null-prototype object so a module named 'constructor.node' (or
    // anything else that collides with Object.prototype) cannot be
    // mistaken for a native.
    var nativeSidecars = Object.create(null);
    var natives = bundle.natives();
    for (var ni = 0; ni < natives.length; ni++)
      nativeSidecars[natives[ni].identity] = natives[ni].sidecar;

    // Shut libjs/loader.js's disk fallback before anything from the
    // container runs. That loader is globalThis.require, which a bundled
    // module can still reach around its own wrapper parameter, and its
    // fallback reads and compiles whatever path it is handed. Nothing in
    // bundle mode needs it: it exists for __loadUserScript, which a bundle
    // never uses. One-way -- see the comment on the global itself.
    globalThis.__closeDiskModuleLoading();

    // True for what the binary carries rather than what the disk does: the
    // builtins plus the vendored packages ('ws'). Module.isBuiltin is
    // BuiltinModule.isBuiltin, which answers for allRequirableIds, and that
    // is exactly the set this predicate wants -- the deliberate opposite of
    // the normalizeRequirableId() check above, which decides PRECEDENCE and
    // must not let 'ws' pre-empt a packaged node_modules copy. This one runs
    // last, after the container has had both of its chances, and only says
    // "the runtime can still serve this without opening a file".
    function isEmbedded(request) {
      return Module.isBuiltin(request);
    }

    // ...and the spelling under which it must be asked for. A bare 'ws' is
    // NOT a builtin as far as Module._resolveFilename is concerned --
    // normalizeRequirableId deliberately does not answer for it, which is
    // what lets an installed node_modules/ws win in a run with no bundle --
    // so handing the bare name to the original loader takes the ordinary
    // path: _findPath over parent.paths, a filesystem walk that would find,
    // compile and run whatever node_modules/ws happens to sit on the
    // deployment machine, in preference to the copy in the binary. That is
    // precisely the hole this whole change closes, so it must not be
    // reopened for the one specifier the closed world still forwards.
    //
    // 'node:ws' returns from Module._resolveFilename's very first line
    // (`if (BuiltinModule.normalizeRequirableId(request)) return request`)
    // and reaches loadBuiltinWithHooks with no resolution at all. Identity
    // is unaffected: require('node:ws') === require('ws') is asserted by
    // test/bundle-build.js's VENDORED case, and the runtime keeps one
    // record under the bare name.
    function embeddedRequest(request) {
      return request.slice(0, 5) === 'node:' ? request : 'node:' + request;
    }

    // The --include value that would have packaged this request, ready to
    // be copied onto a command line -- or undefined when there is no
    // correct value to print.
    //
    // --include resolves from the ENTRY's directory, not from the
    // importer's, so a relative request cannot be echoed back verbatim. A
    // computed require('./helper') inside node_modules/foo/index.js was
    // suggested as `--include=./helper`, and that invocation fails with
    // "--include=./helper cannot be resolved"; the one that works is
    // `--include=./node_modules/foo/helper`. Advice that does not run is
    // worse than no advice, and a computed relative require inside a
    // dependency is the common shape, not an exotic one.
    //
    // Both identities are relative to the bundle root, so the value is
    // path arithmetic and nothing else: join the request onto the
    // importer's directory, then express the result relative to the
    // entry's directory. An absolute path resolves the same way from any
    // directory, so it is suggested as written.
    //
    // A bare specifier ('@babel/preset-env') is suggested as written too,
    // but NOT because it resolves the same way from anywhere -- it doesn't.
    // Node's node_modules walk starts at the importer and climbs, so one
    // required from inside a nested node_modules can need a deeper value:
    // 'baz' required by node_modules/foo/index.js can need
    // --include=./node_modules/foo/node_modules/baz, where plain
    // --include=baz only works if baz sits at the root node_modules (the
    // common case, and the one the design's own --include=@babel/preset-env
    // example is). There is no way to compute the correct value here -- the
    // request never resolved, so there is no candidate directory to build
    // it from -- so the bare value is printed and notInBundle() appends a
    // line saying where --include resolves from, so the message stays
    // truthful even where it isn't automatic.
    function includeSuggestion(request, importer, importerIsIdentity) {
      if (request.charAt(0) !== '.') return request;
      // A request relative to an importer this loader cannot place in the
      // tree has no correct value; the caller says where --include
      // resolves from instead of printing one that fails.
      if (!importerIsIdentity) return undefined;
      var target = path.join(path.dirname(importer), request);
      var value = path.relative(path.dirname(entryIdentity), target);
      if (value === '') return undefined;
      return value.charAt(0) === '.' ? value : './' + value;
    }

    // True for a specifier whose --include suggestion is not guaranteed to
    // work -- a bare specifier, per the comment on includeSuggestion()
    // above. A relative request's suggestion is computed and always
    // correct; an absolute path resolves the same from anywhere.
    function isBareSpecifier(request) {
      return request.charAt(0) !== '.' && !path.isAbsolute(request);
    }

    // The error a closed world reports where the old loader read the disk.
    // A native addon takes this same path as any other missing specifier:
    // the producer packages a literal require() of a .node file, so a
    // miss here means only that nothing packaged it, and
    // --include is exactly as actionable an answer for it as it is for a
    // missing .js -- see includeSuggestion() below. (An addon the container
    // DOES record but whose sidecar file is absent at run time is a
    // different situation, handled by missingSidecar(), not here.)
    //
    // `code` is MODULE_NOT_FOUND in every case: programs branch on it -- an
    // optional-dependency probe is a require() in a try/catch that tests
    // e.code -- and in a closed world the probe's answer is legitimately
    // "no". Only the human-readable text differs, and it names the
    // importer, because a bundled module's stack trace points at bytecode
    // with no source on disk behind it.
    function notInBundle(request, importer, importerIsIdentity) {
      var suggestion =
        includeSuggestion(request, importer, importerIsIdentity);
      // A bare specifier's suggestion is not guaranteed to work (see
      // isBareSpecifier() above), so the message adds where --include
      // resolves from rather than implying the printed value always does.
      var caveat =
        suggestion !== undefined && isBareSpecifier(request)
          ? "\n  (--include resolves from the entry's directory.)"
          : '';
      var err = new Error(
        "Cannot find module '" + request + "'\n" +
        "  required by " + importer + "\n" +
        (suggestion !== undefined
          ? "  Not in the bundle. Add it with:\n" +
            "    --include=" + suggestion + caveat
          : "  Not in the bundle. Add it with --include, whose value is\n" +
            "  resolved from the entry's directory."));
      err.code = 'MODULE_NOT_FOUND';
      return err;
    }

    // The addon is recorded in the container but its file is not beside
    // the bundle. MODULE_NOT_FOUND rather than ERR_DLOPEN_FAILED: the
    // practical meaning is "this addon is unavailable", and the code that
    // exists in the world to handle that -- an optional-dependency probe,
    // a napi-rs try/catch chain -- branches on MODULE_NOT_FOUND. Precision
    // that breaks a fallback is worth less than the fallback.
    function missingSidecar(identity, sidecar) {
      var err = new Error(
        "Cannot find module '" + identity + "'\n" +
        '  This bundle records a native addon, but its file is not beside ' +
        'the bundle.\n' +
        '  Expected: ' + path.join(root, sidecar) + '\n' +
        '  Native addons ship alongside the container; copy it there.');
      err.code = 'MODULE_NOT_FOUND';
      return err;
    }

    // The trace of how the container answered every require() it saw. It
    // used to log only a miss -- the handful of requires that took the disk
    // fallback -- but a miss is fatal now, and knowing what a bundle asked
    // for is only half of what a --include list is built from: the other
    // half is knowing why the requires that DID succeed did, since a hit
    // from the container's resolver (a computed specifier that happens to
    // already be packaged) is one more --include away from a miss the
    // moment the file it depends on is not, and there is no way to tell
    // that apart from an edge-table hit without logging both.
    //
    // `outcome` is 'edge' (found by the row the producer recorded at build
    // time), 'resolve' (found by asking the container's resolver at run
    // time -- the same algorithm the producer used, against the same
    // identity set) or 'miss' (found by neither). `identity` is the
    // container identity a hit resolved to; omitted for a miss, which has
    // none -- that keeps the miss line's wording exactly what it was before
    // this outcome existed, so an existing reader's expectations still
    // hold.
    //
    // Builtins and the vendored packages the runtime serves out of the
    // binary never reach here: isEmbedded()'s callers forward those before
    // either lookup runs, so they have no outcome to report, and a line for
    // every require('path') would drown the log in the entries that are
    // never interesting (test/bundle-scanner.js's MISS case pins this with
    // --implicit-check-not).
    //
    // The gate is read ONCE, here, and not inside logOutcome(). Every
    // require() and every require.resolve() calls logOutcome, hit or miss,
    // and the wrapper runs before the Module._cache check, so a repeated
    // require('./dep') pays for the gate on every call rather than only on
    // the first. process.env is a Proxy whose get trap is a native callback
    // around getenv() (lib/process/node_process.cpp), so the obvious
    // in-function test costs a proxy trap plus a native call plus a getenv
    // on the hottest path of an artifact whose entire reason for existing
    // is startup cost -- twice over, since `&&` reads the property again.
    // Reading it at install time also matches the native side, which takes
    // the same variable from getenv() once during startup
    // (lib/runtime/hermes_node_runtime.cpp); a program that assigns to
    // process.env.HERMES_NODE_DEBUG_NATIVE mid-run does not turn native
    // tracing on either.
    var debugBundle = false;
    var debugSetting = process.env.HERMES_NODE_DEBUG_NATIVE;
    if (typeof debugSetting === 'string')
      debugBundle = debugSetting.indexOf('BUNDLE') >= 0;

    function logOutcome(outcome, request, importer, identity) {
      if (debugBundle) {
        console.error('[bundle] ' + outcome + ': ' + request + ' from ' +
          importer + (identity !== undefined ? ' -> ' + identity : ''));
      }
    }

    // The identity of the module a require() came from, given the `parent`
    // Node's loader hands over, or undefined when there is none.
    //
    // A record loadIdentity() built carries __bundleIdentity outright. A
    // Module built by Module.createRequire() does not -- it is an ordinary
    // Module with a filename and nothing else -- and createRequire
    // (__filename) is plain CommonJS that real packages use. Without this,
    // every specifier such a require() names, INCLUDING one the container
    // holds, took the "no bundled importer" throw.
    //
    // A filename under the bundle root is an identity once the root is
    // stripped, by exactly the rule BundleFileSource::stripRoot uses
    // natively: a lexical prefix, no ".." climbing and no symlink
    // resolution. path.relative() answers with a leading ".." for anything
    // outside, and outside the root there is genuinely nothing to name, so
    // that stays undefined and the closed world reports it.
    //
    // Nothing checks that the derived identity is itself packaged, and
    // nothing should: the container's resolver takes it as the directory
    // to resolve FROM, which is a meaningful question even for a file the
    // container does not hold -- createRequire(path.join(__dirname, 'x'))
    // where 'x' was never packaged is exactly that.
    function identityOf(parent) {
      if (!parent) return undefined;
      if (parent.__bundleIdentity !== undefined)
        return parent.__bundleIdentity;
      if (typeof parent.filename !== 'string') return undefined;
      var rel = path.relative(root, parent.filename);
      if (rel === '' || rel === '..' || rel.slice(0, 3) === '../' ||
          rel.charAt(0) === '/')
        return undefined;
      return rel;
    }

    // The `require` a bundled module sees. Everything except resolve() is
    // Node's own, built by makeRequireFunction off this module record, so
    // require.cache, require.extensions, require.resolve.paths and
    // require.main are the real ones rather than stand-ins.
    //
    // resolve() is the one thing a bundle has to answer differently.
    // Module._resolveFilename walks the filesystem, and a bundled program's
    // tree may not exist any more -- and in a closed world may not be
    // consulted even when it does -- so the two container-backed answers are
    // all there is: the edge table first, then bundle.resolve(), running the
    // producer's own algorithm against the container's identity set. A hit
    // from either answers path.join(root, identity), which is exactly the
    // __filename that module sees when it loads. A miss throws the same
    // MODULE_NOT_FOUND the Module._load wrapper below throws, for the same
    // reason and by the same helper: a resolve() that succeeded where the
    // require() that follows it fails would be worse than either, so the two
    // must agree about what the container holds.
    //
    // Only what the binary itself carries still reaches the real resolver --
    // a builtin, or a vendored package the runtime serves -- which is also
    // where a builtin gets its bare-name answer. That check comes first for
    // the same reason it does in the wrapper below: whatever _load would
    // hand to the original loader, resolve() must resolve the same way.
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
    // present. bundle.resolve() is not skipped for a well-formed paths,
    // though: it runs the same algorithm Node does for a paths option
    // (resolve as if from each entry in turn, first hit wins), so it can
    // answer a paths-qualified request from the container exactly as well
    // as an unqualified one. Babel reaches exactly this path --
    // require.resolve(id, { paths: [dirname] }) in
    // @babel/core/lib/config/files/plugins.js -- so it is not a corner case.
    //
    // A `paths` that is present but not an array is neither well-formed nor
    // absent -- it is Node's own error to throw
    // (Module._resolveFilename's ERR_INVALID_ARG_VALUE), and only
    // baseResolve knows how to construct that error, so a malformed paths
    // bypasses the edge table AND bundle.resolve() entirely and falls
    // straight through to it, exactly as it would with no bundle at all. A
    // non-string request goes the same way for the same reason: baseResolve
    // opens with validateString(), and reporting MODULE_NOT_FOUND for
    // require.resolve(123) instead of ERR_INVALID_ARG_TYPE would be this
    // loader inventing an answer where Node has one.
    function makeRequire(mod, identity) {
      var req = makeRequireFunction(mod);
      var baseResolve = req.resolve;
      function resolve(request, options) {
        var hasPaths = options !== undefined && options !== null &&
          options.paths !== undefined;
        var pathsUsable = !hasPaths || Array.isArray(options.paths);
        if (typeof request !== 'string' || !pathsUsable ||
            BuiltinModule.normalizeRequirableId(request) !== undefined) {
          return baseResolve(request, options);
        }
        if (!hasPaths) {
          var target = bundle.lookup(identity, request);
          if (target !== undefined) {
            logOutcome('edge', request, identity, target);
            return path.join(root, target);
          }
        }
        var viaContainer = bundle.resolve(identity, request,
          hasPaths ? options.paths : undefined);
        if (viaContainer !== undefined) {
          logOutcome('resolve', request, identity, viaContainer);
          return path.join(root, viaContainer);
        }
        if (isEmbedded(request))
          return baseResolve(embeddedRequest(request), options);
        logOutcome('miss', request, identity);
        throw notInBundle(request, identity, true);
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
    // through it too (see the returned run()), so there is no second copy
    // of this logic for the main module to drift away from.
    //
    // The record this builds stands in for a real Module wherever Node's own
    // loader can see it -- it is published into Module._cache, and handed
    // back to Module._load as `parent` whenever a bundled module requires a
    // builtin or a vendored package -- so it carries the fields that loader
    // reads off a parent, not just the five the module wrapper needs. Each
    // one below is there because something in
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
      // Sharing this one also means a module reached through the edge table
      // and the same module reached through the container's resolver (a
      // computed specifier) are one record: both routes end at the same
      // identity and therefore at this filename, so a singleton stays a
      // singleton across them.
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
        // `${parent.path}\x00${request}`, and reads it on every call with a
        // parent -- which is every builtin or vendored require() a bundled
        // module makes. Without `path` every bundled module keys as
        // "undefined" and two of them requiring different things under the
        // same specifier text would share one entry.
        path: dirname,
        // Module._resolveLookupPaths tests parent?.paths?.length; without
        // this a bare specifier handed to the original loader reaches only
        // the global paths, never the importer's node_modules chain.
        paths: Module._nodeModulePaths(dirname),
        // Node's main module has parent === null, and the legacy
        // `if (!module.parent)` entry idiom keys on exactly that: leaving it
        // undefined everywhere would fire that guard in every bundled
        // module. Node's own updateChildren() pushes into children when one
        // of these records is a parent, so it has to be an array.
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
        // The native branch must run before any call to bundle.load(): that
        // call throws for a kNative record (its bytes are not in the
        // container -- see __bundleLoad's refusal), so a native has to be
        // recognized and dispatched here, ahead of the two-way JS/JSON
        // dispatch below rather than as a fallback from it.
        var sidecar = nativeSidecars[target];
        if (sidecar !== undefined) {
          // The addon's bytes are not in the container -- dlopen takes a
          // path, and there is no portable way to load a shared object from
          // memory -- so they ship as a flat file beside the bundle.
          // mod.filename stays the identity path, like every other bundled
          // module (whose file is not on disk either); the real path is
          // what dlopen is given and what its errors name.
          var addonPath = path.join(root, sidecar);
          if (!fs.existsSync(addonPath)) throw missingSidecar(target, sidecar);
          // Its own outcome name, so HERMES_NODE_DEBUG_NATIVE=BUNDLE
          // distinguishes "resolved to an addon and dlopen'd it" from an
          // ordinary container hit. This is a LOAD, where the other
          // logOutcome calls are resolutions, which is exactly why it is
          // worth telling apart.
          // `target` fills both the importer and identity slots: a load has
          // no importer to report (the resolution that produced `target`
          // already logged its own outcome above), and the module being
          // loaded IS the identity, so there is nothing else to put there.
          logOutcome('native', sidecar, target, target);
          process.dlopen(mod, addonPath);
        } else {
          var payload = bundle.load(target);
          if (typeof payload === 'string') {
            mod.exports = JSON.parse(payload);
          } else {
            payload(mod.exports, makeRequire(mod, target), mod, filename,
              dirname);
          }
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
      // A non-string request is Node's to reject, not this loader's to
      // guess at -- the same reasoning as makeRequire's resolve() above.
      // It reaches nothing on disk either way: the original loader stops in
      // its own validation long before a filesystem lookup.
      if (typeof request !== 'string')
        return originalLoad.call(this, request, parent, isMain);

      // Builtins are decided before anything else, because they must win
      // over the bundle: normalizeRequirableId() is the same predicate
      // Node's own _load uses, and it is exactly the list the producer
      // skipped when it walked the graph, so a specifier that is a builtin
      // here never had an edge to find in the first place.
      if (BuiltinModule.normalizeRequirableId(request) !== undefined)
        return originalLoad.call(this, request, parent, isMain);

      // No bundled importer means no row in the edge table to look up: the
      // edges are keyed by (importer, specifier), and only a module inside
      // the bundle's tree can be an importer. identityOf() also covers the
      // `require` a module built with Module.createRequire() hands out,
      // whose Module carries a filename but no __bundleIdentity.
      var importer = identityOf(parent);
      if (importer === undefined) {
        // Nothing in a bundled program's own graph reaches here: the entry
        // is bundled and so is everything it loads, so every importer
        // carries an identity. If it does happen, it is reported rather
        // than quietly reopening the disk -- which is the whole point --
        // except for what the binary carries, which is not a disk read.
        if (isEmbedded(request))
          return originalLoad.call(
            this, embeddedRequest(request), parent, isMain);
        logOutcome('miss', request, '<no bundled importer>');
        throw notInBundle(request, '<no bundled importer>', false);
      }

      var target = bundle.lookup(importer, request);
      if (target !== undefined) {
        logOutcome('edge', request, importer, target);
        return loadIdentity(target, parent, false);
      }

      // A miss in the edge table is not necessarily a miss in the
      // container: a computed require() is invisible to the static scanner
      // that built the edge table, but the file it names may still be
      // sitting in the container regardless, if some other, literal
      // require() elsewhere caused it to be packaged -- or an --include
      // put it there. Answering from the container the same way the
      // producer would have -- same algorithm, same identity set -- is what
      // makes a computed specifier work after the source tree is gone.
      var resolved = bundle.resolve(importer, request);
      if (resolved !== undefined) {
        logOutcome('resolve', request, importer, resolved);
        return loadIdentity(resolved, parent, false);
      }

      // A vendored package with no packaged copy is served by the runtime
      // out of the binary, and asking for it by its 'node:' name is what
      // keeps that from becoming a filesystem read -- see embeddedRequest().
      // The container had both of its chances first, which is what lets an
      // installed node_modules/ws that WAS packaged win over the embedded
      // one (test/bundle-build.js's WSCOPY case).
      if (isEmbedded(request))
        return originalLoad.call(
          this, embeddedRequest(request), parent, isMain);

      // Nothing can answer. In a closed world that is the end of it: the
      // disk is not a source of module code, so this is an error naming
      // the importer and the remedy rather than a filesystem lookup.
      logOutcome('miss', request, importer);
      throw notInBundle(request, importer, true);
    };

    // The resolution half of the same closure, for the one `require` this
    // loader does not build itself.
    //
    // makeRequire() above overrides resolve() on every require handed to a
    // bundled module, so that one never reaches here. A require built by
    // Module.createRequire() does: its resolve() is Node's own, which calls
    // Module._resolveFilename, which walks the real filesystem -- stat by
    // stat, through node_modules directories that may not exist and may not
    // be the artifact's. That loads nothing, so it is a resolution leak
    // rather than an execution hole, but a closed world does not answer
    // resolution off the disk either, and an answer naming a real path
    // outside the container is one a caller will act on.
    //
    // The four passthroughs are makeRequire()'s, for its reasons: a builtin
    // (which _resolveFilename answers on its own first line), a non-string
    // request, a malformed options.paths (ERR_INVALID_ARG_VALUE is Node's
    // to construct), and a request the binary carries. The embedded
    // forwards this loader makes elsewhere never reach here at all --
    // Module._load short-circuits a 'node:' request before resolution --
    // so wrapping this cannot affect them.
    var originalResolveFilename = Module._resolveFilename;
    Module._resolveFilename = function(request, parent, isMain, options) {
      var hasPaths = options !== undefined && options !== null &&
        options.paths !== undefined;
      if (typeof request !== 'string' ||
          (hasPaths && !Array.isArray(options.paths)) ||
          BuiltinModule.normalizeRequirableId(request) !== undefined) {
        return originalResolveFilename.call(
          this, request, parent, isMain, options);
      }

      var importer = identityOf(parent);
      var named = importer === undefined ? '<no bundled importer>' : importer;
      if (!hasPaths) {
        if (importer === undefined) {
          if (isEmbedded(request)) {
            return originalResolveFilename.call(
              this, embeddedRequest(request), parent, isMain, options);
          }
          logOutcome('miss', request, named);
          throw notInBundle(request, named, false);
        }
        var target = bundle.lookup(importer, request);
        if (target !== undefined) {
          logOutcome('edge', request, importer, target);
          return path.join(root, target);
        }
      }

      // With an explicit paths, the importer is not what resolution starts
      // from -- each entry in paths is -- so an unknown importer is not an
      // obstacle there, and the empty identity is only ever a placeholder
      // the native side never reads.
      var resolved = bundle.resolve(
        importer === undefined ? '' : importer,
        request,
        hasPaths ? options.paths : undefined);
      if (resolved !== undefined) {
        logOutcome('resolve', request, named, resolved);
        return path.join(root, resolved);
      }

      if (isEmbedded(request)) {
        return originalResolveFilename.call(
          this, embeddedRequest(request), parent, isMain, options);
      }
      logOutcome('miss', request, named);
      throw notInBundle(request, named, importer !== undefined);
    };

    // Runs the bundle: its recorded preloads, in order, then its entry.
    // Each goes through loadIdentity like every other bundled module, so a
    // preload's own require() is a container require and there is no second
    // code path. A preload is not the main module -- the entry is -- and it
    // runs before the entry exists, so require.main is unset while it runs,
    // exactly as it is for Node's -r.
    return function run() {
      var preloads = bundle.preloads();
      for (var i = 0; i < preloads.length; i++)
        loadIdentity(preloads[i], null, false);
      return loadIdentity(bundle.entry(), null, true);
    };
  };
})()
