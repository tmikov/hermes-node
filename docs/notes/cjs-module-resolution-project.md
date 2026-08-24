# CJS Module Resolution -- Architecture Overview

## Context

Currently hermes-node-compat has a simple bootstrap loader (`libjs/loader.js`) that
resolves internal/embedded modules by ID and user scripts by relative path (trying
`.js`, `.ts`, `/index.js`, `/index.ts`). There is no support for `node_modules/`
lookup, `package.json` fields (`main`, `exports`, `imports`, `type`), `.json` file
loading, or any of the standard Node.js CJS module resolution algorithm.

The goal is to adopt Node's real CJS loader (`internal/modules/cjs/loader.js`) for
user-facing `require()` calls, giving us the full resolution algorithm including
`node_modules/` traversal, `package.json` processing, conditional exports/imports,
extension trying, symlink handling, and module hooks. The ESM resolution and loader
modules should also be vendored and embedded even though Hermes cannot execute ESM
yet -- they will be ready when Hermes adds ESM support, and the CJS loader already
depends on the ESM resolver for `"exports"` field resolution.

Our bootstrap loader (`libjs/loader.js`) stays as-is for internal module loading
during the boot sequence. Node's `Module._load()` takes over for user code.

---

## Architecture After This Work

```
User script / REPL
       |
       v
globalThis.__loadUserScript(filepath)
       |
       v
+----------------------------------------------+
| libjs/loader.js  (bootstrap loader)          |
| - Loads embedded bytecode modules by ID      |
| - Handles relative requires between internal |
|   modules during bootstrap                   |
| - Sets globalThis.require / primordials /    |
|   internalBinding                            |
+----------------------------------------------+
       |  After bootstrap, user require() goes to:
       v
+----------------------------------------------+
| Node's Module._load()                        |
| (libjs-node/internal/modules/cjs/loader.js)  |
|                                               |
| - Module._resolveFilename()                  |
|   - builtin check                            |
|   - trySelf() (package.json self-reference)  |
|   - Module._findPath()                       |
|     - resolveExports() -> ESM resolver       |
|     - tryExtensions() (.js, .json, .node)    |
|     - tryPackage() (package.json "main")     |
|   - Module._nodeModulePaths() traversal      |
|                                               |
| - Module.prototype.load()                    |
|   - _extensions['.js'] -> _compile()         |
|   - _extensions['.json'] -> JSON.parse       |
|                                               |
| - Module.prototype._compile()               |
|   - wrapSafe() -> compileFunctionForCJSLoader|
|   - ReflectApply(wrapper, ...)               |
+----------------------------------------------+
       |
       v
+----------------------------------------------+
| Native bindings                              |
| - fs.internalModuleStat (already have)       |
| - fs.legacyMainResolve (need to add)         |
| - modules.readPackageJSON (need real impl)   |
| - contextify.compileFunctionForCJSLoader     |
|   (need real impl)                           |
| - module_wrap: kEvaluated +                  |
|   createRequiredModuleFacade (need stubs)    |
+----------------------------------------------+
```

### Integration Strategy

The key insight is that `__loadUserScript()` in `libjs/loader.js` currently calls
our simple `loadModule(filepath, filepath)`. After this work, it will instead call
`Module._load(filepath, null, true)` from Node's CJS loader. Internal modules
(loaded during bootstrap, before Node's CJS loader is initialized) continue to use
the bootstrap loader unchanged.

The transition point is in `libjs/loader.js`: after all bootstrap modules are loaded
and Node's Module class is available, `__loadUserScript()` switches to use
`Module._load()`.
