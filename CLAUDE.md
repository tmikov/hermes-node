# CLAUDE.md

## Project

Node.js API compatibility layer for Hermes. Ports Node's native bindings to Node-API; reuses Node's `lib/*.js` files.

## Key Paths

- Hermes (submodule): `hermes/` — `hermes-node` branch of `github.com/tmikov/hermes`, not yet merged to Hermes main
- Hermes Node-API source (separate checkout): `/home/tmikov/work/hermes-n-api`
- Node.js source (separate checkout): `/home/tmikov/3rd/node` — v24.13.0
- Documentation: `docs/` -- specs and plans under `docs/superpowers/`, everything else under `docs/notes/`. See `docs/README.md`.

## Conventions

- C++ libraries: `lib/<name>/` with own `CMakeLists.txt`
- Public headers: `include/hermes/node-compat/<name>/`
- Vendored unmodified deps: `external/$lib/$lib` (outer dir has README + wrapper CMake, inner dir is upstream source)
- `external/hermes-parser-native/` is a temporary vendored copy of the Hermes native parser addon, not a submodule-style unmodified dep; it has its own README covering provenance, re-sync steps, and when to delete it
- Vendored Node JS (will be modified): `libjs-node/`
- Our JS: `libjs/`
- Examples: `examples/` — each subdirectory has its own `package.json` + `package-lock.json`; `node_modules/` is gitignored (users run `npm install`). `examples/ditz2/` is the exception on two counts: its subject is a **submodule** (`examples/ditz2/ditz2`) rather than an npm dependency, because ditz2 is not published to npm, and it needs a build step of its own (`build-cjs.sh`) because it is TypeScript **and ESM**, which this runtime cannot load. Its `package.json` therefore pins ditz2's three runtime dependencies plus TypeScript, and the transpile lands in `examples/ditz2/dist-cjs/` rather than inside the submodule, so the submodule checkout stays clean. Delete that script when the ESM loader lands.
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

**clang-format 18 specifically.** CI installs `clang-format-18` and checks
against it, and other releases disagree with it about this file set -- a
macOS session formatting with 21.1.2 had unrelated parts of a file reflowed
under it. `format.sh` now refuses to run on a different major version and
says how to get the right one; it prefers a `clang-format-18` on `PATH` over
a bare `clang-format`, `$CLANG_FORMAT` overrides both, and `--any-version`
skips the check for anyone who means it.

## Hermes JS Limitations

- No `Atomics`, no `AbortSignal`/`AbortController` globals (`FinalizationRegistry` is supported natively)
- Async generators: require `-Xasync-generators` flag (enabled in hermes-node)
- Async generator prototype chain is flat (Hermes bug)
- Hermes warns about undeclared globals in strict mode IIFEs -- use `var X = globalThis.X`
- No `WebAssembly` (`typeof WebAssembly === 'undefined'`); in progress upstream
- **`eval` does not capture local scope** -- it behaves as *indirect* eval,
  seeing globals only, so `function f(){var x=1; return eval("x")}` throws
  where Node returns 1. (`new Function` is correct: the spec gives it the
  global scope, and it gets it.) This is an engine restriction, not
  something this layer can shim. It breaks any emscripten/asm.js output
  that builds its shims with direct `eval` -- nbind does, which is why
  `yoga-layout-prebuilt`, and therefore Ink 3, does not load. See
  `docs/notes/2026-08-24-ink-findings.md`; it fails identically from
  disk, a bundle and an executable, being underneath all three.

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

**Every `_exit()` path restores the terminal.** `stdin.setRawMode(true)`
reaches `uv_tty_set_mode()`, which saves the original termios in a libuv
global; raw mode clears `ECHO` and `ICANON`, so a program that leaves it set
hands the user back a shell that no longer echoes what they type, with
nothing on screen saying why. The program is not the one that has to undo it
-- tetris-cli sets raw mode on its fifth line and quits with
`process.exit(0)`, never clearing it, and that works under Node, because Node
restores in `ResetStdio()`, registered with `atexit()`. We cannot use
`atexit`: both exit paths call `_exit()` on purpose, to keep ASAN from
reporting the live Hermes runtime as thousands of leaks. So each calls
`uv_tty_reset_mode()` itself, **after** its flush -- queued escape sequences
from an `'exit'` handler should go out under the settings they were written
for, which is also where Node's `atexit` handler sits relative to them.

Falling off the end of the program was never affected: step 16 closes the
stdio handles and libuv's `uv__tty_close` restores termios on the way past.
That is why the fix is two lines in the two `_exit()` callers
(`processExit`, and `triggerUncaughtException` in `lib/bindings/
node_errors.cpp`) rather than one call somewhere central -- adding a third
call on the natural path would work, and would also stop
`test-tty-restore.js`'s `natural` case from noticing if that cleanup ever
broke. `process.abort()` deliberately gets no restore, since `abort()` runs
no `atexit` handler under Node either. Pinned by `test/test-tty-restore.js`,
four cases; it needs a real terminal, so it runs through
`test/fixtures/tty/run-on-pty.py`, which reads the flags back **inside** the
pty session -- on macOS the slave fd is revoked once the session leader
exits, so a parent holding it gets `ENOTTY`.

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
  `docs/superpowers/plans/progress-compile-cache.md` for the full numbers.

## Hermes VM Options

`--vm=<flag>` configures the Hermes VM underneath hermes-node the way the
`hermes` binary's own flags configure it. Design
`docs/superpowers/specs/2026-09-02-vm-options-design.md`, plan
`docs/superpowers/plans/2026-09-02-vm-options-plan.md`, progress
`docs/superpowers/plans/progress-vm-options.md`.

- Repeatable, **one Hermes flag per occurrence, never split on whitespace**:
  a value can contain a space, and `-Xperf-prof-dir=<dir>` is proof that
  such a value exists inside this very flag set. `HERMES_NODE_VM_OPTIONS` is
  whitespace-split, because an environment variable has no other shape --
  that limitation is the variable's, not the feature's. It is honoured for
  a plain script, `-e` and the REPL as well as for a container run, with the
  same precedence in both: the environment first, `--vm=` after it, so an
  explicit flag wins. It was container-only until 2026-09-02, which made
  `HERMES_NODE_VM_OPTIONS=-gc-max-heap=4g hermes-node app.js` do nothing and
  say nothing -- a silent no-op on a VM setting, which is the failure shape
  this whole feature refuses rather than ignores. `--build-bundle` is the
  one mode it stays out of, and for a different reason: there `--vm=` is
  *recorded* into the container rather than applied, so folding the
  environment in would bake a build machine's ambient variable into a
  shipped artifact.
- **Hermes parses the flags, not us.** `hermes::cli::RuntimeFlags` is
  instantiated over a synthesized argv built from the `--vm=` values alone,
  so spellings, `cl::desc` text, defaults and value parsers (`1g`, `512m`,
  the `on|force|off` enum behind `-Xjit`) come from Hermes and cannot drift
  from the binary they are supposed to match. `--vm-help` reads the same
  registry, so the help cannot drift from what is accepted either. Nothing
  but `--vm=` values is ever handed to `llvh::cl`: hermes-node's own parse
  loop stays hand-written, so an unknown `--buidl-bundle` is still our error
  with our message.
- **A standing constraint falls out of that: hermes-node must not link
  `hermesCompilerDriver`.** `CompilerDriver.cpp` defines the
  `hermes::cl::compilerRuntimeFlags` global, which registers the same option
  names our `RuntimeFlags` instance does, and `llvh::cl` keeps its registry
  in process globals -- both present is a duplicate-registration abort at
  static-initialisation time, before `main`. hermes-node compiles JavaScript
  through the Hermes runtime API rather than through `CompilerDriver`, which
  is what makes the delegation possible at all.
- **25 flags are honoured, 13 are refused by name** (`kHonoured` and
  `kConsoleHostOnly` in `lib/vm-options/vm_options.cpp`). The dividing line
  is not editorial: it is whether hermes-node can actually deliver the
  flag's effect, which for most of the refused set means whether the
  `hermes` binary implements it by putting a value into `RuntimeConfig` or
  by calling a method on a live `vm::Runtime` through `ConsoleHost` -- which
  this codebase deliberately never touches, for the reason recorded beside
  `hermes_napi_create_env` in `lib/runtime/hermes_node_runtime.cpp`: those
  headers' struct layout depends on private compile defines that Hermes's
  CMake leaks only within its own subdirectory scope, so including them
  would silently miscompute field offsets. Refusing rather than ignoring is
  the point. An accepted flag that does nothing is the swallow-and-continue
  pattern this codebase has spent several rounds removing, and the allowlist
  has a second job besides: it keeps `--vm=-help` from reaching `llvh::cl`'s
  help printer, which would dump the whole registry, LLVM's internal options
  included, and exit.
- **Three refused flags do set a real config bit**, and are refused anyway:
  `-sample-profiling`, `-gc-print-stats` and `-track-io`. Each parses and
  configures cleanly -- and each costs real work whose only reader is
  `ConsoleHost`, so the effect is never reported. `-sample-profiling`'s
  writer half is `ConsoleHost.cpp:1094-1112`; `-gc-print-stats` makes
  `GCBase` record statistics that only `Runtime::printHeapStats` prints;
  `-track-io` makes `Runtime` attach a page-access tracker that only
  `Runtime::getIOTrackingInfoJSON` reports. (Both of the latter two are also
  reachable through `jsi::Instrumentation`, which this runtime does not use
  either.) `-gc-print-stats` is the sharpest of the three, since its name
  promises printed output. Supporting any of them means deciding where that
  output goes, which is a feature in its own right.
- **`hermes::cli::buildRuntimeConfig()` is deliberately not called.** It
  omits `ES6BlockScoping`, `EnableAsyncGenerators`, `Test262` and every JIT
  field, and the `hermes` binary does not call it either -- it builds its
  config inline at `hermes/tools/hermes/hermes.cpp:110-141`, which is the
  complete mapping. Ours mirrors that inline mapping in the same order, for
  the sole purpose of being diffable against it when Hermes moves.
- **Two `llvh::cl` facts the design got wrong, both found in
  implementation.** A plain `cl::opt` is `cl::Optional` and *rejects* a
  repeated flag ("may only occur zero or one times!") rather than taking the
  last, so `buildVmRuntimeConfig` deduplicates the option list by flag name
  before parsing -- merge logic of ours, where the design claimed there
  would be none, and load-bearing because the whole override story is a
  chain of repeated flags in which the last must win. And passing an `Errs`
  stream to `ParseCommandLineOptions` buys exactly one thing: the function
  returns instead of calling `exit()`. It does not capture per-option
  errors, because `Option::error()` defaults its stream to `llvh::errs()`
  and every internal call site takes that default -- so a bad value prints
  llvh's precise message to real stderr and then ours as well.
- **The invariant that cost four bugs: a field is applied if and only if the
  caller actually named that flag** (`getNumOccurrences()`), never
  unconditionally and never gated by comparing the flag's value against an
  expected default. With no `--vm=` options nothing is written at all, every
  field keeps the Builder's own compiled-in default, and the result is *by
  construction* identical to the hardcoded three-call
  `RuntimeConfig::Builder()` this replaced -- rather than identical by an
  audit of which of Hermes's ~25 `cl::init(...)` values currently happen to
  agree with `RuntimeConfig`'s and `GCConfig`'s. That audit was tried first
  and four fields diverged: `GCSanitizeRate` and `GCSanitizeRandomSeed`
  (0.01 in a handle-sanitizer build against `GCSanitizeConfig`'s bare 0.0,
  which silently turned on 1% random handle sanitization for every plain run
  under ASAN and broke two unrelated tests that assert exact process
  output), `AsyncBreakCheckInEval` (`cl::init` false against
  `RuntimeConfig`'s true, so it flipped in every build) and `VerifyEvalIR`.
  `-gc-init-heap` and `-gc-max-heap` need the gating for a second reason:
  their `MemorySize` type has no `OptionValue` specialization, so
  `setDefault()` is a silent no-op and the gating is the *only* thing
  standing between an earlier call's parsed value and this call's output.
  `VmOptionsTest.EmptyOptionsMatchHardcodedBuilderFieldByField` pins the
  whole property, and all four divergences would have failed it. What the
  rule really guards against is a future Hermes `cl::init` change silently
  adding a fifth, without this code needing to change at all. The same rule
  settles a field `hermes.cpp` *does* set and no flag names:
  `ShouldReleaseUnused` is left at `GCConfig::Builder`'s own default
  (`kReleaseUnusedOld`) rather than copied from `hermes.cpp`'s
  `kReleaseUnusedNone`. Copying it once made every plain hermes-node run
  stop returning old-generation memory to the OS with nothing on the
  command line asking for it -- the mapping's job is to honour flags the
  caller passed, not to import constants the `hermes` binary picks for
  itself, and the identity above holds with no exception because of it.
- **Three fields are hermes-node's own defaults**, applied unconditionally
  and then overridden when the caller names the flag: `ES6BlockScoping`,
  `EnableAsyncGenerators` and `MicrotaskQueue`, all true. Hermes defaults
  the first two to **false**, so losing this removes async generators and
  block scoping from every program, with no flag passed and no message
  printed -- which is why it is the case `test/vm-options.js` asserts first.
- The parse runs once per process under `std::call_once`, its result cached,
  because `llvh::cl`'s registry is global and hermes-node starts *two*
  runtimes under `--inspect`: the inspector runtime is configured from the
  identical `RuntimeConfig` rather than from a second parse, since a
  debugger attached to a differently-configured VM misleads in a way that is
  hard to notice. The option *strings* rather than a built config live on
  `HermesNodeProcessConfig`, whose whole contract is that a field there is
  inherited by every runtime in the process, and strings keep the public
  header free of Hermes VM headers.
- **Format v5** adds a VM-options section -- an offset and count into the
  existing string table, the shape the preload and native tables already use
  -- plus `kBundleFlagAllowVmOptionsOverride`, the first bit in the header's
  container-flags word. `--build-bundle --vm=` **records** rather than
  applies: the producer compiles rather than runs, a build machine's VM
  tuning is not the artifact's business, and applying it would let an
  ordinary flag like `-Xes6-proxy=false` break the build itself, since
  `libjs/primordials.js` runs on the producer's own runtime.
  `--allow-vm-options-override` (which requires `--build-bundle`, and says
  so) sets the bit. `--dump` prints a `VM_OPTIONS` section with the lock
  state, so an artifact's VM configuration can be audited before it ships in
  the same spirit as `--verify-natives`. It prints whenever the container
  records options **or** has the bit set -- not options alone, which is
  nearly the `NATIVES` rule but not quite: the most open artifact there is,
  built with `--allow-vm-options-override` and no `--vm=`, honours
  `HERMES_NODE_VM_OPTIONS` unconditionally and used to be the one container
  `--dump` said nothing about.
- Options are applied in the order **baked, then `HERMES_NODE_VM_OPTIONS`,
  then `--vm=`** -- no merge logic at the call site, just the order the lists
  are appended in, with the dedupe above making the last occurrence win.
  **Locked is the default**, and an override attempt on a locked container
  is an **error** naming which source tried it, not a silent no-op. Locked,
  because the honoured set includes `-enable-eval` and
  `-Xhermes-internal-test-methods`, which are not tuning knobs: an artifact
  that always honours its environment lets whoever controls the environment
  turn those back on. Ignoring the environment variable when locked was
  considered and rejected -- it is quieter for someone who has the variable
  exported for a development hermes-node and then runs a locked executable,
  and that is a real cost, but it is a silent no-op on a VM setting, and the
  escape hatch is a build-time flag the person shipping the artifact
  controls.
- **A produced executable has no `--vm=`**, because every argument belongs to
  the program -- that is what keeps `process.argv.slice(2)` meaning what it
  means under `--bundle=<f> arg`. Its container's options travel inside the
  container's bytes, so `--build-exe` gains no flag of its own and the
  generated payload object gains no symbol; the environment variable is the
  only run-time override path an executable has, and the locked message
  there names both steps, since unlocking is rebuilding the container and
  then linking it again. The container is read for its options *before* any
  runtime exists, by `readBundleVmOptions` / `readEmbeddedBundleVmOptions`,
  which open for inspection and close -- one extra map-and-validate of a
  file about to be mapped again, which is cheaper than restructuring a run
  path that opens the container only after the `napi_env` is built.
- **Some honoured flags cannot be applied to a hermes-node runtime at all,
  and this cost three separate tasks time.** `-Xes6-proxy=false` is the
  worked example: Hermes gates the global `Reflect` on the same
  `hasES6Proxy()` check as `Proxy`, and `libjs/primordials.js` destructures
  `Reflect` during bootstrap, so a container that genuinely applies that
  flag can never finish booting. It fails loudly -- a clear message and exit
  1, which is the acceptable half -- but anyone writing a test that has to
  *run* a container should use `-enable-eval=false` instead: honoured,
  defaulting to true, and observable as `new Function` throwing with no
  other coupling. Separately, `-Xasync-generators` reaches only `eval` and
  `new Function`; a top-level async generator in a script file is compiled
  through a NAPI path that hardcodes the feature on, so a test that checks
  that flag through a script file concludes, wrongly, that the flag does
  nothing.
- Tests: `test/vm-options.js`, `test/vm-options-errors.js`,
  `test/bundle-vm-options.js`, `test/build-exe-vm-options.js` (`REQUIRES:
  linker-available`, like the other `build-exe` tests), plus
  `unittests/VmOptionsTest.cpp` and the v5 cases in `BundleFormatTest`.

## AOT Bundles

`--build-bundle=<file>` walks a script's `require()` graph, compiles every
JavaScript file to bytecode, and writes one container. `--bundle=<file>`
runs it, with no compilation and no source tree needed at run time.

- Design `docs/superpowers/specs/2026-08-15-aot-bundle-design.md`, plan
  `docs/superpowers/plans/2026-08-15-aot-bundle-plan.md`, progress
  `docs/superpowers/plans/progress-aot-bundle.md`. The closed-world round (2026-08-19,
  format v2) supersedes that design's fallback rows: design
  `docs/superpowers/specs/2026-08-19-closed-world-bundle-design.md`, plan
  `docs/superpowers/plans/2026-08-19-closed-world-bundle-plan.md`. The preload round
  (2026-08-20, format v3) adds `--preload`: design
  `docs/superpowers/specs/2026-08-20-bundle-preload-design.md`, plan
  `docs/superpowers/plans/2026-08-20-bundle-preload-plan.md`. The native-addon round
  (2026-08-21, format v4) makes a `.node` addon packageable: design
  `docs/superpowers/specs/2026-08-21-bundle-natives-design.md`, plan
  `docs/superpowers/plans/2026-08-21-bundle-natives-plan.md`. The VM-options
  round (2026-09-02, format v5) lets a container carry Hermes VM options and
  an override bit: design
  `docs/superpowers/specs/2026-09-02-vm-options-design.md`, plan
  `docs/superpowers/plans/2026-09-02-vm-options-plan.md` -- see the Hermes VM
  Options section above for the whole feature.
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
  `docs/superpowers/plans/progress-aot-bundle.md`. A version mismatch is fatal in
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
`docs/superpowers/specs/2026-08-15-bundle-tooling-design.md`, plan
`docs/superpowers/plans/2026-08-15-bundle-tooling-plan.md`, progress
`docs/superpowers/plans/progress-bundle-tooling.md`.

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
  SHA-256, printed only when the container records one), `VM_OPTIONS`
  section (the lock state, then each baked option; printed when the
  container records options **or** has the override bit set -- see the
  Hermes VM Options section) and section sizes (including `natives` and
  `vmopts` rows) to stdout. `--verbose` adds per-module in/out edge counts.
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
executable. Design `docs/superpowers/specs/2026-08-23-single-executable-design.md`,
plan `docs/superpowers/plans/2026-08-23-single-executable-plan.md`, progress
`docs/superpowers/plans/progress-single-executable.md`.

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
  scan for a magic -- the payload is a symbol. The price is a C++ driver on
  the build machine (and the Xcode command line tools on macOS), which
  Static Hermes native compilation needs anyway. **Any** driver, not a
  specific one: the link needs it for the crt objects, the default library
  set and the sysroot, all of which the driver computes on the machine
  doing the link. That late binding is what makes a kit portable, so
  nothing here may pre-compute those paths.
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
- **An absolute shared library is recorded as `-L<dir> -l<name>`, never as
  the path CMake used.** `as_link_flags()` in `make-kit.py` does it, one
  `-L` per directory ahead of the first library needing it, since a `-L`
  applies only to the `-l` flags after it. The absolute path says where the
  library sat on the machine that cut the kit, so recording it verbatim
  failed the link anywhere else, naming a directory the user never chose.
  The directory is kept as a `-L` rather than dropped because that costs
  nothing: a `-L` naming a directory that does not exist is ignored
  silently, so it helps where the layout matches -- a custom library prefix
  included -- and is inert elsewhere, where `-l` finds the system copy. A
  **versioned** soname (`libfoo.so.5`) is deliberately left absolute: `-lfoo`
  resolves through the `libfoo.so` development symlink, which a machine with
  only the runtime package does not have. `test/make-kit-classify.js` covers
  the classification, and is the only test `make-kit.py` has.
- **A failing assemble or link prints the whole command, shell-quoted.**
  `formatCommandLine()` renders it so it can be pasted into a shell, edited
  and rerun, which is the point of printing it at all: reproducing the
  failure by hand is how anyone diagnoses a link. Nothing here goes through
  a shell -- `runCommand()` spawns an argv -- so an earlier version printed
  the arguments bare, as an exact record of what ran. That was accurate and
  useless: a kit under a path with a space, or an `-isysroot /Some SDK`,
  printed as two arguments and pasted back as two arguments. Only arguments
  a shell would re-split are quoted, so an ordinary command still reads as
  one.
- **The recorded `cc:` is a hint, not a requirement, and any C++ driver
  will do.** `cc:` is `CMAKE_CXX_COMPILER`, so it is an absolute path
  belonging to the machine that cut the kit; treating it as the answer made
  a kit unusable anywhere else, and made the whole feature need Clang
  specifically. `driverCandidates()` (`lib/build-exe/build_exe.cpp`) turns
  it into an ordered list -- `--cc=<x>`, else the recorded path if it still
  exists, else its basename on `PATH`, else plain `c++` -- and
  `resolveDriver()` takes the first that runs. The recorded compiler is
  preferred rather than `c++`-first because the kit's archives and driver
  flags came from it: a kit cut with `-stdlib=libc++`, with LTO bitcode, or
  in an ASAN configuration will not link under a different driver, and that
  failure is loud. **`--cc` replaces the list rather than heading it**: a
  named compiler that cannot be run is a hard error, because linking with a
  substitute and saying nothing is worse than not building. `$CXX` is
  deliberately not consulted -- one exported for an unrelated build must
  not silently decide what links your executable. `--verbose` names the
  driver and why it was chosen, and says so when the recorded one was
  tried and rejected.
- **`-Qunused-arguments` is added only for Clang.** The whole `driverflag`
  list is forwarded to the assemble step on purpose (some of it selects a
  target), and the price is link-only flags reaching a compile that warns
  about each one. That suppression flag is a Clang spelling which GCC
  rejects outright. It cannot be recorded in the manifest, since `--cc` can
  change the driver afterwards, so `captureDriverVersion()` runs
  `<driver> --version` once and `versionOutputIsClang()` decides. That probe
  doubles as the "can this be run at all" test, so it costs one subprocess,
  not two. Its usability rule is deliberately **not** "exited 0" --
  `--version`'s status answers neither question, and demanding success would
  fall back off a working compiler that merely reports itself oddly. Only
  exec failure counts: `posix_spawnp`'s error on glibc, exit 127 elsewhere.
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
  is the layout the release tarball uses, and is not this repo's build
  tree, where the binary is in `bin/` and the kit in `kit/`: pass
  `--kit=<build dir>/kit` when running `--build-exe` by hand. The **Linux**
  release workflow stages a kit beside the binary, so `--build-exe` works
  from a release with no `--kit`. That release build is configured
  `CMAKE_POSITION_INDEPENDENT_CODE=ON` on purpose: it is cut in an image
  whose clang defaults to non-PIE, and PIC is what lets the result link on a
  user's PIE-default distribution. Two jobs check it -- one inside the build
  image, and `verify-kit-linux` on a bare runner, which is the only one that
  can catch a kit-versus-host toolchain mismatch, since the other links where
  the toolchain agrees with itself by construction. **macOS releases still ship none**: the kit there
  would be universal, and a universal kit is the one configuration nobody has
  verified end to end -- see the open issues (`dz list`) for why an unverified
  one is worse than none.
- `--build-exe` is dispatched by `runToolVerb()` **before `runHermesNode`**,
  alongside the four read-only verbs. It writes files, so it is not read-only
  -- but the criterion for that dispatch point was never read-only-ness, it is
  whether the verb needs a runtime, and this one reads an already-compiled
  container and runs the toolchain: no parser, compiler, event loop or
  `napi_env`. `checkToolOptions()` gains its rows, each naming both flags:
  `--build-exe` against `--bundle`, `--build-bundle`, `-e`/`--eval`, each of
  the four verbs, and `--inspect`/`--inspect-brk`; `--kit` or `--cc`
  without `--build-exe`; `--build-exe` with no positional container; and an
  empty `--build-exe=`, `--kit=` or `--cc=`, which name the flag rather than
  reporting a missing file with no filename in it. Separately, and just after that
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
  `-arch` included. And a produced **Linux** executable inherits the build
  machine's glibc version, so it runs on distributions carrying that and not
  on others, where a macOS one needs only OS-provided libraries. That is why
  the Linux release job builds inside AlmaLinux 8: measured, the floor lands
  at glibc 2.28 -- Node's own Tier 1 x64 requirement -- with no source change,
  and `release.yml` asserts it rather than trusting it. A local build still
  inherits whatever the developer's machine has.
  What causes it, what each remedy costs and what was already measured and
  rejected are in the tracker -- see the open issues (`dz list`).
- **Two worked cases, chosen as a matched pair.** `examples/tetris` and
  `examples/gtop` each wrap a third-party npm package in a one-line entry and
  build it to a binary; both `run.sh` scripts check the executable with
  `node_modules` moved out of the way, which proves the artifact rather than
  asserting it. They differ in what bundling costs. tetris-cli is entirely
  CommonJS with no computed requires and no addons, so the producer emits no
  warnings at all -- its `run.sh` asserts that, since it is the reason that
  example is trivial. gtop is the opposite on both counts: blessed loads
  every widget through `require('./widgets/' + name)`, so each needs
  `--include` (derived from the directory, so a blessed upgrade cannot
  silently drop one), and blessed ships its own terminfo as **data files**,
  which the producer does not package and which therefore travel beside the
  artifact at `node_modules/blessed/usr` -- the path the bundled module still
  resolves, because its `__dirname` keeps its build-time identity. gtop is
  also what surfaced the `zlib` classification bug (see the builtins bullet
  under AOT Bundles). Checking either from a script needs a real terminal,
  which is what `examples/pty-run.py` is for: `setRawMode` does not exist on a
  pipe, and blessed will not lay out without a window size.
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

Two JS tests fail intermittently under the suite's 16-way parallel load and
pass in isolation. **A single red run naming one of these is not a
regression**; confirm before chasing it, because each has cost a session time
already:

```bash
for i in 1 2 3 4 5 6; do
  TEST_THREAD_ID=$i cmake-build-asan/bin/hermes-node test/<name>.js >/dev/null 2>&1
  echo -n "$? "
done
```

Measured 6/6 passing in isolation for both, on both sides of an
unrelated change:

- `test-inspect.js` -- spawns a child and waits for
  `Debugger listening on ws://` on its stderr with a timeout. The most
  frequent of the two, and the timing dependency is explicit in the test.
- `test-repl-history.js` -- same shape, a spawned REPL session read through
  a pipe.

`test-fs-async-verify.js` was the third entry here and is no longer one.
This note suspected it of being genuinely racy rather than load-sensitive,
on the strength of 2/6 and 5/6 failures in isolation; that suspicion was
right. Test 38 read a file test 36 creates, with nothing ordering the two
chains, and `bbfa4f2` fixed it. Measured 12/12 on Linux after, against
roughly five failures in six before. Recorded rather than deleted because
the reasoning is the useful part: a flake that fails in *isolation* is a
race, not a scheduling artifact, and is worth chasing rather than listing.

Neither of the two remaining is understood, neither has been fixed, and
neither is quarantined -- an `XFAIL`ed flake stops reporting the day it
becomes a real failure. The honest state is that they are known,
reproducible only under load, and someone should eventually find out why.

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

## Issue Tracking

Bugs and tasks that outlive a session live in `dz/`, a
[ditz2](https://github.com/tmikov/ditz2) project: one markdown file per issue,
committed alongside the code. Use it for what a progress file cannot hold --
a defect nobody is working on yet, a limitation recorded rather than fixed, a
follow-up that would otherwise survive only in a commit message.

**Read the open issues (`dz list`) before assuming a rough edge is unknown.**
The tracker, not this file, is where known defects and unfixed limitations
live, with the measurements behind them. Do not copy an issue's content here:
two records of the same thing drift, and the issue is the one that gets
updated when the situation changes. This file describes how the system works
now; the tracker describes what is wrong with it.

**Which `dz` to run.** The bundled ditz2 is the reference: the submodule at
`examples/ditz2/ditz2`, whose `package.json` names the version this repo is
pinned to. A `dz` on `PATH` may be used **only** when it is present *and* its
`dz --version` matches that pinned version to the patch series (`0.1.x` for a
bundled `0.1.0`). Otherwise run the bundled one. The check is one line:

```bash
grep -m1 '"version"' examples/ditz2/ditz2/package.json   # the pinned version
dz --version                                             # the installed one
```

A mismatch matters because the on-disk issue format is the interchange
format. An older or newer `dz` writing into `dz/issues/` can produce files
the pinned version does not round-trip, and the damage is committed before
anyone notices.

**Running the bundled one.** With **Node**, not `hermes-node` -- this repo is
frequently mid-change and its runtime may not be in a runnable state, which
is precisely when you need the tracker most. ditz2 is TypeScript ESM, so it
needs a build once per checkout:

```bash
git submodule update --init examples/ditz2/ditz2
npm --prefix examples/ditz2/ditz2 install
npm --prefix examples/ditz2/ditz2 run build
node examples/ditz2/ditz2/dist/cli/main.js list
```

`dist/` and `node_modules/` are covered by ditz2's own `.gitignore`, so the
submodule checkout stays clean. (`examples/ditz2/` is a *separate* thing: it
builds the same source to CJS under `examples/ditz2/dist-cjs/` to exercise
`hermes-node`. Do not use it to run the tracker -- that is the dependency
this section exists to avoid.)

**Notes.**

- `dz help agents` is the contract for programmatic use: every command takes
  `--json`, exit 0 means stdout parses, errors go to stderr as one object.
- The component list starts empty and `--component` rejects every value until
  it is populated. Add one when an issue needs it: `dz component add <name>`.
- `dz/config.local.yaml` (author identity) and `dz/.lock` are gitignored;
  `dz/config.yaml` and `dz/issues/*.md` are committed.
