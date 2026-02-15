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

## Vendored Deps
- libuv 1.51.0 in `external/libuv/libuv/`, static target `uv_a`

## Hermes NAPI Key Facts
- `hermes_napi_event_loop` (hermes_napi.h:269-300): post_work, cancel_work, post_task
- `napi_env__` takes `Runtime&` + optional `hermes_napi_event_loop*`
- `Runtime::create(RuntimeConfig)` returns `shared_ptr<Runtime>`
- Enable microtasks: `RuntimeConfig::Builder().withMicrotaskQueue(true)`
