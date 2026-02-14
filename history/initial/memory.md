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

## Hermes NAPI Key Facts
- `hermes_napi_event_loop` (hermes_napi.h:269-300): post_work, cancel_work, post_task
- `napi_env__` takes `Runtime&` + optional `hermes_napi_event_loop*`
- `Runtime::create(RuntimeConfig)` returns `shared_ptr<Runtime>`
- Enable microtasks: `RuntimeConfig::Builder().withMicrotaskQueue(true)`
