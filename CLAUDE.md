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

**`process.exitCode` is honoured**, in every mode -- a plain script,
`--bundle`, and a `--build-exe` executable alike. It was ignored until
2026-08-24: `runHermesNode()` kept a native exit code that nothing in
JavaScript could reach, so a test runner that set `process.exitCode = 1` and
let its report finish printing exited 0, and a failing run looked green.

Node keeps one variable where we keep two, so step 15 of the bootstrap
reconciles them in the order Node's single variable would have ended up in.
A **native** failure (an uncaught exception, a module that would not load)
outranks whatever the program assigned earlier -- a throw exits 1 in Node
however the property was set beforehand. But an assignment made *during* an
`'exit'` handler is the last word even over that, because in Node the handler
is simply overwriting the variable the exception wrote to. That is why the
property is read before and after the emit and the two are **compared**: a
handler that changed it wins, one that left it alone does not undo the
reconciliation. A bare `process.exit()` falls back to the property too
(`processExit` in `lib/process/node_process.cpp`); an explicit argument still
wins.

Pinned by `test/test-process-exit-code.js`, whose fourteen cases were each
checked against the status the same program produces under node v24.13.1.

**`process.exit()` emits `'exit'` and flushes first.** It used to do neither.
`processExit` called `_exit()` immediately, so no listener ran -- and that is
where a program puts what must happen whatever else it skips. blessed
restores the terminal there (`screen.js`, `process.on('exit', ...)` ->
`normalBuffer()`), so quitting a TUI left the alternate screen buffer on.

Flushing is the other half, and it is not specific to `process.exit()`:
`libjs/setup-stdio.js` gives a TTY a `tty.WriteStream` and a pipe a
`net.Socket`, both libuv streams whose writes are **queued** rather than
synchronous, where Node writes stdio synchronously on POSIX for a TTY or a
file. So `_exit()` discarded whatever had not reached the fd -- eight
`console.log` calls before `process.exit()` printed **one line** -- and
anything an `'exit'` handler wrote was lost on *both* exit paths, since
step 14's `uv_run` finished before those writes existed. `flushPendingWrites`
(`lib/process/node_process.cpp`) runs the loop `UV_RUN_NOWAIT`, bounded, on
both paths. The caveat is written where it is implemented: running the loop
can also fire an already-expired timer after `'exit'`, which Node does not
do. Draining the stdio write queues specifically would be the better fix and
needs the `uv_stream_t` behind each JS stream; losing a program's output is
the worse of the two divergences. Pinned by
`test/test-process-exit-event.js`, seven cases, each checked against Node.

The one remaining divergence: assigning a **non-numeric** value is ignored
here, where Node throws `ERR_INVALID_ARG_TYPE` at the assignment (so
`process.exitCode = "oops"` exits 1 there and 0 here). Matching it needs a
validating accessor with backing storage on the `process` object; ignoring is
the safer of the two answers available without one, since guessing what the
program meant would turn a typo into a status a shell script branches on.

## Uncaught Exceptions From Async Callbacks

An exception that escapes a **timer, immediate or tick** callback goes to
`process._fatalException`, and the answer decides what happens: `true` means a
listener took it and the program carries on, `false` means the error is
reported and the process exits 1. That is Node's split
(`node::errors::TriggerUncaughtException` asks the same property), and the
reason for it is that only JavaScript knows which listeners exist.

Until 2026-08-24 the native callback that caught the exception printed it and
returned. So `process.on('uncaughtException')` never fired for these
callbacks, and **the process exited 0** -- an assertion that failed inside a
`setTimeout` looked green, the same failure mode `process.exitCode` had above.
A third symptom came from the same line: timers share one libuv handle and the
early return skipped the code that re-arms it, so every timer still pending
after a throw stopped firing. `test/test-repl-features.js` had an assertion
that had been failing unnoticed for precisely this reason.

- `triggerUncaughtException()` (`lib/bindings/node_errors.h`,
  `.cpp`) is the one copy. It **does not return** when nothing handled the
  error, so callers need no unhandled path; a caller holding a libuv handle
  must put it back the way a normal return would have (the timers binding
  re-arms its shared timer).
- `process._fatalException` is built in `libjs/process-events.js`. Node
  installs it from `internal/bootstrap/node.js`, which this runtime does not
  run. There is no `setUncaughtExceptionCaptureCallback`, so that branch of
  Node's version has no equivalent yet.
- Pinned by `test/test-uncaught-exception-async.js`, whose cases were each
  checked against node v24.13.1.
- **Not yet done:** the same swallow-and-continue is still in about ten I/O
  bindings (`node_file.cpp`, `node_tcp_wrap.cpp`, `node_zlib.cpp`,
  `libuv_stream_base.cpp` and others) -- a throw in an `fs.readFile` callback
  still exits 0, and `net`'s `listen` callback hangs. The helper is written to
  be their fix too, but each needs its own decision about what resuming means.

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
  `history/plans/progress-aot-bundle.md`. The closed-world round (2026-08-19,
  format v2) supersedes that design's fallback rows: design
  `history/plans/2026-08-19-closed-world-bundle-design.md`, plan
  `history/plans/2026-08-19-closed-world-bundle-plan.md`. The preload round
  (2026-08-20, format v3) adds `--preload`: design
  `history/plans/2026-08-20-bundle-preload-design.md`, plan
  `history/plans/2026-08-20-bundle-preload-plan.md`. The native-addon round
  (2026-08-21, format v4) makes a `.node` addon packageable: design
  `history/plans/2026-08-21-bundle-natives-design.md`, plan
  `history/plans/2026-08-21-bundle-natives-plan.md`.
- Implementation: `lib/bundle/` (format, writer, reader, generation tag,
  `require()` scanner, resolver, producer, run layer) plus
  `libjs/bundle-loader.js`, which wraps `Module._load`. `hermesNodeBundleRun`
  is deliberately free of the parser and compiler; only the producer links
  them.
- The producer prints `bundle root: <dir>`, the deepest common directory of
  every packaged file; identities are relative to it. The consumer does not
  read that path out of the container -- it re-roots every identity at the
  bundle file's own directory -- so a container is internally consistent
  wherever it is placed, and a bundle built into a directory of its own
  runs fine. What placing it at the printed root buys is that a bundled
  module's `__dirname`/`__filename` name the files they named at build
  time. That matters in exactly two places: reading a data file that was
  never packaged, and the stat-before-require escape hatch below. Nothing
  else depends on the two roots coinciding.
- Packaged as JavaScript: `.js`, `.cjs`, `.ts`, and extensionless files (a
  bare `node_modules/<pkg>/<name>` entry point is real; yargs ships one).
  `.json` is packaged as raw text. A `.node` addon is packaged as a
  `kNative` record with an empty payload and its bytes copied beside the
  container -- see the natives bullet below. Everything else warns and is
  skipped: assets, and `.mjs` (ESM, which the CJS loader cannot run).
  Module kind is stored in the container record and is never re-derived from
  the identity's extension.
- **A bundle is a closed world.** Every module comes from the container;
  `require()`/`require.resolve()` never read or resolve code from the
  filesystem (code that deliberately drops to `Module.prototype.load`,
  `require.extensions`, `process.dlopen` of a path it names itself, or
  reads and evals a file itself sits outside that boundary, same as it
  would unbundled). A `require()` neither the edge
  table nor the container's resolver can place throws
  `Cannot find module '<x>' / required by <identity> / Not in the bundle.
  Add it with: --include=<x>`, with `code = 'MODULE_NOT_FOUND'` so an
  optional-dependency probe still sees what it expects. A `.node` addon the
  container does not record takes that same path, with that same text and
  the same `--include` suggestion: the producer packages addons now, so a
  miss means only that nothing packaged this one. (An addon the container
  *does* record whose sidecar file is absent is a different situation --
  `missingSidecar()`, see the natives bullet.) This is the point of shipping
  a bundle -- otherwise "self-contained" is unverifiable -- and a
  containment property: a computed specifier cannot make a bundled program
  load arbitrary code off disk. `HERMES_NODE_DEBUG_NATIVE=BUNDLE` logs the
  outcome of every bundled require -- an edge-table hit, a container-resolve hit, or a miss -- not
  only the misses; the misses are the list an `--include` set is built
  from. The gate is read once, when `installBundleLoader()` runs, because
  it sits on the hot path of every `require()`; assigning
  `process.env.HERMES_NODE_DEBUG_NATIVE` from inside the running program
  has no effect, matching the native side, which also reads the variable
  once at startup. The only things still served from
  outside the container are what the binary itself carries: builtins, and
  a vendored package (`ws`) with no packaged copy. They reach the original
  loader by different routes. A builtin is intercepted at the top of the
  wrapper by `normalizeRequirableId` and forwarded **verbatim**;
  `Module._resolveFilename` answers it on its first line. Only `ws` is
  rewritten to its **`node:`** spelling by `embeddedRequest()`, and that
  rewrite is load-bearing: a bare `ws` is not a builtin to
  `Module._resolveFilename`, so forwarding it would run `_findPath` over
  the module's `paths` and could execute a `node_modules/ws` sitting on the
  deployment machine. `test/bundle-build.js`'s WSDECOY case plants exactly
  that decoy.
- **Two `require`s that are not the loader's own** used to get around the
  closed world, because a module's own wrapper `require` parameter shadows
  them and so nothing ordinary met them. Both are shut, and pinned by
  `test/bundle-escapes.js`.
  `globalThis.require` (the bootstrap loader, `libjs/loader.js`) is
  reachable as `(0, eval)('require')`, `global.require` or
  `new Function('return require')()`, and its disk fallback read and
  compiled any path handed to it. `installBundleLoader()` calls
  `globalThis.__closeDiskModuleLoading()`, which flips a one-way flag in
  that loader; the global function itself has to stay (`libjs/shims/
  domain.js` requires `events` through it, lazily), so what goes away is
  the disk path inside it and not the function.
  `Module.createRequire()` builds a `Module` with a filename and no
  `__bundleIdentity`, so its every specifier took the "no bundled importer"
  throw and its `resolve()` walked the real filesystem. `identityOf()` in
  `libjs/bundle-loader.js` derives an identity from `parent.filename` when
  that filename is under the bundle root, and `Module._resolveFilename` is
  wrapped alongside `Module._load` so the resolution half is closed too.
- `--include=<specifier>` (repeatable) packages what static discovery
  cannot see. Each value is a bare specifier or a path, resolved from the
  **entry's directory** and then walked exactly like the entry. An
  `--include` that does not resolve is a build error -- the user named this
  one explicitly. Because the value is entry-relative, the not-found error
  cannot echo a relative request back verbatim: `./helper` inside
  `node_modules/foo/index.js` is suggested as
  `--include=./node_modules/foo/helper` (`includeSuggestion()` joins the
  request onto the importer's identity directory, then expresses it
  relative to the entry's). Where no correct value exists -- a relative
  request with no importer identity -- the message says where `--include`
  resolves from instead of printing one that fails. The producer also warns at build time, in two counted
  lines: `require()` **or `require.resolve()`** calls whose argument is not
  a literal, and places where `require` is used as a value rather than
  called (what `@babel/core` does, and why bundling it wants
  `--include=@babel/preset-env`). `--verbose` lists each position --
  `dynamic <file>:<line>:<col>` and `escape <file>:<line>:<col>`.
- **A bundle carries its own preloads.** `--preload=<specifier>` (repeatable,
  at build time) resolves from the **entry's directory** exactly as
  `--include` does, packages the module, and additionally records it in the
  container's preload table, in flag order. `--bundle` runs that table
  before the entry, in order. Run-time `-r`/`--require` is refused with
  `--bundle` -- the artifact decides what runs inside it, not the command
  line that launches it -- and is deliberately untouched with
  `--build-bundle`, since a build runs in the disk world. **Format v3** adds
  the preload table (a list of module indices); `--dump` prints it as a
  `PRELOADS` section so a container that runs code before its entry says so.
- **A native addon ships beside the bundle, not inside it.** `dlopen(3)`
  takes a path and there is no portable way to load a shared object out of
  memory, so a `.node` file's bytes cannot come from the container. It is
  packaged as a `kNative` module record with an **empty payload**, and its
  bytes are copied to a flat **sidecar** file in the bundle's own directory
  -- flat, not a mirrored identity tree, because "bundle plus tree" is not
  a better distribution unit than a tree. The sidecar is named by the
  addon's basename, with a `-<crc32 of the identity>` suffix when two
  addons want the same one; an addon that already *is* the file it would be
  copied to claims its plain basename first, so the ordinary
  `proj/binding.node` plus `node_modules/foo/build/Release/binding.node`
  pair costs zero writes instead of one that overwrites a build input. Two
  addons that still collide is a hard build error, as is a sidecar that
  would land on the container itself or on another addon's source file. The
  copies run **after** compilation, so the window in which a failed build
  can leave this run's sidecars beside the last run's container is narrowed
  to the container write itself. The producer prints
  `native: <sidecar> (from <identity>)` per addon and one `note:` line
  saying the artifact is now more than one file. At run time the loader
  `dlopen`s `<bundle dir>/<sidecar>`; `module.filename` stays the identity
  path, like every other bundled module, and the real path is what
  `dlopen` errors name. A recorded addon whose sidecar is missing throws
  `MODULE_NOT_FOUND` naming the file to ship -- not `ERR_DLOPEN_FAILED`,
  because what exists in the world to handle an unavailable addon (an
  optional-dependency probe, a napi-rs try/catch chain) branches on
  `MODULE_NOT_FOUND`. **Format v4** adds the native table: one
  `BundleNativeRecord {moduleIndex, sidecarString, byteLength, hashString}`
  per `kNative` module, in a section of its own
  (`nativeTableOffset`/`nativeCount`), reached from JS as
  `bundle.natives()` (`__bundleNatives` in `lib/bundle/bundle_run.cpp`) and
  built into an identity-to-sidecar map once, at install time.
  `__bundleLoad` refuses a native outright, since its bytes are not there
  to return. Discovery uses the two routes that already exist: the scanner
  follows a literal `require('./x.node')` (the design doc argues this
  covers napi-rs, whose generated platform switch is literal in every
  branch), and everything computed -- `bindings()`,
  `node-gyp-build(__dirname)`, `node-pre-gyp` -- needs `--include`. An
  absolute specifier is resolved by an explicit branch in
  `resolveSpecifier()` (`lib/bundle/bundle_resolve.cpp`), because
  `require(path.join(__dirname, ...))` is how a computed loader asks; the
  bare walk below it happened to give the same answer, but only through
  `fs::path::operator/` discarding its left operand.
  `examples/hermes-parser-ast` is the worked end-to-end case: the native
  Hermes parser addon, reached only through computed requires, named with
  one `--include`, producing a 240,560-byte container plus a
  2,521,344-byte sidecar whose AST output is byte-identical to the
  unbundled run.
- **A loader that stats before it requires does not find a flat sidecar.**
  `node-gyp-build` and `node-pre-gyp` `readdirSync`/`existsSync` a
  candidate directory and only then require the winner, so a probe for the
  addon's original path finds nothing at the flat sidecar. Measured, not
  predicted, on `examples/bufferutil-addon` (whose `bufferutil/index.js` is
  `try { require('node-gyp-build')(__dirname) } catch { require('./fallback') }`,
  and whose addon nothing names literally, so nothing packages it without
  `--include`): the bundled program **runs and silently uses the pure
  JavaScript fallback**. Which half fails depends on whether the source
  tree is still on disk beside the bundle. With it gone, `readdirSync`
  finds nothing and node-gyp-build throws its own "No native build was
  found". With it present, the stat succeeds against the real tree,
  node-gyp-build returns a real absolute path, and the *following*
  `require()` of it is what the closed world refuses -- so
  `require.resolve`-shaped self-checks can report a native addon that was
  never loaded. Either way the same catch fires and the same fallback
  loads. The escape hatch costs no code: additionally place the real file
  beside the bundle, at `<bundle dir>/<identity>`, and the stat succeeds.
  Teaching `fs` to answer for recorded native identities was considered and
  deliberately deferred -- `existsSync` would say yes where `readFileSync`
  on the same path says no.
- **One resolver, two backends.** `resolveSpecifier`
  (`lib/bundle/bundle_resolve.cpp`) runs against a `FileSource`:
  `DiskFileSource` for the producer, `BundleFileSource` (a sorted index of
  the container's identities, built lazily on the first query) for the
  consumer, reached from JS through
  `bundle.resolve(fromIdentity, request, paths)` (`__bundleResolve` in
  `lib/bundle/bundle_run.cpp`). A second resolver written in JavaScript
  would eventually disagree with the C++ one, and a specifier that resolves
  differently at build and run time is the worst failure this system can
  produce. `BundleFileSourceTest` and `BundleResolveTest`'s agreement cases
  pin the two backends against each other.
- **Format v2** adds a `flags` word to the module record. The
  `package.json` files the producer's resolution read are packaged, because
  the consumer's resolver needs `main`; one packaged only for that has
  `kRequirable` clear, so `BundleFileSource` reads it and `require()`
  cannot. Not all of them: a recorded `package.json` is kept only when its
  directory is an ancestor of (or equal to) some packaged module's
  directory, so a failed probe into an unrelated `node_modules/foo` does
  not drag foo's `package.json` in -- and, before this filter, did not drag
  the bundle root outward with it. The common ancestor is computed *after*
  that filter, from the modules plus the kept files, so a package.json
  sitting one level above every module of its own package (a package whose
  code all lives in `lib/`) is kept AND widens the root to cover itself,
  which is what lets a run-time `require.resolve('foo', {paths})` find it
  by name. The reverse order was tried and reverted -- see
  `history/plans/progress-aot-bundle.md`. A version mismatch is fatal in
  `open()` and reported (not enforced) in `openForInspection`.
- The scanner (`lib/bundle/require_scanner.cpp`) wraps each source in the
  CommonJS module wrapper before parsing, then runs `sema::resolveAST` and
  identifies `require` **by binding**, not by name: the wrapper's `require`
  parameter. Both halves matter. A module body is a function body, so a
  top-level `return` -- an ordinary CommonJS idiom -- is only legal wrapped.
  And a module that declares its own `require` (pre-bundled browserify or
  webpack output) must not contribute specifiers that were only ever
  meaningful inside that bundle. `kCJSWrapperPrefix` is single-sourced in
  `include/hermes/node-compat/bundle/cjs_wrapper.h`, shared with the
  compile step so the scan sees exactly the text that gets compiled.
  Positions are converted back out of the wrapper; compiler diagnostics are
  not, and their column on line 1 is offset by the prefix, as it always was.
- **A literal `require.resolve(spec)` is a discovery edge, exactly like
  `require(spec)`.** The scanner follows both call shapes (and their
  optional spellings, `require?.(...)` / `require?.resolve(...)`) into one
  deduplicated specifier list, so the target of a `require.resolve` -- and
  that target's whole transitive graph -- is packaged even when nothing
  ever `require()`s it. In a closed world it has to be: `require.resolve`
  is answered from the container alone, so a target that was not packaged
  throws at run time with nothing said at build time. The cost is that a
  pure feature probe (`try { require.resolve('typescript'); has = true; }
  catch (e) {}`) now packages that package and everything it pulls in for
  a call whose only use is a boolean. Nothing in `examples/` does this, so
  the corpus does not measure the growth -- it is unexercised, not absent.
  A *computed* `require.resolve(x)` counts toward the same "computed
  require()" warning a computed `require(x)` does, for the same reason:
  both reach the run-time loader by the identical route.
- The static walk reaches code the run never does, so two things it finds
  there warn instead of failing the build. A specifier that resolves to
  nothing (an optional-dependency probe) is left out of the edge table and
  handed to the run-time loader, which throws the same `MODULE_NOT_FOUND` a
  disk run throws. A file the parser or compiler rejects (`import()` inside
  a `.cjs`, and other branches meant for another module system) is packaged
  as a module that throws the same `SyntaxError` -- when required, and only
  then, so a program that never loads it is unaffected and the container
  stays self-contained. The **entry** is the exception and is still a hard
  error, being the one file the program is certain to load.
- A bundled module's `require` comes from Node's `makeRequireFunction`; only
  `require.resolve` is overridden: edge table first, then `bundle.resolve`,
  then throw. (`Module._resolveFilename` is wrapped with the same order for
  the `require`s this loader does not build -- see the `createRequire` note
  above.) There is no fall-through to the original `_resolveFilename` -- with no
  filesystem behind it, deferring would mean failing anyway, and a
  `resolve()` that answered where the following `require()` throws would be
  worse than either. An `options.paths` skips the edge table (the caller is
  replacing the search path, which is a different question) but not
  `bundle.resolve`, which runs the option's own algorithm; Babel's
  `require.resolve(id, { paths: [dir] })` reaches exactly this. A `paths`
  that is present but not an array falls straight through to `baseResolve`,
  whose `ERR_INVALID_ARG_VALUE` is Node's own error to throw.
- `Module._cache`, keyed by filename, is the loader's **only** cache: bundled
  records are published there before their body runs and read back from
  there. So a module reached both by an edge and by a computed specifier is
  instantiated once, and `delete require.cache[require.resolve(x)]` really
  does force a reload. Do not reintroduce a private identity-keyed cache;
  the program cannot invalidate one.
- Builtins are resolved before the edge table, via
  `BuiltinModule.normalizeRequirableId` (NOT `Module.isBuiltin`, which also
  answers for vendored packages such as `ws`), so a bundle can never shadow
  an embedded builtin. The producer's `isBuiltinSpecifier`
  (`lib/bundle/bundle_resolve.cpp`) mirrors the same 44-name list. **Nothing
  forces those two lists to agree with each other, or either of them to agree
  with what is actually compiled in** (`lib/embedded-modules/
  embedded-modules.txt`), and they have drifted in both directions. `zlib`
  was compiled in and requirable from a script while absent from both
  classifier lists, so a bundle routed it into the closed world and threw
  `MODULE_NOT_FOUND` -- a break visible only after bundling, and only to a
  program that used zlib. Seven names drift the other way and are classified
  but not compiled in (`assert/strict`, `dns/promises`, `path/posix`,
  `path/win32`, `stream/consumers`, `stream/web`, `timers/promises`); those
  fail identically bundled and unbundled, so they are a coverage gap rather
  than a bundling defect. `test/bundle-builtins.js` is the forcing function
  for the first kind: it runs one file plain and bundled and diffs the two
  outputs, so a name requirable one way and not the other fails there instead
  of in somebody's program.
- Vendored packages (`ws`) are not builtins: an installed `node_modules` copy
  is packaged and wins, and when there is none the producer warns
  (`warning: not packaging '<id>' ...` via `isVendoredSpecifier`) and the
  embedded copy serves the `require()` at run time.
- `package.json` `exports` is still not consulted; only `main`, then
  `index.*`. This is the most likely source of a resolution mismatch with
  Node -- but now in exactly one place, since both sides run the same
  resolver.
- `--bundle` is rejected with `--inspect`/`--inspect-brk` (bundled bytecode
  lacks the debug info the debugger needs), with `--build-bundle` or
  `-e`/`--eval`, and with `-r`/`--require` (see the preloads bullet above).
  A positional argument in bundle mode belongs to the bundled program, not
  to hermes-node.
- Tests:
  `test/bundle-{build,run,require,errors,scanner,tolerant,include,preload,container-resolve,resolution-inputs,escapes,natives,natives-errors,yargs}.js`
  plus `BundleFormatTest`, `BundleResolveTest`, `BundleFileSourceTest`,
  `RequireScannerTest`. (`bundle-scanner.js` was `bundle-fallback.js` before
  the fallback was removed; it keeps the scanner cases, which are about the
  scan and not the loader.)
  `bundle-yargs.js` is gated on the `examples-installed` lit feature
  (`test/lit.cfg`), set when `examples/yargs-cli/node_modules` exists, so the
  offline default suite reports it UNSUPPORTED rather than failing.

### Bundle tooling

Five diagnostic flags, none of them on the run path. Design
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
- `--bundle=<f> --dump` prints the header, module table, edge table,
  `NATIVES` section (identity, sidecar name, byte length, truncated
  SHA-256, printed only when the container records one) and section sizes
  (including a `natives` row) to stdout. `--verbose` adds per-module in/out
  edge counts.
- `--bundle=<f> --extract-module=<identity> --out=<file>` writes one
  module's payload verbatim (bytecode for a JS module, the original bytes
  for a JSON one). A native is refused, naming its sidecar: its bytes are
  not in the container, so an "extraction" would write the empty payload
  and call it the addon. Unknown identities are an error listing the
  nearest few by edit distance. `--out` naming the same file as `--bundle`
  (compared by `st_dev`/`st_ino`, so `./app.hbb`, a symlink and a hard link all count) is
  refused: the write is a rename, so it would replace the container with one
  module's payload and nothing downstream would notice.
- `--bundle=<f> --verify-natives` checks every recorded addon against the
  file of that name in the container's own directory (realpath'd first,
  exactly as the run path does, so a bundle reached through a symlinked
  directory is checked where the run would look), printing `OK` /
  `MISSING` / `MISMATCH` per addon and exiting 1 if any failed, so it can
  gate a deployment. `--verbose` prints expected and actual length and
  digest for **every** entry, passing ones included: a verification whose
  passing case shows nothing is hard to trust. This is an **audit, not an
  enforcement** -- it reports what the files are when it runs, and the
  program `dlopen`s them later; nothing closes that gap, and nothing on the
  run path hashes an addon (that would read the whole thing on every
  launch). SHA-256, not the CRC32 the generation tag uses, because a CRC
  can be forged to any value and a check offered as a security step should
  not have that as its weakest part. The digest is computed by
  `nativeFileDigest()` (`lib/bundle/native_digest.cpp`, picohash, streamed
  rather than read whole), the single copy the producer and this verb
  share.
- `--dump-bytecode=<f>` disassembles a raw bytecode file or a compile cache
  entry (detected by the cache header's magic and skipped past).
  `--verbose` adds a `; file:line:column` comment per instruction, not the
  source text (a bytecode file does not carry it).
- The four read-only verbs are dispatched by `runToolVerb()` in
  `tools/hermes-node/hermes-node.cpp` **before `runHermesNode`**, so no
  runtime, event loop or `napi_env` exists while they run: a tool that
  describes a file must not fail for reasons belonging to a runtime it never
  needed.
- `checkToolOptions()` in the same file holds the whole flag-conflict
  matrix, all of it after the parse loop so that flag order never matters.
  The rows are the table under "Flag surface" in the design doc: two verbs
  at once, a container verb with no `--bundle`, `--extract-module` without
  `--out` (and `--out` without it), `--verbose` with none of its four
  consumers, `--dump-bytecode` with `--bundle`/`--build-bundle`, and any
  verb with `--inspect`/`--inspect-brk`. Each message names both flags. Two
  checks beyond the table live there too: `--dump-bytecode=` and `--out=`
  with an empty value, which name the flag instead of reporting a missing
  file with no filename in it. An empty `--extract-module=` is deliberately
  not among them -- that flag takes a lookup key, where empty is a lookup
  that legitimately misses, and the miss is reported by the lookup code.
- `--dump`, `--extract-module` and `--verify-natives` open through
  `openBundleForTool()`, one copy of the map/validate sequence that calls
  `BundleReader::openForInspection`, which reports the generation tag
  instead of enforcing it (`MISMATCH` line in the dump); structural
  validation is unchanged and a format-version mismatch stays fatal.
  `open()` keeps its hard error, so nothing on the run path can reach a
  mismatched container.
- Two new libraries, split by dependency: `hermesNodeBundleTools`
  (`lib/bundle/bundle_tools.cpp`, dump, extract and verify) links only
  `hermesNodeBundle` and stays free of the Hermes VM, which is what lets
  `BundleToolsTest` run with no runtime; `hermesNodeBytecodeDump`
  (`lib/bytecode-dump/`) links `hermesvm_a` because Hermes's disassembler
  lives there; it includes `bundle_format.h` for `kBundleMagic` alone (so a
  container pointed at it is named), which is a header of constants and
  costs no link dependency. Folding them together would give dump, extract
  and verify a VM dependency none of them needs.
- Tests:
  `test/bundle-{verbose,dump,extract,dump-bytecode,verify-natives,tool-errors,tool-no-runtime}.js`
  plus `BundleToolsTest` and the inspection-mode cases in
  `BundleFormatTest`. `bundle-tool-no-runtime.js` pins the pre-runtime
  dispatch: a verb given `--compile-cache=<dir>` never creates the
  directory, while running the same container does.

## Single-File Executables

`--build-exe=<out> <bundle.hbb>` turns an AOT container into a standalone
executable. Design `history/plans/2026-08-23-single-executable-design.md`,
plan `history/plans/2026-08-23-single-executable-plan.md`, progress
`history/plans/progress-single-executable.md`.

- It takes **a container, not an entry script**, deliberately: `--build-bundle`
  already produces containers, and the container going in can be inspected
  first with the five diagnostic flags above -- `--dump`, `--extract-module`,
  `--verify-natives`, `--dump-bytecode` and `--verbose`. Node's SEA blob has
  no equivalent. Accepting a `.js` entry directly, dispatched on the magic
  bytes, is strictly additive later and nothing here forecloses it.
- **We link a new binary rather than inject a blob into a prebuilt one**, and
  the payoff is macOS. Node's SEA (postject), Deno (libsui) and Bun all patch
  a copy of their runtime, and all pay the same tax: Apple prohibits appending
  to a Mach-O, and on Apple Silicon a signature that does not verify is a
  SIGKILL rather than a warning. libsui hand-writes an ad-hoc CodeDirectory in
  Rust whose flags read `adhoc|linkerSigned` -- it is reimplementing `ld64`.
  Use `ld64` and it is free: measured on macOS arm64, our linked binary comes
  out `flags=0x20002(adhoc,linker-signed)` and passes `codesign --verify`,
  with no signing step of our own, payload-carrying builds included. So there
  is no fuse, no sentinel, no self-mmap, no `/proc/self/exe` and no backwards
  scan for a magic -- the payload is a symbol. The price is a linker on the
  build machine (and the Xcode command line tools on macOS), which Static
  Hermes native compilation needs anyway.
- **The kit** is what an app links against: `libhermes-node-kit.a` (every
  archive in the closure, merged), `libhermesNapi.a` kept separate,
  `hermes-node-bundle-main.o`, and `kit.manifest`. It is cut by
  `utils/make-kit.py`, which intercepts CMake's own link line: a
  `RULE_LAUNCH_LINK` on the `hermes-node-kit` probe target in
  `tools/hermes-node/CMakeLists.txt`, built with `add_hermes_tool` rather
  than `add_executable` so `HERMES_EXTRA_LINKER_FLAGS` cannot drift between
  probe and binary. That is the point: the manifest cannot disagree with what
  actually links. The spike's hand-written system-library list had `-lresolv`
  missing and `-ldl` spurious, which is exactly the class of error a manifest
  removes. Grammar is four keys -- `version`, `cc`, `driverflag`, `linkarg`
  -- with `{kit}` substituted for the kit directory; `readKitManifest()` in
  `lib/build-exe/kit_manifest.cpp` parses them.
- **A merged archive, not one `ld -r` object.** The `-r` object is smaller
  (14.1 MB against 28 MB) and it is one file, but it drops
  `MH_SUBSECTIONS_VIA_SYMBOLS`. Mach-O has no per-function sections; that flag
  is the linker's permission to synthesize atoms from symbols, so without it
  the final link can neither fold identical code nor dead-strip -- about 12%
  of the produced binary. The loss is macOS-only: on Linux the section already
  is the granularity unit, and an `-r` object dead-strips to within 0.035% of
  the original inputs. One story across both platforms was worth more than the
  kit size. The merged archive linked 8 bytes smaller than CMake's own binary,
  with identical exports and identical lit results.
- **Two constraints that fall out of merging, and will bite whoever edits
  this.** Nothing may `-force_load` the merged archive: merging preserves
  duplicate symbols the real link never sees because it never pulls both
  members (`hermes::vm::matchTypeOfIs` in two `Operations.cpp.o` members,
  `llvh::DisplayGraph` in two `GraphWriter.cpp.o`) -- harmless in an ordinary
  archive, a duplicate-symbol failure the moment something force-loads it.
  That is why `libhermesNapi.a`, which genuinely is force-loaded so `dlopen`ed
  addons resolve the NAPI surface, ships beside it rather than inside it. And
  entry objects stay out: with `hermes-node.cpp.o` in the archive, `main`
  would be pulled from it and the two link configurations below would collapse
  into one.
- **Two link configurations, no fuse and no weak symbols.** `hermes-node`
  links the CLI entry `hermes-node.cpp`; an app links the generated payload
  object plus `tools/hermes-node/bundle_main.cpp` (compiled as the
  `hermesNodeBundleMain` OBJECT library and copied into the kit). "Am I an
  app?" is a link-time fact, not a run-time question. An app carries neither
  the CLI parser nor the tool verbs, which dead-stripping turns into a real
  difference: measured 154,032 bytes smaller than `hermes-node` itself.
  `bundle_main.cpp` pushes `argv[0]` twice, so `process.argv` is
  `[exe, exe, ...userArgs]` and `process.argv.slice(2)` means what it means
  under `--bundle=<f> arg`, and what it means to Node's SEA.
- **The payload is `.incbin` in a generated `.s`** -- lowercase, because
  clang preprocesses `.S` and not `.s`, and the assemble step forwards the
  kit's `-D` flags over a file holding a user-supplied path
  (`payloadAssembly()` in `lib/build-exe/build_exe.cpp`) -- not a C array whose
  multi-megabyte initializer would explode compile time for nothing. Size is
  `end - start`, computed by the linker, so no stored length can disagree with
  the bytes. Two directives are load-bearing rather than decorative.
  `.p2align 4` clears the format's `kBundlePayloadAlign`
  (`include/hermes/node-compat/bundle/bundle_format.h`), and
  `openEmbeddedBundle()` (`lib/bundle/bundle_run.cpp`) now enforces that base
  alignment: the reader's offset checks are modulo the offset and sufficient
  only when the base is already aligned, which `mmap` gave `openBundle()` for
  free and a linked section does not -- so a generator mistake is a message
  rather than undefined behaviour that x86-64 happens to execute. And on ELF
  the file MUST end with `.section .note.GNU-stack,"",@progbits`: without it
  the linker cannot tell that a hand-written object needs no executable stack
  and marks the whole program `GNU_STACK RWE` (measured with `readelf`;
  `hermes-node` itself is `RW`) -- a security regression in every binary the
  feature ships. Mach-O needs no equivalent. Both spellings are compiled on
  every host: `ObjectFormat` is a parameter
  (`include/hermes/node-compat/build-exe/build_exe.h`), not an `#ifdef`,
  because the branch for the platform you are not on is the one a typo
  survives in. The container's path goes into a quoted assembler string, so it
  is made absolute and rejected if it holds `"`, `\`, CR or LF -- GAS resolves
  `\t` inside a path and would happily `.incbin` a file the literal path does
  not name. The toolchain is run through `posix_spawnp` with an argv, never a
  shell.
- **The root is the executable's own directory**, realpath'd, where
  `openBundle()` uses the container's; `rootDirectoryFor()` in
  `lib/bundle/bundle_run.cpp` is the one copy both call. Everything downstream
  is unchanged -- the closed world, the resolver and its two backends,
  preloads -- except that a native addon's sidecar now sits beside the
  **executable**, and so does the `<root>/<identity>` copy that the
  stat-before-require escape hatch needs.
- **Version agreement is checked twice, in different currencies.**
  `buildExecutable()` opens the container through `BundleReader::open()` with
  this binary's own `bundleGenerationTag()`, so a container the produced
  executable could not run is a build-time error rather than a startup
  failure in the customer's hands; and `kit.manifest`'s `version:` is compared
  for exact string equality against `HERMES_NODE_VERSION_STRING`, so a kit cut
  from a different build than the producer is one too. The manifest
  deliberately does not record the generation tag: that is computed in C++
  from the bytecode version and build configuration, and is not available to
  CMake, which writes the file. **Both currencies are commit-granular, so
  neither catches an edit to an already-dirty tree**: the first edit to a
  clean tree does move `git describe` (it gains `-dirty`), but every edit
  after that leaves it unchanged, and `hermes-node-kit` is
  `EXCLUDE_FROM_ALL`, so rebuilding `hermes-node` after fixing a runtime bug
  does not re-cut the kit -- a hand-run `--kit=<build dir>/kit` then links
  the app against the *previous* runtime, with the two version strings
  agreeing and nothing warning. The in-tree suite is safe (`check-hermes-node-js`
  DEPENDS on the kit target, so it re-cuts), so the exposure is exactly the
  manual path documented below: re-cut the kit yourself after touching
  runtime code.
- The `hermes-node-kit` target is `EXCLUDE_FROM_ALL` -- the merged archive is
  a full copy of every archive in the closure, ~40 MB in a Release build and
  ~755 MB in an ASAN one -- and `check-hermes-node-js` `DEPENDS` on it. That
  dependency is load-bearing, not belt-and-braces: `kit.manifest` pins the
  `git describe` version, so **every commit invalidates the kit**, and without
  it every gated test would go red on the first commit after a cut. Re-cutting
  takes a few seconds and both error paths name the command
  (`cmake --build <build dir> --target hermes-node-kit`). The kit's real
  outputs are written behind the build system's back, so the probe link's `-o`
  is aimed at a stamp *inside* the kit directory: `rm -rf kit` then re-cuts it
  rather than reporting nothing to do.
- **The default kit location is "beside the running binary"**
  (`resolveKitDir()` in `tools/hermes-node/hermes-node.cpp`, via `uv_exepath`
  and `realpath` -- `argv[0]` is whatever the caller chose to exec with). That
  is where a release layout WOULD put one, and is not this repo's build
  tree, where the binary is in `bin/` and the kit in `kit/`: pass
  `--kit=<build dir>/kit` when running `--build-exe` by hand. **No release
  ships a kit today.** `.github/workflows/release.yml` stages the binary
  alone and there is no `install()` rule for the kit, so `--build-exe` on a
  released `hermes-node` fails with the missing-manifest error. Packaging
  one is a product decision (a Release kit is ~40 MB, several times the
  binary) and is deliberately not made here.
- `--build-exe` is dispatched by `runToolVerb()` **before `runHermesNode`**,
  alongside the four read-only verbs. It writes files, so it is not read-only
  -- but the criterion for that dispatch point was never read-only-ness, it is
  whether the verb needs a runtime, and this one reads an already-compiled
  container and runs the toolchain: no parser, compiler, event loop or
  `napi_env`. `checkToolOptions()` gains its rows, each naming both flags:
  `--build-exe` against `--bundle`, `--build-bundle`, `-e`/`--eval`, each of
  the four verbs, and `--inspect`/`--inspect-brk`; `--kit` without
  `--build-exe`; `--build-exe` with no positional container; and an empty
  `--build-exe=` or `--kit=`, which name the flag rather than reporting a
  missing file with no filename in it. Separately, and just after that
  block: an argument beginning with `-` that appears **after** the
  container is refused by name. The parse loop stops at the first
  positional, as everywhere else, and `--build-exe` is the only verb whose
  own input sits in that slot -- so it is the only one where the
  convention is observable, and it used to be observable only as a flag
  having no effect (`--build-exe=out app.hbb --verbose` narrated nothing;
  `--kit=/other` there silently used the default kit).
  `--verbose` narrates the kit, the
  container, the generated assembly and both toolchain commands, to stderr.
  The producer is `hermesNodeBuildExe` (`lib/build-exe/`), VM-free like
  `hermesNodeBundleTools`, which is what lets `BuildExeTest` run with no
  runtime.
- **Every produced executable creates a compile-cache tree it never uses.**
  Startup makes `~/.cache/hermes-node/compile-cache/v1/<generation>/` (or
  the `XDG_CACHE_HOME` equivalent) and writes nothing into it, because a
  bundled program compiles nothing. Identical under `--bundle`, so this is
  pre-existing rather than something linking introduced -- but a
  `--build-exe` artifact is shipped to people who have never heard of
  hermes-node, and they cannot pass `--no-compile-cache`: every argument
  belongs to the program. `HERMES_NODE_DISABLE_COMPILE_CACHE=1` still works,
  since the native side reads it directly.
- **What does not work.** Windows is unwritten and unclaimed -- nothing here
  is Windows-hostile, we simply cannot test it. Cross-compilation is the one
  capability injection has that linking does not; that is the accepted trade,
  since a linker is a prerequisite Static Hermes brings anyway. Universal
  macOS binaries are unproven end to end, and carry a trap worth respecting:
  `ld -r`, `libtool` and `lipo` emit a valid-looking **empty slice** for a
  missing architecture -- warnings only, exit 0, a slice `lipo -info` reports
  as real -- which surfaces as an undefined `_main` at the customer's link, so
  whatever cuts a universal kit must assert per-slice symbol counts and never
  trust exit status. `utils/make-kit.py` therefore warns, loudly and by
  architecture, when the link line it was handed names more than one --
  release CI's macOS job configures `CMAKE_OSX_ARCHITECTURES="x86_64;arm64"`
  and then runs `check-hermes-node`, which depends on the kit target, so
  that job is where a two-slice kit gets cut first. A warning and not an
  error, because failing there would block macOS releases outright to report
  something that may well be fine. (Its `libtool -static` branch has never
  executed anywhere either; the macOS spike predates the script.) The
  payload object at least reaches that link built for the right targets:
  `buildAssembleCommand()` forwards the manifest's whole `driverflag` list,
  `-arch` included. And a produced **Linux** executable is not
  self-contained: it needs ICU 74, `libstdc++.so.6` and `libgcc_s.so.1`, where
  a macOS one needs only OS-provided libraries.
  `HERMES_USE_STATIC_ICU=ON` would fix the ICU half, at the cost of a static
  system ICU at build time; its own round.
- Tests: `test/build-exe{,-errors,-tool-errors,-natives,-escapes}.js` plus
  `BuildExeTest`. Three of the five -- `build-exe.js`, `build-exe-natives.js`
  and `build-exe-escapes.js` -- carry `REQUIRES: linker-available`, the lit
  feature (`test/lit.cfg`) that is on when lit was given `--param kit_dir`
  naming a directory holding a `kit.manifest`; a bare `hermes-lit`
  invocation without the param reports those three UNSUPPORTED rather than
  failing them. `build-exe-errors.js` and `build-exe-tool-errors.js` are
  deliberately **not** gated, and each says so in its own header: every case
  in them is refused before the toolchain is reached (the kit-shaped ones
  use a two-line hand-written `kit.manifest`, and the two that do run a
  driver name one chosen to fail), so they need neither a linker nor a
  built kit and are the coverage that survives a kitless checkout.
  `BuildExeTest` is a GTest, run by `check-hermes-node-unit`, which lit's
  feature list does not reach at all -- it links `hermesNodeBuildExe`, which
  is VM-free, and never runs a toolchain.

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

### Known flaky tests

Three JS tests fail intermittently under the suite's 16-way parallel load and
pass in isolation. **A single red run naming one of these is not a
regression**; confirm before chasing it, because each has cost a session time
already:

```bash
for i in 1 2 3 4 5 6; do
  TEST_THREAD_ID=$i cmake-build-asan/bin/hermes-node test/<name>.js >/dev/null 2>&1
  echo -n "$? "
done
```

Measured 6/6 passing in isolation for all three, on both sides of an
unrelated change:

- `test-inspect.js` -- spawns a child and waits for
  `Debugger listening on ws://` on its stderr with a timeout. The most
  frequent of the three, and the timing dependency is explicit in the test.
- `test-repl-history.js` -- same shape, a spawned REPL session read through
  a pipe.
- `test-fs-async-verify.js` -- was measured at 2/6 and 5/6 failures in
  isolation during the single-executable work, so it may be genuinely
  racy rather than only load-sensitive. It is currently 6/6; if it starts
  failing in isolation again, that is a real bug and not this note.

None is understood, none has been fixed, and none is quarantined -- a
`XFAIL`ed flake stops reporting the day it becomes a real failure. The
honest state is that they are known, reproducible only under load, and
someone should eventually find out why.

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
