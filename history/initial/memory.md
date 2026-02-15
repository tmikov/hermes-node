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

## Hermes NAPI Key Facts
- `hermes_napi_event_loop` (hermes_napi.h:269-300): post_work, cancel_work, post_task
- `napi_env__` takes `Runtime&` + optional `hermes_napi_event_loop*`
- `Runtime::create(RuntimeConfig)` returns `shared_ptr<Runtime>`
- Enable microtasks: `RuntimeConfig::Builder().withMicrotaskQueue(true)`
