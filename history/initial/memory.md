# Implementation Memory

## Conventions
- Copyright: "Copyright (c) Tzvetan Mikov." (NOT Meta)
- Vendoring: always include upstream LICENSE

## Hermes Build
- CMake macros: `add_hermes_library`, `add_hermes_tool`, `add_hermes_executable`
- GTest: `add_unittest` (needs `LLVH_SOURCE_DIR` = `hermes/external/llvh`)
- Link targets: `hermesNapi`, `hermesvm_a`, `gtest_main`
- NAPI headers: public `hermes/include/hermes/napi/`, internal `hermes/API/napi/hermes_napi.h`
- Our test target: `check-hermes-node` -> `NodeCompatUnitTests`

## Build Gotchas
- ASAN flags: Hermes sets `-fsanitize=address` via `CMAKE_CXX_FLAGS` inside its subdirectory scope only. Our top-level CMakeLists.txt must propagate them via `add_compile_options`/`add_link_options` when `HERMES_ENABLE_ADDRESS_SANITIZER` is ON.
- LLVH includes: gtest headers need `llvh/Support/raw_ostream.h`. The `add_node_compat_unittest()` helper adds LLVH include dirs (`hermes/external/llvh/include`, `gen/include`, build-dir `include`).
- **hermes_napi.h is heavyweight**: It includes `hermes/VM/Runtime.h` etc., pulling in all VM internals (needs LLVH + `libhermesvm-config.h` from `cmake-build-*/hermes/lib/config/`). Avoid including it in public headers. Use our standalone `hermes_napi_event_loop.h` for the event loop struct.
- **GC define not propagated**: Hermes uses `add_definitions(-DHERMESVM_GC_${HERMESVM_GCKIND})` scoped to its subdirectory. Our targets that include Hermes VM headers need `target_compile_definitions(... PRIVATE HERMESVM_GC_${HERMESVM_GCKIND})`.
- **Handle scopes required**: All NAPI calls that create JS values require an open handle scope (`napi_open_handle_scope`). Tests must open one in SetUp and close in TearDown.

## Vendored Deps
- libuv 1.51.0 in `external/libuv/libuv/`, static target `uv_a`

## Event Loop Adapter
- `hermesNodeEventLoop` lib in `lib/event-loop/`, links `uv_a` (PUBLIC)
- `UvEventLoop` class (PIMPL): init/run/close lifecycle, getEventLoop() for NAPI, getLoop() for libuv
- `uv_async_t` handle: unref'd when idle, ref'd when tasks pending (so loop exits cleanly)
- Task queue: mutex + singly-linked list, LIFO push, reversed to FIFO on drain

## Hermes JS Engine Limitations
- No `FinalizationRegistry` (typeof is undefined)
- No `Atomics` (typeof is undefined)
- No async generators (SyntaxError)
- `WeakRef`: supported
- Private class fields (`#x`): supported
- `BigInt`, `BigInt64Array`: supported
- RegExp: lookbehind, named groups, hasIndices/d flag all supported
- `AggregateError`: supported
- Hermes warns about undeclared globals in strict mode — use `var X = globalThis.X` in IIFEs

## JS Test Infrastructure
- `check-hermes-node-js` CMake target runs JS tests via `test/run-primordials-test.sh`
- Tests use stock `hermes` CLI binary (CMake target `hermes`)
- Hermes doesn't support multiple file args; concatenate files before running
- Hermes doesn't have `load()` function

## Primordials
- `libjs/primordials.js`: thin shim, sets `globalThis.primordials`
- Reuses Node's algorithmic approach (enumerate+copy prototypes)
- Safe* variants are just the originals (no tamper-resistance)
- 156 test assertions in `test/primordials.js`

## Binding Registry
- `hermesNodeBindingRegistry` lib in `lib/binding-registry/`, depends only on NAPI headers (lightweight)
- `BindingRegistry::registerBinding(name, initFunc)` + `getBinding(env, name, &result)` with lazy init + `napi_ref` cache
- `createInternalBindingFunction(env, &fn)` creates JS `internalBinding(name)` function
- `attach(env)` / `detach(env)` lifecycle; detach clears all cached refs

## NAPI Test Pattern
- Tests needing `napi_env`: link `hermesvm_a`, include `hermes/API/napi/`, add `HERMESVM_GC_${HERMESVM_GCKIND}` define, add `${CMAKE_BINARY_DIR}/hermes/lib/config` include
- Create Runtime via `vm::Runtime::create(config)`, env via `hermes_napi_create_env(*rt_)`
- Open handle scope in SetUp, close in TearDown

## Module Loader
- `hermesNodeModuleLoader` lib in `lib/module-loader/`, depends only on NAPI headers
- `ModuleLoader` class: `setLibJsPath/setLibJsNodePath` -> `init(env, primordials, internalBindingFn)` -> `require(env, name, &result)` -> `detach(env)`
- JS side (`libjs/loader.js`): IIFE returning setup function; setup returns require function
- Shim override: checks `libjs/shims/<name>.js` before `libjs-node/<name>.js`
- Sets `globalThis.primordials` and `globalThis.internalBinding` during init
- `//# sourceURL=<path>` for Hermes stack traces in wrapped modules
- `(0, eval)(wrapped)` for global-scope indirect eval
- Tests use `TEST_DATA_PATH` and `LIBJS_PATH` CMake compile definitions for filesystem paths

## Hermes NAPI Key Facts
- `hermes_napi_event_loop` (hermes_napi.h:269-300): post_work, cancel_work, post_task
- `napi_env__` takes `Runtime&` + optional `hermes_napi_event_loop*`
- `Runtime::create(RuntimeConfig)` returns `shared_ptr<Runtime>`
- Enable microtasks: `RuntimeConfig::Builder().withMicrotaskQueue(true)`
- `napi_run_script` does global eval with `compileFlags.strict = false`; no way to set source URL via API, use `//# sourceURL=` comment instead
- Hermes supports `//# sourceURL=` for custom filenames in stack traces
