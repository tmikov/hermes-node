# Implementation Progress

Tracks progress on `history/initial/2026-02-15-async-generators-fix-plan.md`.

The file has two sections: "Status" and "Context Notes".

**Status Section**:

Each row contains the step label from the detailed, plan, a brief description, list of
dependency labels, Status (initially empty), optional brief note (initially empty).

The status of a row is one of:
- "" (empty) initially, before work has started
- "wip" as soon as work on that raw has started.
- "done" when work has completed successfully. Rarely "Brief Note" may contain very brief
explanation. More details in "Context Notes".
- "blocked" when work cannot proceed for some reason. "Brief Note" must contain a brief
explanation. More details in "Context Notes".

Example on start:

| Step 11 | Port util binding | 5 |  |  |

**Context Notes**:

After completing work on a step, either successfully or by blocking, a section for that
step is added. It needs to have the format from the following example (empty bullets can
be omitted):

```
### Step 11: Port util binding
- **Files**: created `foo.c`, modified `bar.c`.
- **Decisions**:
-- Decision 1 concise explanation
-- Decision 2 concise explanation
- **What was done**: ...
- **Issues**: ...
- **Notes for next step**: ...
```

## Status

| Step | Description | Depends On | Status | Brief Note (optional) |
|------|-------------|------------|--------|-----------------------|
| Fix 1 | Enable async generators in runtime (`withEnableAsyncGenerators(true)`) [amend Step 8] | — | done | |
| Fix 2 | Fix primordials: AsyncIteratorPrototype + FinalizationRegistry polyfill [amend Step 4] | Fix 1 | done | |
| Fix 3 | Revert stream async generator workarounds [amend Step 26] | Fix 1 | done | |
| Fix 4 | Revert internal/fs/dir.js workaround [amend Step 29] | Fix 1 | done | |
| Fix 5 | Test fs.promises [amend Step 32] | Fix 1, Fix 3 | done | |
| Fix 6 | Update documentation | Fix 1–5 | done | |

## Context Notes

### Fix 1: Enable async generators in runtime
- **Files**: modified `tools/hermes-node/hermes-node.cpp`, `test/test-fs-async-verify.js`
- **What was done**: Added `.withEnableAsyncGenerators(true)` to `RuntimeConfig::Builder`. Updated test-fs-async-verify.js to no longer assert that `fs.promises` throws SyntaxError (it now fails on missing `credentials` binding instead, which is expected).

### Fix 2: Fix primordials — AsyncIteratorPrototype + FinalizationRegistry polyfill
- **Files**: modified `libjs/primordials.js`, `test/primordials.js`, `test/run-primordials-test.sh`
- **Decisions**:
-- AsyncIteratorPrototype cannot be derived from `async function*` prototype chain in Hermes (Hermes's chain is flat: `gen.prototype.__proto__` is `Object.prototype`, not `AsyncGeneratorPrototype`). Kept as a standalone spec-compliant object with `[Symbol.asyncIterator]`.
-- FinalizationRegistry polyfill is no-op (register/unregister do nothing). Guarded by `typeof FinalizationRegistry === 'undefined'` so it disappears when Hermes adds native support.
-- Primordials test runner (`run-primordials-test.sh`) needs `-Xasync-generators` flag since it uses the stock `hermes` binary, not `hermes-node`.
- **Issues**: Hermes async generator prototype chain is incomplete — a Hermes bug.

### Fix 3: Revert stream async generator workarounds
- **Files**: reverted `libjs-node/internal/streams/readable.js`, `libjs-node/internal/streams/pipeline.js`; deleted `libjs/shims/internal/streams/operators.js`; created `libjs/shims/internal/event_target.js`; updated `libjs-node/README.md`
- **Decisions**:
-- Needed new `internal/event_target` shim because the original `operators.js` requires `internal/event_target` which depends on `internalBinding('performance')`. The shim exports the symbols (`kWeakHandler`, `kResistStopPropagation`) and minimal stubs.
-- Stream operators (`.map`, `.filter`, etc.) still don't fully work at runtime: they go through `compose -> duplexify -> fromAsyncGen` which needs `Promise.withResolvers()` (not available in Hermes). Parse errors are gone though.
- **Issues**: `Promise.withResolvers` missing in Hermes blocks stream operator runtime functionality.

### Fix 4: Revert internal/fs/dir.js workaround
- **Files**: reverted `libjs-node/internal/fs/dir.js` to original; updated `libjs-node/README.md` (removed modification log entry, no vendored files are modified now)
- **What was done**: Restored original `async* entries()` method. Existing fs dir tests pass with original code.

### Fix 5: Test fs.promises
- **Files**: created `libjs/shims/internal/readline/interface.js`, `libjs/shims/internal/worker/js_transferable.js`; modified `lib/bindings/node_file.cpp`, `test/test-fs-async-verify.js`
- **Decisions**:
-- `internal/readline/interface` shimmed because original chain goes readline/interface -> repl/history -> os -> internalBinding('credentials'). fs/promises uses Interface only for FileHandle.readLines() (niche), so stub throws ERR_METHOD_NOT_IMPLEMENTED.
-- `internal/worker/js_transferable` shimmed because original needs internalBinding('messaging'). Worker threads not supported; shim provides Symbol-based kDeserialize/kTransfer/kTransferList and no-op markTransferMode.
-- Promise-mode stat operations get fresh (non-shared) typed arrays to avoid race conditions with concurrent callback-mode operations that write to the shared Float64Array.
- **What was done**: Made fs.promises fully operational. Added `openFileHandle` binding (creates {fd, getAsyncId(), close()} object), `fileHandleClose` (closes fd via uv_fs_close, returns promise), `createFreshStats` for promise-mode stat results. Added 12 promise-based tests (Tests 33-44) covering mkdir+readdir, rename, copyFile, chmod, symlink+readlink, access, FileHandle open/write/stat/close, mkdtemp, realpath, utimes, unlink, link. All 44 tests pass.
- **Issues**: Shared stats buffer race condition -- promise-mode fstat used the shared Float64Array which could be overwritten by concurrent callback-mode stat before the promise JS code read it, causing truncated readFile results. Fixed by allocating fresh typed arrays for promise mode.
- **Notes for next step**: `fs.promises` is now documented as available (was previously marked permanently unavailable). Memory.md entries about fs.promises need updating.

### Fix 6: Update documentation
- **Files**: modified `history/initial/issues-and-workarounds.md`, `history/initial/memory.md`, `history/initial/progress.md`
- **What was done**: Rewrote the "No Async Generators" section in issues-and-workarounds.md to reflect that async generators ARE supported. Updated shims table (removed deleted `operators.js` shim, added `event_target.js`, `readline/interface.js`, `worker/js_transferable.js` shims). Replaced vendored file modifications table (no vendored files are now modified). Added `Promise.withResolvers` as a separate Hermes limitation. Updated memory.md: removed dir.js patch note from FS Dir Binding, updated FS Async Verification to reflect fs.promises availability, updated Streams Verification to remove workaround references.

