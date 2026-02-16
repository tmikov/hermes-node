# REPL Implementation Analysis

Date: 2026-02-16

## Goal

Implement Node.js-compatible REPL (`node` interactive mode) in hermes-node.
The REPL is one of the first things users see; being as close to Node's real
REPL as possible matters.

## Architecture Overview

Node's REPL is built from four layers:

```
repl.js                  — REPL logic (eval, output, error recovery)
  └── readline.js        — line editing (arrow keys, history, tab completion)
       └── tty.js        — raw mode, terminal size
            └── tty_wrap  — native binding (libuv TTY handle)
  └── vm.js              — script compilation and execution
       └── contextify    — native binding (V8 contexts)
```

All JS modules are already vendored in `libjs-node/`. The blockers are
native bindings and the stdin stream.

## Dependency Analysis

### What We Have

- `events`, `path`, `buffer`, `util`, `stream`, `fs`, `console` all work
- `process.stdout` / `process.stderr` exist as minimal write-only stubs
- TTY detection works (`stdio.getHandleType()` uses `uv_guess_handle`)
- libuv 1.51.0 vendored with full TTY support
- `acorn` JS parser vendored in `libjs-node/internal/deps/acorn/`
- All internal REPL modules present in `libjs-node/internal/repl/`
- All internal readline modules present in `libjs-node/internal/readline/`

### What's Missing

Four native bindings and one infrastructure piece:

1. **`contextify`** — CRITICAL, blocks vm.js and REPL eval
2. **`tty_wrap`** — needed for raw mode and terminal size
3. **`credentials`** — needed to unblock `os` module, which readline
   history uses
4. **`inspector`** — optional, REPL works without it
5. **Readable stdin stream** — needed for interactive input

Plus one existing shim must be replaced:

6. **`internal/readline/interface.js`** — currently a stub that throws
   on construction. Must be replaced with the real module once `os` →
   `credentials` dependency is satisfied.

## Detailed Binding Specifications

### 1. `contextify` Binding (CRITICAL)

**Source**: `node/src/node_contextify.cc` (1068 lines in Node)

The binding provides three capabilities:

#### 1a. ContextifyScript — Script compilation and execution

This is what `vm.runInThisContext()` uses. The REPL creates a
ContextifyScript for each line of user input.

**Constructor** — compiles source code:
```
ContextifyScript(code, filename, lineOffset, columnOffset,
                 cachedData, produceCachedData, parsingContext,
                 hostDefinedOptionId)
```

In Node/V8, this calls `ScriptCompiler::CompileUnboundScript()` to produce
bytecode, then stores the `UnboundScript`.

**runInContext method** — executes the compiled script:
```
runInContext(sandbox, timeout, displayErrors, breakOnSigint,
            breakOnFirstLine)
```

When `sandbox` is null/undefined → runs in current context (this is
`runInThisContext`). When sandbox is an object → runs in a sandboxed
context (this is `runInContext`).

In V8: binds the UnboundScript to a context, calls `script->Run(context)`,
handles timeout via a watchdog thread that calls
`isolate->TerminateExecution()`, handles SIGINT via SigintWatchdog.

**How the REPL uses it** (repl.js lines 530, 587-613):
```javascript
// Compile
script = vm.createScript(userInput, { filename: 'REPL' });

// Execute (useGlobal=true is the default)
if (self.useGlobal) {
  result = script.runInThisContext({ breakOnSigint: true });
} else {
  result = script.runInContext(context, { breakOnSigint: true });
}
```

#### 1b. makeContext — Context isolation (sandboxing)

Creates a new V8 context where a sandbox object becomes the global.
Uses V8 property interceptors (NamedPropertyHandlerConfiguration) to
proxy reads/writes between the sandbox and the context's real global.

The interceptor chain for property access:
1. Check sandbox object first
2. Fall through to context's global proxy
3. Writes go to both sandbox and global proxy

This is deeply V8-specific — V8 Contexts are first-class with their
own global objects and property interceptors. Hermes doesn't have an
equivalent concept.

**However**: The REPL defaults to `useGlobal: true`, which does NOT use
sandboxing. It just runs in the main context. So this can be deferred.

#### 1c. Watchdogs — Timeout and SIGINT handling

**Timeout watchdog**: A separate thread running a uv_timer. If it fires,
calls `isolate->TerminateExecution()`. The eval loop catches the
termination and throws `ERR_SCRIPT_EXECUTION_TIMEOUT`.

**SIGINT watchdog**: Listens for SIGINT signal. On receipt, calls
`isolate->TerminateExecution()`. The REPL uses this so Ctrl+C aborts
the current expression:
```javascript
startSigintWatchdog();
try {
  result = script.runInThisContext({ breakOnSigint: true });
} finally {
  if (stopSigintWatchdog()) {
    self.emit('SIGINT');
  }
}
```

The REPL also temporarily exits raw mode before eval and restores it
after, so Ctrl+C is handled as a signal rather than a raw keypress.

#### Minimal contextify for REPL

For `runInThisContext` with `useGlobal: true` (the default), the
behavior is essentially `eval()` with:
- Source location metadata (filename, line/column offsets)
- Timeout support (optional, nice-to-have)
- SIGINT interruption (nice-to-have for Ctrl+C during eval)
- Proper error decoration

**Hermes approach**: Hermes has `HermesRuntime::evaluateJavaScript()` or
we could use `napi_run_script`. The ContextifyScript class would:
- Constructor: store the source code string (and metadata)
- runInContext with null sandbox: call the Hermes eval equivalent
- Timeout: could implement via a watchdog thread + Hermes's async
  break mechanism (if available), or defer
- SIGINT: similar approach, or defer

**What can be deferred**:
- `makeContext` / `runInContext` with sandbox (not used by default REPL)
- `createCachedData` (code caching, optimization only)
- `measureMemory` (profiling)
- `compileFunctionForCJSLoader` (internal to module loader)
- `containsModuleSyntax` (ESM detection)
- Timeout watchdog (nice-to-have)
- SIGINT watchdog (nice-to-have, but important for UX)

**What's essential**:
- ContextifyScript constructor (compile/store code)
- runInContext with null sandbox (eval in current context)
- startSigintWatchdog / stopSigintWatchdog (for Ctrl+C in REPL)

**Estimated effort**: Medium. The eval part is easy. SIGINT watchdog
requires a signal handler thread + Hermes async break integration.

### 2. `tty_wrap` Binding

**Source**: `node/src/tty_wrap.cc` (172 lines in Node)

Four functions, all thin libuv wrappers:

| Export | libuv call | Description |
|--------|-----------|-------------|
| `new TTY(fd, errArr)` | `uv_tty_init(loop, handle, fd, 0)` | Create TTY handle |
| `getWindowSize(arr)` | `uv_tty_get_winsize(handle, &w, &h)` | Terminal dimensions |
| `setRawMode(flag)` | `uv_tty_set_mode(handle, mode)` | Raw/cooked mode switch |
| `isTTY(fd)` | `uv_guess_handle(fd)` | Check if fd is TTY |

`isTTY` is already effectively implemented in our `stdio` binding's
`getHandleType()`.

**The catch**: In Node, `TTY` extends `LibuvStreamWrap`, which provides
stream read/write via libuv. The TTY constructor creates a `uv_tty_t`
handle that is both readable and writable as a libuv stream.

This means `process.stdin` in Node is a `tty.ReadStream` backed by a
`uv_tty_t` handle on fd 0, with data arriving via libuv's stream read
callbacks (`uv_read_start`).

Our current stdout/stderr are minimal C++ stubs with synchronous
`uv_fs_write` — not real libuv streams. For the REPL to work, we need:

1. The `tty_wrap` binding itself (~50 LOC, trivial)
2. A readable stdin backed by the TTY handle's libuv stream
3. The stream read infrastructure (`uv_read_start` → JS callbacks)

The stream read infrastructure is the real work. It's similar to what
we built for async fs operations — a libuv callback that marshals data
into JS — but for continuous streaming rather than one-shot operations.

**Estimated effort for the binding**: Low (~50 LOC).
**Estimated effort for stream read infra**: Medium-High. Requires
`LibuvStreamWrap` equivalent or at minimum `uv_read_start` integration.

### 3. `credentials` Binding

**Source**: `node/src/node_credentials.cc` (531 lines in Node)

13 functions, all POSIX syscall wrappers:

**Always available (2)**:
- `safeGetenv(key)` — `getenv()` with setuid safety check
  (`getauxval(AT_SECURE)`, compare real/effective uid/gid)
- `getTempDir()` — checks TMPDIR, TMP, TEMP env vars

**POSIX getters (5)**:
- `getuid()` → `getuid(2)`
- `getgid()` → `getgid(2)`
- `geteuid()` → `geteuid(2)`
- `getegid()` → `getegid(2)`
- `getgroups()` → `getgroups(2)` (two calls: first for count, second
  for data; ensures egid is in the list)

**POSIX setters (6)**:
- `setuid(id)` → `setuid(2)` (accepts number or username string via
  `getpwnam_r`)
- `setgid(id)` → `setgid(2)` (accepts number or group name via
  `getgrnam_r`)
- `seteuid(id)` → `seteuid(2)`
- `setegid(id)` → `setegid(2)`
- `setgroups(arr)` → `setgroups(2)`
- `initgroups(user, group)` → `initgroups(3)`

All setter functions include an io_uring security check
(CVE-2024-22017) — if libuv version supports io_uring, the setters
warn/refuse because io_uring operations retain the old credentials.

**Why it matters for REPL**: The dependency chain is:
```
readline/interface.js
  └── internal/repl/history.js
       └── os.js
            └── internalBinding('credentials')  // getTempDir, safeGetenv
```
Also, Node's bootstrap (`internal/bootstrap/node.js` lines 213-220)
loads credentials to set `process.getuid` etc.

**Estimated effort**: Low. ~230 LOC total, pure syscall wrappers, no
external dependencies. Could implement in 2-3 hours.

### 4. `inspector` Binding (OPTIONAL)

Used by `repl.js` via `sendInspectorCommand()` from
`internal/util/inspector.js` for two things:

1. **Context ID for non-global REPL** (repl.js:1123-1132): When
   `useGlobal: false`, uses `Runtime.enable` /
   `Runtime.executionContextCreated` to get the V8 context ID for the
   sandbox. Fallback: creates the context without an ID. Only affects
   the non-default REPL mode.

2. **Tab completion of `let`/`const` variables** (repl.js:1284-1293):
   Uses `Runtime.globalLexicalScopeNames` to discover variables
   declared with `let`/`const` in the REPL scope (these aren't
   properties on `globalThis`, so normal introspection misses them).
   Fallback: returns empty array. Tab completion still works for `var`
   declarations and global object properties.

Both uses have graceful fallbacks — `sendInspectorCommand(cb, onError)`
calls `onError` when the inspector is unavailable (checked via
`internalBinding('config').hasInspector`). The REPL is fully functional
without it. Can be fully deferred.

### 5. Readable stdin Stream

Currently hermes-node has no `process.stdin`. The bootstrap creates
`process.stdout` and `process.stderr` as minimal objects with a C++
`write` function, but stdin doesn't exist.

For the REPL, we need `process.stdin` as a readable stream. In Node:
- `process.stdin` is a `tty.ReadStream` (when fd 0 is a TTY)
- `tty.ReadStream` extends `net.Socket`
- `net.Socket` wraps a `LibuvStreamWrap`
- `LibuvStreamWrap` manages `uv_read_start` / `uv_read_stop`

The full `net.Socket` → `LibuvStreamWrap` chain is heavy. A simpler
approach for initial REPL support:

**Option A**: Minimal C++ stdin binding that uses `uv_read_start` on a
`uv_tty_t` handle and pushes data to a JS callback. Wire this into a
basic readable stream or even a raw event emitter. The readline module
only needs `stream.on('data', cb)` and `stream.on('end', cb)`.

**Option B**: Implement enough of `net.Socket` / stream wrapping to use
Node's real `tty.ReadStream`. More work but more compatible.

**Option C**: Synchronous line reading via `fs.readSync(0, ...)` for a
bare-minimum REPL without line editing. No raw mode, no arrow keys,
just read-eval-print. Could be a starting point.

## REPL Module Internals

### What repl.js does

1. Creates a readline.Interface on stdin/stdout
2. On each line: compiles via `vm.Script`, executes via
   `runInThisContext`, prints result via `util.inspect`
3. Error recovery: uses acorn to detect incomplete input (missing
   closing brace/paren/bracket, unterminated string) and prompts for
   continuation (`...` prompt)
4. Tab completion: introspects the current context's global properties
5. History: persists to `~/.node_repl_history` via
   `internal/repl/history.js`
6. Special commands: `.help`, `.exit`, `.break`, `.clear`, `.save`,
   `.load`, `.editor`
7. SIGINT handling: Ctrl+C aborts current input or expression

### What readline.Interface needs

From the underlying stream:
- `stream.on('data', callback)` — receive input chunks
- `stream.on('end', callback)` — detect EOF
- `stream.on('keypress', callback)` — individual keypress events
  (emitted by `internal/readline/emitKeypressEvents.js` which
  parses ANSI escape sequences from raw data)

From the TTY:
- `setRawMode(true/false)` — character-by-character vs line-buffered
- `getWindowSize()` — for line wrapping calculations
- `columns` property — terminal width

For output:
- `stream.write(string)` — we already have this on stdout

## Implementation Plan

### Phase 1: credentials binding (unblocks os, readline history)

Create `lib/bindings/node_credentials.cpp`:
- Implement all 13 functions as POSIX syscall wrappers
- Register as `internalBinding('credentials')`
- This unblocks: `os` module, `process.getuid()` etc., readline
  history, `safeGetenv` for module loader

Remove the `internal/readline/interface.js` shim (the real module
should now load since its dependency chain is satisfied).

### Phase 2: minimal contextify (unblocks vm, REPL eval)

Create `lib/bindings/node_contextify.cpp`:
- `ContextifyScript` class:
  - Constructor: store source code, filename, offsets
  - `runInContext(sandbox, ...)`: when sandbox is null, eval code in
    current Hermes context via `napi_run_script` or Hermes
    `evaluateJavaScript`
  - Return the result value
- `makeContext`: stub that throws "not implemented" (REPL uses
  `useGlobal: true` by default)
- `startSigintWatchdog` / `stopSigintWatchdog`: implement with a
  signal handler thread. On SIGINT, use Hermes async break to
  interrupt execution. Or stub initially (Ctrl+C won't interrupt
  long-running eval, but REPL still works).
- Constants and symbols: provide the expected exports

### Phase 3: tty_wrap + stdin stream (makes REPL interactive)

Create `lib/bindings/node_tty_wrap.cpp`:
- `TTY` class wrapping `uv_tty_t`:
  - Constructor: `uv_tty_init()`
  - `setRawMode()`: `uv_tty_set_mode()`
  - `getWindowSize()`: `uv_tty_get_winsize()`
  - `isTTY()`: `uv_guess_handle()` (move from stdio binding)
- Stream read support: `uv_read_start` on the TTY handle, with an
  alloc callback and a read callback that pushes `Buffer` chunks to JS

Create `process.stdin`:
- In bootstrap, create a readable stream on fd 0 backed by the TTY
  handle (or pipe handle if not a TTY)
- Wire up the libuv read callbacks to emit 'data' events
- This enables readline to receive keypress input

### Phase 4: REPL entry point

Add `--interactive` / `-i` flag to hermes-node, or default to REPL
when no script argument is given (matching Node's behavior).

In bootstrap or hermes-node.cpp:
```javascript
if (no script argument) {
  require('repl').start({ useGlobal: true });
}
```

### Phase 5: Polish

- SIGINT watchdog for Ctrl+C during eval
- Timeout watchdog for `vm.runInThisContext({ timeout: N })`
- Tab completion (should work out of the box once REPL loads)
- `let`/`const` tab completion (depends on inspector, optional)
- `.node_repl_history` persistence (should work once credentials +
  os + readline are all functional)

## Open Questions

1. **Hermes eval API**: What's the best way to evaluate a string in the
   current Hermes context and get the result? `napi_run_script` exists
   but may not preserve all the semantics ContextifyScript expects
   (e.g., source location metadata, strict mode handling).

2. **Hermes async break**: Does Hermes support interrupting execution
   from another thread (like V8's `TerminateExecution`)? This is needed
   for SIGINT watchdog and timeout. If not, Ctrl+C during a long eval
   won't work — the REPL will hang until the expression completes.

3. **Stream wrap architecture**: Should we implement a minimal
   LibuvStreamWrap equivalent, or wire uv_read_start directly to JS
   callbacks without the full stream wrap layer? The former is more
   compatible (enables net.Socket, tty.ReadStream), the latter is
   faster to implement.

4. **net module**: The full `tty.ReadStream` extends `net.Socket`.
   How much of the `net` module do we need? Could we create a simpler
   readable stream that just wraps the TTY handle without going through
   net.Socket?

## Estimated Total Effort

| Phase | Effort | Blocker for |
|-------|--------|-------------|
| 1. credentials | 2-3 hours | os module, readline history |
| 2. contextify (minimal) | 4-6 hours | vm.runInThisContext, REPL eval |
| 3. tty_wrap + stdin | 6-10 hours | interactive input, raw mode |
| 4. REPL entry point | 1-2 hours | `hermes-node` with no args |
| 5. Polish | 4-8 hours | SIGINT, timeout, completion |

Phases 1-4 give a working REPL. Phase 5 makes it production-quality.
