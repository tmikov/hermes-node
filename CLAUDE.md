# CLAUDE.md

## Project

Node.js API compatibility layer for Hermes. Ports Node's native bindings to Node-API; reuses Node's `lib/*.js` files.

## Key Paths

- Hermes (submodule): `hermes/` — n-api branch, not yet merged to Hermes main
- Hermes Node-API source (separate checkout): `/home/tmikov/work/hermes-n-api`
- Node.js source (separate checkout): `/home/tmikov/3rd/node` — v24.13.0
- Plans and history: `history/`

## Conventions

- C++ libraries: `lib/<name>/` with own `CMakeLists.txt`
- Public headers: `include/hermes/node-compat/<name>/`
- Vendored unmodified deps: `external/$lib/$lib` (outer dir has README + wrapper CMake, inner dir is upstream source)
- `external/hermes-parser-native/` is a temporary vendored copy of the Hermes native parser addon, not a submodule-style unmodified dep; it has its own README covering provenance, re-sync steps, and when to delete it
- Vendored Node JS (will be modified): `libjs-node/`
- Our JS: `libjs/`
- Examples: `examples/` — each subdirectory has its own `package.json` + `package-lock.json`; `node_modules/` is gitignored (users run `npm install`)
- Build: CMake + Ninja
- Tests: GTest (`unittests/`), lit (`test/`)
- Test target: `check-hermes-node`
- Commit messages: ASCII only, no emojis

## Build Configurations

Build directories follow the convention:
- `cmake-build-asan` — **Primary development configuration.** Debug, Clang, ASAN. Depends on a matching Hermes build (also ASAN + handle sanitizer enabled). Use this for all development work.
- `cmake-build-debug` — Debug, Clang
- `cmake-build-release` — Release, Clang

Always use Clang, never GCC.

Before any commit, format C++ code and run tests:
```bash
./utils/format.sh -f
cmake --build cmake-build-asan --target check-hermes-node
```

## Hermes JS Limitations

- No `Atomics`, no `AbortSignal`/`AbortController` globals (`FinalizationRegistry` is supported natively)
- Async generators: require `-Xasync-generators` flag (enabled in hermes-node)
- Async generator prototype chain is flat (Hermes bug)
- Hermes warns about undeclared globals in strict mode IIFEs -- use `var X = globalThis.X`

## Bootstrap Sequence

`hermes-node` binary in `tools/hermes-node/hermes-node.cpp`. Boot order:
runtime -> event loop -> napi_env -> console -> bindings -> primordials -> process -> module loader -> timers globals -> `globalThis.Buffer` -> debuglog -> user script -> drainJobs -> uv_run -> emit 'exit' -> cleanup

## Module Loader

- `libjs/loader.js`: CJS module loading with shim override (`libjs/shims/` before `libjs-node/`)
- User scripts loaded via `globalThis.__loadUserScript(filepath)` (NOT `napi_run_script`)
- `globalThis.require`, `globalThis.primordials`, `globalThis.internalBinding` set by loader
- Native bindings registered in `hermes-node.cpp` via `registry.registerBinding("name", initFunc)`

## Native Addons

Node-API (N-API) native addons are supported. V8-API addons (`v8.h`, NAN) are not (no V8).

- `process.dlopen(module, filename[, flags])` in `lib/process/node_process.cpp`: `dlopen()` -> look up `napi_register_module_v1` (modern) -> fall back to deprecated `napi_module_register()`.
- `tools/hermes-node/CMakeLists.txt` exports NAPI symbols from the binary (`-rdynamic` equivalent) so dlopen'd addons can resolve them at link time.
- `.node` extension resolved by the CJS loader (`lib/bindings/node_file.cpp`).
- `os.dlopen` constants defined in `lib/bindings/node_constants.cpp`.
- Hermes side: `hermes_napi_load_module()` in `hermes/API/napi/hermes_napi.cpp` handles the in-process module registration table.

## Test Infrastructure

JS tests use LLVM Lit (`test/lit.cfg`), run in parallel via `check-hermes-node-js` target.

- **PASS-check tests** (`test/*.js`): `// RUN: %hermes-node %s | %FileCheck %s` + `// CHECK: PASS`
- **Node-ported tests** (`test/node-tests/parallel/*.js`): `// RUN: TEST_THREAD_ID=$$ %hermes-node %s`
- **Primordials test**: `// RUN: cat %source_dir/libjs/primordials.js %s > %t.js && %hermes -Xasync-generators %t.js`
- **Expected-failure tests**: `%not` runs a command that must exit non-zero, e.g. `// RUN: %not %hermes-node %s 2>&1 | %FileCheck %s`
- Run single test (paths must be absolute): `python3 cmake-build-asan/bin/hermes-lit $(pwd)/test/test-foo.js --param hermes_node=$(pwd)/cmake-build-asan/bin/hermes-node --param hermes=$(pwd)/cmake-build-asan/bin/hermes --param FileCheck=$(pwd)/cmake-build-asan/bin/FileCheck --param not=$(pwd)/cmake-build-asan/bin/not --param source_dir=$(pwd) --param test_exec_root=$(pwd)/cmake-build-asan/test`

Unit tests (GTest) are discovered and run by Lit too (`unittests/lit.cfg`), via
the `check-hermes-node-unit` target. `check-hermes-node` runs both suites, so a
failing unit test fails the build.

- Run one binary directly: `cmake-build-asan/unittests/NodeProcessTest --gtest_filter=...`

`check-hermes-node-examples` runs the examples under `examples/` and is
**not** part of `check-hermes-node`: examples need a network `npm install`,
while the default suite stays offline. Run it against `cmake-build-release`
(it skips under an ASAN build -- the bundler example is too slow under ASAN
to be useful as a check).

## Decisions

- Primordials: thin shim (Option B) — re-export builtins, no tamper-resistance
- Event loop: single libuv loop, `uv_run(UV_RUN_DEFAULT)`, standalone CLI only
- Async hooks: stubbed (no-op)
- Node version: v24.13.0 LTS
- Built-in JS (`libjs/`, `libjs-node/`, shims) is compiled to Hermes bytecode at build time and embedded into the binary; only the user's script is parsed at run time
- **C++ porting philosophy**: Keep our native binding implementations as close to Node's as reasonable. When Node uses a third-party library (simdutf, Ada, llhttp, c-ares, etc.), vendor and use that same library rather than hand-rolling equivalent functionality. This ensures behavioral parity, gets us battle-tested optimizations, and makes future porting easier since our code structure mirrors Node's.

## Progress Tracking

Each plan has its own progress file. The progress file says which plan it tracks. Update it when completing or blocking on steps. Add context notes per the format documented there.
