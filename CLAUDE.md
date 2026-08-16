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

## Compile Cache

Compiled bytecode for user and `node_modules` JavaScript is cached on disk,
on by default. Built-in JS is unaffected (already embedded as bytecode).

- Root: `$XDG_CACHE_HOME/hermes-node/compile-cache`, else
  `~/.cache/hermes-node/compile-cache`. Layout `v1/<generation>/<ab>/<key>`.
- Controls: `--compile-cache=<dir>`, `--no-compile-cache`,
  `HERMES_NODE_COMPILE_CACHE`, `HERMES_NODE_DISABLE_COMPILE_CACHE`,
  `HERMES_NODE_DEBUG_NATIVE=COMPILE_CACHE`. No `NODE_*` variables.
- Disabled under `--inspect` / `--inspect-brk`: cache entries are compiled at
  `DebugInfoSetting::THROWING`, the debugger needs `ALL`.
- Not observable from JavaScript. `module.enableCompileCache()` still reports
  `FAILED` and `getCompileCacheDir()` still returns `undefined`, deliberately.
- `test/lit.cfg` sets `HERMES_NODE_DISABLE_COMPILE_CACHE=1` for the whole
  suite; compile-cache tests opt in with their own directory under `%t`.
- Implementation: `lib/compile-cache/`, consulted from
  `node_contextify.cpp` (`compileFunctionForCJSLoaderCb`) and
  `module_loader.cpp` (`compileAndRunCallback`).
- Cache key is derived from the module's absolute path, not its content, so
  re-requiring the same path reuses the same entry across separate module
  graphs (e.g. multiple bundler runs in one process). Each entry also stores
  a CRC32 and byte length of the source, checked on every lookup, so an
  edited file misses (and is rewritten) rather than serving stale bytecode.
- Measured on the flow-bundler example (`examples/flow-bundler`, ~2841
  compile-cache consults, ~1506 distinct files): warm runs were consistently
  36-48% faster than cold, depending on OS file-cache state, with the cache
  landing at ~16 MB / ~1506 entries. See
  `history/plans/progress-compile-cache.md` for the full numbers.

## AOT Bundles

`--build-bundle=<file>` walks a script's `require()` graph, compiles every
JavaScript file to bytecode, and writes one container. `--bundle=<file>`
runs it, with no compilation and no source tree needed at run time.

- Design `history/plans/2026-08-15-aot-bundle-design.md`, plan
  `history/plans/2026-08-15-aot-bundle-plan.md`, progress
  `history/plans/progress-aot-bundle.md`.
- Implementation: `lib/bundle/` (format, writer, reader, generation tag,
  `require()` scanner, resolver, producer, run layer) plus
  `libjs/bundle-loader.js`, which wraps `Module._load`. `hermesNodeBundleRun`
  is deliberately free of the parser and compiler; only the producer links
  them.
- The producer prints `bundle root: <dir>`, the deepest common directory of
  every packaged file. Identities are relative to it and the consumer takes
  the root from the bundle file's own directory, so **the bundle must sit at
  the printed root**.
- Packaged as JavaScript: `.js`, `.cjs`, `.ts`, and extensionless files (a
  bare `node_modules/<pkg>/<name>` entry point is real; yargs ships one).
  `.json` is packaged as raw text. Everything else warns and is skipped:
  `.node` addons, assets, and `.mjs` (ESM, which the CJS loader cannot run).
  Module kind is stored in the container record and is never re-derived from
  the identity's extension.
- Skipped files, and specifiers only a computed `require()` can reach, fall
  back to disk through the original `Module._load`. Log the fallbacks with
  `HERMES_NODE_DEBUG_NATIVE=BUNDLE`.
- A bundled module's `require` comes from Node's `makeRequireFunction`; only
  `require.resolve` is overridden (edge table first, then
  `Module._resolveFilename`), and it skips the edge table when the caller
  passes `options.paths`.
- `Module._cache`, keyed by filename, is the loader's **only** cache: bundled
  records are published there before their body runs and read back from
  there. So a module reached both from the container and through the disk
  fallback is instantiated once, and
  `delete require.cache[require.resolve(x)]` really does force a reload. Do
  not reintroduce a private identity-keyed cache; the program cannot
  invalidate one.
- Builtins are resolved before the edge table, via
  `BuiltinModule.normalizeRequirableId` (NOT `Module.isBuiltin`, which also
  answers for vendored packages such as `ws`), so a bundle can never shadow
  an embedded builtin. The producer's `isBuiltinSpecifier`
  (`lib/bundle/bundle_resolve.cpp`) mirrors the same 43-name list.
- Vendored packages (`ws`) are not builtins: an installed `node_modules` copy
  is packaged and wins, and when there is none the producer warns
  (`warning: not packaging '<id>' ...` via `isVendoredSpecifier`) and the
  embedded copy serves the `require()` at run time.
- `package.json` `exports` is not consulted in v1; only `main`, then
  `index.*`. This is the most likely source of a resolution mismatch with
  Node.
- `--bundle` is rejected with `--inspect`/`--inspect-brk` (bundled bytecode
  lacks the debug info the debugger needs), and with `--build-bundle` or
  `-e`/`--eval`. A positional argument in bundle mode belongs to the bundled
  program, not to hermes-node.
- Tests: `test/bundle-{build,run,require,errors,fallback,yargs}.js` plus
  `BundleFormatTest`, `BundleResolveTest`, `RequireScannerTest`.
  `bundle-yargs.js` is gated on the `examples-installed` lit feature
  (`test/lit.cfg`), set when `examples/yargs-cli/node_modules` exists, so the
  offline default suite reports it UNSUPPORTED rather than failing.

### Bundle tooling

Four diagnostic flags, none of them on the run path. Design
`history/plans/2026-08-15-bundle-tooling-design.md`, plan
`history/plans/2026-08-15-bundle-tooling-plan.md`, progress
`history/plans/progress-bundle-tooling.md`.

- `--build-bundle=<f> --verbose` narrates configuration (entry, absolute
  output path, generation tag with the version/arch/bytecode-version/
  optimization it folds), discovery (with the specifier that pulled each
  module in), compilation (source/bytecode bytes, ratio, timing) and a
  summary of the finished container (modules by kind, edges and distinct
  specifiers, string/payload/bytecode bytes, the largest module, total file
  size, total compile time) **to stderr**. The container is byte-for-byte
  the same with or without it. The summary runs after
  `BundleWriter::serialize()` and reads its section sizes back out of the
  serialized bytes, so it and a later `--dump` cannot disagree.
- `--bundle=<f> --dump` prints the header, module table, edge table and
  section sizes to stdout. `--verbose` adds per-module in/out edge counts.
- `--bundle=<f> --extract-module=<identity> --out=<file>` writes one
  module's payload verbatim (bytecode for a JS module, the original bytes
  for a JSON one). Unknown identities are an error listing the nearest few
  by edit distance. `--out` naming the same file as `--bundle` (compared by
  `st_dev`/`st_ino`, so `./app.hbb`, a symlink and a hard link all count) is
  refused: the write is a rename, so it would replace the container with one
  module's payload and nothing downstream would notice.
- `--dump-bytecode=<f>` disassembles a raw bytecode file or a compile cache
  entry (detected by the cache header's magic and skipped past).
  `--verbose` adds a `; file:line:column` comment per instruction, not the
  source text (a bytecode file does not carry it).
- The three read-only verbs are dispatched by `runToolVerb()` in
  `tools/hermes-node/hermes-node.cpp` **before `runHermesNode`**, so no
  runtime, event loop or `napi_env` exists while they run: a tool that
  describes a file must not fail for reasons belonging to a runtime it never
  needed.
- `checkToolOptions()` in the same file holds the whole flag-conflict
  matrix, all of it after the parse loop so that flag order never matters.
  The rows are the table under "Flag surface" in the design doc: two verbs
  at once, a container verb with no `--bundle`, `--extract-module` without
  `--out` (and `--out` without it), `--verbose` with none of its three
  consumers, `--dump-bytecode` with `--bundle`/`--build-bundle`, and any
  verb with `--inspect`/`--inspect-brk`. Each message names both flags. Two
  checks beyond the table live there too: `--dump-bytecode=` and `--out=`
  with an empty value, which name the flag instead of reporting a missing
  file with no filename in it. An empty `--extract-module=` is deliberately
  not among them -- that flag takes a lookup key, where empty is a lookup
  that legitimately misses, and the miss is reported by the lookup code.
- `--dump` and `--extract-module` open through
  `BundleReader::openForInspection`, which reports the generation tag
  instead of enforcing it (`MISMATCH` line in the dump); structural
  validation is unchanged and a format-version mismatch stays fatal.
  `open()` keeps its hard error, so nothing on the run path can reach a
  mismatched container.
- Two new libraries, split by dependency: `hermesNodeBundleTools`
  (`lib/bundle/bundle_tools.cpp`, dump and extract) links only
  `hermesNodeBundle` and stays free of the Hermes VM, which is what lets
  `BundleToolsTest` run with no runtime; `hermesNodeBytecodeDump`
  (`lib/bytecode-dump/`) links `hermesvm_a` because Hermes's disassembler
  lives there; it includes `bundle_format.h` for `kBundleMagic` alone (so a
  container pointed at it is named), which is a header of constants and
  costs no link dependency. Folding them together would give dump and
  extract a VM dependency neither needs.
- Tests:
  `test/bundle-{verbose,dump,extract,dump-bytecode,tool-errors,tool-no-runtime}.js`
  plus `BundleToolsTest` and the inspection-mode cases in
  `BundleFormatTest`. `bundle-tool-no-runtime.js` pins the pre-runtime
  dispatch: a verb given `--compile-cache=<dir>` never creates the
  directory, while running the same container does.

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
