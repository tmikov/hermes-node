# Implementation Memory

Non-obvious gotchas and patterns. For project overview, build instructions, bootstrap sequence,
module loader, JS limitations, and test infrastructure, see `CLAUDE.md`.

## Build Gotchas
- ASAN flags: Hermes sets them inside its subdirectory scope only. Top-level CMakeLists.txt must propagate.
- LLVH includes: gtest headers need `llvh/Support/raw_ostream.h`. `add_node_compat_unittest()` helper adds the paths.
- **hermes_napi.h is heavyweight**: pulls in all VM internals. Avoid in public headers. Use `hermes_napi_event_loop.h` for the event loop struct.
- **GC define not propagated**: `HERMESVM_GC_${HERMESVM_GCKIND}` is subdirectory-scoped. Our targets need `target_compile_definitions`.
- **Handle scopes required**: All NAPI calls creating JS values require `napi_open_handle_scope`.
- **Include order**: `hermes_napi.h` must come before `uv_event_loop.h` (struct redefinition).

## NAPI Gotchas
- `napi_default` (=0) = non-enumerable. Use `napi_enumerable` for bindings whose exports are spread in JS.
- `napi_get_value_string_utf8` writes null terminator — can't write directly into an ArrayBuffer of exact string length; use temp buffer + memcpy.
- `napi_property_descriptor` getter: must be `napi_callback` (fn ptr), NOT `napi_value`.
- `napi_add_finalizer` on getter functions: WRONG. Attach to target object instead (getter outlives finalizer).

## Event Loop Adapter
- `UvEventLoop` class (PIMPL): init/run/close lifecycle
- `uv_async_t` handle: unref'd when idle, ref'd when tasks pending (so loop exits cleanly)
- Task queue: mutex + singly-linked list, LIFO push, reversed to FIFO on drain

## FS Gotchas
- Stats layout: Float64Array(36), 18 fields per entry. Promise-mode stats must use fresh (non-shared) typed arrays — shared Float64Array causes race conditions with concurrent callback-mode ops.
- `uv_dirent_t.name` points to `uv_fs_t` request-owned memory. Build JS result BEFORE `uv_fs_req_cleanup`.
- Recursive mkdir: stat after EEXIST to detect file-vs-dir. After loop, stat final path.
- `readFileUtf8`/`writeFileUtf8`: compound open+read/write+close in C++.
- `internalModuleStat(path)`: 0=file, 1=directory, negative=error (no throw).

## V8 API Polyfills
- `Error.captureStackTrace`: polyfilled in `primordials.js`. Creates Error for stack, sets lazy getter.
- `Error.stackTraceLimit`: writable property (default 10) if missing.
- Both picked up by primordials `copyPropsRenamed(Error, ...)`.

## Key Shims
- `internal/abort_controller.js`: minimal AbortController/AbortSignal using EventEmitter (original depends on `event_target` -> `internalBinding('performance')`)
- `internal/event_target.js`: exports `kWeakHandler`/`kResistStopPropagation` symbols only
- `internal/readline/interface.js`: stub (original chain: readline -> repl/history -> os -> credentials)
- `internal/worker/js_transferable.js`: stub (original needs internalBinding('messaging'))
- `internal/blob.js`, `internal/url.js`, `internal/process/permission.js`: stubs

## Hermes NAPI Key Facts
- `hermes_napi_event_loop` (hermes_napi.h): post_work, cancel_work, post_task
- `napi_env__` takes `Runtime&` + optional `hermes_napi_event_loop*`
- Enable microtasks: `RuntimeConfig::Builder().withMicrotaskQueue(true)`, drain with `runtime->drainJobs()`
- `napi_run_script` does global eval with `compileFlags.strict = false`
- Hermes supports `//# sourceURL=` for custom filenames in stack traces

## OS Binding
- `os.js` depends on `internalBinding('credentials')` at load time (for `getTempDir`) -- both bindings must exist.
- Error reporting: ctx-object pattern. Set errno/message/syscall/code on ctx, return undefined. JS side uses `getCheckedFunction()` to throw `ERR_SYSTEM_ERROR`.
- `getInterfaceAddresses` returns flat array [name, addr, netmask, family, mac, internal, scopeid, ...] -- 7 fields per interface. JS reassembles into nested object.
- `getCPUs` returns flat array [model, speed, user, nice, sys, idle, irq, ...] -- 7 fields per CPU. JS reassembles.
- `getOSInformation` returns [sysname, version, release, machine]. Called once at module load; type/version/release/machine are cached as module-level constants.
- `getUserInfo` takes (options, ctx) -- options may be null/object. Returns {uid, gid, username, homedir, shell}.
- `credentials` binding: `getTempDir()` checks TMPDIR/TMP/TEMP env vars; `safeGetenv(key)` is simplified getenv.

## Unverified
- `Duplex.from()` (in `duplexify.js`) may still have issues
