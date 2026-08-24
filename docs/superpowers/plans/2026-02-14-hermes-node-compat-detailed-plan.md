# Hermes Node.js Compatibility Layer — Detailed Implementation Plan

Phases 1–4: Bootstrap through File System. Standalone CLI only (no React Native).

## Decisions

- **Repository:** Separate `hermes-node-compat` repo. Hermes is included as a git submodule (pinned to the n-api branch where the Node-API implementation lives — this branch hasn't been merged to Hermes main yet).
- **Vendoring (external deps):** Unmodified external dependencies (libuv, etc.) live under `external/$lib/$lib`. The outer `$lib/` dir contains a README (provenance: upstream URL, git hash, any local patches) and a wrapper `CMakeLists.txt`. The inner `$lib/` dir is the unmodified vendored source.
- **Vendoring (Node.js JS):** Node's `lib/*.js` files are vendored under `libjs-node/`. These WILL be modified. A README tracks provenance (Node version, git hash) and a changelog of modifications.
- **Primordials:** Option B — thin shim (re-export builtins, no tamper-resistance)
- **JS loading:** Load JS from disk at runtime. `libjs/` contains our own JS (primordials shim, loader, shims). `libjs-node/` contains vendored (potentially modified) Node.js JS files.
- **Build system:** CMake. Top-level `CMakeLists.txt` pulls in the Hermes submodule and vendored deps.
- **Directory structure:** Follows Hermes conventions — C++ libraries under `lib/`, public headers under `include/hermes/node-compat/<lib>/`, CLI tools under `tools/`, GTest under `unittests/`, lit tests under `test/`.
- **Event loop:** Single libuv loop, `uv_run(UV_RUN_DEFAULT)`, standalone only
- **Async hooks:** Stubbed (no-op)
- **Node version:** v24.13.0 LTS (the checked-out source)

## Notation

- `[depends: 3, 5]` — cannot start until steps 3 and 5 are complete
- `[native]` — requires writing C++ (Node-API) code
- `[js]` — requires writing JavaScript
- `[cmake]` — build system changes
- `[verify]` — testing/verification milestone

---

## Directory Structure

```
hermes-node-compat/
├── CMakeLists.txt                          # top-level: hermes submodule + external/ + lib/ + tools/
├── hermes/                                 # git submodule → Hermes repo (n-api branch)
│
├── external/                               # vendored, UNMODIFIED third-party deps
│   └── libuv/
│       ├── README.md                       # provenance: upstream URL, git hash, patches
│       ├── CMakeLists.txt                  # wrapper that adds vendored source to build
│       └── libuv/                          # unmodified vendored source tree
│
├── include/hermes/node-compat/             # public C++ headers (one subdir per library)
│   ├── binding-registry/
│   ├── event-loop/
│   ├── bindings/
│   ├── module-loader/
│   └── process/
│
├── lib/                                    # C++ library source (one subdir per library, each with CMakeLists.txt)
│   ├── binding-registry/                   # internalBinding() registry
│   ├── event-loop/                         # libuv-backed hermes_napi_event_loop adapter
│   ├── bindings/                           # ported native bindings (constants, fs, types, util, etc.)
│   ├── module-loader/                      # internal require() + module wrapper
│   └── process/                            # process object construction
│
├── libjs/                                  # JS files WE write
│   ├── primordials.js                      # thin primordials shim (Option B)
│   ├── loader.js                           # internal module loader JS side
│   └── shims/                              # JS shims for Node internals we override
│       └── internal/
│           └── options.js
│
├── libjs-node/                             # vendored Node.js v24.13.0 lib/*.js (WILL be modified)
│   ├── README.md                           # provenance + modification changelog
│   ├── fs.js
│   ├── events.js
│   ├── path.js
│   ├── internal/
│   │   ├── errors.js
│   │   ├── validators.js
│   │   └── ...
│   └── ...
│
├── tools/                                  # CLI executables
│   └── hermes-node/
│       ├── CMakeLists.txt
│       └── hermes-node.cpp
│
├── unittests/                              # GTest unit tests (gtest from Hermes submodule)
│   ├── CMakeLists.txt
│   ├── event-loop/
│   ├── bindings/
│   └── binding-registry/
│
└── test/                                   # lit tests (lit from Hermes submodule)
```

**Key conventions:**
- C++ libraries live under `lib/<name>/`, each with its own `CMakeLists.txt`. They are reusable by tests, tools, etc.
- Public headers live under `include/hermes/node-compat/<name>/`, mirroring the `lib/` structure.
- `external/$lib/$lib` — outer dir has README (provenance) + wrapper CMakeLists.txt; inner dir is unmodified upstream source.
- `libjs-node/` is vendored from Node but **will** be modified. Not under `external/` because that's reserved for unmodified deps.
- `libjs/` is our own JS code, distinct from vendored Node JS.

---

## A. Build Infrastructure

### Step 1: Create hermes-node-compat repo and CMake scaffolding [cmake]

Create the `hermes-node-compat` repository with:

```
hermes-node-compat/
├── CMakeLists.txt                          # top-level: hermes submodule + external/ + lib/ + tools/
├── hermes/                                 # git submodule → Hermes repo, pinned to n-api branch
│
├── external/                               # vendored, UNMODIFIED third-party deps
│   └── (empty for now)
│
├── include/hermes/node-compat/             # public C++ headers
│   └── (empty for now)
│
├── lib/                                    # C++ library source (reusable)
│   └── placeholder/
│       ├── CMakeLists.txt                  # placeholder lib to verify build
│       └── placeholder.cpp
│
├── libjs/                                  # JS files WE write
│   └── (empty for now — primordials, loader, shims go here)
│
├── libjs-node/                             # vendored Node.js lib/*.js (WILL be modified)
│   ├── README.md                           # provenance: Node version, git hash, modification log
│   ├── fs.js
│   ├── events.js
│   ├── path.js
│   ├── internal/
│   │   ├── errors.js
│   │   ├── validators.js
│   │   └── ...
│   └── ...
│
├── tools/                                  # CLI executables
│   └── hermes-node/
│       ├── CMakeLists.txt
│       └── hermes-node.cpp                 # main() — initially just prints usage
│
├── unittests/                              # GTest unit tests (gtest from Hermes submodule)
│   └── CMakeLists.txt
│
└── test/                                   # lit tests (lit from Hermes submodule)
    └── (empty for now)
```

- Top-level `CMakeLists.txt` adds the Hermes submodule via `add_subdirectory(hermes)`
- Top-level `CMakeLists.txt` adds `lib/`, `tools/`, `unittests/`
- `tools/hermes-node/CMakeLists.txt` defines the `hermes-node` executable, links against libs from `lib/`
- Copy Node.js v24.13.0 `lib/` tree into `libjs-node/`. Add `libjs-node/README.md` documenting the source (Node repo URL, git hash `def0bdf8`, version `v24.13.0`). Commit the vendored files as-is — modifications come in later steps.

**Test:** `cmake --build build --target hermes-node` succeeds and produces a binary. Running it prints a usage message or exits 0.

### Step 2: Vendor libuv [cmake] [depends: 1]

Vendor libuv under `external/libuv/` following the project vendoring pattern:

```
external/libuv/
├── README.md           # provenance: upstream URL, git hash, any local patches
├── CMakeLists.txt      # wrapper that adds the vendored source to the build
└── libuv/              # unmodified vendored libuv source tree
```

- `external/libuv/CMakeLists.txt` calls `add_subdirectory(libuv)` with appropriate options (e.g., disable libuv tests)
- Top-level `CMakeLists.txt` adds `external/libuv`
- Libraries in `lib/` that need libuv link against `uv_a` (static libuv)

**Test:** Add a GTest unit test (`unittests/UvIntegrationTest.cpp`) that calls `uv_version()` and asserts the returned version is > 0. Run it:
```bash
cmake --build build --target NodeCompatTests && build/unittests/NodeCompatTests
```

### Step 3: Implement libuv-backed event loop adapter [native] [depends: 2]

Implement the `hermes_napi_event_loop` interface (defined in `hermes_napi.h:269-300`) backed by libuv.

- `post_work` → submit to libuv threadpool via `uv_queue_work`; on completion, call `complete` from the main loop via `uv_async_send`
- `cancel_work` → `uv_cancel`
- `post_task` → use `uv_async_t` to schedule a callback on the main thread
- `data` → pointer to `uv_loop_t`

The adapter owns the `uv_loop_t` and provides start/stop/run methods.

**Files:** `include/hermes/node-compat/event-loop/uv_event_loop.h`, `lib/event-loop/uv_event_loop.cpp`

**Test:** GTest unit test (`UvEventLoopTest.cpp`) that:
1. Creates the adapter, posts async work (a function that sets a flag), runs `uv_run`, asserts the flag was set.
2. Posts a task via `post_task`, runs `uv_run`, asserts the task callback fired on the main thread.
3. Posts work then cancels it before execution, asserts the complete callback receives `napi_cancelled`.

---

## B. Module Loading System

### Step 4: Implement primordials thin shim [js]

Create `libjs/primordials.js` that builds and exports a `primordials` object matching Node's expected shape.

Node's internal modules destructure from primordials like:
```js
const { ArrayPrototypePush, SafeMap, StringPrototypeSlice } = primordials;
```

The shim must provide:
- `FooPrototypeBar` → `Function.prototype.call.bind(Foo.prototype.bar)` for every method on every built-in prototype (Array, String, Object, Number, Boolean, RegExp, Map, Set, WeakMap, WeakSet, Date, Symbol, Promise, Error, TypedArrays, etc.)
- `FooBar` static methods → `Foo.bar` (e.g., `ArrayIsArray` → `Array.isArray`, `ObjectKeys` → `Object.keys`, `ObjectDefineProperty` → `Object.defineProperty`)
- `Safe*` variants → just the original (`SafeMap` → `Map`, `SafeSet` → `Set`, `SafeWeakMap` → `WeakMap`, `SafeWeakRef` → `WeakRef`, `SafeArrayIterator` → Array[Symbol.iterator], `SafeStringIterator`, etc.)
- `FooPrototypeGetBar` getters → bound getter functions
- `uncurryThis` → `fn => Function.prototype.call.bind(fn)`

Strategy: Use `Object.getOwnPropertyDescriptors` to enumerate all built-in prototypes programmatically. Node's actual primordials.js (`lib/internal/per_context/primordials.js`) can serve as the reference for which names are expected.

**Test:** The primordials file should be self-testable with the stock Hermes CLI (`hermes test-primordials.js`), no module loader needed. The test file executes `primordials.js` via `load()` or by concatenating it with assertions:
```js
// test-primordials.js — run with: hermes test-primordials.js
// primordials.js sets globalThis.primordials = { ... }
load('libjs/primordials.js');
var p = globalThis.primordials;

// Prototype methods work as uncurried functions
assert(p.ArrayPrototypePush([1,2], 3) === 3);
assert(p.StringPrototypeSlice('hello', 1) === 'ello');
assert(p.ObjectKeys({a:1, b:2}).length === 2);
// Safe variants are the originals
assert(p.SafeMap === Map);
assert(p.SafeSet === Set);
// Static methods
assert(p.ArrayIsArray([]) === true);
assert(p.NumberIsNaN(NaN) === true);

function assert(cond) { if (!cond) throw new Error('assertion failed'); }
print('PASS: primordials shim');
```
If Hermes doesn't support `load()`, concatenate the files: `cat primordials.js test-primordials.js | hermes`.

### Step 5: Implement internalBinding registry [native] [depends: 1]

Create a C++ registry that maps binding names (strings) to initialization functions.

- Define a `BindingRegistry` class that stores name → `napi_addon_register_func` mappings
- Provide `registerBinding(name, initFunc)` and `getBinding(env, name)` methods
- `getBinding` calls the init function the first time (lazy init), caches the resulting `napi_value` object
- Expose the registry to JavaScript as the `internalBinding` function via `napi_create_function`

Pattern for each ported binding:
```cpp
// In each binding file:
napi_value InitConstants(napi_env env, napi_value exports);

// Registration:
registry.registerBinding("constants", InitConstants);
```

**Files:** `include/hermes/node-compat/binding-registry/binding_registry.h`, `lib/binding-registry/binding_registry.cpp`

**Test:** GTest unit test that:
1. Creates a BindingRegistry, registers a dummy binding that sets `exports.testValue = 42`.
2. Calls `getBinding(env, "dummy")`, asserts the returned object has `testValue === 42`.
3. Calls `getBinding(env, "dummy")` again, asserts it returns the same cached object.
4. Calls `getBinding(env, "nonexistent")`, asserts it throws/returns an error.

### Step 6: Implement internal module loader [native] [js] [depends: 5]

Build the module loader that loads Node's `lib/*.js` files from disk and provides them to each other via `require()`.

**C++ side:**
- Resolve the path to `libjs-node/` (vendored Node JS files) — either relative to the executable or via a `--node-lib-path` override
- Provide a `loadModuleSource(name)` function that reads `libjs-node/{name}.js` from disk
- Wrap each module source in the standard Node module wrapper:
  ```js
  (function(exports, require, module, __filename, __dirname, primordials, internalBinding) {
    // ... module source ...
  });
  ```
- Compile and call the wrapper using `napi_run_script` (or Hermes's script evaluation)

**JS side (loader.js):**
- Maintain a module cache (map from module ID to exports)
- Resolve `require('internal/foo')` → `libjs-node/internal/foo.js`
- Resolve `require('foo')` → `libjs-node/foo.js` (for public modules like `path`, `events`)
- Handle circular dependencies (set `module.exports` before executing, same as Node's CJS)
- Inject `primordials` and `internalBinding` into each module's wrapper

**Files:** `include/hermes/node-compat/module-loader/module_loader.h`, `lib/module-loader/module_loader.cpp`, `libjs/loader.js`

**Test:** Create two test JS files:
- `test_module_a.js`: `exports.name = 'a'; exports.b = require('test_module_b');`
- `test_module_b.js`: `exports.name = 'b';`

Integration test (GTest or script) that:
1. Loads `test_module_a`, asserts `a.name === 'a'` and `a.b.name === 'b'`.
2. Loads `test_module_a` again, asserts it returns the cached copy (same object).
3. Tests circular require: `circ_a.js` requires `circ_b.js` which requires `circ_a.js` — both load without hanging.

### Step 7: Implement process object (basic properties) [native] [depends: 5]

Create the `process` global object with non-I/O properties. Many modules check `process.platform`, `process.env`, etc. at load time.

**Native properties:**
- `process.pid` → `getpid()`
- `process.ppid` → `getppid()`
- `process.platform` → `'linux'`, `'darwin'`, `'win32'`
- `process.arch` → `'x64'`, `'arm64'`, etc.
- `process.version` → version string (e.g., `'v0.1.0-hermes'`)
- `process.versions` → `{ hermes: '0.12.0', uv: uv_version_string(), ... }`
- `process.argv` → from command-line args
- `process.execPath` → from `/proc/self/exe` or equivalent
- `process.title` → get/set via `uv_get_process_title` / `uv_set_process_title`

**process.env proxy:**
- Use a JS `Proxy` (or define property accessors) so that `process.env.FOO` calls `getenv("FOO")` and `process.env.FOO = "bar"` calls `setenv("FOO", "bar")`
- Support enumeration (iterate `environ`)

**Native methods (sync, no event loop needed):**
- `process.cwd()` → `uv_cwd()`
- `process.chdir(dir)` → `uv_chdir()`
- `process.hrtime()` / `process.hrtime.bigint()` → `uv_hrtime()`
- `process.cpuUsage()` → `uv_getrusage()`
- `process.memoryUsage()` → `uv_resident_set_memory()` + Hermes heap stats
- `process.uptime()` → time since start
- `process.exit(code)` → cleanup + `exit()`
- `process.abort()` → `abort()`
- `process.umask([mask])` → `umask()`

**Deferred (need event loop):** `process.nextTick`, `process.stdout/stderr/stdin`, signal handling, `process.on('exit')`.

**Files:** `lib/process/node_process.cpp`

**Test:** GTest unit test that creates a napi_env, calls process initialization, then verifies from JS:
```js
assert(typeof process.pid === 'number' && process.pid > 0);
assert(['linux','darwin','win32'].includes(process.platform));
assert(typeof process.arch === 'string');
assert(typeof process.cwd() === 'string');
process.env.HERMES_TEST_VAR = 'hello';
assert(process.env.HERMES_TEST_VAR === 'hello');
delete process.env.HERMES_TEST_VAR;
assert(process.env.HERMES_TEST_VAR === undefined);
const t1 = process.hrtime.bigint();
const t2 = process.hrtime.bigint();
assert(t2 > t1);
```

### Step 8: Implement bootstrap sequence [native] [js] [depends: 3, 4, 6, 7]

Create the main entry point that ties everything together.

Sequence:
1. Parse command-line arguments (script path, optional `--node-lib-path` override)
2. Create Hermes runtime (with microtask queue enabled)
3. Create libuv event loop adapter (step 3)
4. Create `napi_env` via `hermes_napi_create_env(runtime, &eventLoop)`
5. Register all native bindings in the registry (step 5)
6. Load and execute `libjs/primordials.js` (step 4) — set `primordials` global
7. Create and set `process` global (step 7)
8. Initialize the module loader (step 6)
9. Load user script (or REPL)
10. Run `uv_run(loop, UV_RUN_DEFAULT)` until done
11. Cleanup

**Files:** `tools/hermes-node/hermes-node.cpp`

**Test:** Run:
```bash
hermes-node test-boot.js
```
where `test-boot.js` contains:
```js
// Minimal bootstrap test — no Node modules loaded yet, just the scaffolding
console.log('platform:', process.platform);
console.log('pid:', process.pid);
console.log('cwd:', process.cwd());
```
Passes if all three lines print correct values and process exits 0.

---

## C. Foundational Native Bindings

These bindings are required by the bootstrap modules (`internal/errors`, `internal/validators`, `internal/util`). Each is an atomic porting task.

### Step 9: Port `constants` binding [native] [depends: 5]

**Source:** `src/node_constants.cc`
**Binding name:** `internalBinding('constants')`

Provides a large object of platform constants organized by category:
- `constants.os.signals` — signal numbers (`SIGINT`, `SIGTERM`, etc.)
- `constants.os.errno` — errno values (`ENOENT`, `EACCES`, etc.)
- `constants.fs` — file open flags (`O_RDONLY`, `O_WRONLY`, `O_CREAT`), file type flags (`S_IFREG`, `S_IFDIR`), access flags (`F_OK`, `R_OK`, `W_OK`, `X_OK`)
- `constants.crypto` — OpenSSL constants (can be empty/stubbed for now)
- `constants.zlib` — zlib constants (can be empty/stubbed for now)
- `constants.trace` — trace constants (can be empty/stubbed for now)

**Approach:** Purely mechanical — create nested objects, set numeric properties from C `#define` values. No behavioral logic. Use `napi_define_properties` or individual `napi_set_named_property` calls.

**Test:** JS test via `hermes-node`:
```js
const c = internalBinding('constants');
assert(typeof c.os.signals.SIGINT === 'number');
assert(c.os.signals.SIGINT === 2);
assert(typeof c.fs.O_RDONLY === 'number');
assert(c.fs.O_RDONLY === 0);
assert(typeof c.fs.S_IFREG === 'number');
assert(typeof c.os.errno.ENOENT === 'number');
```

### Step 10: Port `types` binding [native] [depends: 5]

**Source:** `src/node_types.cc`
**Binding name:** `internalBinding('types')`

Provides type-checking functions used by `internal/util/types.js`:
- `isArrayBuffer`, `isArrayBufferView`, `isAsyncFunction`, `isDataView`
- `isDate`, `isExternal`, `isMap`, `isMapIterator`, `isModuleNamespaceObject`
- `isNativeError`, `isPromise`, `isRegExp`, `isSet`, `isSetIterator`
- `isSharedArrayBuffer`, `isTypedArray`, `isUint8Array`, `isAnyArrayBuffer`
- `isGeneratorFunction`, `isGeneratorObject`, `isWeakMap`, `isWeakSet`
- `isBigIntObject`, `isBooleanObject`, `isNumberObject`, `isStringObject`, `isSymbolObject`
- `isBoxedPrimitive`, `isProxy`

**Node-API mapping:**
- Most map directly: `napi_is_typedarray`, `napi_is_arraybuffer`, `napi_is_date`, `napi_is_promise`, `napi_is_dataview`, `napi_is_buffer`
- Some require `napi_typeof` checks or `napi_instanceof`
- A few V8-specifics (`isProxy`, `isModuleNamespaceObject`) may need Hermes-specific solutions or stubs

**Test:** JS test via `hermes-node`:
```js
const t = internalBinding('types');
assert(t.isArrayBuffer(new ArrayBuffer(8)) === true);
assert(t.isArrayBuffer({}) === false);
assert(t.isTypedArray(new Uint8Array(4)) === true);
assert(t.isDate(new Date()) === true);
assert(t.isDate({}) === false);
assert(t.isPromise(Promise.resolve()) === true);
assert(t.isRegExp(/abc/) === true);
assert(t.isMap(new Map()) === true);
assert(t.isSet(new Set()) === true);
assert(t.isNativeError(new TypeError()) === true);
assert(t.isNativeError({}) === false);
```

### Step 11: Port `util` binding [native] [depends: 5]

**Source:** `src/node_util.cc`
**Binding name:** `internalBinding('util')`

Provides:
- **Type checks** (duplicating some from `types`, used directly by `internal/util.js`)
- `getPromiseDetails(promise)` → returns `[state, result]` (V8-specific; **stub** to return `undefined`)
- `getProxyDetails(proxy)` → returns `[target, handler]` (V8-specific; **stub** to return `undefined`)
- `previewEntries(obj)` → get entries from Map/Set iterators (V8-specific; **stub**)
- `getOwnNonIndexProperties(obj, filter)` → used by `util.inspect` (implement via `napi_get_all_property_names` with filtering)
- `sleep(msec)` → `uv_sleep()` or `usleep()`
- `guessHandleType(fd)` → `uv_guess_handle(fd)`
- `defineLazyProperties(target, id, keys)` → defines lazy getters
- `privateSymbols` → object with private symbols used internally
- `constructSharedArrayBuffer` → constructor wrapper

**Test:** JS test via `hermes-node`:
```js
const u = internalBinding('util');
// sleep doesn't throw
u.sleep(1);
// guessHandleType returns a string for known fds
assert(['TTY','FILE','PIPE','UNKNOWN'].includes(u.guessHandleType(1)));
// getOwnNonIndexProperties returns array
const props = u.getOwnNonIndexProperties({a:1, b:2}, 0);
assert(Array.isArray(props));
// privateSymbols is an object
assert(typeof u.privateSymbols === 'object');
// Stubs don't crash
u.getPromiseDetails(Promise.resolve(42));
u.getProxyDetails({});
```

### Step 12: Port `string_decoder` binding [native] [depends: 5]

**Source:** `src/string_decoder.cc`
**Binding name:** `internalBinding('string_decoder')`

Minimal: provides the `encodings` object (list of supported encoding names) and native string decoding functions.

**Test:** JS test via `hermes-node`:
```js
const sd = internalBinding('string_decoder');
// Has encoding-related exports
assert(typeof sd !== 'undefined');
// Module loads without error — sufficient for bootstrap
```

### Step 13: Port `errors` binding [native] [depends: 5]

**Source:** `src/node_errors.cc`
**Binding name:** `internalBinding('errors')`

Provides:
- `triggerUncaughtException(error, fromPromise)` — calls the uncaught exception handler
- Error system initialization helpers

**For now:** Implement `triggerUncaughtException` to print the error and call `process.exit(1)`.

**Test:** JS test via `hermes-node`:
```js
const e = internalBinding('errors');
assert(typeof e.triggerUncaughtException === 'function');
// Don't actually call it — would exit the process
```

### Step 14: Port `config` binding [native] [depends: 5]

**Source:** `src/node_config.cc`
**Binding name:** `internalBinding('config')`

Provides compile-time configuration flags:
- `hasOpenSSL`, `hasInspector`, `hasIntl`, `hasSmallICU`, `hasTracing`
- Various boolean feature flags

**Approach:** Return an object with all flags set to reflect what's available in the Hermes build (most will be `false` initially).

**Test:** JS test via `hermes-node`:
```js
const cfg = internalBinding('config');
assert(typeof cfg.hasOpenSSL === 'boolean');
assert(typeof cfg.hasInspector === 'boolean');
// Most should be false for initial Hermes build
assert(cfg.hasInspector === false);
```

### Step 15: Port `symbols` binding [native] [depends: 5]

**Source:** `src/node_symbols.cc`
**Binding name:** `internalBinding('symbols')`

Creates well-known internal symbols used across the codebase:
- `owner_symbol`, `onread_symbol`, `trigger_async_id_symbol`, `async_id_symbol`
- Various other private symbols

**Approach:** Create unique symbols via `napi_create_symbol` for each.

**Test:** JS test via `hermes-node`:
```js
const s = internalBinding('symbols');
assert(typeof s.owner_symbol === 'symbol');
assert(typeof s.async_id_symbol === 'symbol');
// Each symbol is unique
assert(s.owner_symbol !== s.async_id_symbol);
```

### Step 16: Implement `internal/options` shim [js] [depends: 6]

`internal/util.js` requires `require('internal/options')` which provides `getOptionValue()`. In Node this reads from a C++ options parser.

Create a JavaScript shim that:
- Returns sensible defaults for commonly queried options
- Backs `getOptionValue('--some-flag')` with a simple map
- Can be populated from command-line args at bootstrap time

Alternatively, port the `options` native binding minimally.

**Files:** `libjs/shims/internal/options.js` (or `lib/bindings/node_options.cpp`)

**Test:** JS test via `hermes-node`:
```js
const { getOptionValue } = require('internal/options');
assert(typeof getOptionValue === 'function');
// Returns undefined or a default for unknown options
const val = getOptionValue('--nonexistent');
assert(val === undefined || val === false);
```

---

## D. Bootstrap Verification

### Step 17: Verify bootstrap modules load [verify] [depends: 8, 9, 10, 11, 12, 13, 14, 15, 16]

Run `hermes-node` with a test script that:

```js
// test-bootstrap.js
const errors = require('internal/errors');
assert(typeof errors.codes.ERR_INVALID_ARG_TYPE === 'function');

const validators = require('internal/validators');
assert(typeof validators.validateString === 'function');

const util = require('internal/util');
assert(typeof util.deprecate === 'function');

const assert_ = require('internal/assert');
assert(typeof assert_ === 'function');

console.log('PASS: all bootstrap modules loaded');
```

Debug and fix any load failures. Common issues:
- Missing primordial names in the shim
- Missing or incorrectly shaped binding properties
- `Error.captureStackTrace` (V8-ism) — needs polyfill in primordials or bootstrap
- Lazy requires pulling in unexpected modules

**Test:** Script exits 0 and prints `PASS`. All four modules are callable.

---

## E. Core Module Native Bindings

### Step 18: Port `buffer` binding [native] [depends: 5]

**Source:** `src/node_buffer.cc` (~800 lines relevant)
**Binding name:** `internalBinding('buffer')`

Functions to port:
- `byteLengthUtf8(string)` — UTF-8 byte length of a JS string
- `compare(buf1, buf2)` — `memcmp` wrapper
- `compareOffset(buf1, buf2, targetStart, targetEnd, sourceStart, sourceEnd)`
- `copy(source, target, targetStart, sourceStart, sourceEnd)`
- `fill(buf, val, start, end, encoding)` — fill buffer with value
- `indexOfBuffer(buf, val, byteOffset, isForward)` — search for buffer in buffer
- `indexOfNumber(buf, val, byteOffset, isForward)` — search for byte value
- `indexOfString(buf, val, byteOffset, encoding, isForward)` — search for string
- `swap16(buf)`, `swap32(buf)`, `swap64(buf)` — byte swap in place
- `isAscii(buf)`, `isUtf8(buf)` — validation
- `atob(input)`, `btoa(input)` — base64 encode/decode
- `kMaxLength`, `kStringMaxLength` — constants

**Approach:** All functions operate on `ArrayBuffer`/`TypedArray` backing stores. Use `napi_get_typedarray_info` / `napi_get_arraybuffer_info` to get data pointers, then do pure C memory operations.

**Test:** GTest unit test and JS integration test:
```js
const b = internalBinding('buffer');
// byteLengthUtf8
assert(b.byteLengthUtf8('hello') === 5);
assert(b.byteLengthUtf8('héllo') === 6); // é is 2 bytes
// compare
const a1 = new Uint8Array([1,2,3]);
const a2 = new Uint8Array([1,2,4]);
assert(b.compare(a1, a2) < 0);
assert(b.compare(a2, a1) > 0);
assert(b.compare(a1, a1) === 0);
// isUtf8
assert(b.isUtf8(new Uint8Array([0x68, 0x65, 0x6c, 0x6c, 0x6f])) === true);
assert(b.isUtf8(new Uint8Array([0xff, 0xfe])) === false);
// isAscii
assert(b.isAscii(new Uint8Array([0x41, 0x42])) === true);
assert(b.isAscii(new Uint8Array([0x80])) === false);
// swap16
const s = new Uint8Array([1,2,3,4]);
b.swap16(s);
assert(s[0] === 2 && s[1] === 1 && s[2] === 4 && s[3] === 3);
// constants
assert(typeof b.kMaxLength === 'number' && b.kMaxLength > 0);
```

### Step 19: Port `encoding_binding` [native] [depends: 5]

**Source:** `src/encoding_binding.cc`
**Binding name:** `internalBinding('encoding_binding')`

Provides `TextEncoder`/`TextDecoder` native support and encoding conversion functions.

**Test:** JS test via `hermes-node`:
```js
const enc = internalBinding('encoding_binding');
assert(typeof enc !== 'undefined');
// Verify encoding functions exist (exact API depends on what Node's JS layer expects)
```

### Step 20: Port `async_wrap` binding (stub) [native] [depends: 5]

**Source:** `src/async_wrap.cc`
**Binding name:** `internalBinding('async_wrap')`

Many modules reference `async_wrap` for async resource tracking. Since we're stubbing async hooks:

- Provide `AsyncWrap` constants (`Providers` enum values)
- `asyncIdStackSize()` → return 0
- `pushAsyncContext` / `popAsyncContext` → no-ops
- `executionAsyncId()` → return 0
- `triggerAsyncId()` → return 0
- `registerDestroyHook` → no-op

**Test:** JS test via `hermes-node`:
```js
const aw = internalBinding('async_wrap');
assert(typeof aw.Providers === 'object');
assert(typeof aw.executionAsyncId === 'function');
assert(aw.executionAsyncId() === 0);
assert(aw.asyncIdStackSize() === 0);
// no-ops don't throw
aw.pushAsyncContext(1, 0, {});
aw.popAsyncContext(1);
```

### Step 21: Implement `process.nextTick` [native] [js] [depends: 3, 7]

**Binding name:** Part of `internalBinding('task_queue')`
**Source:** `src/node_task_queue.cc`

`process.nextTick` is a JS-level queue, not backed by libuv timers.

- Maintain a JS array as the tick queue
- After each libuv callback returns to JS, drain the tick queue
- After microtask draining, drain nextTick queue
- Order: libuv callback → microtasks → nextTick → return to loop

The native binding provides `setTickCallback` which registers the JS drain function, and `runMicrotasks` to explicitly drain the microtask queue.

**Integration point:** After each callback from libuv enters JS and completes, call `hermes::Runtime::drainJobs()` (microtasks) then the nextTick drain function.

**Test:** JS test via `hermes-node` (requires event loop running):
```js
let order = [];
process.nextTick(() => {
  order.push('tick1');
  process.nextTick(() => order.push('tick3')); // nested tick
});
process.nextTick(() => order.push('tick2'));

setTimeout(() => {
  assert(order.join(',') === 'tick1,tick2,tick3');
  console.log('PASS: nextTick ordering correct');
}, 50);
```

### Step 22: Implement timers binding [native] [depends: 3, 5]

**Source:** `src/timers.cc`
**Binding name:** `internalBinding('timers')`

Node's JS timer code (`lib/internal/timers.js`) is sophisticated — it batches timers with the same delay into a shared `uv_timer_t`. The native binding is small:

- `scheduleTimer(msecs)` → `uv_timer_start` with the given delay
- `toggleTimerRef(ref)` → `uv_ref`/`uv_unref` the timer handle
- `immediateInfo` → shared `Int32Array` for immediate queue state
- `toggleImmediateRef(ref)` → ref/unref the immediate check handle

Also need `setImmediate` support:
- Use `uv_check_t` or `uv_idle_t` handle to drain the immediate queue at the right point in the event loop

**Test:** JS test via `hermes-node`:
```js
let results = [];

setTimeout(() => results.push('timeout50'), 50);
setTimeout(() => results.push('timeout10'), 10);
setImmediate(() => results.push('immediate'));
setInterval(() => {
  results.push('interval');
  if (results.filter(r => r === 'interval').length >= 2) {
    clearInterval(/* this interval */);
  }
}, 20);

setTimeout(() => {
  assert(results.includes('immediate'));
  assert(results.includes('timeout10'));
  assert(results.includes('timeout50'));
  assert(results.filter(r => r === 'interval').length >= 2);
  // immediate fires before timers in same loop iteration
  assert(results.indexOf('immediate') < results.indexOf('timeout10'));
  console.log('PASS: timers work');
}, 200);
```

### Step 23: Implement process.stdout/stderr (minimal) [native] [js] [depends: 7, 21]

For initial implementation, provide synchronous write streams for stdout/stderr:

- Create writable stream-like objects for fd 1 and fd 2
- `process.stdout.write(data)` → synchronous `uv_fs_write` to fd 1
- `process.stderr.write(data)` → synchronous `uv_fs_write` to fd 2
- Support `.isTTY` property via `uv_guess_handle`

This unblocks `console.log` working through Node's `console` module rather than Hermes's built-in.

`process.stdin` can be deferred (more complex — requires readable stream + event loop).

**Test:** Run `hermes-node` with:
```js
process.stdout.write('stdout-test\n');
process.stderr.write('stderr-test\n');
console.log('console-test');
assert(typeof process.stdout.isTTY === 'boolean');
```
Capture stdout and stderr separately, verify `stdout-test` and `console-test` appear on stdout, `stderr-test` appears on stderr.

---

## F. Core Module Verification

### Step 24: Verify core modules load and work [verify] [depends: 17, 18, 19, 20, 21, 22, 23]

Test script:

```js
const events = require('events');
const path = require('path');
const buffer = require('buffer');
const util = require('util');

// EventEmitter
const ee = new events.EventEmitter();
let emitted = false;
ee.on('test', (msg) => { emitted = true; assert(msg === 'hello'); });
ee.emit('test', 'hello');
assert(emitted);

// Path
assert(path.join('/foo', 'bar', 'baz') === '/foo/bar/baz');
assert(path.dirname('/foo/bar') === '/foo');
assert(path.extname('file.txt') === '.txt');

// Buffer
const buf = buffer.Buffer.from('hello world');
assert(buf.toString() === 'hello world');
assert(buf.length === 11);
assert(buffer.Buffer.isBuffer(buf) === true);

// util.format
assert(util.format('%s %d', 'test', 42) === 'test 42');

// process.nextTick
let ticked = false;
process.nextTick(() => { ticked = true; });

// Timers
let timedOut = false;
setTimeout(() => {
  assert(ticked);
  timedOut = true;
}, 10);

setTimeout(() => {
  assert(timedOut);
  console.log('PASS: all core modules working');
}, 100);
```

**Test:** Script exits 0 and prints `PASS`.

---

## G. Streams

### Step 25: Port `stream_wrap` binding (minimal) [native] [depends: 5, 3]

**Source:** `src/stream_wrap.cc`, `src/stream_base.cc`, `src/handle_wrap.cc`
**Binding names:** `internalBinding('stream_wrap')`

For Phase 4 (fs), we need the stream base infrastructure that `fs.createReadStream`/`fs.createWriteStream` build on. However, the core `stream` module (`lib/stream.js` and `lib/internal/streams/*.js`) is mostly pure JavaScript.

What's needed minimally:
- `WriteWrap` and `ShutdownWrap` classes (JS objects with native pointers)
- These are used by native stream implementations (TCP, pipes) but not by fs streams

**For now:** Provide a minimal stub so the stream module loads. The full implementation is needed for Phase 5 (networking).

**Test:** JS test via `hermes-node`:
```js
const sw = internalBinding('stream_wrap');
assert(typeof sw !== 'undefined');
// The module loads without error — sufficient for now
```

### Step 26: Verify streams work [verify] [depends: 24, 25]

```js
const { Readable, Writable, Transform, pipeline } = require('stream');

// Transform stream test
let transformed = '';
const upper = new Transform({
  transform(chunk, encoding, callback) {
    callback(null, chunk.toString().toUpperCase());
  }
});
upper.on('data', (chunk) => { transformed += chunk.toString(); });
upper.on('end', () => {
  assert(transformed === 'HELLO');
});
upper.write('hello');
upper.end();

// Pipeline test
const readable = Readable.from(['hello', ' ', 'world']);
const chunks = [];
const writable = new Writable({
  write(chunk, enc, cb) { chunks.push(chunk.toString()); cb(); }
});
pipeline(readable, writable, (err) => {
  assert(!err);
  assert(chunks.join('') === 'hello world');
  console.log('PASS: streams work');
});
```

**Test:** Script exits 0 and prints `PASS`.

---

## H. File System

### Step 27: Port `fs` binding — synchronous operations [native] [depends: 2, 5, 9]

**Source:** `src/node_file.cc` (~2500 lines)
**Binding name:** `internalBinding('fs')`

Port the synchronous variants first (simpler — no callback/promise machinery):

| Function | libuv call |
|---|---|
| `open(path, flags, mode)` | `uv_fs_open` (sync) |
| `close(fd)` | `uv_fs_close` (sync) |
| `read(fd, buffer, offset, length, position)` | `uv_fs_read` (sync) |
| `write(fd, buffer, offset, length, position)` | `uv_fs_write` (sync) |
| `stat(path)` / `lstat(path)` / `fstat(fd)` | `uv_fs_stat` / `uv_fs_lstat` / `uv_fs_fstat` (sync) |
| `rename(oldPath, newPath)` | `uv_fs_rename` (sync) |
| `unlink(path)` | `uv_fs_unlink` (sync) |
| `mkdir(path, mode)` | `uv_fs_mkdir` (sync) |
| `rmdir(path)` | `uv_fs_rmdir` (sync) |
| `readdir(path)` | `uv_fs_scandir` (sync) |
| `chmod(path, mode)` / `fchmod(fd, mode)` | `uv_fs_chmod` / `uv_fs_fchmod` (sync) |
| `chown(path, uid, gid)` / `fchown(fd, uid, gid)` | `uv_fs_chown` / `uv_fs_fchown` (sync) |
| `link(src, dst)` / `symlink(target, path, type)` | `uv_fs_link` / `uv_fs_symlink` (sync) |
| `readlink(path)` / `realpath(path)` | `uv_fs_readlink` / `uv_fs_realpath` (sync) |
| `ftruncate(fd, length)` | `uv_fs_ftruncate` (sync) |
| `utimes` / `futimes` / `lutimes` | `uv_fs_utime` / `uv_fs_futime` / `uv_fs_lutime` (sync) |
| `mkdtemp(prefix)` | `uv_fs_mkdtemp` (sync) |
| `copyfile(src, dst, flags)` | `uv_fs_copyfile` (sync) |
| `access(path, mode)` | `uv_fs_access` (sync) |

**Key implementation detail — Stats population:**
Node uses a shared `Float64Array` ("stats array buffer") to pass stat results from C++ to JS. The native side writes `uv_stat_t` fields into the array; the JS side reads them out. Replicate this pattern:
1. Allocate a `Float64Array` with enough slots for all stat fields
2. On stat completion, write fields into the typed array
3. The JS `Stats` constructor reads from the array

**Error handling:**
- On failure, each `uv_fs_*` returns a negative error code
- Convert to a JS error using the `UVException` pattern: set `code`, `errno`, `syscall`, `path`, `message` properties
- Match Node's exact error shapes so `lib/internal/fs/utils.js` error handling works

**Files:** `lib/bindings/node_file.cpp`

**Test:** JS test via `hermes-node`:
```js
const fs = require('fs');
const path = require('path');

// mkdtemp + writeFile + readFile + stat + unlink + rmdir
const tmpDir = fs.mkdtempSync('/tmp/hermes-test-');
const testFile = path.join(tmpDir, 'test.txt');

fs.writeFileSync(testFile, 'hello world');
assert(fs.readFileSync(testFile, 'utf8') === 'hello world');

const stat = fs.statSync(testFile);
assert(stat.isFile() === true);
assert(stat.isDirectory() === false);
assert(stat.size === 11);

// rename
const newFile = path.join(tmpDir, 'renamed.txt');
fs.renameSync(testFile, newFile);
assert(fs.readFileSync(newFile, 'utf8') === 'hello world');

// mkdir + readdir
fs.mkdirSync(path.join(tmpDir, 'subdir'));
const entries = fs.readdirSync(tmpDir);
assert(entries.includes('renamed.txt'));
assert(entries.includes('subdir'));

// error handling: ENOENT
let threw = false;
try { fs.readFileSync('/nonexistent/path'); } catch(e) {
  threw = true;
  assert(e.code === 'ENOENT');
  assert(e.syscall === 'open');
}
assert(threw);

// cleanup
fs.unlinkSync(newFile);
fs.rmdirSync(path.join(tmpDir, 'subdir'));
fs.rmdirSync(tmpDir);

console.log('PASS: sync fs operations');
```

### Step 28: Port `fs` binding — async operations [native] [depends: 3, 27]

Extend the fs binding with async variants of all functions from step 27.

**Architecture:**
- Each async operation creates a request object (`FSReqCallback` or `FSReqPromise`)
- The request holds: `uv_fs_t`, a reference to the JS callback/promise, and any buffers
- Call `uv_fs_*(loop, &req, ..., callback)` with async callback
- On completion (libuv fires callback on main thread), unwrap the JS callback, construct result, call it
- Clean up the request

**Node-API implementation:**
- Use `napi_wrap` to associate a C struct with a JS request object
- The C struct contains `uv_fs_t` and `napi_ref` to the callback
- On completion callback: `napi_get_reference_value` to recover the callback, call it with `napi_call_function`
- Use `napi_ref`/`napi_unref` to prevent GC of buffers during async operations

**Two call patterns:**
- Callback API: `fs.readFile(path, cb)` → creates `FSReqCallback`, passes callback directly
- Promise API: `fs.promises.readFile(path)` → creates `FSReqPromise`, wraps result in a Promise

**Test:** JS test via `hermes-node`:
```js
const fs = require('fs');
const fsp = require('fs/promises');

let passed = 0;
const expect = 3;

// Callback API
const tmpDir = fs.mkdtempSync('/tmp/hermes-async-');
const testFile = tmpDir + '/test.txt';
fs.writeFile(testFile, 'async-hello', (err) => {
  assert(!err);
  fs.readFile(testFile, 'utf8', (err, data) => {
    assert(!err);
    assert(data === 'async-hello');
    passed++;
    checkDone();
  });
});

// Callback stat
fs.stat(testFile, (err, stat) => {
  // May race with writeFile — that's OK, test the callback shape
  if (!err) {
    assert(typeof stat.size === 'number');
  }
  passed++;
  checkDone();
});

// Promise API
(async () => {
  const tmpDir2 = await fsp.mkdtemp('/tmp/hermes-promise-');
  await fsp.writeFile(tmpDir2 + '/p.txt', 'promise-hello');
  const data = await fsp.readFile(tmpDir2 + '/p.txt', 'utf8');
  assert(data === 'promise-hello');
  await fsp.rm(tmpDir2, { recursive: true });
  passed++;
  checkDone();
})();

function checkDone() {
  if (passed === expect) {
    // cleanup
    fs.rmSync(tmpDir, { recursive: true });
    console.log('PASS: async fs operations');
  }
}
```

### Step 29: Port `fs_dir` binding [native] [depends: 27]

**Source:** `src/node_dir.cc`
**Binding name:** `internalBinding('fs_dir')`

Provides `opendir`, `readdir` (the iterator-based API), and `closedir` for the `Dir`/`Dirent` classes.

- `uv_fs_opendir` → returns a `uv_dir_t`
- `uv_fs_readdir` → reads entries from the dir handle
- `uv_fs_closedir` → closes the dir handle

**Test:** JS test via `hermes-node`:
```js
const fs = require('fs');
const tmpDir = fs.mkdtempSync('/tmp/hermes-dir-');
fs.writeFileSync(tmpDir + '/a.txt', 'a');
fs.writeFileSync(tmpDir + '/b.txt', 'b');
fs.mkdirSync(tmpDir + '/sub');

const dir = fs.opendirSync(tmpDir);
const names = [];
let dirent;
while ((dirent = dir.readSync()) !== null) {
  names.push(dirent.name);
  if (dirent.name === 'sub') assert(dirent.isDirectory());
  else assert(dirent.isFile());
}
dir.closeSync();

assert(names.sort().join(',') === 'a.txt,b.txt,sub');

// cleanup
fs.rmSync(tmpDir, { recursive: true });
console.log('PASS: fs_dir operations');
```

### Step 30: Port `fs_event_wrap` binding [native] [depends: 3, 5]

**Source:** `src/fs_event_wrap.cc`
**Binding name:** `internalBinding('fs_event_wrap')`

Wraps `uv_fs_event_t` for `fs.watch()`:
- `uv_fs_event_init(loop, handle)`
- `uv_fs_event_start(handle, callback, path, flags)`
- `uv_fs_event_stop(handle)`

Also needs `uv_fs_poll_t` for `fs.watchFile()`:
- `uv_fs_poll_init(loop, handle)`
- `uv_fs_poll_start(handle, callback, path, interval)`
- `uv_fs_poll_stop(handle)`

**Test:** JS test via `hermes-node`:
```js
const fs = require('fs');
const tmpDir = fs.mkdtempSync('/tmp/hermes-watch-');
const watchFile = tmpDir + '/watched.txt';
fs.writeFileSync(watchFile, 'initial');

let eventFired = false;
const watcher = fs.watch(tmpDir, (eventType, filename) => {
  eventFired = true;
  watcher.close();
});

// Trigger a change after a short delay
setTimeout(() => {
  fs.writeFileSync(watchFile, 'changed');
}, 50);

setTimeout(() => {
  assert(eventFired);
  fs.rmSync(tmpDir, { recursive: true });
  console.log('PASS: fs.watch works');
}, 500);
```

---

## I. File System Verification

### Step 31: Verify fs sync operations [verify] [depends: 27, 9]

(Covered by step 27's test. This step runs the full sync test suite.)

**Test:** Run step 27's test script. Additionally test edge cases:
```js
const fs = require('fs');
// access
fs.accessSync('/tmp', fs.constants.R_OK | fs.constants.W_OK);
// symlink + readlink
const tmp = fs.mkdtempSync('/tmp/hermes-sym-');
fs.writeFileSync(tmp + '/target', 'data');
fs.symlinkSync(tmp + '/target', tmp + '/link');
assert(fs.readlinkSync(tmp + '/link').endsWith('/target'));
assert(fs.lstatSync(tmp + '/link').isSymbolicLink());
assert(fs.statSync(tmp + '/link').isFile()); // follows symlink
fs.rmSync(tmp, { recursive: true });
console.log('PASS: fs sync edge cases');
```

### Step 32: Verify fs async operations [verify] [depends: 28, 29]

(Covered by step 28's test. This step adds promise API and directory tests.)

**Test:** Run step 28's test script plus:
```js
const fsp = require('fs/promises');
(async () => {
  const tmpDir = await fsp.mkdtemp('/tmp/hermes-async2-');
  await fsp.writeFile(tmpDir + '/a.txt', 'aaa');
  await fsp.writeFile(tmpDir + '/b.txt', 'bbb');

  const dir = await fsp.opendir(tmpDir);
  const names = [];
  for await (const dirent of dir) names.push(dirent.name);
  assert(names.sort().join(',') === 'a.txt,b.txt');

  await fsp.rm(tmpDir, { recursive: true });
  console.log('PASS: async fs + dir iteration');
})();
```

### Step 33: Run Node.js fs test subset [verify] [depends: 28, 29, 30]

Set up enough of the Node test harness (`test/common/index.js`) to run existing Node tests:

- `test/parallel/test-fs-read.js`
- `test/parallel/test-fs-write.js`
- `test/parallel/test-fs-stat.js`
- `test/parallel/test-fs-readfile.js`
- `test/parallel/test-fs-writefile.js`
- `test/parallel/test-fs-mkdir.js`
- `test/parallel/test-fs-readdir.js`
- `test/parallel/test-fs-promises.js`

Fix failures until a meaningful subset passes.

**Test:** At least 5 of the 8 listed Node.js test files pass without modification (or with only test-harness shim changes, not fs behavior changes). Track which tests pass in a results file.

---

## Dependency Graph (Summary)

```
1 (CMake target)
├── 2 (libuv) ── 3 (event loop adapter)
├── 5 (binding registry) ── 9-15 (foundational bindings)
│   ├── 10 (types)
│   ├── 11 (util)
│   ├── 12 (string_decoder)
│   ├── 13 (errors)
│   ├── 14 (config)
│   └── 15 (symbols)
├── 4 (primordials)
├── 6 (module loader) [depends: 5]
│   └── 16 (options shim)
├── 7 (process object) [depends: 5]
└── 8 (bootstrap) [depends: 3, 4, 6, 7]
    └── 17 (bootstrap verify) [depends: 8, 9-16]
        ├── 18 (buffer binding)
        ├── 19 (encoding_binding)
        ├── 20 (async_wrap stub)
        ├── 21 (nextTick) [depends: 3, 7]
        ├── 22 (timers) [depends: 3]
        └── 23 (stdout/stderr) [depends: 7, 21]
            └── 24 (core verify) [depends: 17-23]
                ├── 25 (stream_wrap minimal)
                │   └── 26 (stream verify)
                ├── 27 (fs sync) [depends: 2, 9]
                │   ├── 28 (fs async) [depends: 3]
                │   └── 29 (fs_dir)
                ├── 30 (fs_event_wrap) [depends: 3]
                └── 31-33 (fs verification)
```

## Modules NOT Ported (Out of Scope)

These are explicitly deferred beyond Phase 4:
- `net`, `dns`, `dgram` (networking — Phase 5)
- `http`, `https`, `tls` (HTTP — Phase 5)
- `child_process` (Phase 6)
- `crypto` (Phase 6)
- `zlib` (Phase 6)
- `worker_threads` (Phase 6, requires Hermes threading design)
- `inspector` (V8-specific)
- `vm` / `contextify` (V8-specific)
- `v8` module (V8-specific)
- `wasi`, `sea`, `permission`

## Known Risk Areas

1. **`Error.captureStackTrace`** — V8-ism used pervasively in `internal/errors.js`. Hermes doesn't have it. Needs a polyfill (set `.stack` from `new Error().stack`).

2. **`AbortController`/`AbortSignal`** — Used by `events.js` and others. Check Hermes support; may need polyfill.

3. **`WeakRef`/`FinalizationRegistry`** — Used in some modules. Hermes supports `WeakRef` but check `FinalizationRegistry`.

4. **Circular dependencies** — The module loader must handle circular `require()` (module.exports is set before execution, same as Node CJS).

5. **Lazy requires** — Many Node modules use `require()` inside functions (lazy loading). The module loader must work at any point, not just during bootstrap.

6. **`internal/options` depth** — `getOptionValue` is called extensively. Need to map which options are actually checked and provide sensible defaults.

7. **Missing OS module** — `os.tmpdir()` is used in fs tests. May need a minimal `os` binding (`internalBinding('os')`) for `tmpdir`, `hostname`, `homedir`.
