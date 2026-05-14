# Plan: Full CJS Module Resolution

See `history/initial/cjs-module-resolution-project.md` for the
architecture overview, goals, and integration strategy.

---

## Dependencies Between Steps

```
S1 (module_wrap binding)
S2 (readPackageJSON binding)
S3 (compileFunctionForCJSLoader binding)
S4 (legacyMainResolve binding)
    |
    v
S5 (embed new modules) -- depends on S1, S2
    |
    v
S6 (remove/update shims) -- depends on S5
    |
    v
S7 (integrate with bootstrap loader) -- depends on S3, S6
    |
    v
S8 (test: basic node_modules resolution) -- depends on S7
S9 (test: package.json "main" field) -- depends on S7
S10 (test: package.json "exports" field) -- depends on S7
S11 (test: .json file loading) -- depends on S7
S12 (test: nested node_modules) -- depends on S7
S13 (test: circular deps across node_modules) -- depends on S7
S14 (test: require.resolve) -- depends on S7
S15 (test: real npm package) -- depends on S8-S14
```

---

## Steps

### S1: Add `module_wrap` binding stub

**Depends on:** nothing

**What:** Create a new `module_wrap` native binding that exports the constants and
functions the CJS loader imports at top level:

```javascript
const { kEvaluated, createRequiredModuleFacade } = internalBinding('module_wrap');
```

**Details:**

- `kEvaluated` is an integer constant (value `4`, representing V8's
  `Module::Status::kEvaluated`). The CJS loader uses it at line 1007 to detect
  circular `require()` of ESM modules. Since we don't support ESM execution, this
  code path won't trigger, but the constant must exist.

- `createRequiredModuleFacade(wrap)` creates a facade module namespace for
  `require()` of ESM modules. Since we don't support ESM execution, this function
  will never be called. Stub it to throw an error:
  `"require() of ES modules is not supported"`.

**Files:**
- Create `lib/bindings/node_module_wrap.cpp` and
  `include/hermes/node-compat/bindings/node_module_wrap.h`
- Register in `tools/hermes-node/hermes-node.cpp`:
  `registry.registerBinding("module_wrap", initModuleWrapBinding)`
- Add to `lib/bindings/CMakeLists.txt`

**Completion criteria:** `internalBinding('module_wrap').kEvaluated === 4` from JS.

---

### S2: Implement real `readPackageJSON` in `modules` binding

**Depends on:** nothing

**What:** Replace the stub `readPackageJSONCb` (currently returns `undefined`) with
a real implementation that reads and parses `package.json` files.

**API contract** (from Node's `src/node_modules.cc` lines 245-281):

Arguments:
- `args[0]`: string -- file path to package.json
- `args[1]`: boolean -- isESM flag (we can ignore initially)
- `args[2]`: string -- base URL (optional, error context only)
- `args[3]`: string -- specifier (optional, error context only)

Return value: `undefined` if file not found/read fails, or an Array of 6 elements:
```
[name, main, type, imports, exports, file_path]
```
Where:
- `name`: string or undefined
- `main`: string or undefined
- `type`: string ("commonjs", "module", or "none")
- `imports`: raw JSON string of imports field, or undefined
- `exports`: raw JSON string of exports field, or undefined
- `file_path`: string path to the package.json

The JS side (`internal/modules/package_json_reader.js`) deserializes this array.
`imports` and `exports` are kept as raw JSON strings and lazily parsed because they
can be large objects.

**Implementation approach:**
1. Read the file with `uv_fs_read` (sync)
2. Parse JSON. We can use Hermes's JSON parser via `napi_run_script` with
   `JSON.parse(content)`, or parse manually. Using `napi_run_script` with
   `JSON.parse` is simplest and matches behavior.
3. Extract `name`, `main`, `type` as strings
4. Extract `imports` and `exports` as raw JSON strings (use `JSON.stringify` on the
   parsed sub-objects)
5. Return the 6-element array

Alternative simpler approach: since the JS-side `package_json_reader.js` module
also has logic to read and parse, we could implement `readPackageJSON` to just
read the file and return the raw JSON string, letting JS do all parsing. Check
what fields the JS side actually needs from native.

**Actually**, looking more carefully at `package_json_reader.js`, its
`deserializePackageJSON` function expects the 6-element array from native.
But there is also a `read(jsonPath)` function that:
1. Calls `modulesBinding.readPackageJSON(jsonPath, ...)`
2. If result is undefined, returns `{exists: false, ...}`
3. Otherwise calls `deserializePackageJSON(path, result)`

So native MUST return the 6-element array. Implement accordingly.

**Files:**
- Modify `lib/bindings/node_modules.cpp`: replace `readPackageJSONCb` stub
- May need to add `getNearestParentPackageJSONType` as well (currently stub,
  check if the JS module handles the undefined return gracefully)

**Completion criteria:** `require('internal/modules/package_json_reader').read('/path/to/package.json')` returns `{exists: true, data: {name: ..., main: ...}}` for a real package.json.

---

### S3: Implement real `compileFunctionForCJSLoader` in `contextify` binding

**Depends on:** nothing

**What:** Replace the stub (returns `undefined`) with a real implementation that
compiles CJS source code into a callable wrapper function.

**API contract** (from Node's `src/node_contextify.cc`):

Arguments:
- `args[0]`: string -- source code
- `args[1]`: string -- filename
- `args[2]`: boolean -- is_sea_main (always false for us)
- `args[3]`: boolean -- should_detect_module (ESM detection)

Return value: Object with fields:
```javascript
{
  function: Function,      // the compiled wrapper function
  sourceMapURL: undefined, // we don't support source maps yet
  sourceURL: undefined,
  canParseAsESM: false     // we don't detect ESM yet
}
```

**Implementation approach:**

The CJS loader calls `wrapSafe()` which wraps the source in
`Module.wrapper[0] + content + Module.wrapper[1]` then calls
`compileFunctionForCJSLoader`. In Node, the native side does the wrapping.
But looking at the code path:
- If `patched` is false (no monkey-patching), it goes directly to
  `compileFunctionForCJSLoader(content, filename, false, shouldDetectModule)`
  where `content` is the RAW source (wrapper is added by native code).
- If `patched` is true, it uses `Module.wrap(content)` + `makeContextifyScript`
  + `runScriptInThisContext` instead.

So `compileFunctionForCJSLoader` receives UNWRAPPED source and must:
1. Wrap it: `(function(exports, require, module, __filename, __dirname) { <source> \n})`
2. Compile to bytecode via `hermes_compile_to_bytecode()` or just evaluate via
   `napi_run_script`
3. Return the result as `{function: <the wrapper fn>, sourceMapURL: undefined, canParseAsESM: false}`

We already have `hermes_compile_to_bytecode()` and `hermes_run_bytecode()` from
Hermes NAPI. We can also use `napi_run_script` which is simpler.

For `shouldDetectModule`: always set `canParseAsESM` to false since Hermes doesn't
support ESM. If compilation fails and `shouldDetectModule` is true, still just
throw the error.

**Files:**
- Modify `lib/bindings/node_contextify.cpp`: replace `compileFunctionForCJSLoaderCb`

**Completion criteria:** `internalBinding('contextify').compileFunctionForCJSLoader('return 42', 'test.js', false, false).function` returns a callable function.

---

### S4: Add `legacyMainResolve` to `fs` binding

**Depends on:** nothing

**What:** Add the `legacyMainResolve` function to the `fs` binding. This is used by
`internal/modules/esm/resolve.js` to resolve the `main` field in `package.json`.

**API contract** (from Node's `src/node_file.cc` lines 3636-3758):

Arguments:
- `args[0]`: string -- package directory path
- `args[1]`: string (optional) -- `packageConfig.main` value
- `args[2]`: string (optional) -- base URL for error context

Return value: integer index (0-9) into `legacyMainResolveExtensions`:
```
With main:     0='', 1='.js', 2='.json', 3='.node', 4='/index.js', 5='/index.json', 6='/index.node'
Without main:  7='.js', 8='.json', 9='.node' (tried as ./index + ext)
```

Returns `undefined` (no match found) if none of the files exist.

**Implementation:** Try each extension in order using `uv_fs_stat` (sync). For the
`main` case: `resolve(pkgPath, main) + ext`. For the fallback:
`resolve(pkgPath, "index") + ext`. Return the index of the first file that exists.

This is ~60 lines of C++ (simpler than Node's because we skip Windows namespace
paths and permission checks).

**Files:**
- Modify `lib/bindings/node_file.cpp`: add `fsLegacyMainResolve` function
- Register as `"legacyMainResolve"` in `initFsBinding`

**Completion criteria:** `internalBinding('fs').legacyMainResolve('/path/to/pkg', 'index.js')` returns an integer index.

---

### S5: Embed required modules

**Depends on:** S1 (module_wrap binding must exist for loader.js to load), S2 (readPackageJSON must work for package_json_reader.js)

**What:** Add all modules that Node's CJS loader transitively depends on to
`embedded-modules.txt`. Many already exist in `libjs-node/` but aren't embedded.

**Modules to add to `lib/embedded-modules/embedded-modules.txt`:**

CJS loader direct dependencies (not already embedded):
```
internal/modules/package_json_reader
internal/modules/customization_hooks
internal/modules/typescript
internal/modules/run_main
```

ESM resolver and its dependencies (needed for `"exports"` resolution):
```
internal/modules/esm/resolve
internal/modules/esm/assert
internal/modules/esm/get_format
internal/modules/esm/load
internal/modules/esm/loader
internal/modules/esm/hooks
internal/modules/esm/module_map
internal/modules/esm/module_job
internal/modules/esm/translators
internal/modules/esm/create_dynamic_module
internal/modules/esm/initialize_import_meta
internal/modules/esm/shared_constants
internal/modules/esm/worker
```

**Verification:** Check each module compiles to bytecode during build. Some may fail
due to unsupported syntax or missing dependencies. For those that fail, create shims
(see S6).

**Process for each module:**
1. Add to `embedded-modules.txt`
2. Run `cmake --build cmake-build-asan --target hermesNodeEmbeddedModules` (or full build)
3. If it fails to compile, check the error:
   - Missing dependency? Add it or create a shim.
   - Unsupported JS syntax? Create a shim.
4. Continue until all modules compile.

**Files:**
- `lib/embedded-modules/embedded-modules.txt`

**Completion criteria:** All added modules compile to bytecode successfully.

---

### S6: Create/update shims for newly embedded modules

**Depends on:** S5

**What:** Some of the newly embedded modules will fail to load because they depend
on bindings or features we don't have. Create targeted shims for those.

**Known shims needed:**

1. **`internal/modules/esm/utils.js`** -- current shim only exports
   `registerModule`. The real module is needed for `getConditionsSet()` which the
   ESM resolver calls. The real module depends on `internalBinding('module_wrap')`
   for more things. We may need to expand the shim or see if the real module works
   with our `module_wrap` stub from S1.

2. **`internal/modules/esm/loader.js`** -- The main ESM loader. Heavy dependency
   on `module_wrap` binding. Likely needs a shim that exports enough for the CJS
   loader's `loadESMFromCJS()` path (which won't trigger since we return
   `canParseAsESM: false`).

3. **`internal/modules/esm/translators.js`** -- Depends on many things
   (module_wrap, wasm, etc). May need a shim.

4. **`internal/modules/esm/worker.js`** -- Worker threads. Needs stub.

5. **`internal/modules/typescript.js`** -- Depends on `internal/deps/amaro`. Needs
   a shim that stubs `stripTypeScriptModuleTypes` (or a no-op that returns the
   source as-is).

6. **`internal/modules/customization_hooks.js`** -- Exports `resolveForCJSWithHooks`,
   `loadWithHooks`, `registerHooks`, etc. The real module should work if its
   dependencies are met. Check if it loads clean.

7. **Remove existing shims** that are no longer needed:
   - `libjs/shims/internal/modules/cjs/loader.js` -- replaced by the real module
   - `libjs/shims/internal/modules/helpers.js` -- replaced by the real module
   - `libjs/shims/internal/modules/esm/formats.js` -- the real module should work

**Process:**
For each module that fails to load at runtime:
1. Identify what's missing from the error
2. Decide: fix the missing dependency (add binding/shim) or shim this module
3. Shims should export exactly what consumers need, nothing more
4. Each shim file goes in `libjs/shims/` mirroring the `libjs-node/` path

**Critical: do NOT remove shims that provide different behavior than the original.**
The `internal/modules/esm/formats.js` shim might differ in defaults. Test before
removing.

**Files:**
- Various files in `libjs/shims/internal/modules/`
- Remove shims that are superseded

**Completion criteria:** `require('internal/modules/cjs/loader')` succeeds and
returns a Module class with `_resolveFilename`, `_findPath`, `_load`, `_compile`
methods.

---

### S7: Integrate Node's CJS loader with bootstrap

**Depends on:** S3 (compileFunctionForCJSLoader), S6 (shims updated)

**What:** Wire `__loadUserScript()` to use Node's `Module._load()` instead of our
simple `loadModule()`. Also wire the `require()` given to user scripts to use
Node's resolution.

**Changes to `libjs/loader.js`:**

The current `__loadUserScript` is:
```javascript
globalThis.__loadUserScript = function(filepath) {
  loadModule(filepath, filepath);
};
```

After this change:
```javascript
globalThis.__loadUserScript = function(filepath) {
  // Use Node's CJS loader for user scripts.
  // This gives full node_modules resolution, package.json support, etc.
  var CJSModule = require('internal/modules/cjs/loader').Module;
  CJSModule._load(filepath, null, true);
};
```

**But there is a subtlety:** Node's `Module._compile()` calls
`compileFunctionForCJSLoader()` to compile the source. This means the compilation
path for user modules shifts from our `compileAndRun()` in `libjs/loader.js` to
the native `compileFunctionForCJSLoader` binding. That's why S3 must be done first.

**Another subtlety:** Node's Module._load creates a `require` function via
`makeRequireFunction(mod)` (from `helpers.js`), which creates a require that calls
`mod.require(id)` -> `Module._load(id, this, false)`. This is self-contained and
does not need to call back into our bootstrap loader. Internal modules that the
user script depends on (like `fs`, `path`, etc.) are resolved by
`Module._resolveFilename` which checks `BuiltinModule.normalizeRequirableId()`
first, then falls through to file system resolution.

**The critical question:** When `Module._load` encounters a built-in (like `fs`),
it calls `loadBuiltinModule()` from helpers.js. Our current shim returns
`undefined`. The real helpers.js calls `BuiltinModule.compileForPublicLoader()`.
We need `BuiltinModule` (from `internal/bootstrap/realm.js`) to know how to load
built-in modules.

Our `internal/bootstrap/realm.js` shim provides `BuiltinModule` with a list of
module names and `exists()`/`isBuiltin()`. But does it have
`compileForPublicLoader()`? Check and add if needed. The implementation should
delegate to our bootstrap loader's `globalThis.require()`.

**Changes needed:**
1. `libjs/loader.js`: modify `__loadUserScript()` to use `Module._load()`
2. `libjs/shims/internal/bootstrap/realm.js`: ensure `BuiltinModule` has
   `compileForPublicLoader()` or equivalent that delegates to `globalThis.require`
3. `tools/hermes-node/hermes-node.cpp`: ensure Node's CJS loader module is loaded
   during bootstrap, before user script execution (it's already in
   embedded-modules.txt, so `require('internal/modules/cjs/loader')` should work)

**Files:**
- `libjs/loader.js`
- `libjs/shims/internal/bootstrap/realm.js` (if needed)
- Possibly `tools/hermes-node/hermes-node.cpp`

**Completion criteria:** `hermes-node test.js` where test.js contains
`const path = require('path'); console.log(path.join('a', 'b'));` prints `a/b`.

---

### S8: Test -- basic `node_modules/` resolution

**Depends on:** S7

**What:** Verify that `require('some-package')` finds and loads a module from
`node_modules/`.

**Test setup:**
```
test/fixtures/node-modules-basic/
  node_modules/
    my-package/
      index.js        // module.exports = { hello: 'world' };
  main.js             // console.log(require('my-package').hello);
```

**Test file:** `test/test-cjs-node-modules-basic.js`
```javascript
// RUN: %hermes-node %s | %FileCheck %s
const result = require('./fixtures/node-modules-basic/node_modules/my-package');
// or set up so that main.js is run and resolution works
console.log('PASS');
// CHECK: PASS
```

Actually, the test needs to be structured so that `require('my-package')` resolves
from the right directory. Better approach: create a test fixture script that
hermes-node runs directly.

**Completion criteria:** Test passes under `check-hermes-node`.

---

### S9: Test -- `package.json` `"main"` field

**Depends on:** S7

**What:** Verify that `package.json` `"main"` field is respected.

**Test setup:**
```
test/fixtures/node-modules-main/
  node_modules/
    my-package/
      package.json     // {"name": "my-package", "main": "lib/entry.js"}
      lib/
        entry.js       // module.exports = 'main-entry';
  main.js              // const r = require('my-package'); assert(r === 'main-entry');
```

**Completion criteria:** Test passes.

---

### S10: Test -- `package.json` `"exports"` field

**Depends on:** S7

**What:** Verify conditional exports work.

**Test setup:**
```
test/fixtures/node-modules-exports/
  node_modules/
    my-package/
      package.json     // {"name": "my-package", "exports": {".": {"require": "./cjs.js"}, "./utils": "./utils.js"}}
      cjs.js           // module.exports = 'cjs-entry';
      utils.js         // module.exports = 'utils';
  main.js              // assert(require('my-package') === 'cjs-entry');
                       // assert(require('my-package/utils') === 'utils');
```

**Completion criteria:** Test passes.

---

### S11: Test -- `.json` file loading

**Depends on:** S7

**What:** Verify `require('./data.json')` parses JSON and returns the object.

**Test file:** `test/test-cjs-require-json.js`

**Completion criteria:** `require('./fixtures/data.json').key === 'value'` works.

---

### S12: Test -- nested `node_modules/` resolution

**Depends on:** S7

**What:** Verify that nested `node_modules/` directories are searched correctly
(package A depends on package B, each in their own `node_modules/`).

**Completion criteria:** Test passes with 2+ levels of node_modules nesting.

---

### S13: Test -- circular dependencies across `node_modules/`

**Depends on:** S7

**What:** Package A requires package B, package B requires package A. Verify
partial exports are returned (not infinite loop).

**Completion criteria:** Test passes, partial exports are visible.

---

### S14: Test -- `require.resolve()`

**Depends on:** S7

**What:** Verify `require.resolve('my-package')` returns the absolute path to the
resolved file, and `require.resolve.paths('my-package')` returns the search paths.

**Completion criteria:** Test passes.

---

### S15: Test -- real npm package (integration test)

**Depends on:** S8-S14

**What:** Install a small, real npm package (e.g., `minimist`, `ms`, or
`color-name` -- something with no native deps and minimal transitive deps) and
verify it loads and works correctly.

**Process:**
1. Create a test fixture directory
2. `npm install minimist` in it
3. Create a test script that `require('minimist')` and uses it
4. Run with `hermes-node`

**Completion criteria:** Real npm package loads and produces correct output.

---

## Overall Completion Criteria

All of the following must pass:

1. **Existing tests still pass:** `cmake --build cmake-build-asan --target check-hermes-node`
2. **Built-in modules work:** `require('fs')`, `require('path')`, `require('net')` etc. still resolve correctly
3. **REPL works:** `hermes-node` with no args starts REPL, tab completion works
4. **`node_modules/` resolution:** `require('pkg-name')` finds packages in `node_modules/`
5. **`package.json` "main":** Packages with custom `main` field load the right entry point
6. **`package.json` "exports":** Conditional exports with `"require"` condition work
7. **`.json` loading:** `require('./foo.json')` returns parsed JSON
8. **`require.resolve()`:** Returns absolute path to resolved module
9. **Nested dependencies:** Multi-level `node_modules/` nesting works
10. **Circular deps:** Cross-package circular requires return partial exports
11. **Real npm package:** At least one real npm package (e.g., `minimist`) loads and runs

---

## Key Files Reference

### Files to modify:
- `lib/bindings/node_modules.cpp` -- implement `readPackageJSON` (S2)
- `lib/bindings/node_file.cpp` -- add `legacyMainResolve` (S4)
- `lib/bindings/node_contextify.cpp` -- implement `compileFunctionForCJSLoader` (S3)
- `lib/bindings/CMakeLists.txt` -- add new binding source (S1)
- `lib/embedded-modules/embedded-modules.txt` -- add modules (S5)
- `libjs/loader.js` -- integrate Module._load (S7)
- `libjs/shims/internal/bootstrap/realm.js` -- add compileForPublicLoader (S7)
- `tools/hermes-node/hermes-node.cpp` -- register module_wrap binding (S1)

### Files to create:
- `lib/bindings/node_module_wrap.cpp` -- module_wrap binding (S1)
- `include/hermes/node-compat/bindings/node_module_wrap.h` (S1)
- Various shims in `libjs/shims/internal/modules/` (S6)
- Test files in `test/` (S8-S15)
- Test fixtures in `test/fixtures/` (S8-S15)

### Files to remove (shims replaced by real modules):
- `libjs/shims/internal/modules/cjs/loader.js` (S6)
- `libjs/shims/internal/modules/helpers.js` (S6)
- Possibly `libjs/shims/internal/modules/esm/formats.js` (S6)

### Node source reference:
- `/home/tmikov/3rd/node/lib/internal/modules/cjs/loader.js` (2074 lines)
- `/home/tmikov/3rd/node/lib/internal/modules/esm/resolve.js` (1052 lines)
- `/home/tmikov/3rd/node/lib/internal/modules/package_json_reader.js` (378 lines)
- `/home/tmikov/3rd/node/lib/internal/modules/helpers.js` (491 lines)
- `/home/tmikov/3rd/node/lib/internal/modules/esm/utils.js` (354 lines)
- `/home/tmikov/3rd/node/lib/internal/modules/customization_hooks.js` (430 lines)
- `/home/tmikov/3rd/node/src/node_modules.cc` (readPackageJSON native)
- `/home/tmikov/3rd/node/src/node_file.cc` (legacyMainResolve native)
- `/home/tmikov/3rd/node/src/node_contextify.cc` (compileFunctionForCJSLoader native)

---

## Risk Notes

1. **Shim cascade:** The ESM modules have deep dependency chains. Embedding them
   may surface many missing bindings/shims. S5 and S6 may iterate heavily.
   Approach: embed one module at a time, fix failures incrementally.

2. **`compileFunctionForCJSLoader` correctness:** This is the most critical binding.
   It must wrap source correctly and return a callable function. If the wrapper
   signature doesn't match what `_compile()` expects, modules will fail silently.
   Test carefully.

3. **BuiltinModule integration:** Node's CJS loader calls
   `BuiltinModule.compileForPublicLoader()` to load built-ins. Our realm.js shim
   must bridge this to our bootstrap loader. If this doesn't work, built-in modules
   like `fs` and `path` will fail to load through the new path.

4. **Performance:** Node caches `stat()` results aggressively. Our implementation
   should use the same `_stat` cache (`statCache` in loader.js) to avoid excessive
   syscalls during resolution.

5. **Bootstrap ordering:** Node's CJS loader must be fully initialized before any
   user code runs. The bootstrap sequence in `hermes-node.cpp` must ensure
   `require('internal/modules/cjs/loader')` is called at the right time.
